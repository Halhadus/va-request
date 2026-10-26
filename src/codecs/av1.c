/*
 * v4l2 — AV1 parameter translation
 *
 * Maps VA-API AV1 buffers to V4L2 stateless controls and submits
 * a decode request.
 */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_dec_av1.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#include "va_v4l2.h"

static void av1_fill_sequence_params(struct v4l2_ctrl_av1_sequence *seq,
                                     const VADecPictureParameterBufferAV1 *pic)
{
    memset(seq, 0, sizeof(*seq));

    seq->seq_profile = pic->profile;
    seq->max_frame_width_minus_1 = pic->frame_width_minus1;
    seq->max_frame_height_minus_1 = pic->frame_height_minus1;
    seq->order_hint_bits = pic->order_hint_bits_minus_1 + 1;

    /* bit_depth_idx: 0/1/2 -> 8/10/12 bits */
    seq->bit_depth = 8 + pic->bit_depth_idx * 2;

    seq->flags = 0;
    if (pic->seq_info_fields.fields.still_picture)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_STILL_PICTURE;
    if (pic->seq_info_fields.fields.use_128x128_superblock)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_USE_128X128_SUPERBLOCK;
    if (pic->seq_info_fields.fields.enable_filter_intra)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_FILTER_INTRA;
    if (pic->seq_info_fields.fields.enable_intra_edge_filter)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTRA_EDGE_FILTER;
    if (pic->seq_info_fields.fields.enable_interintra_compound)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_INTERINTRA_COMPOUND;
    if (pic->seq_info_fields.fields.enable_masked_compound)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_MASKED_COMPOUND;
    if (pic->seq_info_fields.fields.enable_dual_filter)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_DUAL_FILTER;
    if (pic->seq_info_fields.fields.enable_order_hint)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_ORDER_HINT;
    if (pic->seq_info_fields.fields.enable_jnt_comp)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_JNT_COMP;
    if (pic->seq_info_fields.fields.enable_cdef)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_CDEF;
    if (pic->seq_info_fields.fields.mono_chrome)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_MONO_CHROME;
    if (pic->seq_info_fields.fields.color_range)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_COLOR_RANGE;
    if (pic->seq_info_fields.fields.subsampling_x)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_X;
    if (pic->seq_info_fields.fields.subsampling_y)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_SUBSAMPLING_Y;
    if (pic->seq_info_fields.fields.film_grain_params_present)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_FILM_GRAIN_PARAMS_PRESENT;
    if (pic->pic_info_fields.bits.use_superres)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_SUPERRES;
    /* Warped motion and loop restoration live in different VA sub-structs */
    if (pic->pic_info_fields.bits.allow_warped_motion)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_WARPED_MOTION;
    if (pic->pic_info_fields.bits.use_ref_frame_mvs)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_REF_FRAME_MVS;
    if (pic->loop_restoration_fields.bits.yframe_restoration_type ||
        pic->loop_restoration_fields.bits.cbframe_restoration_type ||
        pic->loop_restoration_fields.bits.crframe_restoration_type)
        seq->flags |= V4L2_AV1_SEQUENCE_FLAG_ENABLE_RESTORATION;
    /* VA's seq_info_fields exposes no sequence-level separate_uv_delta_q /
     * warped-motion / ref-frame-mvs bits — map only what VA actually
     * reports (the per-picture bits above). Never force sequence tools on
     * from unrelated signals; the kernel trusts this control verbatim. */
}

static uint32_t av1_surface_order_hint(struct vav4l2_driver_data *dd,
                                      VASurfaceID sid)
{
    struct vav4l2_surface *s;

    s = vav4l2_surface_by_id(dd, sid);
    return s ? s->order_hint : 0;
}

/* Spec 7.8 get_relative_dist. VA stores order_hint_bits_minus_1. */
static int av1_relative_dist(unsigned a, unsigned b, unsigned bits_minus_1)
{
    unsigned diff = a - b;
    unsigned m = 1u << bits_minus_1;

    return (int)((diff & (m - 1)) - (diff & m));
}

/*
 * Spec 5.9.22 skip_mode_params — VA only gives skip_mode_present.
 * skip_mode_frame[] are LAST..ALTREF type ids (1..7), not DPB slots.
 */
static void av1_fill_skip_mode(struct v4l2_ctrl_av1_frame *frame,
                               const VADecPictureParameterBufferAV1 *pic,
                               struct vav4l2_driver_data *dd)
{
    int forward_idx = -1, backward_idx = -1;
    int forward_hint = 0, backward_hint = 0;
    int second_forward_idx = -1, second_forward_hint = 0;
    unsigned bits_minus_1 = pic->order_hint_bits_minus_1;
    int i;

    frame->skip_mode_frame[0] = 0;
    frame->skip_mode_frame[1] = 0;

    if (pic->pic_info_fields.bits.frame_type == V4L2_AV1_KEY_FRAME ||
        pic->pic_info_fields.bits.frame_type == V4L2_AV1_INTRA_ONLY_FRAME)
        return;
    if (!pic->mode_control_fields.bits.reference_select)
        return;
    if (!pic->seq_info_fields.fields.enable_order_hint)
        return;

    for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
        uint8_t slot = pic->ref_frame_idx[i];
        VASurfaceID sid;
        uint32_t ref_hint;
        int dist;

        if (slot >= V4L2_AV1_TOTAL_REFS_PER_FRAME)
            continue;
        sid = pic->ref_frame_map[slot];
        ref_hint = av1_surface_order_hint(dd, sid);
        dist = av1_relative_dist(ref_hint, pic->order_hint, bits_minus_1);
        if (dist < 0) {
            if (forward_idx < 0 ||
                av1_relative_dist(ref_hint, (unsigned)forward_hint, bits_minus_1) > 0) {
                forward_idx = i;
                forward_hint = (int)ref_hint;
            }
        } else if (dist > 0) {
            if (backward_idx < 0 ||
                av1_relative_dist(ref_hint, (unsigned)backward_hint, bits_minus_1) < 0) {
                backward_idx = i;
                backward_hint = (int)ref_hint;
            }
        }
    }

    if (forward_idx < 0)
        return;

    if (backward_idx >= 0) {
        int lo = forward_idx < backward_idx ? forward_idx : backward_idx;
        int hi = forward_idx > backward_idx ? forward_idx : backward_idx;

        frame->skip_mode_frame[0] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + lo);
        frame->skip_mode_frame[1] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + hi);
        return;
    }

    for (i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
        uint8_t slot = pic->ref_frame_idx[i];
        VASurfaceID sid;
        uint32_t ref_hint;

        if (slot >= V4L2_AV1_TOTAL_REFS_PER_FRAME)
            continue;
        sid = pic->ref_frame_map[slot];
        ref_hint = av1_surface_order_hint(dd, sid);
        if (av1_relative_dist(ref_hint, (unsigned)forward_hint, bits_minus_1) < 0) {
            if (second_forward_idx < 0 ||
                av1_relative_dist(ref_hint, (unsigned)second_forward_hint,
                                  bits_minus_1) > 0) {
                second_forward_idx = i;
                second_forward_hint = (int)ref_hint;
            }
        }
    }

    if (second_forward_idx < 0)
        return;

    {
        int lo = forward_idx < second_forward_idx ? forward_idx : second_forward_idx;
        int hi = forward_idx > second_forward_idx ? forward_idx : second_forward_idx;

        frame->skip_mode_frame[0] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + lo);
        frame->skip_mode_frame[1] = (uint8_t)(V4L2_AV1_REF_LAST_FRAME + hi);
    }
}

