// SPDX-License-Identifier: MIT
// HEVC parameter-set and slice-header reconstruction adapted from
// radxa-pkg/libva-v4l2 (MIT), for the Raphael Venus VA-API backend.
#include "hevc_annexb.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <vector>

namespace irisva {
struct Error {
    VAStatus status;
};

static void check(bool condition, const char *,
                  VAStatus status = VA_STATUS_ERROR_OPERATION_FAILED) {
    if (!condition)
        throw Error{status};
}

struct HevcSlice {
    VASliceParameterBufferHEVC params{};
    std::vector<uint8_t> bytes;
};
struct HevcPicture {
    VAPictureParameterBufferHEVC params{};
    VAIQMatrixBufferHEVC iq{};
    bool has_params = false;
    bool has_iq = false;
    std::vector<HevcSlice> slices;
    std::vector<VASliceParameterBufferHEVC> pending_slices;
};
struct HevcState {
    bool started = false;
    bool no_rasl_output = false;
    std::vector<uint8_t> vps, sps;
    std::map<unsigned, std::vector<uint8_t>> pps;
};

namespace {
constexpr VAStatus invalid = VA_STATUS_ERROR_INVALID_BUFFER;
unsigned ceil_log2(unsigned n) {
    unsigned bits = 0;
    for (unsigned v = n ? n - 1 : 0; v; v >>= 1)
        ++bits;
    return bits;
}
class Bits {
    std::vector<uint8_t> bytes_;
    size_t pos_ = 0;

  public:
    bool aligned() const {
        return !(pos_ & 7);
    }
    void append(const uint8_t *data, size_t size) {
        check(aligned(), "HEVC byte append not aligned", invalid);
        bytes_.insert(bytes_.end(), data, data + size);
        pos_ += size * 8;
    }
    void bit(unsigned value) {
        if (!(pos_ & 7))
            bytes_.push_back(0);
        bytes_.back() |= (value & 1) << (7 - (pos_++ & 7));
    }
    void bits(uint32_t value, unsigned count) {
        check(count <= 32, "HEVC bit field too wide", invalid);
        for (unsigned i = count; i; --i)
            bit(value >> (i - 1));
    }
    void ue(uint32_t value) {
        check(value < UINT32_MAX, "HEVC Exp-Golomb overflow", invalid);
        unsigned n = 0;
        for (uint32_t v = value + 1; v >>= 1;)
            ++n;
        for (unsigned i = 0; i < n; ++i)
            bit(0);
        bits(value + 1, n + 1);
    }
    void se(int value) {
        int64_t code = value <= 0 ? -2ll * value : 2ll * value - 1;
        check(code < UINT32_MAX, "HEVC signed Exp-Golomb overflow", invalid);
        ue(unsigned(code));
    }
    void align() {
        bit(1);
        while (pos_ & 7)
            bit(0);
    }
    void nal(std::vector<uint8_t> &out, unsigned header, bool trailing = true) {
        if (trailing)
            align();
        check(!(pos_ & 7), "HEVC NAL not byte aligned", invalid);
        out.insert(out.end(), {0, 0, 0, 1, uint8_t(header >> 8), uint8_t(header)});
        unsigned zeros = 0;
        for (uint8_t v : bytes_) {
            if (zeros == 2 && v <= 3) {
                out.push_back(3);
                zeros = 0;
            }
            out.push_back(v);
            zeros = v == 0 ? zeros + 1 : 0;
        }
    }
};
class Reader {
    std::vector<uint8_t> bytes_;
    size_t pos_ = 0;

