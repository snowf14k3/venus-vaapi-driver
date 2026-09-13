// SPDX-License-Identifier: MIT
#include "h264_annexb.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct bit_reader {
    const uint8_t *data;
    size_t size;
    size_t bit;
};

static uint32_t read_bits(struct bit_reader *reader, unsigned int count)
{
    uint32_t value = 0;
    unsigned int index;

    assert(count <= 32);
    assert(reader->bit + count <= reader->size * 8);

    for (index = 0; index < count; index++) {
        size_t bit = reader->bit++;

        value = (value << 1) |
                ((reader->data[bit / 8] >> (7 - bit % 8)) & 1u);
    }

    return value;
}

static uint32_t read_ue(struct bit_reader *reader)
{
    unsigned int leading_zero_bits = 0;

    while (read_bits(reader, 1) == 0)
        leading_zero_bits++;

    if (leading_zero_bits == 0)
        return 0;

    return ((1u << leading_zero_bits) - 1) +
           read_bits(reader, leading_zero_bits);
}

static size_t start_codes(const uint8_t *data, size_t size)
{
    size_t index;
    size_t count = 0;

    for (index = 0; index + 3 < size; index++) {
        if (data[index] == 0 && data[index + 1] == 0 &&
            data[index + 2] == 0 && data[index + 3] == 1)
            count++;
    }

    return count;
}

static void initialize_picture(VAPictureParameterBufferH264 *picture)
{
    memset(picture, 0, sizeof(*picture));
    picture->picture_width_in_mbs_minus1 = 39;
    picture->picture_height_in_mbs_minus1 = 29;
    picture->num_ref_frames = 1;
    picture->seq_fields.bits.chroma_format_idc = 1;
    picture->seq_fields.bits.frame_mbs_only_flag = 1;
    picture->seq_fields.bits.direct_8x8_inference_flag = 1;
    picture->seq_fields.bits.log2_max_frame_num_minus4 = 0;
    picture->seq_fields.bits.pic_order_cnt_type = 0;
    picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 2;
    picture->pic_fields.bits.deblocking_filter_control_present_flag = 1;
    picture->second_chroma_qp_index_offset =
        picture->chroma_qp_index_offset;
}

int main(int argc, char **argv)
{
    VAPictureParameterBufferH264 picture;
    VASliceParameterBufferH264 parameters[2] = { 0 };
    const uint8_t slices[] = {
        0xaa, 0xbb,
        0x65, 0x88, 0x84,
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9a,
    };
    struct venus_h264_slice_batch batches[2];
    uint8_t output[1024];
    size_t output_size = 0;

    initialize_picture(&picture);

    parameters[0].slice_data_offset = 2;
    parameters[0].slice_data_size = 3;
    parameters[0].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

    parameters[1].slice_data_offset = 5;
    parameters[1].slice_data_size = 6;
    parameters[1].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

    batches[0] = (struct venus_h264_slice_batch) {
        .parameters = &parameters[0],
        .num_parameters = 1,
        .data = slices,
        .data_size = sizeof(slices),
    };
    batches[1] = (struct venus_h264_slice_batch) {
        .parameters = &parameters[1],
        .num_parameters = 1,
        .data = slices,
        .data_size = sizeof(slices),
    };

    assert(venus_h264_build_access_unit(
               VAProfileH264High, &picture, batches, 2,
               output, sizeof(output), &output_size) == 0);
    assert(output_size > 20);
    assert(start_codes(output, output_size) == 4);
    assert(output[4] == 0x67);

    {
        struct bit_reader sps = {
            .data = output + 5,
            .size = output_size - 5,
        };

        assert(read_bits(&sps, 8) == 100);
        assert(read_bits(&sps, 8) == 0);
        assert(read_bits(&sps, 8) == 41);
        assert(read_ue(&sps) == 0);
        assert(read_ue(&sps) == 1);
        assert(read_ue(&sps) == 0);
        assert(read_ue(&sps) == 0);
        assert(read_bits(&sps, 1) == 0);
        assert(read_bits(&sps, 1) == 0);
        assert(read_ue(&sps) == 0);
        assert(read_ue(&sps) == 0);
        assert(read_ue(&sps) == 2);
        assert(read_ue(&sps) == 1);
        assert(read_bits(&sps, 1) == 0);
        assert(read_ue(&sps) == 39);
        assert(read_ue(&sps) == 29);
        assert(read_bits(&sps, 1) == 1);
    }

    {
        const uint8_t *second =
            memmem(output + 5, output_size - 5,
                   "\x00\x00\x00\x01\x68", 5);
        const uint8_t *idr =
            memmem(output + 5, output_size - 5,
                   "\x00\x00\x00\x01\x65", 5);

        assert(second != NULL);
        assert(idr != NULL);
    }

    if (argc == 2) {
        FILE *stream = fopen(argv[1], "wb");

        assert(stream != NULL);
        assert(fwrite(output, 1, output_size, stream) == output_size);
        assert(fclose(stream) == 0);
    }

    assert(venus_h264_build_access_unit(
               VAProfileHEVCMain, &picture, batches, 2,
               output, sizeof(output), &output_size) == -EINVAL);

    picture.bit_depth_luma_minus8 = 2;
    assert(venus_h264_build_access_unit(
               VAProfileH264High, &picture, batches, 2,
               output, sizeof(output), &output_size) == -ENOTSUP);
    picture.bit_depth_luma_minus8 = 0;

    parameters[0].slice_data_size = UINT32_MAX;
    assert(venus_h264_build_access_unit(
               VAProfileH264High, &picture, batches, 2,
               output, sizeof(output), &output_size) == -EINVAL);
    parameters[0].slice_data_size = 3;

    assert(venus_h264_build_access_unit(
               VAProfileH264High, &picture, batches, 2,
               output, 8, &output_size) == -ENOSPC);

    puts("PASS: H.264 SPS/PPS and slice Annex-B assembly");
    return 0;
}