/* Recover refresh metadata from a complete OBU when the client supplies it. */
struct av1_br { const uint8_t *d; size_t n; size_t bit; };

static unsigned av1_br_f(struct av1_br *br, unsigned nb)
{
    unsigned v = 0;
    while (nb--) {
        size_t byte = br->bit >> 3;
        if (byte >= br->n) {
            br->bit = (size_t)-1;
            return 0;
        }
        v = (v << 1) | (unsigned)((br->d[byte] >> (7 - (br->bit & 7))) & 1u);
        br->bit++;
    }
    return v;
}

/* Sequence syntax determines the offsets of original frame metadata. */
static int av1_sequence_header(const uint8_t *data, size_t size, struct vav4l2_av1_header *header)
{
    struct av1_br b = {data, size, 0};
    struct vav4l2_av1_header h = {.valid = 1, .screen_tools = 2, .integer_mv = 2};
    unsigned delay_bits = 0;
    unsigned profile = av1_br_f(&b, 3); av1_br_f(&b, 1); h.reduced = av1_br_f(&b, 1);
    if (profile) return 0;
    if (h.reduced) av1_br_f(&b, 5);
    else {
        if (av1_br_f(&b, 1)) {
            av1_br_f(&b, 32); av1_br_f(&b, 32);
            h.equal_interval = av1_br_f(&b, 1);
            if (h.equal_interval) {
                unsigned zeros = 0;
                while (!av1_br_f(&b, 1) && b.bit != SIZE_MAX) if (++zeros > 31) return 0;
                av1_br_f(&b, zeros);
            }
            h.decoder_model = av1_br_f(&b, 1);
            if (h.decoder_model) {
                delay_bits = av1_br_f(&b, 5) + 1;
                av1_br_f(&b, 32);
                h.removal_bits = av1_br_f(&b, 5) + 1;
                h.presentation_bits = av1_br_f(&b, 5) + 1;
            }
        }
        unsigned display_delay = av1_br_f(&b, 1), points = av1_br_f(&b, 5) + 1;
        h.points = points;
        for (unsigned i = 0; i < points; i++) {
            h.point_idc[i] = av1_br_f(&b, 12); unsigned level = av1_br_f(&b, 5); if (level > 7) av1_br_f(&b, 1);
            if (h.decoder_model && (h.model_present[i] = av1_br_f(&b, 1))) {
                av1_br_f(&b, delay_bits); av1_br_f(&b, delay_bits); av1_br_f(&b, 1);
            }
            if (display_delay && av1_br_f(&b, 1)) av1_br_f(&b, 4);
        }
    }
    unsigned wb = av1_br_f(&b, 4) + 1, hb = av1_br_f(&b, 4) + 1;
    av1_br_f(&b, wb); av1_br_f(&b, hb);
    if (!h.reduced && av1_br_f(&b, 1)) {
        unsigned delta = av1_br_f(&b, 4) + 2; h.frame_id_bits = delta + av1_br_f(&b, 3) + 1;
    }
    av1_br_f(&b, 3);
    if (!h.reduced) {
        av1_br_f(&b, 4);
        unsigned order = av1_br_f(&b, 1); if (order) av1_br_f(&b, 2);
        h.screen_tools = av1_br_f(&b, 1) ? 2 : av1_br_f(&b, 1);
        if (h.screen_tools) h.integer_mv = av1_br_f(&b, 1) ? 2 : av1_br_f(&b, 1);
        if (order) h.order_hint_bits = av1_br_f(&b, 3) + 1;
    }
    if (b.bit == SIZE_MAX) return 0;
    *header = h; return 1;
}
static int av1_header_refresh(const uint8_t *data, size_t size, struct vav4l2_av1_header *h,
    const VADecPictureParameterBufferAV1 *p, unsigned temporal, unsigned spatial, uint8_t *out)
{
    struct av1_br b = {data, size, 0};
    unsigned ft = 0, show = 1, er = 1;
    if (!h->reduced) {
        if (av1_br_f(&b, 1)) return 0;
        ft = av1_br_f(&b, 2); show = av1_br_f(&b, 1);
        if (show && h->decoder_model && !h->equal_interval) av1_br_f(&b, h->presentation_bits);
        if (!show) av1_br_f(&b, 1);
        if (ft != 3 && !(ft == 0 && show)) er = av1_br_f(&b, 1);
    }
    unsigned disable_cdf = av1_br_f(&b, 1);
    unsigned screen = h->screen_tools == 2 ? av1_br_f(&b, 1) : h->screen_tools;
    unsigned integer = screen ? h->integer_mv == 2 ? av1_br_f(&b, 1) : h->integer_mv : 0;
    int intra = ft == 0 || ft == 2;
    uint32_t frame_id = h->frame_id_bits ? av1_br_f(&b, h->frame_id_bits) : 0;
    if (ft != 3 && !h->reduced) av1_br_f(&b, 1);
    unsigned order = av1_br_f(&b, h->order_hint_bits), primary = intra || er ? 7 : av1_br_f(&b, 3);
    unsigned removal_present = h->decoder_model ? av1_br_f(&b, 1) : 0;
    uint32_t removal[32] = {0};
    if (removal_present) for (unsigned i = 0; i < h->points; i++) {
        unsigned idc = h->point_idc[i];
        if (h->model_present[i] && (!idc || ((idc & (1u << temporal)) && (idc & (1u << (spatial + 8))))))
            removal[i] = av1_br_f(&b, h->removal_bits);
    }
    unsigned refresh = ft == 3 || (ft == 0 && show) ? 255 : av1_br_f(&b, 8);
    if (b.bit == SIZE_MAX || ft != p->pic_info_fields.bits.frame_type || show != p->pic_info_fields.bits.show_frame ||
        er != p->pic_info_fields.bits.error_resilient_mode || disable_cdf != p->pic_info_fields.bits.disable_cdf_update ||
        screen != p->pic_info_fields.bits.allow_screen_content_tools || (!intra && integer != p->pic_info_fields.bits.force_integer_mv) ||
        h->order_hint_bits != (p->seq_info_fields.fields.enable_order_hint ? (unsigned)p->order_hint_bits_minus_1 + 1 : 0) ||
        (h->order_hint_bits && order != p->order_hint) || primary != p->primary_ref_frame) return 0;
    h->frame_id = frame_id; h->removal_present = removal_present; memcpy(h->removal_time, removal, sizeof(removal));
    *out = refresh; return 1;
}
VAStatus vav4l2_av1_refresh_flags(struct vav4l2_av1_header *header, const VADecPictureParameterBufferAV1 *p,
    const uint8_t *data, size_t size, uint8_t *out)
{
    if (!header || !p || !out || (!data && size)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vav4l2_av1_header next = *header;
    next.frame_id = next.removal_present = 0; memset(next.removal_time, 0, sizeof(next.removal_time));
    size_t pos = 0; int found = 0; uint8_t refresh = 0;
    while (pos < size) {
        uint8_t h = data[pos++]; unsigned type = h >> 3 & 15;
        if ((h & 0x81) || !(h & 2)) break;
        unsigned temporal = 0, spatial = 0;
        if (h & 4) { if (pos == size || (data[pos] & 7)) break; temporal = data[pos] >> 5; spatial = data[pos] >> 3 & 3; pos++; }
        size_t length = 0; unsigned shift = 0; int complete = 0;
        for (unsigned i = 0; i < 8 && pos < size; i++) {
            unsigned v = data[pos++];
            if (shift >= sizeof(size_t) * 8 || (size_t)(v & 127) > (SIZE_MAX >> shift)) return VA_STATUS_ERROR_INVALID_PARAMETER;
            length |= (size_t)(v & 127) << shift;
            if (!(v & 128)) { complete = 1; break; } shift += 7;
        }
        if (!complete || length > size - pos) break;
        if (type == 1 && !av1_sequence_header(data + pos, length, &next)) next.valid = 0;
        if ((type == 3 || type == 6) && next.valid) {
            uint8_t value;
            if (av1_header_refresh(data + pos, length, &next, p, temporal, spatial, &value)) {
                if (found) return VA_STATUS_ERROR_INVALID_PARAMETER;
                refresh = value; found = 1;
            }
        }
        pos += length;
    }
    if (found) { *header = next; *out = refresh; return VA_STATUS_SUCCESS; }
    /* These two frame types have an implicit mask in the AV1 specification.
     * Other raw-tile pictures need the original sequence/frame OBU metadata. */
    if ((!next.valid || (!next.decoder_model && !next.frame_id_bits)) &&
        (p->pic_info_fields.bits.frame_type == 3 || (p->pic_info_fields.bits.frame_type == 0 && p->pic_info_fields.bits.show_frame))) {
        *header = next; *out = 255; return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* AV1 5.9.14: uniform tiles partition superblocks, not pixels or MI units. */
static int av1_tile_axis(uint32_t *starts, uint32_t *widths, unsigned count,
                          uint32_t mi, uint32_t sb_mi, int uniform,
                          const uint16_t *sizes)
{
    uint32_t sb_count = (mi + sb_mi - 1) / sb_mi;
    uint32_t pos = 0, step = 0;
    unsigned log2 = 0;

    if (!count || count > 32 || count > sb_count)
        return -1;
    if (uniform) {
        while ((1u << log2) < count)
            log2++;
        step = (sb_count + (1u << log2) - 1) >> log2;
        if ((sb_count + step - 1) / step != count)
            return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        uint32_t width = uniform ? step :
                         (i + 1 == count ? sb_count - pos : (uint32_t)sizes[i] + 1);
        if (!width || pos >= sb_count || (i + 1 < count && width >= sb_count - pos))
            return -1;
        if (width > sb_count - pos) {
            if (!uniform || i + 1 != count)
                return -1;
            width = sb_count - pos;
        }
        starts[i] = pos * sb_mi;
        widths[i] = width - 1;
        pos += width;
    }
    if (pos != sb_count)
        return -1;
    starts[count] = mi;
    return 0;
}

VAStatus vav4l2_av1_fill_tile_info(struct v4l2_av1_tile_info *info,
                                  const VADecPictureParameterBufferAV1 *pic)
{
    unsigned cols = pic->tile_cols, rows = pic->tile_rows;
    uint32_t width = (uint32_t)pic->frame_width_minus1 + 1;
    uint32_t height = (uint32_t)pic->frame_height_minus1 + 1;
    unsigned denom = pic->superres_scale_denominator ? pic->superres_scale_denominator : 8;
    unsigned sb_mi = pic->seq_info_fields.fields.use_128x128_superblock ? 32 : 16;
    int uniform = pic->pic_info_fields.bits.uniform_tile_spacing_flag;

    memset(info, 0, sizeof(*info));
    if (!cols || !rows || cols > 32 || rows > 32 || cols * rows > 32 ||
        pic->context_update_tile_id >= cols * rows || denom < 8 || denom > 16 ||
        pic->order_hint_bits_minus_1 > 7 || pic->pic_info_fields.bits.large_scale_tile)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (denom > 8)
        width = (width * 8 + denom / 2) / denom;
    /* Spec compute_image_size: MiCols = 2 * ((FrameWidth + 7) >> 3). */
    uint32_t mi_cols = 2 * ((width + 7) / 8);
    uint32_t mi_rows = 2 * ((height + 7) / 8);
    if (av1_tile_axis(info->mi_col_starts, info->width_in_sbs_minus_1,
                     cols, mi_cols, sb_mi, uniform, pic->width_in_sbs_minus_1) < 0 ||
        av1_tile_axis(info->mi_row_starts, info->height_in_sbs_minus_1,
                     rows, mi_rows, sb_mi, uniform, pic->height_in_sbs_minus_1) < 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    info->flags = uniform ? V4L2_AV1_TILE_INFO_FLAG_UNIFORM_TILE_SPACING : 0;
    info->tile_cols = cols;
    info->tile_rows = rows;
    info->context_update_tile_id = pic->context_update_tile_id;
    info->tile_size_bytes = 1; /* Single tile: unused, but never underflows. */
    return VA_STATUS_SUCCESS;
}

VAStatus vav4l2_av1_prepare_tiles(const VADecPictureParameterBufferAV1 *pic,
                                  const VASliceParameterBufferAV1 * const *tiles,
                                  unsigned n, const uint8_t *data, uint32_t size,
                                  uint32_t capacity, struct v4l2_av1_tile_info *info,
                                  struct v4l2_ctrl_av1_tile_group_entry *entries,
                                  uint32_t *start, uint32_t *length)
{
    VAStatus st = vav4l2_av1_fill_tile_info(info, pic);
    unsigned grid = (unsigned)pic->tile_cols * pic->tile_rows;
    if (st != VA_STATUS_SUCCESS)
        return st;
    if (!data || !size || !capacity || n > 32 || (n != grid && (n || grid != 1)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    memset(entries, 0, sizeof(*entries) * grid);
    if (!n) {
        if (size > capacity)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        entries[0].tile_size = size;
        *start = 0;
        *length = size;
        return VA_STATUS_SUCCESS;
    }
    for (unsigned i = 0; i < n; i++) {
        const VASliceParameterBufferAV1 *t = tiles[i];
        if (!t || t->tile_row != i / pic->tile_cols || t->tile_column != i % pic->tile_cols ||
            t->slice_data_flag != VA_SLICE_DATA_FLAG_ALL || !t->slice_data_size ||
            t->slice_data_offset > size || t->slice_data_size > size - t->slice_data_offset)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (i && t->slice_data_offset < tiles[i - 1]->slice_data_offset + tiles[i - 1]->slice_data_size)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    *start = tiles[0]->slice_data_offset;
    *length = tiles[n - 1]->slice_data_offset + tiles[n - 1]->slice_data_size - *start;
    if (*length > capacity)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (n > 1) {
        /* VA omits TileSizeBytes. Recover it only from a complete raw tile
         * group: inter-tile gaps unambiguously carry the next size field.
         * For two tiles, only a raw group starting at its size field is
         * sufficient. Whole-OBU two-tile spans and split groups are refused
         * rather than guessing a field consumed by the hardware. */
        uint32_t bytes = n > 2 ? tiles[1]->slice_data_offset -
            (tiles[0]->slice_data_offset + tiles[0]->slice_data_size) : *start;
        if (bytes < 1 || bytes > 4)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        for (unsigned i = 0; i < n; i++) {
            uint32_t off = tiles[i]->slice_data_offset;
            if (i) {
                uint32_t gap = off - (tiles[i - 1]->slice_data_offset + tiles[i - 1]->slice_data_size);
                if (gap != (i + 1 < n ? bytes : 0))
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            if (i + 1 < n) {
                uint32_t value = 0;
                if (off < bytes)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                for (unsigned b = 0; b < bytes; b++)
                    value |= (uint32_t)data[off - bytes + b] << (8 * b);
                if (value != tiles[i]->slice_data_size - 1)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
        info->tile_size_bytes = bytes;
    }
    for (unsigned i = 0; i < n; i++) {
        entries[i].tile_offset = tiles[i]->slice_data_offset - *start;
        entries[i].tile_size = tiles[i]->slice_data_size;
        entries[i].tile_row = tiles[i]->tile_row;
        entries[i].tile_col = tiles[i]->tile_column;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus av1_fill_frame_params(struct v4l2_ctrl_av1_frame *frame,
                                  const VADecPictureParameterBufferAV1 *pic,
                                  struct vav4l2_context *ctx,
                                  const uint8_t *span, size_t span_n,
                                  const uint8_t * const *all_spans,
                                  const uint32_t *all_sizes, struct vav4l2_av1_header *header)
{
    struct vav4l2_driver_data *dd = ctx ? ctx->driver_data : NULL;
    memset(frame, 0, sizeof(*frame));

    /* Super-res: VA gives the DISPLAY size and the denominator; the coded
     * size (what the tile grid and SB layout live on) must be derived.
     * Equal when denominator == 8, which is why non-superres streams
     * never noticed the two fields being filled identically. */
    uint32_t disp_w = (uint32_t)pic->frame_width_minus1 + 1;
    uint32_t disp_h = (uint32_t)pic->frame_height_minus1 + 1;
    uint32_t sr_denom = pic->superres_scale_denominator ?
                        pic->superres_scale_denominator : 8;
    uint32_t coded_w = sr_denom > 8 ?
                       (disp_w * 8 + sr_denom / 2) / sr_denom : disp_w;

    /* Quantization */
    frame->quantization.flags = 0;
    if (pic->qmatrix_fields.bits.using_qmatrix)
        frame->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_USING_QMATRIX;
    if (pic->mode_control_fields.bits.delta_q_present_flag)
        frame->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_DELTA_Q_PRESENT;
    if (pic->u_dc_delta_q != pic->v_dc_delta_q ||
        pic->u_ac_delta_q != pic->v_ac_delta_q)
        frame->quantization.flags |= V4L2_AV1_QUANTIZATION_FLAG_DIFF_UV_DELTA;
    frame->quantization.base_q_idx = pic->base_qindex;
    frame->quantization.delta_q_y_dc = pic->y_dc_delta_q;
    frame->quantization.delta_q_u_dc = pic->u_dc_delta_q;
    frame->quantization.delta_q_u_ac = pic->u_ac_delta_q;
    frame->quantization.delta_q_v_dc = pic->v_dc_delta_q;
    frame->quantization.delta_q_v_ac = pic->v_ac_delta_q;
    frame->quantization.qm_y = pic->qmatrix_fields.bits.qm_y;
    frame->quantization.qm_u = pic->qmatrix_fields.bits.qm_u;
    frame->quantization.qm_v = pic->qmatrix_fields.bits.qm_v;
    frame->quantization.delta_q_res = pic->mode_control_fields.bits.log2_delta_q_res;

    frame->superres_denom = pic->superres_scale_denominator;
    if (frame->superres_denom == 0)
        frame->superres_denom = 8;

    /* Segmentation */
    frame->segmentation.flags = 0;
    if (pic->seg_info.segment_info_fields.bits.enabled)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_ENABLED;
    if (pic->seg_info.segment_info_fields.bits.update_map)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_MAP;
    if (pic->seg_info.segment_info_fields.bits.temporal_update)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
    if (pic->seg_info.segment_info_fields.bits.update_data)
        frame->segmentation.flags |= V4L2_AV1_SEGMENTATION_FLAG_UPDATE_DATA;
    memcpy(frame->segmentation.feature_enabled, pic->seg_info.feature_mask,
           sizeof(frame->segmentation.feature_enabled));
    for (int s = 0; s < 8; s++) {
        for (int f = 0; f < 8 && f < V4L2_AV1_SEG_LVL_MAX; f++)
            frame->segmentation.feature_data[s][f] = pic->seg_info.feature_data[s][f];
        if (pic->seg_info.feature_mask[s])
            frame->segmentation.last_active_seg_id = (uint8_t)s;
    }

    /* Loop filter */
    frame->loop_filter.flags = 0;
    if (pic->loop_filter_info_fields.bits.mode_ref_delta_enabled)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_ENABLED;
    if (pic->loop_filter_info_fields.bits.mode_ref_delta_update)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_UPDATE;
    if (pic->mode_control_fields.bits.delta_lf_present_flag)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_PRESENT;
    if (pic->mode_control_fields.bits.delta_lf_multi)
        frame->loop_filter.flags |= V4L2_AV1_LOOP_FILTER_FLAG_DELTA_LF_MULTI;
    frame->loop_filter.level[0] = pic->filter_level[0];
    frame->loop_filter.level[1] = pic->filter_level[1];
    frame->loop_filter.level[2] = pic->filter_level_u;
    frame->loop_filter.level[3] = pic->filter_level_v;
    frame->loop_filter.sharpness = pic->loop_filter_info_fields.bits.sharpness_level;
    frame->loop_filter.delta_lf_res = pic->mode_control_fields.bits.log2_delta_lf_res;
    memcpy(frame->loop_filter.ref_deltas, pic->ref_deltas, sizeof(frame->loop_filter.ref_deltas));
    memcpy(frame->loop_filter.mode_deltas, pic->mode_deltas, sizeof(frame->loop_filter.mode_deltas));

    /* CDEF */
    frame->cdef.damping_minus_3 = pic->cdef_damping_minus_3;
    frame->cdef.bits = pic->cdef_bits;
    for (int i = 0; i < 8; i++) {
        frame->cdef.y_pri_strength[i] = pic->cdef_y_strengths[i] >> 2;
        frame->cdef.y_sec_strength[i] = pic->cdef_y_strengths[i] & 0x03;
        frame->cdef.uv_pri_strength[i] = pic->cdef_uv_strengths[i] >> 2;
        frame->cdef.uv_sec_strength[i] = pic->cdef_uv_strengths[i] & 0x03;
    }

    av1_fill_skip_mode(frame, pic, dd);
    frame->primary_ref_frame = pic->primary_ref_frame;

    /* Loop restoration */
    frame->loop_restoration.flags = 0;
    frame->loop_restoration.lr_unit_shift = pic->loop_restoration_fields.bits.lr_unit_shift;
    frame->loop_restoration.lr_uv_shift = pic->loop_restoration_fields.bits.lr_uv_shift;
    frame->loop_restoration.frame_restoration_type[0] =
        pic->loop_restoration_fields.bits.yframe_restoration_type;
    frame->loop_restoration.frame_restoration_type[1] =
        pic->loop_restoration_fields.bits.cbframe_restoration_type;
    frame->loop_restoration.frame_restoration_type[2] =
        pic->loop_restoration_fields.bits.crframe_restoration_type;
    if (frame->loop_restoration.frame_restoration_type[0])
        frame->loop_restoration.flags |= V4L2_AV1_LOOP_RESTORATION_FLAG_USES_LR;
    if (frame->loop_restoration.frame_restoration_type[1] ||
        frame->loop_restoration.frame_restoration_type[2])
        frame->loop_restoration.flags |= V4L2_AV1_LOOP_RESTORATION_FLAG_USES_CHROMA_LR;
    if (frame->loop_restoration.flags) {
        uint32_t luma = 256u >> frame->loop_restoration.lr_unit_shift;
        uint32_t chroma = luma >> frame->loop_restoration.lr_uv_shift;
        frame->loop_restoration.loop_restoration_size[0] = luma;
        frame->loop_restoration.loop_restoration_size[1] = chroma;
        frame->loop_restoration.loop_restoration_size[2] = chroma;
    }

    /* Global motion: VA wm[0..6] is LAST..ALTREF. INTRA (slot 0) is identity. */
    for (int i = 0; i < 7; i++) {
        int ref = i + 1;
        uint8_t type = (uint8_t)pic->wm[i].wmtype;
        frame->global_motion.type[ref] = type;
        memcpy(frame->global_motion.params[ref], pic->wm[i].wmmat,
               6 * sizeof(int32_t));
        if (pic->wm[i].invalid)
            frame->global_motion.invalid |= (uint8_t)(1u << ref);
        if (type != 0)
            frame->global_motion.flags[ref] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_GLOBAL;
        if (type == 2)
            frame->global_motion.flags[ref] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_ROT_ZOOM;
        if (type == 1)
            frame->global_motion.flags[ref] |= V4L2_AV1_GLOBAL_MOTION_FLAG_IS_TRANSLATION;
    }

    /* Frame basics */
    frame->frame_type = pic->pic_info_fields.bits.frame_type;
    frame->interpolation_filter = pic->interp_filter;
    frame->tx_mode = pic->mode_control_fields.bits.tx_mode;
    frame->order_hint = pic->order_hint;
    frame->upscaled_width = disp_w;
    frame->frame_width_minus_1 = coded_w - 1;
    frame->frame_height_minus_1 = disp_h - 1;
    frame->render_width_minus_1 = disp_w - 1;
    frame->render_height_minus_1 = disp_h - 1;

    /* Reference frames: VA surface ids -> our V4L2 timestamps */
    for (int i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++) {
        VASurfaceID sid = pic->ref_frame_map[i];
        uint64_t ts = 0;
        struct vav4l2_surface *s = vav4l2_surface_by_id(dd, sid);

        if (s)
            ts = s->timestamp;
        frame->reference_frame_ts[i] = ts;
    }
    for (int i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++)
        frame->ref_frame_idx[i] = pic->ref_frame_idx[i];

    /* Spec OrderHints: index by ref type LAST..ALTREF, not DPB slot.
     * Kernel copies this onto frame_refs[] for temporal-MV overlay. */
    frame->order_hints[V4L2_AV1_REF_INTRA_FRAME] = 0;
    for (int i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
        uint8_t slot = pic->ref_frame_idx[i];
        VASurfaceID sid = VA_INVALID_SURFACE;

        if (slot < V4L2_AV1_TOTAL_REFS_PER_FRAME)
            sid = pic->ref_frame_map[slot];
        frame->order_hints[V4L2_AV1_REF_LAST_FRAME + i] =
            av1_surface_order_hint(dd, sid);
    }

    VAStatus refresh_status = vav4l2_av1_refresh_flags(header, pic, span, span_n, &frame->refresh_frame_flags);
    for (unsigned si = 0; refresh_status != VA_STATUS_SUCCESS && si < VAV4L2_MAX_SLICE_DATAS; si++)
        if (all_spans[si] && all_spans[si] != span)
            refresh_status = vav4l2_av1_refresh_flags(header, pic, all_spans[si], all_sizes[si], &frame->refresh_frame_flags);
    if (refresh_status != VA_STATUS_SUCCESS) return refresh_status;

    frame->flags = header->removal_present ? V4L2_AV1_FRAME_FLAG_BUFFER_REMOVAL_TIME_PRESENT : 0;
    frame->current_frame_id = header->frame_id;
    memcpy(frame->buffer_removal_time, header->removal_time, sizeof(frame->buffer_removal_time));
    if (pic->pic_info_fields.bits.show_frame)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SHOW_FRAME;
    if (pic->pic_info_fields.bits.showable_frame)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SHOWABLE_FRAME;
    if (pic->pic_info_fields.bits.error_resilient_mode)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ERROR_RESILIENT_MODE;
    if (pic->pic_info_fields.bits.disable_cdf_update)
        frame->flags |= V4L2_AV1_FRAME_FLAG_DISABLE_CDF_UPDATE;
    if (pic->pic_info_fields.bits.allow_screen_content_tools)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_SCREEN_CONTENT_TOOLS;
    if (pic->pic_info_fields.bits.force_integer_mv)
        frame->flags |= V4L2_AV1_FRAME_FLAG_FORCE_INTEGER_MV;
    if (pic->pic_info_fields.bits.allow_intrabc)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_INTRABC;
    if (pic->pic_info_fields.bits.use_ref_frame_mvs)
        frame->flags |= V4L2_AV1_FRAME_FLAG_USE_REF_FRAME_MVS;
    if (pic->pic_info_fields.bits.allow_warped_motion)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_WARPED_MOTION;
    if (pic->pic_info_fields.bits.is_motion_mode_switchable)
        frame->flags |= V4L2_AV1_FRAME_FLAG_IS_MOTION_MODE_SWITCHABLE;
    if (pic->pic_info_fields.bits.disable_frame_end_update_cdf)
        frame->flags |= V4L2_AV1_FRAME_FLAG_DISABLE_FRAME_END_UPDATE_CDF;
    if (pic->pic_info_fields.bits.use_superres)
        frame->flags |= V4L2_AV1_FRAME_FLAG_USE_SUPERRES;
    if (pic->pic_info_fields.bits.allow_high_precision_mv)
        frame->flags |= V4L2_AV1_FRAME_FLAG_ALLOW_HIGH_PRECISION_MV;
    if (pic->mode_control_fields.bits.reference_select)
        frame->flags |= V4L2_AV1_FRAME_FLAG_REFERENCE_SELECT;
    if (pic->mode_control_fields.bits.reduced_tx_set_used)
        frame->flags |= V4L2_AV1_FRAME_FLAG_REDUCED_TX_SET;
    if (pic->mode_control_fields.bits.skip_mode_present)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SKIP_MODE_PRESENT;
    if (frame->skip_mode_frame[0] > 0)
        frame->flags |= V4L2_AV1_FRAME_FLAG_SKIP_MODE_ALLOWED;

    return VA_STATUS_SUCCESS;
}

/*
 * AV1 decode pipeline — translate and submit
 */
/* AV1 copy-out release. pull_capture has already snapshotted this frame
 * (GBM bo for display surfaces, memfd otherwise), so a capture buffer's
 * only remaining duty is being a kernel DPB reference. Track the slots
 * with the same refresh_frame_flags we submit to the kernel and hand
 * buffers back as soon as their slot is overwritten; non-reference
 * frames (refresh == 0) return immediately. This is what bounds the
 * pool against zero-copy clients that queue one surface per buffered
 * frame (Chrome WebCodecs/MSE decode-ahead) instead of the buffer
 * lifetime following the surface lifetime. */
void vav4l2_av1_dpb_model_reset(struct vav4l2_context *ctx)
{
    int i;

    if (!ctx)
        return;
    ctx->av1.model_active = 0;
    ctx->av1.header.valid = 0;
    for (i = 0; i < V4L2_AV1_TOTAL_REFS_PER_FRAME; i++)
        ctx->av1.slot_buf[i] = -1;
    for (i = 0; i < VAV4L2_MAX_CAPTURE_BUFS; i++)
        ctx->av1.buf_owner[i] = VA_INVALID_SURFACE;
}

static void av1_release_unrefd(struct vav4l2_context *ctx,
                               struct vav4l2_surface *surf,
                               int buf, uint8_t refresh)
{
    int s, t, still;

    if (!ctx || !surf || buf < 0 || buf >= VAV4L2_MAX_CAPTURE_BUFS)
        return;
    ctx->av1.buf_owner[buf] = surf->surface_id;

    if (!refresh) {
        if (vav4l2_debug)
            fprintf(stderr, "v4l2: AV1 rel buf=%d (nonref)\n", buf);
        surf->buf_index = -1;
        surf->cap_view = NULL;
        ctx->av1.buf_owner[buf] = VA_INVALID_SURFACE;
        vav4l2_cap_pool_push(ctx, buf);
        return;
    }
    for (s = 0; s < V4L2_AV1_TOTAL_REFS_PER_FRAME; s++) {
        int old;

        if (!(refresh & (1u << s)))
            continue;
        old = ctx->av1.slot_buf[s];
        ctx->av1.slot_buf[s] = buf;
        if (old < 0 || old == buf)
            continue;
        still = 0;
        for (t = 0; t < V4L2_AV1_TOTAL_REFS_PER_FRAME; t++)
            if (ctx->av1.slot_buf[t] == old)
                still = 1;
        if (still)
            continue;
        {
            VASurfaceID id = ctx->av1.buf_owner[old];
            struct vav4l2_surface *os = (id != VA_INVALID_SURFACE)
                ? vav4l2_surface_by_id(ctx->driver_data, id) : NULL;
            if (os && os->buf_index == old) {
                os->buf_index = -1;
                os->cap_view = NULL;
            }
            if (vav4l2_debug)
                fprintf(stderr, "v4l2: AV1 rel buf=%d (slot %d -> %d)\n",
                        old, s, buf);
            ctx->av1.buf_owner[old] = VA_INVALID_SURFACE;
            vav4l2_cap_pool_push(ctx, old);
        }
    }
}

/* CDF and motion references must still belong to this decoder queue. A
 * display snapshot cannot replace the CAPTURE buffer holding codec state. */
VAStatus vav4l2_av1_validate_references(const struct vav4l2_context *ctx, const VADecPictureParameterBufferAV1 *p)
{
    if (!ctx || !p) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (p->primary_ref_frame > 7) return VA_STATUS_ERROR_INVALID_PARAMETER;
    unsigned type = p->pic_info_fields.bits.frame_type;
    if (type == V4L2_AV1_KEY_FRAME || type == V4L2_AV1_INTRA_ONLY_FRAME || p->pic_info_fields.bits.error_resilient_mode) {
        if (p->primary_ref_frame != 7) return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (type == V4L2_AV1_KEY_FRAME || type == V4L2_AV1_INTRA_ONLY_FRAME) return VA_STATUS_SUCCESS;
    }
    for (unsigned i = 0; i < V4L2_AV1_REFS_PER_FRAME; i++) {
        unsigned slot = p->ref_frame_idx[i];
        if (slot >= V4L2_AV1_TOTAL_REFS_PER_FRAME) return VA_STATUS_ERROR_INVALID_PARAMETER;
        VASurfaceID id = p->ref_frame_map[slot];
        VAStatus status = vav4l2_validate_reference(ctx, id);
        if (status != VA_STATUS_SUCCESS) return status;
        const struct vav4l2_surface *s = vav4l2_surface_by_id(ctx->driver_data, id);
        if (s == ctx->current_surface) return VA_STATUS_ERROR_INVALID_SURFACE;
        if (ctx->av1.model_active && (ctx->av1.slot_buf[slot] != s->buf_index ||
            ctx->av1.buf_owner[s->buf_index] != id)) return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    return VA_STATUS_SUCCESS;
}

VAStatus vav4l2_av1_translate(struct vav4l2_context *ctx,
                              struct vav4l2_buffer **buffers,
                              int num_buffers)
{
    VADecPictureParameterBufferAV1 *pic_param = NULL;
    const VASliceParameterBufferAV1 *tile_params[32];
    int n_tiles = 0;
    uint8_t *tile_data = NULL;
    uint32_t tile_data_size = 0;

    struct vav4l2_collected cb;
    int i;

    for (i = 0; i < num_buffers; i++) {
        struct vav4l2_buffer *b = buffers[i];
        if (b && ((b->type == VAPictureParameterBufferType && b->size < sizeof(*pic_param)) ||
                  (b->type == VASliceParameterBufferType && b->size < sizeof(*tile_params[0]))))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    vav4l2_collect_decode_buffers(buffers, num_buffers, &cb);
    pic_param = cb.pic;
    for (i = 0; i < cb.n_slice_params && i < 32; i++)
        tile_params[i] = cb.slice_params[i];
    n_tiles = cb.n_slice_params;
    /* Largest slice-data buffer — ffmpeg's AV1 VAAPI hwaccel submits the
     * whole OBU in one buffer. */
    tile_data = (uint8_t *)cb.largest;
    tile_data_size = cb.largest_size;
    if (!pic_param) {
        fprintf(stderr, "v4l2: AV1 decode missing picture params\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    VAStatus reference_status = vav4l2_av1_validate_references(ctx, pic_param);
    if (reference_status != VA_STATUS_SUCCESS) return reference_status;
    struct v4l2_av1_tile_info tile_info;
    struct v4l2_ctrl_av1_tile_group_entry entries[32];
    uint32_t tile_start, tile_length;
    if (cb.n_slice_params_seen > 32 || cb.n_slice_datas != 1 ||
        vav4l2_av1_prepare_tiles(pic_param, tile_params, n_tiles, tile_data,
                                tile_data_size, ctx->output_buf_size, &tile_info,
                                entries, &tile_start, &tile_length) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "v4l2: AV1 invalid tile geometry, payload or missing tile-size metadata\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (!tile_data || tile_data_size == 0) {
        fprintf(stderr, "v4l2: AV1 decode missing tile data\n");
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (!ctx->av1.grain_checked) {
        struct v4l2_query_ext_ctrl q = {.id = V4L2_CID_STATELESS_AV1_FILM_GRAIN};
        int rc = vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERY_EXT_CTRL, &q);
        if (rc < 0 && errno != EINVAL && errno != ENOTTY) return VA_STATUS_ERROR_OPERATION_FAILED;
        ctx->av1.has_grain = rc == 0 && !(q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY));
        ctx->av1.grain_checked = 1;
    }
    if (!ctx->av1.has_grain && pic_param->film_grain_info.film_grain_info_fields.bits.apply_grain)
        return VA_STATUS_ERROR_UNIMPLEMENTED;

    struct v4l2_ctrl_av1_sequence seq;
    struct v4l2_ctrl_av1_frame frame;

    av1_fill_sequence_params(&seq, pic_param);
    struct vav4l2_av1_header header = ctx->av1.header;
    VAStatus frame_status = av1_fill_frame_params(&frame, pic_param, ctx,
                          (const uint8_t *)cb.largest, cb.largest_size,
                          cb.slice_datas, cb.slice_sizes, &header);
    if (frame_status != VA_STATUS_SUCCESS) return frame_status;
    frame.tile_info = tile_info;

    int request_fd = ctx->request_fd;
    int v4l2_fd = ctx->v4l2_fd;

    if (request_fd < 0) {
        fprintf(stderr, "v4l2: AV1 no request fd available\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* Set AV1 sequence params */
    struct v4l2_ext_control seq_ctrl = { 0 };
    seq_ctrl.id = V4L2_CID_STATELESS_AV1_SEQUENCE;
    seq_ctrl.p_av1_sequence = &seq;
    seq_ctrl.size = sizeof(seq);

    struct v4l2_ext_controls seq_ctrls = { 0 };
    seq_ctrls.controls = &seq_ctrl;
    seq_ctrls.count = 1;

    /* Sequence is a GLOBAL control on hantro AV1 — request-scoped
     * submissions succeed the ioctl but leave the device unconfigured,
     * so the subsequent OUTPUT QBUF returns EINVAL. */
    /* Sequence-level control: resubmit only when the payload changed. */
    _Static_assert(sizeof(seq) <= sizeof(ctx->g_ctrl_payload), "grow cache");
    if (!ctx->g_ctrl_valid ||
        memcmp(ctx->g_ctrl_payload, &seq, sizeof(seq)) != 0) {
        if (vav4l2_codec_controls(ctx, -1, &seq_ctrls) < 0) {
            fprintf(stderr, "v4l2: failed to set AV1 sequence params\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        memcpy(ctx->g_ctrl_payload, &seq, sizeof(seq));
        ctx->g_ctrl_valid = 1;
    }

    {
        int w = (int)pic_param->frame_width_minus1 + 1;
        int h = (int)pic_param->frame_height_minus1 + 1;
        int chroma_idc = pic_param->seq_info_fields.fields.subsampling_x ? 1 : 2;
        uint32_t cap = vav4l2_capture_fourcc_from_sps(
            pic_param->bit_depth_idx * 2, chroma_idc);

        if (vav4l2_ensure_capture(ctx, w, h, cap) < 0) {
            fprintf(stderr, "v4l2: AV1 capture reconfig failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    if (!ctx->streamed) {
        if (vav4l2_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            vav4l2_streamon(v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
            if (errno == ENOMEM)
                fprintf(stderr, "v4l2: AV1 STREAMON ENOMEM at %dx%d; check kernel CMA allocation failures in dmesg and CmaFree in /proc/meminfo\n",
                        (int)pic_param->frame_width_minus1 + 1,
                        (int)pic_param->frame_height_minus1 + 1);
            fprintf(stderr, "v4l2: AV1 STREAMON failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ctx->streamed = 1;
    }

    /* Send optional film-grain state only when the node exposes it. */
    struct v4l2_ctrl_av1_film_grain grain;
    memset(&grain, 0, sizeof(grain));
    {
        const VAFilmGrainStructAV1 *fg = &pic_param->film_grain_info;
        if (fg->film_grain_info_fields.bits.apply_grain)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_APPLY_GRAIN;
        if (fg->film_grain_info_fields.bits.chroma_scaling_from_luma)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_CHROMA_SCALING_FROM_LUMA;
        if (fg->film_grain_info_fields.bits.overlap_flag)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_OVERLAP;
        if (fg->film_grain_info_fields.bits.clip_to_restricted_range)
            grain.flags |= V4L2_AV1_FILM_GRAIN_FLAG_CLIP_TO_RESTRICTED_RANGE;
        grain.cr_mult = fg->cr_mult;
        grain.grain_seed = fg->grain_seed;
        grain.num_y_points = fg->num_y_points;
        memcpy(grain.point_y_value, fg->point_y_value, sizeof(fg->point_y_value));
        memcpy(grain.point_y_scaling, fg->point_y_scaling, sizeof(fg->point_y_scaling));
        grain.num_cb_points = fg->num_cb_points;
        memcpy(grain.point_cb_value, fg->point_cb_value, sizeof(fg->point_cb_value));
        memcpy(grain.point_cb_scaling, fg->point_cb_scaling, sizeof(fg->point_cb_scaling));
        grain.num_cr_points = fg->num_cr_points;
        memcpy(grain.point_cr_value, fg->point_cr_value, sizeof(fg->point_cr_value));
        memcpy(grain.point_cr_scaling, fg->point_cr_scaling, sizeof(fg->point_cr_scaling));
        grain.grain_scaling_minus_8 = fg->film_grain_info_fields.bits.grain_scaling_minus_8;
        grain.ar_coeff_lag = fg->film_grain_info_fields.bits.ar_coeff_lag;
        for (int i = 0; i < 24; i++)
            grain.ar_coeffs_y_plus_128[i] = (uint8_t)(fg->ar_coeffs_y[i] + 128);
        for (int i = 0; i < 25; i++) {
            grain.ar_coeffs_cb_plus_128[i] = (uint8_t)(fg->ar_coeffs_cb[i] + 128);
            grain.ar_coeffs_cr_plus_128[i] = (uint8_t)(fg->ar_coeffs_cr[i] + 128);
        }
        grain.ar_coeff_shift_minus_6 = fg->film_grain_info_fields.bits.ar_coeff_shift_minus_6;
        grain.grain_scale_shift = fg->film_grain_info_fields.bits.grain_scale_shift;
        grain.cb_mult = fg->cb_mult;
        grain.cb_luma_mult = fg->cb_luma_mult;
        grain.cr_luma_mult = fg->cr_luma_mult;
        grain.cb_offset = fg->cb_offset;
        grain.cr_offset = fg->cr_offset;
    }
    struct v4l2_ext_control req_ctrls[3];
    unsigned n_req_ctrls = 0;

    memset(req_ctrls, 0, sizeof(req_ctrls));
    if (ctx->av1.has_grain) {
        req_ctrls[n_req_ctrls].id = V4L2_CID_STATELESS_AV1_FILM_GRAIN;
        req_ctrls[n_req_ctrls].ptr = &grain;
        req_ctrls[n_req_ctrls].size = sizeof(grain);
        n_req_ctrls++;
    }

    req_ctrls[n_req_ctrls].id = V4L2_CID_STATELESS_AV1_FRAME;
    req_ctrls[n_req_ctrls].p_av1_frame = &frame;
    req_ctrls[n_req_ctrls].size = sizeof(frame);
    n_req_ctrls++;

    req_ctrls[n_req_ctrls].id = V4L2_CID_STATELESS_AV1_TILE_GROUP_ENTRY;
    req_ctrls[n_req_ctrls].ptr = entries;
    req_ctrls[n_req_ctrls].size = sizeof(entries[0]) * pic_param->tile_cols * pic_param->tile_rows;
    n_req_ctrls++;

    {
        struct v4l2_ext_controls req_ctrls_batch = { 0 };
        req_ctrls_batch.controls = req_ctrls;
        req_ctrls_batch.count = n_req_ctrls;
        /* One S_EXT_CTRLS per frame. Failure is terminal — no per-control
         * fallback. Submitted before the output-buffer pop so a failure
         * has no pool state to unwind. */
        if (vav4l2_codec_controls(ctx, request_fd, &req_ctrls_batch) < 0) {
            fprintf(stderr, "v4l2: failed to set AV1 request controls\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    /*
     * Synchronous decode pipeline - mirrors H.264/HEVC: pop from pools,
     * queue, submit, wait, attach the frame to the surface.
     */
    if (ctx->n_free_out == 0) {
        fprintf(stderr, "v4l2: AV1 no free output buffer\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int out_buf_idx = ctx->free_out_bufs[--ctx->n_free_out];

    uint64_t timestamp = ctx->current_surface ? ctx->current_surface->timestamp : 0;

    /* Preserve the size fields between tiles. The validated entries are
     * rebased to the first tile payload, matching the kernel stream base. */
    uint8_t *dst = ctx->output_buf_ptr[out_buf_idx];
    if (!dst || tile_length > ctx->output_buf_size) {
        vav4l2_out_pool_push(ctx, out_buf_idx);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    memcpy(dst, tile_data + tile_start, tile_length);
    tile_data_size = tile_length;

    /* On failure decode_submit resets both queues — do not push back. */
    int done_cap = vav4l2_decode_submit(ctx, out_buf_idx, tile_data_size, timestamp);
    if (done_cap == -2) {
        /* Corrupt frame (V4L2_BUF_FLAG_ERROR): mark and succeed — a failed
         * entrypoint would be cached by Chrome for the whole session. */
        if (ctx->current_surface)
            ctx->current_surface->status = VASurfaceSkipped;
        return VA_STATUS_SUCCESS;
    }
    if (done_cap < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    struct vav4l2_surface *surf = ctx->current_surface;
    if (surf) {
        if (vav4l2_surface_pull_capture(ctx, surf, done_cap) < 0) {
            fprintf(stderr, "v4l2: AV1 pull capture failed\n");
            vav4l2_cap_pool_push(ctx, done_cap);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        } else {
            surf->order_hint = pic_param->order_hint;
            ctx->av1.model_active = 1; ctx->av1.header = header;
            if (vav4l2_debug)
                fprintf(stderr, "v4l2: AV1 frame surf=%#x oh=%u buf=%d refresh=%02x type=%u\n",
                        surf->surface_id, pic_param->order_hint, done_cap,
                        frame.refresh_frame_flags,
                        pic_param->pic_info_fields.bits.frame_type);
            if (ctx->av1.model_active)
                av1_release_unrefd(ctx, surf, done_cap,
                                   frame.refresh_frame_flags);
        }
    } else {
        vav4l2_cap_pool_push(ctx, done_cap);
    }

    return VA_STATUS_SUCCESS;
}

const int vav4l2_has_av1 = 1;
