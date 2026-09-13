// SPDX-License-Identifier: MIT
#include "h264_annexb.h"

#include "bit_writer.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

struct byte_writer {
    uint8_t *data;
    size_t capacity;
    size_t length;
};

static int append_bytes(struct byte_writer *writer, const void *data,
                        size_t size)
{
    if (size > writer->capacity - writer->length)
        return -ENOSPC;

    memcpy(writer->data + writer->length, data, size);
    writer->length += size;
    return 0;
}

static int append_start_code(struct byte_writer *writer)
{
    static const uint8_t start_code[] = { 0, 0, 0, 1 };

    return append_bytes(writer, start_code, sizeof(start_code));
}

static int profile_idc(VAProfile profile)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        return 66;
    case VAProfileH264Main:
        return 77;
    case VAProfileH264High:
        return 100;
    default:
        return -EINVAL;
    }
}

static uint8_t constraint_flags(int profile)
{
    if (profile == 66)
        return 0xc0;
    if (profile == 77)
        return 0x40;
    return 0;
}

static int append_escaped_nal(struct byte_writer *output, uint8_t nal_header,
                              const uint8_t *rbsp, size_t rbsp_size)
{
    size_t index;
    unsigned int zero_count = 0;
    int result;

    result = append_start_code(output);
    if (result < 0)
        return result;

    result = append_bytes(output, &nal_header, sizeof(nal_header));
    if (result < 0)
        return result;

    for (index = 0; index < rbsp_size; index++) {
        uint8_t byte = rbsp[index];

        if (zero_count >= 2 && byte <= 3) {
            static const uint8_t prevention = 3;

            result = append_bytes(output, &prevention, sizeof(prevention));
            if (result < 0)
                return result;
            zero_count = 0;
        }

        result = append_bytes(output, &byte, sizeof(byte));
        if (result < 0)
            return result;

        zero_count = byte == 0 ? zero_count + 1 : 0;
    }

    return 0;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
static bool picture_uses_fmo(const VAPictureParameterBufferH264 *picture)
{
    return picture->num_slice_groups_minus1 != 0;
}
#pragma GCC diagnostic pop

static int append_sps(struct byte_writer *output, VAProfile profile,
                      const VAPictureParameterBufferH264 *picture)
{
    struct venus_bit_writer bits;
    uint8_t rbsp[256];
    int profile_value;
    uint32_t poc_type;

    profile_value = profile_idc(profile);
    if (profile_value < 0)
        return profile_value;

    if (picture->bit_depth_luma_minus8 != 0 ||
        picture->bit_depth_chroma_minus8 != 0 ||
        picture->seq_fields.bits.chroma_format_idc != 1 ||
        !picture->seq_fields.bits.frame_mbs_only_flag ||
        picture_uses_fmo(picture))
        return -ENOTSUP;

    poc_type = picture->seq_fields.bits.pic_order_cnt_type;
    if (poc_type == 1)
        return -ENOTSUP;

    venus_bits_init(&bits, rbsp, sizeof(rbsp));
    venus_bits_write(&bits, (uint32_t)profile_value, 8);
    venus_bits_write(&bits, constraint_flags(profile_value), 8);
    venus_bits_write(&bits, 41, 8);
    venus_bits_write_ue(&bits, 0);

    if (profile_value == 100) {
        venus_bits_write_ue(&bits,
                            picture->seq_fields.bits.chroma_format_idc);
        venus_bits_write_ue(&bits, picture->bit_depth_luma_minus8);
        venus_bits_write_ue(&bits, picture->bit_depth_chroma_minus8);
        venus_bits_write(&bits, 0, 1);
        venus_bits_write(&bits, 0, 1);
    }

    venus_bits_write_ue(
        &bits, picture->seq_fields.bits.log2_max_frame_num_minus4);
    venus_bits_write_ue(&bits, poc_type);
    if (poc_type == 0)
        venus_bits_write_ue(
            &bits,
            picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);

    venus_bits_write_ue(&bits, picture->num_ref_frames);
    venus_bits_write(
        &bits,
        picture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag, 1);
    venus_bits_write_ue(&bits, picture->picture_width_in_mbs_minus1);
    venus_bits_write_ue(&bits, picture->picture_height_in_mbs_minus1);
    venus_bits_write(&bits, 1, 1);
    venus_bits_write(
        &bits, picture->seq_fields.bits.direct_8x8_inference_flag, 1);
    venus_bits_write(&bits, 0, 1);
    venus_bits_write(&bits, 0, 1);
    venus_bits_finish_rbsp(&bits);

    if (!venus_bits_ok(&bits))
        return -ENOSPC;

    return append_escaped_nal(output, 0x67, rbsp, venus_bits_size(&bits));
}

static int append_pps(struct byte_writer *output,
                      const VAPictureParameterBufferH264 *picture,
                      const VASliceParameterBufferH264 *first_slice)
{
    struct venus_bit_writer bits;
    uint8_t rbsp[256];
    bool has_extension;

    venus_bits_init(&bits, rbsp, sizeof(rbsp));
    venus_bits_write_ue(&bits, 0);
    venus_bits_write_ue(&bits, 0);
    venus_bits_write(
        &bits, picture->pic_fields.bits.entropy_coding_mode_flag, 1);
    venus_bits_write(
        &bits, picture->pic_fields.bits.pic_order_present_flag, 1);
    venus_bits_write_ue(&bits, 0);
    venus_bits_write_ue(
        &bits, first_slice->num_ref_idx_l0_active_minus1);
    venus_bits_write_ue(
        &bits, first_slice->num_ref_idx_l1_active_minus1);
    venus_bits_write(&bits, picture->pic_fields.bits.weighted_pred_flag, 1);
    venus_bits_write(
        &bits, picture->pic_fields.bits.weighted_bipred_idc, 2);
    venus_bits_write_se(&bits, picture->pic_init_qp_minus26);
    venus_bits_write_se(&bits, picture->pic_init_qs_minus26);
    venus_bits_write_se(&bits, picture->chroma_qp_index_offset);
    venus_bits_write(
        &bits,
        picture->pic_fields.bits.deblocking_filter_control_present_flag, 1);
    venus_bits_write(
        &bits, picture->pic_fields.bits.constrained_intra_pred_flag, 1);
    venus_bits_write(
        &bits, picture->pic_fields.bits.redundant_pic_cnt_present_flag, 1);

    has_extension =
        picture->pic_fields.bits.transform_8x8_mode_flag ||
        picture->second_chroma_qp_index_offset !=
            picture->chroma_qp_index_offset;
    if (has_extension) {
        venus_bits_write(
            &bits, picture->pic_fields.bits.transform_8x8_mode_flag, 1);
        venus_bits_write(&bits, 0, 1);
        venus_bits_write_se(
            &bits, picture->second_chroma_qp_index_offset);
    }

    venus_bits_finish_rbsp(&bits);
    if (!venus_bits_ok(&bits))
        return -ENOSPC;

    return append_escaped_nal(output, 0x68, rbsp, venus_bits_size(&bits));
}

static bool has_start_code(const uint8_t *data, size_t size)
{
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
        return true;

    return size >= 4 && data[0] == 0 && data[1] == 0 &&
           data[2] == 0 && data[3] == 1;
}

static int append_slices(struct byte_writer *output,
                         const struct venus_h264_slice_batch *batches,
                         size_t num_batches)
{
    size_t batch_index;

    for (batch_index = 0; batch_index < num_batches; batch_index++) {
        const struct venus_h264_slice_batch *batch =
            &batches[batch_index];
        size_t parameter_index;

        if (!batch->parameters || !batch->data ||
            batch->num_parameters == 0)
            return -EINVAL;

        for (parameter_index = 0;
             parameter_index < batch->num_parameters;
             parameter_index++) {
            const VASliceParameterBufferH264 *parameter =
                &batch->parameters[parameter_index];
            const uint8_t *slice;
            size_t slice_size;
            int result;

            if (parameter->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
                parameter->slice_data_offset > batch->data_size ||
                parameter->slice_data_size >
                    batch->data_size - parameter->slice_data_offset ||
                parameter->slice_data_size == 0)
                return -EINVAL;

            slice = batch->data + parameter->slice_data_offset;
            slice_size = parameter->slice_data_size;

            if (!has_start_code(slice, slice_size)) {
                result = append_start_code(output);
                if (result < 0)
                    return result;
            }

            result = append_bytes(output, slice, slice_size);
            if (result < 0)
                return result;
        }
    }

    return 0;
}

int venus_h264_build_access_unit(
    VAProfile profile, const VAPictureParameterBufferH264 *picture,
    const struct venus_h264_slice_batch *batches, size_t num_batches,
    uint8_t *output, size_t output_capacity, size_t *output_size)
{
    struct byte_writer writer = {
        .data = output,
        .capacity = output_capacity,
    };
    const VASliceParameterBufferH264 *first_slice;
    int result;

    if (!picture || !batches || num_batches == 0 || !output ||
        output_capacity == 0 || !output_size ||
        !batches[0].parameters || batches[0].num_parameters == 0)
        return -EINVAL;

    *output_size = 0;
    first_slice = &batches[0].parameters[0];

    result = append_sps(&writer, profile, picture);
    if (result < 0)
        return result;

    result = append_pps(&writer, picture, first_slice);
    if (result < 0)
        return result;

    result = append_slices(&writer, batches, num_batches);
    if (result < 0)
        return result;

    *output_size = writer.length;
    return 0;
}