  public:
    explicit Reader(const std::vector<uint8_t> &nal) {
        check(nal.size() >= 3, "truncated HEVC NAL", invalid);
        unsigned zeros = 0;
        for (size_t i = 2; i < nal.size(); ++i) {
            uint8_t v = nal[i];
            if (zeros == 2 && v == 3) {
                zeros = 0;
                continue;
            }
            bytes_.push_back(v);
            zeros = v == 0 ? zeros + 1 : 0;
        }
    }
    size_t position() const {
        return pos_;
    }
    size_t size_bits() const {
        return bytes_.size() * 8;
    }
    void seek(size_t p) {
        check(p <= size_bits(), "HEVC header offset out of range", invalid);
        pos_ = p;
    }
    void skip(size_t n) {
        check(n <= size_bits() - pos_, "truncated HEVC header", invalid);
        pos_ += n;
    }
    unsigned bit() {
        check(pos_ < size_bits(), "truncated HEVC slice header", invalid);
        size_t p = pos_++;
        return (bytes_[p / 8] >> (7 - p % 8)) & 1;
    }
    uint32_t bits(unsigned n) {
        check(n <= 32, "HEVC read field too wide", invalid);
        uint32_t v = 0;
        while (n--)
            v = (v << 1) | bit();
        return v;
    }
    uint32_t ue() {
        unsigned n = 0;
        while (!bit())
            check(++n < 31, "invalid HEVC Exp-Golomb", invalid);
        return ((1u << n) - 1) + bits(n);
    }
    void copy(Bits &out, size_t end) {
        check(end >= pos_ && end <= size_bits(), "invalid HEVC copy range", invalid);
        // CABAC payload is already byte aligned. Preserve it in bulk rather
        // than spending CPU time rewriting each compressed-data bit.
        if (!(pos_ & 7) && out.aligned()) {
            size_t n = (end - pos_) / 8;
            if (n) {
                out.append(bytes_.data() + pos_ / 8, n);
                pos_ += n * 8;
            }
        }
        while (pos_ < end)
            out.bit(bit());
    }
};
void ptl(Bits &b, unsigned profile) {
    b.bits(0, 3);
    b.bits(profile, 5);
    b.bits(1u << (31 - profile), 32);
    b.bits(0xb, 4); // progressive, non-packed, frame-only
    b.bits(0, 32);
    b.bits(0, 12);
    b.bits(186, 8); // level 6.2
    // VA omits temporal-layer PTL; allow every legal temporal_id (1..7).
    b.bits(0, 16); // six absent sub-layer PTLs + two reserved 2-bit fields
}
void ordering(Bits &b, const VAPictureParameterBufferHEVC &p) {
    b.bit(0);
    b.ue(p.sps_max_dec_pic_buffering_minus1);
    b.ue(p.sps_max_dec_pic_buffering_minus1);
    b.ue(0);
}
void scaling_list(Bits &b, const uint8_t *values, unsigned side, unsigned dc, bool large) {
    b.bit(1); // explicit coefficients
    int last = 8;
    if (large) {
        check(dc, "zero HEVC scaling DC", invalid);
        b.se(int(dc) - 8);
        last = dc;
    }
    // HEVC diagonal scan, beginning (0,0), (0,1), (1,0).
    for (unsigned sum = 0; sum <= 2 * (side - 1); ++sum) {
        for (unsigned x = 0; x < side; ++x) {
            if (sum < x || sum - x >= side)
                continue;
            unsigned y = sum - x;
            int value = values[y * side + x];
            check(value, "zero HEVC scaling coefficient", invalid);
            b.se(((value - last + 128) & 255) - 128);
            last = value;
        }
    }
}
void matrices(Bits &b, const VAIQMatrixBufferHEVC &iq) {
    for (unsigned i = 0; i < 6; ++i)
        scaling_list(b, iq.ScalingList4x4[i], 4, 0, false);
    for (unsigned i = 0; i < 6; ++i)
        scaling_list(b, iq.ScalingList8x8[i], 8, 0, false);
    for (unsigned i = 0; i < 6; ++i)
        scaling_list(b, iq.ScalingList16x16[i], 8, iq.ScalingListDC16x16[i], true);
    for (unsigned i = 0; i < 2; ++i)
        scaling_list(b, iq.ScalingList32x32[i], 8, iq.ScalingListDC32x32[i], true);
}
struct Reference {
    unsigned index;
    int poc;
    bool used;
};
struct References {
    std::vector<Reference> before, after, long_term;
    std::vector<unsigned> list[2];
};
References references(const VAPictureParameterBufferHEVC &p) {
    References result;
    std::set<int> pocs;
    for (unsigned i = 0; i < 15; ++i) {
        const auto &r = p.ReferenceFrames[i];
        if (r.picture_id == VA_INVALID_SURFACE || (r.flags & VA_PICTURE_HEVC_INVALID))
            continue;
        check(r.pic_order_cnt != p.CurrPic.pic_order_cnt && pocs.insert(r.pic_order_cnt).second,
              "invalid HEVC reference POC", invalid);
        bool lt = r.flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE;
        bool used = r.flags & (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
                               VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR);
        auto &list = lt                                          ? result.long_term
                     : r.pic_order_cnt < p.CurrPic.pic_order_cnt ? result.before
                                                                 : result.after;
        list.push_back({i, r.pic_order_cnt, used});
    }
    std::sort(result.before.begin(), result.before.end(),
              [](auto a, auto b) { return a.poc > b.poc; });
    std::sort(result.after.begin(), result.after.end(),
              [](auto a, auto b) { return a.poc < b.poc; });
    std::sort(result.long_term.begin(), result.long_term.end(),
              [](auto a, auto b) { return a.poc > b.poc; });
    for (unsigned l = 0; l < 2; ++l) {
        for (const auto &r : l ? result.after : result.before)
            if (r.used)
                result.list[l].push_back(r.index);
        for (const auto &r : l ? result.before : result.after)
            if (r.used)
                result.list[l].push_back(r.index);
        for (const auto &r : result.long_term)
            if (r.used)
                result.list[l].push_back(r.index);
    }
    return result;
}
void rps(Bits &b, const References &refs, const VAPictureParameterBufferHEVC &p) {
    b.bit(0); // short_term_ref_pic_set_sps_flag; SPS has zero RPS entries
    b.ue(refs.before.size());
    b.ue(refs.after.size());
    for (unsigned l = 0; l < 2; ++l) {
        int64_t previous = p.CurrPic.pic_order_cnt;
        for (const auto &r : l ? refs.after : refs.before) {
            int64_t delta = l ? int64_t(r.poc) - previous : previous - r.poc;
            check(delta > 0 && delta <= 32768, "HEVC short-term POC delta out of range", invalid);
            b.ue(unsigned(delta - 1));
            b.bit(r.used);
            previous = r.poc;
        }
    }
    if (p.slice_parsing_fields.bits.long_term_ref_pics_present_flag) {
        b.ue(refs.long_term.size()); // no SPS long-term entries
        unsigned n = p.log2_max_pic_order_cnt_lsb_minus4 + 4, mask = (1u << n) - 1;
        int64_t current_msb =
            int64_t(p.CurrPic.pic_order_cnt) - (unsigned(p.CurrPic.pic_order_cnt) & mask);
        int64_t previous_cycle = 0;
        for (const auto &r : refs.long_term) {
            unsigned lsb = unsigned(r.poc) & mask;
            int64_t cycle = (current_msb - (int64_t(r.poc) - lsb)) / (1u << n);
            check(cycle >= previous_cycle && cycle < UINT32_MAX,
                  "HEVC long-term MSB cycle unsupported", VA_STATUS_ERROR_UNIMPLEMENTED);
            b.bits(lsb, n);
            b.bit(r.used);
            b.bit(1);
            b.ue(unsigned(cycle - previous_cycle));
            previous_cycle = cycle;
        }
    } else
        check(refs.long_term.empty(), "HEVC long-term references without SPS flag", invalid);
}
// Preserve the original header except its RPS and reference-list mapping. The
// original SPS RPS definitions are absent from VA; explicit picture-local RPS
// and list modifications express the same VA reference graph to the firmware.
unsigned rewrite_slice(const HevcSlice &slice, const VAPictureParameterBufferHEVC &p,
                       const References &refs, std::vector<uint8_t> &out) {
    Reader r(slice.bytes);
    Bits b;
    unsigned header = unsigned(slice.bytes[0]) << 8 | slice.bytes[1];
    unsigned type = (header >> 9) & 63;
    check(!(header & 0x8000) && !(header & 0x1f8) && (header & 7) && type <= 31,
          "invalid or multilayer HEVC slice NAL", invalid);
    const auto &f = p.slice_parsing_fields.bits;
    unsigned first = r.bit();
    b.bit(first);
    // VA owns presentation/bumping. Every submitted VA picture needs a
    // completed surface, even when the original bitstream suppresses output.
    if (type >= 16 && type <= 23) {
        r.bit();
        b.bit(0);
    }
    size_t copied = r.position();
    unsigned pps_id = r.ue();
    check(pps_id <= 63, "invalid HEVC PPS ID", invalid);
    bool dependent = false;
    if (!first) {
        if (f.dependent_slice_segments_enabled_flag)
            dependent = r.bit();
        unsigned ctb = 1u << (p.log2_min_luma_coding_block_size_minus3 + 3 +
                              p.log2_diff_max_min_luma_coding_block_size);
        unsigned count = ((p.pic_width_in_luma_samples + ctb - 1) / ctb) *
                         ((p.pic_height_in_luma_samples + ctb - 1) / ctb);
        check(r.bits(ceil_log2(count)) == slice.params.slice_segment_address,
              "HEVC slice address mismatch", invalid);
    }
    auto copy_to = [&](size_t end) {
        size_t old = r.position();
        r.seek(copied);
        r.copy(b, end);
        copied = end;
        r.seek(old);
    };
    if (!dependent) {
        r.skip(p.num_extra_slice_header_bits);
        unsigned slice_type = r.ue();
        check(slice_type <= 2 && slice_type == slice.params.LongSliceFlags.fields.slice_type,
              "HEVC slice type mismatch", invalid);
        if (f.output_flag_present_flag) {
            copy_to(r.position());
            r.bit();
            b.bit(1);
            copied = r.position();
        }
        if (type != 19 && type != 20) {
            r.bits(p.log2_max_pic_order_cnt_lsb_minus4 + 4);
            copy_to(r.position());
            bool from_sps = r.bit();
            if (from_sps) {
                check(p.num_short_term_ref_pic_sets > 0, "HEVC missing SPS RPS", invalid);
                r.bits(ceil_log2(p.num_short_term_ref_pic_sets));
            } else {
                size_t start = r.position();
                bool predicted = p.num_short_term_ref_pic_sets && r.bit();
                if (predicted) {
                    check(p.st_rps_bits > 0, "HEVC predicted RPS bit count missing", invalid);
                    r.seek(start);
                    r.skip(p.st_rps_bits);
                } else {
                    unsigned neg = r.ue(), pos = r.ue();
                    check(neg + pos <= 15, "invalid HEVC RPS size", invalid);
                    for (unsigned i = 0; i < neg + pos; ++i) {
                        r.ue();
                        r.bit();
                    }
                }
            }
            if (f.long_term_ref_pics_present_flag) {
                unsigned ns = p.num_long_term_ref_pic_sps ? r.ue() : 0;
                unsigned np = r.ue();
                check(ns <= p.num_long_term_ref_pic_sps && ns + np <= 15,
                      "invalid HEVC long-term RPS", invalid);
                for (unsigned i = 0; i < ns + np; ++i) {
                    if (i < ns)
                        r.bits(ceil_log2(p.num_long_term_ref_pic_sps));
                    else {
                        r.bits(p.log2_max_pic_order_cnt_lsb_minus4 + 4);
                        r.bit();
                    }
                    if (r.bit())
                        r.ue();
                }
            }
            copied = r.position();
            rps(b, refs, p);
            if (f.sps_temporal_mvp_enabled_flag)
                r.bit();
        }
        if (f.sample_adaptive_offset_enabled_flag) {
            r.bit();
            r.bit();
        }
        if (slice_type != 2) {
            unsigned counts[2] = {p.num_ref_idx_l0_default_active_minus1 + 1u,
                                  p.num_ref_idx_l1_default_active_minus1 + 1u};
            if (r.bit()) {
                counts[0] = r.ue() + 1;
                if (slice_type == 0)
                    counts[1] = r.ue() + 1;
            }
            unsigned lists = slice_type == 0 ? 2 : 1, total = refs.list[0].size();
            check(total > 0 && total <= 15, "HEVC inter slice without reference pictures", invalid);
            copy_to(r.position());
            for (unsigned l = 0; l < lists; ++l) {
                check(counts[l] <= 15, "invalid HEVC active reference count", invalid);
                if (f.lists_modification_present_flag && total > 1 && r.bit())
                    r.skip(counts[l] * ceil_log2(total));
                if (total > 1)
                    b.bit(1); // PPS always enables list modifications
                for (unsigned j = 0; j < counts[l]; ++j) {
                    unsigned index = slice.params.RefPicList[l][j];
                    auto it = std::find(refs.list[l].begin(), refs.list[l].end(), index);
                    check(it != refs.list[l].end(), "HEVC active reference missing from RPS",
                          invalid);
                    if (total > 1)
                        b.bits(unsigned(it - refs.list[l].begin()), ceil_log2(total));
                }
            }
            copied = r.position();
        }
    }
    // VA's data offset includes the two-byte NAL header, excludes EPBs.
    check(slice.params.slice_data_byte_offset >= 3, "missing HEVC slice data offset", invalid);
    size_t data = size_t(slice.params.slice_data_byte_offset - 2) * 8;
    check(data <= r.size_bits() && data >= r.position(), "invalid HEVC slice data offset", invalid);
    size_t alignment = data, parsed = r.position();
    do {
        check(alignment > parsed && data - alignment < 8, "invalid HEVC byte alignment", invalid);
        r.seek(--alignment);
    } while (!r.bit());
    r.seek(copied);
    r.copy(b, alignment);
    b.align();
    r.seek(data);
    r.copy(b, r.size_bits());
    b.nal(out, header, false);
    return pps_id;
}
void cache(std::vector<uint8_t> &out, std::vector<uint8_t> &previous, Bits &b, unsigned type) {
    std::vector<uint8_t> nal;
    b.nal(nal, (type << 9) | 1);
    if (previous != nal) {
        out.insert(out.end(), nal.begin(), nal.end());
        previous = std::move(nal);
    }
}
} // namespace

std::vector<uint8_t> hevc_bitstream(VAProfile profile, const HevcPicture &pic, HevcState &state) {
    check(pic.has_params && !pic.slices.empty() && pic.pending_slices.empty(),
          "HEVC picture missing parameters or slices", invalid);
    check(pic.slices.front().bytes.size() >= 2, "truncated HEVC NAL", invalid);
    unsigned type = (pic.slices.front().bytes[0] >> 1) & 63;
    if (!state.started) {
        // Kodi can recreate the VA context mid-GOP when switching/refreshing a
        // stream. Iris consumes dependent pictures before the first IRAP without
        // producing CAPTURE buffers. Reject them before queuing anything, so the
        // caller can discard the pictures and advance to BLA/IDR/CRA instead of
        // filling its surface pool with requests which will never complete.
        check(type >= 16 && type <= 21, "HEVC context requires an initial random-access picture",
              VA_STATUS_ERROR_DECODING_ERROR);
    }
    // A first CRA/BLA has NoRaslOutputFlag set in the stateful decoder. Its
    // RASL pictures can still reference the preceding GOP and produce no output.
    // RADL pictures are independently decodable and must remain accepted.
    check(!(state.no_rasl_output && (type == 8 || type == 9)),
          "HEVC RASL picture unavailable after random access", VA_STATUS_ERROR_DECODING_ERROR);
    const auto &p = pic.params;
    const auto &f = p.pic_fields.bits;
    const auto &s = p.slice_parsing_fields.bits;
    unsigned depth = profile == VAProfileHEVCMain10 ? 2 : 0;
    check(f.chroma_format_idc == 1 && !f.separate_colour_plane_flag &&
              p.bit_depth_luma_minus8 == depth && p.bit_depth_chroma_minus8 == depth,
          "HEVC requires matching Main NV12 or Main10 P010", VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
    check(!(p.CurrPic.flags & VA_PICTURE_HEVC_FIELD_PIC), "interlaced HEVC unsupported",
          VA_STATUS_ERROR_UNIMPLEMENTED);
    check(p.pic_width_in_luma_samples && p.pic_height_in_luma_samples &&
              p.sps_max_dec_pic_buffering_minus1 <= 15 &&
              p.log2_max_pic_order_cnt_lsb_minus4 <= 12 && p.num_short_term_ref_pic_sets <= 64 &&
              p.num_long_term_ref_pic_sps <= 32 && p.log2_min_luma_coding_block_size_minus3 <= 3 &&
              p.log2_diff_max_min_luma_coding_block_size <= 3 &&
              p.log2_min_luma_coding_block_size_minus3 +
                      p.log2_diff_max_min_luma_coding_block_size <=
                  3 &&
              p.num_tile_columns_minus1 <= 19 && p.num_tile_rows_minus1 <= 21 &&
              p.num_extra_slice_header_bits <= 7,
          "invalid HEVC sequence parameters", invalid);
    check(!f.scaling_list_enabled_flag || pic.has_iq, "HEVC scaling matrices missing", invalid);
    unsigned profile_id = depth ? 2 : 1;
    std::vector<uint8_t> out, slices;
    std::set<unsigned> ids;
    auto refs = references(p);
    for (const auto &slice : pic.slices)
        ids.insert(rewrite_slice(slice, p, refs, slices));
    Bits vps;
    vps.bits(0, 4);
    vps.bits(3, 2);
    vps.bits(0, 6);
    vps.bits(6, 3);
    vps.bit(1);
    vps.bits(65535, 16);
    ptl(vps, profile_id);
    ordering(vps, p);
    vps.bits(0, 6);
    vps.ue(0);
    vps.bit(0);
    vps.bit(0);
    cache(out, state.vps, vps, 32);
    Bits sps;
    sps.bits(0, 4);
    sps.bits(6, 3);
    sps.bit(1);
    ptl(sps, profile_id);
    sps.ue(0);
    sps.ue(1);
    sps.ue(p.pic_width_in_luma_samples);
    sps.ue(p.pic_height_in_luma_samples);
    sps.bit(0);
    sps.ue(depth);
    sps.ue(depth);
    sps.ue(p.log2_max_pic_order_cnt_lsb_minus4);
    ordering(sps, p);
    sps.ue(p.log2_min_luma_coding_block_size_minus3);
    sps.ue(p.log2_diff_max_min_luma_coding_block_size);
    sps.ue(p.log2_min_transform_block_size_minus2);
    sps.ue(p.log2_diff_max_min_transform_block_size);
    sps.ue(p.max_transform_hierarchy_depth_inter);
    sps.ue(p.max_transform_hierarchy_depth_intra);
    sps.bit(f.scaling_list_enabled_flag);
    if (f.scaling_list_enabled_flag)
        sps.bit(0);
    sps.bit(f.amp_enabled_flag);
    sps.bit(s.sample_adaptive_offset_enabled_flag);
    sps.bit(f.pcm_enabled_flag);
    if (f.pcm_enabled_flag) {
        sps.bits(p.pcm_sample_bit_depth_luma_minus1, 4);
        sps.bits(p.pcm_sample_bit_depth_chroma_minus1, 4);
        sps.ue(p.log2_min_pcm_luma_coding_block_size_minus3);
        sps.ue(p.log2_diff_max_min_pcm_luma_coding_block_size);
        sps.bit(f.pcm_loop_filter_disabled_flag);
    }
    sps.ue(0);
    sps.bit(s.long_term_ref_pics_present_flag);
    if (s.long_term_ref_pics_present_flag)
        sps.ue(0);
    sps.bit(s.sps_temporal_mvp_enabled_flag);
    sps.bit(f.strong_intra_smoothing_enabled_flag);
    sps.bit(0);
    sps.bit(0); // presentation metadata belongs to the VA application
    cache(out, state.sps, sps, 33);
    for (unsigned id : ids) {
        Bits pps;
        pps.ue(id);
        pps.ue(0);
        pps.bit(s.dependent_slice_segments_enabled_flag);
        pps.bit(s.output_flag_present_flag);
        pps.bits(p.num_extra_slice_header_bits, 3);
        pps.bit(f.sign_data_hiding_enabled_flag);
        pps.bit(s.cabac_init_present_flag);
        pps.ue(p.num_ref_idx_l0_default_active_minus1);
        pps.ue(p.num_ref_idx_l1_default_active_minus1);
        pps.se(p.init_qp_minus26);
        pps.bit(f.constrained_intra_pred_flag);
        pps.bit(f.transform_skip_enabled_flag);
        pps.bit(f.cu_qp_delta_enabled_flag);
        if (f.cu_qp_delta_enabled_flag)
            pps.ue(p.diff_cu_qp_delta_depth);
        pps.se(p.pps_cb_qp_offset);
        pps.se(p.pps_cr_qp_offset);
        pps.bit(s.pps_slice_chroma_qp_offsets_present_flag);
        pps.bit(f.weighted_pred_flag);
        pps.bit(f.weighted_bipred_flag);
        pps.bit(f.transquant_bypass_enabled_flag);
        pps.bit(f.tiles_enabled_flag);
        pps.bit(f.entropy_coding_sync_enabled_flag);
        if (f.tiles_enabled_flag) {
            pps.ue(p.num_tile_columns_minus1);
            pps.ue(p.num_tile_rows_minus1);
            pps.bit(0);
            for (unsigned i = 0; i < p.num_tile_columns_minus1; ++i)
                pps.ue(p.column_width_minus1[i]);
            for (unsigned i = 0; i < p.num_tile_rows_minus1; ++i)
                pps.ue(p.row_height_minus1[i]);
            pps.bit(f.loop_filter_across_tiles_enabled_flag);
        }
        pps.bit(f.pps_loop_filter_across_slices_enabled_flag);
        pps.bit(1);
        pps.bit(s.deblocking_filter_override_enabled_flag);
        pps.bit(s.pps_disable_deblocking_filter_flag);
        if (!s.pps_disable_deblocking_filter_flag) {
            pps.se(p.pps_beta_offset_div2);
            pps.se(p.pps_tc_offset_div2);
        }
        pps.bit(f.scaling_list_enabled_flag);
        if (f.scaling_list_enabled_flag)
            matrices(pps, pic.iq);
        pps.bit(1);
        pps.ue(p.log2_parallel_merge_level_minus2);
        pps.bit(s.slice_segment_header_extension_present_flag);
        pps.bit(0);
        cache(out, state.pps[id], pps, 34);
    }
    out.insert(out.end(), slices.begin(), slices.end());
    // end_picture commits this candidate state only after Decoder::submit succeeds.
    if (type >= 16 && type <= 21)
        state.no_rasl_output = !state.started || type <= 20;
    state.started = true;
    return out;
}
} // namespace irisva

struct venus_hevc_state {
    irisva::HevcState value;
};

static int hevc_error_to_errno(VAStatus status)
{
    switch (status) {
    case VA_STATUS_ERROR_INVALID_BUFFER:
    case VA_STATUS_ERROR_INVALID_PARAMETER:
        return -EINVAL;
    case VA_STATUS_ERROR_UNIMPLEMENTED:
    case VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT:
        return -ENOTSUP;
    default:
        return -EIO;
    }
}

extern "C" int venus_hevc_build_access_unit(
    VAProfile profile,
    const VAPictureParameterBufferHEVC *picture,
    const VAIQMatrixBufferHEVC *iq_matrix,
    const struct venus_hevc_slice *slices, size_t num_slices,
    const struct venus_hevc_state *previous_state,
    struct venus_hevc_state **next_state,
    uint8_t **access_unit, size_t *access_unit_size)
{
    if (!picture || !slices || !num_slices || num_slices > 64 ||
        !next_state || !access_unit || !access_unit_size)
        return -EINVAL;
    if (profile != VAProfileHEVCMain && profile != VAProfileHEVCMain10)
        return -ENOTSUP;

    *next_state = nullptr;
    *access_unit = nullptr;
    *access_unit_size = 0;
    try {
        std::unique_ptr<venus_hevc_state> candidate(
            new venus_hevc_state{});
        irisva::HevcPicture payload;
        if (previous_state)
            candidate->value = previous_state->value;
        payload.params = *picture;
        payload.has_params = true;
        if (iq_matrix) {
            payload.iq = *iq_matrix;
            payload.has_iq = true;
        }
        size_t total_slice_bytes = 0;
        for (size_t index = 0; index < num_slices; ++index) {
            if (!slices[index].parameters || !slices[index].data ||
                !slices[index].size ||
                slices[index].size >
                    8u * 1024u * 1024u - total_slice_bytes)
                return -EINVAL;
            total_slice_bytes += slices[index].size;
            irisva::HevcSlice slice;
            slice.params = *slices[index].parameters;
            slice.bytes.assign(slices[index].data,
                               slices[index].data + slices[index].size);
            if (slice.bytes.size() >= 4 &&
                slice.bytes[0] == 0 && slice.bytes[1] == 0) {
                size_t prefix = slice.bytes[2] == 1 ? 3u
                    : slice.bytes[2] == 0 && slice.bytes[3] == 1
                          ? 4u : 0u;
                slice.bytes.erase(slice.bytes.begin(),
                                  slice.bytes.begin() + prefix);
            }
            payload.slices.push_back(std::move(slice));
        }

        std::vector<uint8_t> bytes =
            irisva::hevc_bitstream(profile, payload, candidate->value);
        if (bytes.empty() || bytes.size() > 8u * 1024u * 1024u)
            return -EOVERFLOW;
        uint8_t *copy = static_cast<uint8_t *>(std::malloc(bytes.size()));
        if (!copy)
            return -ENOMEM;
        std::memcpy(copy, bytes.data(), bytes.size());
        *access_unit_size = bytes.size();
        *access_unit = copy;
        *next_state = candidate.release();
        return 0;
    } catch (const irisva::Error &error) {
        return hevc_error_to_errno(error.status);
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (...) {
        return -EIO;
    }
}

extern "C" void venus_hevc_state_free(struct venus_hevc_state *state)
{
    delete state;
}
