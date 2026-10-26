/* SPDX-License-Identifier: MIT */
/* AV1 OBU normalization adapted from Radxa libva-v4l2; see NOTICE. */
#include <limits.h>
#include "bytestream.h"
#define AV1_TILES 32
static void av1_put(struct bits *b, unsigned n, uint32_t value)
{
    if (n > 32 || (n < 32 && value >= (UINT32_C(1) << n))) { b->error = 1; return; }
    put(b, n, value);
}
#define put av1_put
struct av1_picture {
    unsigned refs;
    VADecPictureParameterBufferAV1 p;
    uint32_t offset[AV1_TILES], size[AV1_TILES];
    uint8_t *data;
};
struct vav4l2_av1_packet {
    struct { VASurfaceID id; VADecPictureParameterBufferAV1 p; } ref[8];
    uint8_t sequence[8192]; size_t sequence_size;
    struct av1_picture *pending;
};
static unsigned log2_ceil(unsigned n)
{
    unsigned b = 0; for (unsigned v = n ? n - 1 : 0; v; v >>= 1) b++; return b;
}
static unsigned tile_log2(unsigned block, unsigned target)
{
    unsigned b = 0; while ((block << b) < target && b < 16) b++; return b;
}
static void signed_bits(struct bits *b, int64_t v, unsigned n)
{
    if (!n || n > 31 || v < -(INT64_C(1) << (n - 1)) || v >= (INT64_C(1) << (n - 1))) { b->error = 1; return; }
    put(b, n, (uint32_t)v & ((1u << n) - 1));
}
static void ns(struct bits *b, unsigned v, unsigned count)
{
    if (!count || v >= count) { b->error = 1; return; }
    unsigned n = log2_ceil(count), cutoff = (1u << n) - count;
    if (n) put(b, v < cutoff ? n - 1 : n, v < cutoff ? v : v + cutoff);
}
static void subexp(struct bits *b, unsigned v, unsigned count)
{
    unsigned base = 0;
    for (unsigned i = 0; !b->error; i++) {
        unsigned n = i ? i + 2 : 3, step = 1u << n;
        if (count <= base + 3 * step) { ns(b, v - base, count - base); return; }
        put(b, 1, v >= base + step);
        if (v < base + step) { put(b, n, v - base); return; }
        base += step;
    }
}
static void align_bits(struct bits *b) { while (!b->error && b->bit % 8) put(b, 1, 0); }
static int obu(uint8_t *dst, size_t cap, size_t *used, unsigned type, const uint8_t *data, size_t size)
{
    uint8_t header[11]; unsigned n = 1; size_t v = size;
    header[0] = (type << 3) | 2;
    do { header[n++] = (v & 127) | (v > 127 ? 128 : 0); v >>= 7; } while (v);
    if (*used > cap || n > cap - *used || size > cap - *used - n) return -1;
    memcpy(dst + *used, header, n); if (size) memcpy(dst + *used + n, data, size); *used += n + size; return 0;
}
static void picture_free(struct av1_picture *p)
{
    if (p && !--p->refs) { free(p->data); free(p); }
}
void vav4l2_av1_packet_free(struct vav4l2_av1_packet *s)
{
    if (s) { picture_free(s->pending); free(s); }
}
static VAStatus picture_collect(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count, size_t cap, struct av1_picture **out)
{
    struct vav4l2_collected cb; vav4l2_collect_decode_buffers(buffers, count, &cb);
    if (!cb.pic) return VA_STATUS_ERROR_INVALID_PARAMETER;
    const VADecPictureParameterBufferAV1 *p = cb.pic;
    if (p->profile || p->bit_depth_idx > 1 || p->seq_info_fields.fields.mono_chrome ||
        !p->seq_info_fields.fields.subsampling_x || !p->seq_info_fields.fields.subsampling_y)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    if (p->pic_info_fields.bits.large_scale_tile || p->anchor_frames_num)
        return VA_STATUS_ERROR_UNIMPLEMENTED;
    unsigned tiles = (unsigned)p->tile_cols * p->tile_rows;
    if (!tiles || tiles > AV1_TILES || p->context_update_tile_id >= tiles || p->primary_ref_frame > 7 || p->cdef_bits > 3 ||
        p->order_hint_bits_minus_1 > 7 || p->current_frame != ctx->current_surface_id ||
        ctx->width < 128 || ctx->height < 128 || ctx->width > 4096 || ctx->height > 4096 ||
        p->frame_width_minus1 >= ctx->width || p->frame_height_minus1 >= ctx->height ||
        !(p->frame_width_minus1 & 1) || !(p->frame_height_minus1 & 1) ||
        (p->pic_info_fields.bits.use_superres && (p->superres_scale_denominator < 9 || p->superres_scale_denominator > 16)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    int intra = p->pic_info_fields.bits.frame_type == 0 || p->pic_info_fields.bits.frame_type == 2;
    if (p->seq_info_fields.fields.still_picture && (p->pic_info_fields.bits.frame_type || !p->pic_info_fields.bits.show_frame))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if ((p->pic_info_fields.bits.allow_intrabc && (!intra || !p->pic_info_fields.bits.allow_screen_content_tools || p->pic_info_fields.bits.use_superres)) ||
        (p->pic_info_fields.bits.force_integer_mv && !intra && !p->pic_info_fields.bits.allow_screen_content_tools) ||
        (p->mode_control_fields.bits.delta_q_present_flag && !p->base_qindex) ||
        (p->mode_control_fields.bits.delta_lf_present_flag && (!p->mode_control_fields.bits.delta_q_present_flag || p->pic_info_fields.bits.allow_intrabc)))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    const uint8_t *data[AV1_TILES] = {0}; uint32_t sizes[AV1_TILES] = {0};
    VASliceParameterBufferAV1 pending[AV1_TILES]; unsigned np = 0; size_t total = 0;
    for (int i = 0; i < count; i++) {
        struct vav4l2_buffer *b = buffers[i]; unsigned elements = b->num_elements ? b->num_elements : 1;
        if (b->type == VASliceParameterBufferType) {
            if (b->size < sizeof(pending[0]) || elements > AV1_TILES - np) return VA_STATUS_ERROR_INVALID_PARAMETER;
            for (unsigned j = 0; j < elements; j++) memcpy(&pending[np++], (uint8_t *)b->data + (size_t)j * b->size, sizeof(pending[0]));
        } else if (b->type == VASliceDataBufferType) {
            size_t bytes = (size_t)b->size * elements;
            if (elements && bytes / elements != b->size) return VA_STATUS_ERROR_INVALID_PARAMETER;
            if (!np && tiles == 1 && cb.n_slice_datas == 1) pending[np++] = (VASliceParameterBufferAV1){.slice_data_size = bytes, .slice_data_flag = VA_SLICE_DATA_FLAG_ALL};
            if (!np) return VA_STATUS_ERROR_INVALID_PARAMETER;
            for (unsigned j = 0; j < np; j++) {
                const VASliceParameterBufferAV1 *t = &pending[j]; unsigned index = t->tile_row * p->tile_cols + t->tile_column;
                if (t->tile_row >= p->tile_rows || t->tile_column >= p->tile_cols || data[index] || t->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
                    !t->slice_data_size || t->slice_data_offset > bytes || t->slice_data_size > bytes - t->slice_data_offset ||
                    total > cap || t->slice_data_size > cap - total) return VA_STATUS_ERROR_INVALID_PARAMETER;
                data[index] = (uint8_t *)b->data + t->slice_data_offset; sizes[index] = t->slice_data_size; total += sizes[index];
            }
            np = 0;
        }
    }
    if (np || !total) return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (unsigned i = 0; i < tiles; i++) if (!data[i]) return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct av1_picture *pic = calloc(1, sizeof(*pic)); if (!pic) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    pic->data = malloc(total); if (!pic->data) { free(pic); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    pic->p = *p; pic->refs = 1; total = 0;
    for (unsigned i = 0; i < tiles; i++) { pic->offset[i] = total; pic->size[i] = sizes[i]; memcpy(pic->data + total, data[i], sizes[i]); total += sizes[i]; }
    *out = pic; return VA_STATUS_SUCCESS;
}
static void sequence_header(struct bits *b, const VADecPictureParameterBufferAV1 *p, unsigned width, unsigned height)
{
#define S(name) p->seq_info_fields.fields.name
    put(b, 3, 0); put(b, 1, S(still_picture)); put(b, 1, 0); put(b, 1, 0); put(b, 1, 0); put(b, 5, 0); put(b, 12, 0);
    put(b, 5, (uint64_t)width * height <= 2359296 ? 9 : (uint64_t)width * height <= 8912896 ? 13 : 17); put(b, 1, 0);
    unsigned wb = log2_ceil(width), hb = log2_ceil(height);
    put(b, 4, wb - 1); put(b, 4, hb - 1); put(b, wb, width - 1); put(b, hb, height - 1); put(b, 1, 0);
    put(b, 1, S(use_128x128_superblock)); put(b, 1, S(enable_filter_intra)); put(b, 1, S(enable_intra_edge_filter));
    put(b, 1, S(enable_interintra_compound)); put(b, 1, S(enable_masked_compound)); put(b, 1, 1);
    put(b, 1, S(enable_dual_filter)); put(b, 1, S(enable_order_hint));
    if (S(enable_order_hint)) { put(b, 1, S(enable_jnt_comp)); put(b, 1, 1); }
    put(b, 1, 1); put(b, 1, 1);
    if (S(enable_order_hint)) put(b, 3, p->order_hint_bits_minus_1);
    put(b, 1, 1); put(b, 1, S(enable_cdef)); put(b, 1, 1); put(b, 1, p->bit_depth_idx == 1);
    put(b, 1, 0); put(b, 1, 1); put(b, 8, 2); put(b, 8, 2); put(b, 8, p->matrix_coefficients);
    put(b, 1, S(color_range)); put(b, 2, 0); put(b, 1, 1); put(b, 1, 1); put(b, 1, 1); align_bits(b);
#undef S
}
static void film_grain(struct bits *b, const VADecPictureParameterBufferAV1 *p, int show)
{
    if (!show) return; /* Hidden replay is not showable and carries no grain. */
    const VAFilmGrainStructAV1 *f = &p->film_grain_info;
    put(b, 1, f->film_grain_info_fields.bits.apply_grain);
    if (!f->film_grain_info_fields.bits.apply_grain) return;
    unsigned chroma = f->film_grain_info_fields.bits.chroma_scaling_from_luma;
    if (f->num_y_points > 14 || f->num_cb_points > 10 || f->num_cr_points > 10 ||
        ((chroma || !f->num_y_points) && (f->num_cb_points || f->num_cr_points))) { b->error = 1; return; }
    const uint8_t *values[] = {f->point_y_value, f->point_cb_value, f->point_cr_value};
    unsigned counts[] = {f->num_y_points, f->num_cb_points, f->num_cr_points};
    for (unsigned c = 0; c < 3; c++) for (unsigned i = 1; i < counts[c]; i++)
        if (values[c][i] <= values[c][i-1]) { b->error = 1; return; }
    put(b, 16, f->grain_seed);
    /* VA supplies resolved coefficients, so serialize a complete update. */
    if (p->pic_info_fields.bits.frame_type == 1) put(b, 1, 1);
    put(b, 4, f->num_y_points);
    for (unsigned i = 0; i < f->num_y_points; i++) { put(b, 8, f->point_y_value[i]); put(b, 8, f->point_y_scaling[i]); }
    put(b, 1, chroma);
    if (!chroma && f->num_y_points) {
        put(b, 4, f->num_cb_points);
        for (unsigned i = 0; i < f->num_cb_points; i++) { put(b, 8, f->point_cb_value[i]); put(b, 8, f->point_cb_scaling[i]); }
        put(b, 4, f->num_cr_points);
        for (unsigned i = 0; i < f->num_cr_points; i++) { put(b, 8, f->point_cr_value[i]); put(b, 8, f->point_cr_scaling[i]); }
    }
    put(b, 2, f->film_grain_info_fields.bits.grain_scaling_minus_8);
    unsigned lag = f->film_grain_info_fields.bits.ar_coeff_lag, n = 2 * lag * (lag + 1);
    put(b, 2, lag);
    if (f->num_y_points) for (unsigned i = 0; i < n; i++) put(b, 8, f->ar_coeffs_y[i] + 128);
    n += !!f->num_y_points;
    if (chroma || f->num_cb_points) for (unsigned i = 0; i < n; i++) put(b, 8, f->ar_coeffs_cb[i] + 128);
    if (chroma || f->num_cr_points) for (unsigned i = 0; i < n; i++) put(b, 8, f->ar_coeffs_cr[i] + 128);
    put(b, 2, f->film_grain_info_fields.bits.ar_coeff_shift_minus_6); put(b, 2, f->film_grain_info_fields.bits.grain_scale_shift);
    if (f->num_cb_points) { put(b, 8, f->cb_mult); put(b, 8, f->cb_luma_mult); put(b, 9, f->cb_offset); }
    if (f->num_cr_points) { put(b, 8, f->cr_mult); put(b, 8, f->cr_luma_mult); put(b, 9, f->cr_offset); }
    put(b, 1, f->film_grain_info_fields.bits.overlap_flag); put(b, 1, f->film_grain_info_fields.bits.clip_to_restricted_range);
}
static void tile_layout(struct bits *b, const VADecPictureParameterBufferAV1 *p)
{
    unsigned sb = p->seq_info_fields.fields.use_128x128_superblock ? 128 : 64;
    unsigned width = p->frame_width_minus1 + 1u;
    if (p->pic_info_fields.bits.use_superres) width = (width * 8 + p->superres_scale_denominator / 2) / p->superres_scale_denominator;
    unsigned cols = (width + sb - 1) / sb, rows = (p->frame_height_minus1 + sb) / sb;
    unsigned maxw = 4096 / sb, maxa = (4096 * 2304) / (sb * sb);
    unsigned minc = tile_log2(maxw, cols), mint = tile_log2(maxa, cols * rows);
    if (mint < minc) mint = minc;
    unsigned lc = log2_ceil(p->tile_cols), lr = log2_ceil(p->tile_rows);
    put(b, 1, p->pic_info_fields.bits.uniform_tile_spacing_flag);
    if (p->pic_info_fields.bits.uniform_tile_spacing_flag) {
        for (unsigned axis = 0; axis < 2; axis++) {
            unsigned blocks = axis ? rows : cols, count = axis ? p->tile_rows : p->tile_cols;
            unsigned minimum = axis ? mint > lc ? mint - lc : 0 : minc, maximum = log2_ceil(blocks < 64 ? blocks : 64), n;
            for (n = minimum; n <= maximum; n++) { unsigned span = (blocks + (1u << n) - 1) >> n; if ((blocks + span - 1) / span == count) break; }
            if (n > maximum) { b->error = 1; return; }
            for (unsigned i = minimum; i < n; i++) put(b, 1, 1);
            if (n < maximum) put(b, 1, 0);
            if (axis) lr = n; else lc = n;
        }
    } else {
        unsigned widest = 0, left = cols;
        for (unsigned i = 0; i < p->tile_cols; i++) {
            unsigned span = i + 1 == p->tile_cols ? left : p->width_in_sbs_minus_1[i] + 1u;
            if (!span || span > left) { b->error = 1; return; }
            ns(b, span - 1, left < maxw ? left : maxw); if (widest < span) widest = span; left -= span;
        }
        if (left || !widest) { b->error = 1; return; }
        maxa = cols * rows; if (mint) maxa >>= mint + 1;
        unsigned maxh = maxa / widest; if (!maxh) maxh = 1; left = rows;
        for (unsigned i = 0; i < p->tile_rows; i++) {
            unsigned span = i + 1 == p->tile_rows ? left : p->height_in_sbs_minus_1[i] + 1u;
            if (!span || span > left) { b->error = 1; return; }
            ns(b, span - 1, left < maxh ? left : maxh); left -= span;
        }
        if (left) b->error = 1;
    }
    if (lc || lr) { put(b, lc + lr, p->context_update_tile_id); put(b, 2, 3); }
}
static void delta_q(struct bits *b, int delta) { put(b, 1, delta != 0); if (delta) signed_bits(b, delta, 7); }
static void quantization(struct bits *b, const VADecPictureParameterBufferAV1 *p)
{
    put(b, 8, p->base_qindex); delta_q(b, p->y_dc_delta_q);
    int separate = p->u_dc_delta_q != p->v_dc_delta_q || p->u_ac_delta_q != p->v_ac_delta_q;
    put(b, 1, separate); delta_q(b, p->u_dc_delta_q); delta_q(b, p->u_ac_delta_q);
    if (separate) { delta_q(b, p->v_dc_delta_q); delta_q(b, p->v_ac_delta_q); }
    put(b, 1, p->qmatrix_fields.bits.using_qmatrix);
    if (p->qmatrix_fields.bits.using_qmatrix) { put(b, 4, p->qmatrix_fields.bits.qm_y); put(b, 4, p->qmatrix_fields.bits.qm_u); put(b, 4, p->qmatrix_fields.bits.qm_v); }
}
static void segmentation(struct bits *b, const VADecPictureParameterBufferAV1 *p, unsigned primary)
{
    put(b, 1, p->seg_info.segment_info_fields.bits.enabled);
    if (!p->seg_info.segment_info_fields.bits.enabled) return;
    if (primary != 7) {
        put(b, 1, p->seg_info.segment_info_fields.bits.update_map);
        if (p->seg_info.segment_info_fields.bits.update_map) put(b, 1, p->seg_info.segment_info_fields.bits.temporal_update);
        put(b, 1, 1); /* VA supplies the resolved feature table. */
    }
    static const unsigned widths[] = {8,6,6,6,6,3,0,0};
    for (unsigned s = 0; s < 8; s++) for (unsigned f = 0; f < 8; f++) {
        int enabled = !!(p->seg_info.feature_mask[s] & (1u << f)); put(b, 1, enabled);
        if (!enabled) continue;
        int value = p->seg_info.feature_data[s][f], limit = (1 << widths[f]) - 1;
        if (value < (f < 5 ? -limit : 0) || value > limit) { b->error = 1; return; }
        if (f < 5) signed_bits(b, value, widths[f] + 1); else put(b, widths[f], value);
    }
}
static int lossless(const VADecPictureParameterBufferAV1 *p)
{
    if (p->y_dc_delta_q || p->u_dc_delta_q || p->u_ac_delta_q || p->v_dc_delta_q || p->v_ac_delta_q) return 0;
    for (unsigned i = 0; i < 8; i++) {
        int q = p->base_qindex;
        if (p->seg_info.segment_info_fields.bits.enabled && (p->seg_info.feature_mask[i] & 1)) q += p->seg_info.feature_data[i][0];
        if (q > 0) return 0;
    }
    return 1;
}
static void filters(struct bits *b, const VADecPictureParameterBufferAV1 *p, int ll)
{
    if (!ll && !p->pic_info_fields.bits.allow_intrabc) {
        put(b, 6, p->filter_level[0]); put(b, 6, p->filter_level[1]);
        if (p->filter_level[0] || p->filter_level[1]) { put(b, 6, p->filter_level_u); put(b, 6, p->filter_level_v); }
        put(b, 3, p->loop_filter_info_fields.bits.sharpness_level); put(b, 1, p->loop_filter_info_fields.bits.mode_ref_delta_enabled);
        if (p->loop_filter_info_fields.bits.mode_ref_delta_enabled) {
            put(b, 1, 1);
            for (unsigned i = 0; i < 8; i++) { put(b, 1, 1); signed_bits(b, p->ref_deltas[i], 7); }
            for (unsigned i = 0; i < 2; i++) { put(b, 1, 1); signed_bits(b, p->mode_deltas[i], 7); }
        }
        if (p->seq_info_fields.fields.enable_cdef) {
            put(b, 2, p->cdef_damping_minus_3); put(b, 2, p->cdef_bits);
            for (unsigned i = 0; i < (1u << p->cdef_bits); i++) { put(b, 6, p->cdef_y_strengths[i]); put(b, 6, p->cdef_uv_strengths[i]); }
        }
    }
    if (!(ll && !p->pic_info_fields.bits.use_superres) && !p->pic_info_fields.bits.allow_intrabc) {
        static const unsigned syntax[] = {0,2,3,1};
        unsigned y = p->loop_restoration_fields.bits.yframe_restoration_type, u = p->loop_restoration_fields.bits.cbframe_restoration_type;
        unsigned v = p->loop_restoration_fields.bits.crframe_restoration_type;
        put(b, 2, syntax[y]); put(b, 2, syntax[u]); put(b, 2, syntax[v]);
        if (y || u || v) {
            unsigned shift = p->loop_restoration_fields.bits.lr_unit_shift;
            if (shift > 2) { b->error = 1; return; }
            if (p->seq_info_fields.fields.use_128x128_superblock) {
                if (!shift) { b->error = 1; return; } put(b, 1, shift - 1);
            } else { put(b, 1, shift != 0); if (shift) put(b, 1, shift == 2); }
            if (u || v) put(b, 1, p->loop_restoration_fields.bits.lr_uv_shift);
        }
    }
}
static int relative(int a, int b, unsigned bits)
{
    unsigned delta = (unsigned)(a - b), sign = 1u << (bits - 1); return (int)(delta & (sign - 1)) - (int)(delta & sign);
}
static int skip_allowed(const VADecPictureParameterBufferAV1 *p, const struct vav4l2_av1_packet *s, const unsigned slots[7])
{
    if (!p->seq_info_fields.fields.enable_order_hint || !p->mode_control_fields.bits.reference_select) return 0;
    int forward = -1, backward = -1; unsigned bits = p->order_hint_bits_minus_1 + 1;
    for (unsigned i = 0; i < 7; i++) {
        int h = s->ref[slots[i]].p.order_hint, d = relative(h, p->order_hint, bits);
        if (d < 0 && (forward < 0 || relative(h, forward, bits) > 0)) forward = h;
        if (d > 0 && (backward < 0 || relative(h, backward, bits) < 0)) backward = h;
    }
    if (forward < 0) return 0;
    if (backward >= 0) return 1;
    for (unsigned i = 0; i < 7; i++) if (relative(s->ref[slots[i]].p.order_hint, forward, bits) < 0) return 1;
    return 0;
}
static unsigned recenter(unsigned r, unsigned v) { return v > 2 * r ? v : v >= r ? 2 * (v - r) : 2 * (r - v) - 1; }
static void global_motion(struct bits *b, const VADecPictureParameterBufferAV1 *p, const VADecPictureParameterBufferAV1 *primary)
{
    for (unsigned ref = 0; ref < 7; ref++) {
        unsigned type = p->wm[ref].wmtype;
        if (type > VAAV1TransformationAffine || p->wm[ref].invalid) { b->error = 1; return; }
        put(b, 1, type != VAAV1TransformationIdentity); if (!type) continue;
        put(b, 1, type == VAAV1TransformationRotzoom);
        if (type != VAAV1TransformationRotzoom) put(b, 1, type == VAAV1TransformationTranslation);
        static const unsigned order[] = {2,3,4,5,0,1};
        for (unsigned j = 0; j < 6; j++) {
            unsigned index = order[j];
            if ((index == 2 || index == 3) && type < VAAV1TransformationRotzoom) continue;
            if ((index == 4 || index == 5) && type != VAAV1TransformationAffine) continue;
            unsigned absolute = 12, precision = 15;
            if (index < 2) {
                unsigned low = !p->pic_info_fields.bits.allow_high_precision_mv;
                absolute = type == VAAV1TransformationTranslation ? 9 - low : 12;
                precision = type == VAAV1TransformationTranslation ? 3 - low : 6;
            }
            int unit = 1 << (16 - precision), identity = index % 3 == 2 ? 65536 : 0;
            int64_t value = ((int64_t)p->wm[ref].wmmat[index] - identity) / unit;
            int previous = primary ? primary->wm[ref].wmmat[index] : identity;
            int64_t predictor = previous / unit;
            if (previous < 0 && previous % unit) predictor--; /* Arithmetic right shift. */
            predictor -= identity / unit;
            int limit = 1 << absolute;
            if (p->wm[ref].wmmat[index] % unit || value < -limit || value > limit || predictor < -limit || predictor > limit) { b->error = 1; return; }
            unsigned count = 2 * limit + 1, r = predictor + limit, v = value + limit;
            subexp(b, 2 * r <= count ? recenter(r, v) : recenter(count - 1 - r, count - 1 - v), count);
        }
    }
}
static int frame_obu(const struct av1_picture *pic, const struct vav4l2_av1_packet *s, unsigned width, unsigned height,
    unsigned refresh, int show, int reset, uint8_t *dst, size_t cap, size_t *used)
{
    const VADecPictureParameterBufferAV1 *p = &pic->p;
#define F(name) p->pic_info_fields.bits.name
#define S(name) p->seq_info_fields.fields.name
#define M(name) p->mode_control_fields.bits.name
    unsigned type = F(frame_type) == 0 && !reset ? 2 : F(frame_type);
    int intra = type == 0 || type == 2, resilient = (type == 0 && show) || type == 3 || F(error_resilient_mode);
    unsigned primary = intra || resilient ? 7 : p->primary_ref_frame, slots[7] = {0};
    if (!intra) for (unsigned i = 0; i < 7; i++) {
        if (p->ref_frame_idx[i] >= 8) return -1;
        VASurfaceID id = p->ref_frame_map[p->ref_frame_idx[i]]; unsigned slot = 0;
        for (; slot < 8; slot++) if (id != VA_INVALID_SURFACE && s->ref[slot].id == id) break;
        if (slot == 8) return -1;
        slots[i] = slot;
    }
    struct bits b = {0};
    put(&b, 1, 0); put(&b, 2, type); put(&b, 1, show); if (!show) put(&b, 1, 0);
    if (type != 3 && !(type == 0 && show)) put(&b, 1, resilient);
    put(&b, 1, F(disable_cdf_update)); put(&b, 1, F(allow_screen_content_tools));
    if (F(allow_screen_content_tools)) put(&b, 1, F(force_integer_mv));
    if (type != 3) put(&b, 1, 1);
    if (S(enable_order_hint)) put(&b, p->order_hint_bits_minus_1 + 1, p->order_hint);
    if (!intra && !resilient) put(&b, 3, primary);
    if (type != 3 && !(type == 0 && show)) put(&b, 8, refresh);
    if ((!intra || refresh != 255) && resilient && S(enable_order_hint)) for (unsigned i = 0; i < 8; i++) put(&b, p->order_hint_bits_minus_1 + 1, s->ref[i].p.order_hint);
    if (!intra) {
        if (S(enable_order_hint)) put(&b, 1, 0);
        for (unsigned i = 0; i < 7; i++) put(&b, 3, slots[i]);
        if (!resilient) for (unsigned i = 0; i < 7; i++) put(&b, 1, 0);
    }
    put(&b, log2_ceil(width), p->frame_width_minus1); put(&b, log2_ceil(height), p->frame_height_minus1);
    put(&b, 1, F(use_superres)); if (F(use_superres)) put(&b, 3, p->superres_scale_denominator - 9);
    put(&b, 1, 0);
    if (intra) { if (F(allow_screen_content_tools) && !F(use_superres)) put(&b, 1, F(allow_intrabc)); }
    else {
        if (!F(force_integer_mv)) put(&b, 1, F(allow_high_precision_mv));
        put(&b, 1, p->interp_filter == 4); if (p->interp_filter != 4) put(&b, 2, p->interp_filter);
        put(&b, 1, F(is_motion_mode_switchable)); if (!resilient && S(enable_order_hint)) put(&b, 1, F(use_ref_frame_mvs));
    }
    if (!F(disable_cdf_update)) put(&b, 1, F(disable_frame_end_update_cdf));
    tile_layout(&b, p); quantization(&b, p); segmentation(&b, p, primary);
    if (p->base_qindex) put(&b, 1, M(delta_q_present_flag));
    if (M(delta_q_present_flag)) {
        put(&b, 2, M(log2_delta_q_res)); if (!F(allow_intrabc)) put(&b, 1, M(delta_lf_present_flag));
        if (M(delta_lf_present_flag)) { put(&b, 2, M(log2_delta_lf_res)); put(&b, 1, M(delta_lf_multi)); }
    }
    int ll = lossless(p); filters(&b, p, ll);
    if (!ll) put(&b, 1, M(tx_mode) == 2);
    if (!intra) put(&b, 1, M(reference_select));
    int skip = !intra && skip_allowed(p, s, slots);
    if (!skip && M(skip_mode_present)) return -1;
    if (skip) put(&b, 1, M(skip_mode_present));
    if (!intra && !resilient) put(&b, 1, F(allow_warped_motion));
    put(&b, 1, M(reduced_tx_set_used));
    if (!intra) global_motion(&b, p, primary == 7 ? NULL : &s->ref[slots[primary]].p);
    film_grain(&b, p, show);
    align_bits(&b);
    unsigned tiles = p->tile_cols * p->tile_rows;
    if (tiles > 1) { put(&b, 1, 0); align_bits(&b); }
    if (b.error) return -1;
    size_t bytes = b.bit / 8;
    for (unsigned i = 0; i < tiles; i++) { if (bytes > cap || pic->size[i] + (uint64_t)(i + 1 < tiles ? 4 : 0) > cap - bytes) return -2; bytes += pic->size[i] + (i + 1 < tiles ? 4 : 0); }
    uint8_t *payload = malloc(bytes); if (!payload) return -2;
    size_t off = b.bit / 8; memcpy(payload, b.data, off);
    for (unsigned i = 0; i < tiles; i++) {
        if (i + 1 < tiles) for (unsigned j = 0; j < 4; j++) payload[off++] = (pic->size[i] - 1) >> (j * 8);
        memcpy(payload + off, pic->data + pic->offset[i], pic->size[i]); off += pic->size[i];
    }
    int result = obu(dst, cap, used, 6, payload, bytes); free(payload); return result < 0 ? -2 : 0;
#undef F
#undef S
#undef M
}
static int referenced(const VADecPictureParameterBufferAV1 *p, VASurfaceID id)
{
    if (id != VA_INVALID_SURFACE) for (unsigned i = 0; i < 8; i++) if (p->ref_frame_map[i] == id) return 1;
    return 0;
}
static int free_slot(const struct vav4l2_av1_packet *s, const VADecPictureParameterBufferAV1 *p)
{
    for (unsigned i = 0; i < 8; i++) if (!referenced(p, s->ref[i].id)) return i;
    return -1;
}
static void remember(struct vav4l2_av1_packet *s, const struct av1_picture *pic, unsigned refresh)
{
    if (!refresh) return;
    int assigned = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (s->ref[i].id == pic->p.current_frame) s->ref[i].id = VA_INVALID_SURFACE;
        if (refresh & (1u << i)) { s->ref[i].p = pic->p; s->ref[i].id = assigned ? VA_INVALID_SURFACE : pic->p.current_frame; assigned = 1; }
    }
}
VAStatus vav4l2_av1_packet_prepare(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count,
    uint8_t *dst, size_t cap, size_t *size, struct vav4l2_av1_packet **prepared)
{
    struct av1_picture *pic = NULL;
    VAStatus status = picture_collect(ctx, buffers, count, cap, &pic); if (status) return status;
    struct bits sequence = {0}; sequence_header(&sequence, &pic->p, ctx->width, ctx->height);
    if (sequence.error) { picture_free(pic); return VA_STATUS_ERROR_INVALID_PARAMETER; }
    struct vav4l2_av1_packet *s = calloc(1, sizeof(*s)); uint8_t *packet = malloc(cap ? cap : 1);
    if (!s || !packet) { free(s); free(packet); picture_free(pic); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    if (ctx->packet_state.av1) { *s = *ctx->packet_state.av1; if (s->pending) s->pending->refs++; }
    else for (unsigned i = 0; i < 8; i++) s->ref[i].id = VA_INVALID_SURFACE;
    size_t used = 0, seq_size = sequence.bit / 8;
    int new_sequence = s->sequence_size != seq_size || memcmp(s->sequence, sequence.data, seq_size);
    if (new_sequence) {
        if (pic->p.pic_info_fields.bits.frame_type != 0) { status = VA_STATUS_ERROR_INVALID_PARAMETER; goto done; }
        picture_free(s->pending); s->pending = NULL;
        for (unsigned i = 0; i < 8; i++) { memset(&s->ref[i], 0, sizeof(s->ref[i])); s->ref[i].id = VA_INVALID_SURFACE; }
        memcpy(s->sequence, sequence.data, seq_size); s->sequence_size = seq_size;
    }
    int reset = pic->p.pic_info_fields.bits.frame_type == 3 || (pic->p.pic_info_fields.bits.frame_type == 0 && (pic->p.pic_info_fields.bits.show_frame || new_sequence));
    if (obu(packet, cap, &used, 2, NULL, 0) < 0 || (new_sequence && obu(packet, cap, &used, 1, sequence.data, seq_size) < 0)) {
        status = VA_STATUS_ERROR_ALLOCATION_FAILED; goto done;
    }
    /* Install a previously uncommitted reference only after the next VA map
     * identifies a retired slot. Hidden replay preserves its CDF/MV state. */
    if (s->pending && !reset && referenced(&pic->p, s->pending->p.current_frame)) {
        int slot = free_slot(s, &pic->p); if (slot < 0) goto invalid;
        unsigned refresh = 1u << slot;
        int rc = frame_obu(s->pending, s, ctx->width, ctx->height, refresh, 0, 0, packet, cap, &used);
        if (rc < 0) { status = rc == -2 ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_ERROR_INVALID_PARAMETER; goto done; }
        remember(s, s->pending, refresh);
    }
    picture_free(s->pending); s->pending = NULL;
    int slot = free_slot(s, &pic->p); unsigned refresh = reset ? 255 : slot < 0 ? 0 : 1u << slot;
    int rc = frame_obu(pic, s, ctx->width, ctx->height, refresh, 1, reset, packet, cap, &used);
    if (rc < 0) { status = rc == -2 ? VA_STATUS_ERROR_ALLOCATION_FAILED : VA_STATUS_ERROR_INVALID_PARAMETER; goto done; }
    remember(s, pic, refresh);
    if (!refresh) { s->pending = pic; pic->refs++; }
    memcpy(dst, packet, used); *size = used;
    if (prepared) { *prepared = s; s = NULL; }
    goto done;
invalid:
    status = VA_STATUS_ERROR_INVALID_PARAMETER;
done:
    vav4l2_av1_packet_free(s); picture_free(pic); free(packet); return status;
}
