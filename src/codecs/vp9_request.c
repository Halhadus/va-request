/*
 * VP9 VA-API to V4L2 stateless translation.
 *
 * The V4L2 interface wants bitstream-level values (segmentation feature
 * data, loop filter deltas, quantiser deltas) and the raw probability
 * updates of the compressed header, none of which VA-API carries. Both
 * frame headers are re-parsed from the bitstream: the uncompressed one
 * with a plain bit reader and the compressed one with a VPX boolean
 * decoder, mirroring the FFmpeg v4l2-request hwaccel.
 *
 * Copyright (C) 2026 Ondrej Jirman <megi@xff.cz>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include "va_v4l2.h"
#include "probe.h"
#include "vp9_request.h"

#include <va/va_dec_vp9.h>

struct vav4l2_vp9_bits {
    const uint8_t *data;
    size_t size, pos;
    bool error;
};
static unsigned vav4l2_vp9_bits_read(struct vav4l2_vp9_bits *b, unsigned n)
{
    unsigned value = 0;
    if (n > 32 || b->size > SIZE_MAX / 8 || b->pos > b->size * 8 || n > b->size * 8 - b->pos) {
        b->error = true; return 0;
    }
    while (n--) { value = (value << 1) | ((b->data[b->pos / 8] >> (7 - b->pos % 8)) & 1); b->pos++; }
    return value;
}
static unsigned vav4l2_vp9_bits_bit(struct vav4l2_vp9_bits *b) { return vav4l2_vp9_bits_read(b, 1); }
static void vav4l2_vp9_bits_init(struct vav4l2_vp9_bits *b, const uint8_t *data, size_t size, bool rbsp)
{
    *b = (struct vav4l2_vp9_bits){.data = data, .size = size, .error = rbsp || !data};
}

/* --- VPX boolean decoder (libvpx dboolhuff layout) --- */

#define VP9_BD_VALUE_SIZE   ((int)(sizeof(uint64_t) * 8))

struct vp9_bool {
    const uint8_t *buffer;
    const uint8_t *end;
    uint64_t value;     /* MSB-aligned code word */
    int count;      /* number of loaded bits below the top byte */
    unsigned int range;
    bool error;
};

/*
 * Load whole bytes into the low end of the code word until it is full.
 * The next byte goes just below the lowest currently-valid bit, at
 * VP9_BD_VALUE_SIZE - 8 - (count + 8); refilling only ever runs when
 * count has just gone negative (down to -7 after a renormalisation), so
 * the shift stays <= 56 and the byte never overflows the 64-bit word.
 */
static void vp9_bool_fill(struct vp9_bool *b)
{
    int shift = VP9_BD_VALUE_SIZE - 8 - (b->count + 8);

    while (shift >= 0 && b->buffer < b->end) {
        b->count += 8;
        b->value |= (uint64_t)(*b->buffer++) << shift;
        shift -= 8;
    }

    if (b->buffer >= b->end && b->count < 0)
        b->error = true;
}

static void vp9_bool_init(struct vp9_bool *b, const uint8_t *data, size_t size)
{
    memset(b, 0, sizeof(*b));
    b->buffer = data;
    b->end = data + size;
    b->range = 255;
    b->count = -8;
    vp9_bool_fill(b);
}

static int vp9_bool_bit(struct vp9_bool *b, unsigned int prob)
{
    unsigned int split = 1 + (((b->range - 1) * prob) >> 8);
    uint64_t bigsplit = (uint64_t)split << (VP9_BD_VALUE_SIZE - 8);
    int bit;

    if (b->count < 0)
        vp9_bool_fill(b);

    if (b->value >= bigsplit) {
        b->range -= split;
        b->value -= bigsplit;
        bit = 1;
    } else {
        b->range = split;
        bit = 0;
    }

    while (b->range < 128) {
        b->range <<= 1;
        b->value <<= 1;
        b->count--;
    }

    return bit;
}

static unsigned int vp9_bool_lit(struct vp9_bool *b, unsigned int bits)
{
    unsigned int value = 0;

    while (bits--)
        value = (value << 1) | vp9_bool_bit(b, 128);

    return value;
}

static bool vp9_bool_valid(const struct vp9_bool *b)
{
    /* Renormalizing the final symbol can exhaust the buffered bits before
     * another read would refill them. Unread input still makes that valid. */
    return !b->error && (b->count >= 0 || b->buffer < b->end);
}

/* --- codec state --- */

struct vp9_frame_header {
    uint8_t bit_depth, reset_frame_context;
    bool keyframe;
    bool intra_only;
    bool error_resilient;
    bool color_range_full;
    bool lossless;
    bool allow_high_precision_mv;
    bool switchable_filter;

    uint8_t lf_level;
    uint8_t lf_sharpness;
    bool lf_delta_enabled;
    bool lf_delta_update;

    uint8_t base_q_idx;
    int8_t delta_q_y_dc;
    int8_t delta_q_uv_dc;
    int8_t delta_q_uv_ac;

    bool seg_enabled;
    bool seg_update_map;
    bool seg_temporal_update;
    bool seg_update_data;
    uint8_t seg_tree_probs[7];
    uint8_t seg_pred_probs[3];

    bool valid;
};

struct vp9_context {
    VADecPictureParameterBufferVP9 va_pic;

    struct vp9_frame_header hdr;
    struct v4l2_ctrl_vp9_frame frame;
    struct v4l2_ctrl_vp9_compressed_hdr compressed_hdr;
    unsigned int reference_mode;

    struct vav4l2_vp9_state state, pending;
};

/* --- uncompressed header parser (VP9 spec 6.2) --- */

static const int16_t seg_feature_bits[4] = { 8, 6, 2, 0 };
static const bool seg_feature_signed[4] = { true, true, false, false };

static void vp9_parse_color_config(struct vav4l2_vp9_bits *b,
                   struct vp9_frame_header *hdr,
                   unsigned int profile)
{
    unsigned int color_space;

    hdr->bit_depth = profile >= 2 ?
        (vav4l2_vp9_bits_bit(b) ? 12 : 10) : 8;

    color_space = vav4l2_vp9_bits_read(b, 3);
    if (color_space != 7 /* CS_RGB */) {
        hdr->color_range_full = vav4l2_vp9_bits_bit(b);
        if (profile == 1 || profile == 3) {
            vav4l2_vp9_bits_read(b, 2); /* subsampling_x/y */
            vav4l2_vp9_bits_bit(b); /* reserved_zero */
        }
    } else {
        hdr->color_range_full = true;
        if (profile == 0 || profile == 2)
            b->error = true; /* RGB requires profile 1 or 3. */
        if (profile == 1 || profile == 3)
            vav4l2_vp9_bits_bit(b); /* reserved_zero */
    }
}

static void vp9_parse_frame_size(struct vav4l2_vp9_bits *b,
                 const VADecPictureParameterBufferVP9 *pic)
{
    unsigned int width = vav4l2_vp9_bits_read(b, 16) + 1;
    unsigned int height = vav4l2_vp9_bits_read(b, 16) + 1;
    if (width != pic->frame_width || height != pic->frame_height)
        b->error = true;
}

static void vp9_parse_frame_and_render_size(struct vav4l2_vp9_bits *b,
                      const VADecPictureParameterBufferVP9 *pic)
{
    vp9_parse_frame_size(b, pic);
    if (vav4l2_vp9_bits_bit(b)) {       /* render_and_frame_size_different */
        vav4l2_vp9_bits_read(b, 16);
        vav4l2_vp9_bits_read(b, 16);
    }
}

static void vp9_parse_uncompressed_header(struct vp9_context *codec,
                      const uint8_t *data, size_t size)
{
    struct vp9_frame_header *hdr = &codec->hdr;
    struct vav4l2_vp9_bits bits, *b = &bits;
    unsigned int profile;

    memset(hdr, 0, sizeof(*hdr));
    hdr->color_range_full = codec->pending.color_range_full;

    vav4l2_vp9_bits_init(b, data, size, false);

    if (vav4l2_vp9_bits_read(b, 2) != 2)        /* frame_marker */
        return;

    profile = vav4l2_vp9_bits_bit(b);
    profile |= vav4l2_vp9_bits_bit(b) << 1;
    if (profile != codec->va_pic.profile || (profile != 0 && profile != 2))
        return; /* Only the advertised 4:2:0 profiles are implemented. */

    if (vav4l2_vp9_bits_bit(b))         /* show_existing_frame */
        return;

    hdr->keyframe = !vav4l2_vp9_bits_bit(b);    /* frame_type */
    bool show_frame = vav4l2_vp9_bits_bit(b);
    hdr->error_resilient = vav4l2_vp9_bits_bit(b);
    if (show_frame != !!codec->va_pic.pic_fields.bits.show_frame ||
        hdr->error_resilient != !!codec->va_pic.pic_fields.bits.error_resilient_mode)
        return;

    if (hdr->keyframe) {
        if (vav4l2_vp9_bits_read(b, 24) != 0x498342)
            return;
        vp9_parse_color_config(b, hdr, profile);
        vp9_parse_frame_and_render_size(b, &codec->va_pic);
    } else {
        if (!show_frame)
            hdr->intra_only = vav4l2_vp9_bits_bit(b);

        if (!hdr->error_resilient)
            hdr->reset_frame_context = vav4l2_vp9_bits_read(b, 2);

        if (hdr->intra_only) {
            if (vav4l2_vp9_bits_read(b, 24) != 0x498342)
                return;
            if (profile > 0)
                vp9_parse_color_config(b, hdr, profile);
            else
                hdr->color_range_full = false;
            vav4l2_vp9_bits_read(b, 8); /* refresh_frame_flags */
            vp9_parse_frame_and_render_size(b, &codec->va_pic);
        } else {
            vav4l2_vp9_bits_read(b, 8); /* refresh_frame_flags */

            for (int i = 0; i < 3; i++) {
                vav4l2_vp9_bits_read(b, 3); /* ref_frame_idx */
                vav4l2_vp9_bits_bit(b); /* sign bias */
            }

            /* frame_size_with_refs() */
            {
                bool found = false;
                for (int i = 0; i < 3 && !found; i++)
                    found = vav4l2_vp9_bits_bit(b);
                if (!found) {
                    vp9_parse_frame_size(b, &codec->va_pic);
                }
                if (vav4l2_vp9_bits_bit(b)) {
                    vav4l2_vp9_bits_read(b, 16);
                    vav4l2_vp9_bits_read(b, 16);
                }
            }

            hdr->allow_high_precision_mv = vav4l2_vp9_bits_bit(b);

            /* read_interpolation_filter(): only whether the filter
             * is switchable matters (for the compressed header);
             * the filter type itself comes from the VA fields. */
            hdr->switchable_filter = vav4l2_vp9_bits_bit(b);
            if (!hdr->switchable_filter)
                vav4l2_vp9_bits_read(b, 2);
        }
    }

    if (!hdr->error_resilient) {
        if (vav4l2_vp9_bits_bit(b) != codec->va_pic.pic_fields.bits.refresh_frame_context ||
            vav4l2_vp9_bits_bit(b) != codec->va_pic.pic_fields.bits.frame_parallel_decoding_mode)
            return;
    }

    if (vav4l2_vp9_bits_read(b, 2) != codec->va_pic.pic_fields.bits.frame_context_idx)
        return;

    /*
     * setup_past_independence(): key, intra-only and error-resilient frames
     * reset the persistent loop-filter deltas to their VP9 defaults and
     * clear the segmentation feature data. Other frames inherit both from
     * the previous frame and update only what the bitstream re-sends.
     */
    if (hdr->keyframe || hdr->intra_only || hdr->error_resilient) {
        static const int8_t default_ref_deltas[4] = { 1, 0, -1, -1 };

        memcpy(codec->pending.lf_ref_deltas, default_ref_deltas,
               sizeof(codec->pending.lf_ref_deltas));
        memset(codec->pending.lf_mode_deltas, 0, sizeof(codec->pending.lf_mode_deltas));
        memset(codec->pending.seg_feature_enabled, 0,
               sizeof(codec->pending.seg_feature_enabled));
        memset(codec->pending.seg_feature_data, 0,
               sizeof(codec->pending.seg_feature_data));
        codec->pending.seg_abs_delta = false;
    }

    /* loop_filter_params() */
    hdr->lf_level = vav4l2_vp9_bits_read(b, 6);
    hdr->lf_sharpness = vav4l2_vp9_bits_read(b, 3);
    hdr->lf_delta_enabled = vav4l2_vp9_bits_bit(b);
    if (hdr->lf_delta_enabled) {
        hdr->lf_delta_update = vav4l2_vp9_bits_bit(b);
        if (hdr->lf_delta_update) {
            for (int i = 0; i < 4; i++) {
                if (vav4l2_vp9_bits_bit(b)) {
                    int v = vav4l2_vp9_bits_read(b, 6);
                    codec->pending.lf_ref_deltas[i] =
                        vav4l2_vp9_bits_bit(b) ? -v : v;
                }
            }
            for (int i = 0; i < 2; i++) {
                if (vav4l2_vp9_bits_bit(b)) {
                    int v = vav4l2_vp9_bits_read(b, 6);
                    codec->pending.lf_mode_deltas[i] =
                        vav4l2_vp9_bits_bit(b) ? -v : v;
                }
            }
        }
    }

    /* quantization_params() */
    hdr->base_q_idx = vav4l2_vp9_bits_read(b, 8);
    for (int i = 0; i < 3; i++) {
        int8_t *delta = i == 0 ? &hdr->delta_q_y_dc :
                  i == 1 ? &hdr->delta_q_uv_dc : &hdr->delta_q_uv_ac;
        if (vav4l2_vp9_bits_bit(b)) {
            int v = vav4l2_vp9_bits_read(b, 4);
            *delta = vav4l2_vp9_bits_bit(b) ? -v : v;
        }
    }
    hdr->lossless = hdr->base_q_idx == 0 && !hdr->delta_q_y_dc &&
            !hdr->delta_q_uv_dc && !hdr->delta_q_uv_ac;

    /* segmentation_params() */
    memset(hdr->seg_pred_probs, 255, sizeof(hdr->seg_pred_probs));
    memset(hdr->seg_tree_probs, 255, sizeof(hdr->seg_tree_probs));
    hdr->seg_enabled = vav4l2_vp9_bits_bit(b);
    if (hdr->seg_enabled) {
        hdr->seg_update_map = vav4l2_vp9_bits_bit(b);
        if (hdr->seg_update_map) {
            for (int i = 0; i < 7; i++)
                hdr->seg_tree_probs[i] = vav4l2_vp9_bits_bit(b) ?
                    vav4l2_vp9_bits_read(b, 8) : 255;

            hdr->seg_temporal_update = vav4l2_vp9_bits_bit(b);
            for (int i = 0; i < 3; i++)
                hdr->seg_pred_probs[i] =
                    (hdr->seg_temporal_update &&
                     vav4l2_vp9_bits_bit(b)) ?
                    vav4l2_vp9_bits_read(b, 8) : 255;
        }

        hdr->seg_update_data = vav4l2_vp9_bits_bit(b);
        if (hdr->seg_update_data) {
            codec->pending.seg_abs_delta = vav4l2_vp9_bits_bit(b);

            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 4; j++) {
                    int16_t value = 0;

                    codec->pending.seg_feature_enabled[i][j] =
                        vav4l2_vp9_bits_bit(b);
                    if (!codec->pending.seg_feature_enabled[i][j])
                        continue;

                    if (seg_feature_bits[j])
                        value = vav4l2_vp9_bits_read(b,
                            seg_feature_bits[j]);
                    if (seg_feature_signed[j] &&
                        vav4l2_vp9_bits_bit(b))
                        value = -value;

                    codec->pending.seg_feature_data[i][j] = value;
                }
            }
        }
    }

    /* Finish the uncompressed header so client-provided boundaries cannot
     * redirect the probability parser into a different section of the frame. */
    unsigned int sb_cols = (codec->va_pic.frame_width + 63u) / 64;
    unsigned int min_cols = 0, max_cols = 0;
    while ((64u << min_cols) < sb_cols)
        min_cols++;
    while ((sb_cols >> (max_cols + 1)) >= 4)
        max_cols++;
    unsigned int cols = min_cols;
    while (cols < max_cols && vav4l2_vp9_bits_bit(b))
        cols++;
    unsigned int rows = vav4l2_vp9_bits_bit(b);
    if (rows)
        rows += vav4l2_vp9_bits_bit(b);
    unsigned int compressed_size = vav4l2_vp9_bits_read(b, 16);
    if (cols != codec->va_pic.log2_tile_columns ||
        rows != codec->va_pic.log2_tile_rows ||
        !compressed_size || compressed_size != codec->va_pic.first_partition_size ||
        (b->pos + 7) / 8 != codec->va_pic.frame_header_length_in_bytes ||
        (hdr->bit_depth && hdr->bit_depth != codec->va_pic.bit_depth) ||
        hdr->keyframe == !!codec->va_pic.pic_fields.bits.frame_type ||
        hdr->intra_only != !!codec->va_pic.pic_fields.bits.intra_only)
        return;
    hdr->valid = !b->error;
    if (hdr->valid)
        codec->pending.color_range_full = hdr->color_range_full;
}

/* --- compressed header parser, follows FFmpeg v4l2_request_vp9.c --- */

static int vp9_read_prob_delta(struct vp9_bool *c)
{
    static const uint8_t inv_map_table[255] = {
          7,  20,  33,  46,  59,  72,  85,  98, 111, 124, 137, 150, 163, 176,
        189, 202, 215, 228, 241, 254,   1,   2,   3,   4,   5,   6,   8,   9,
         10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  21,  22,  23,  24,
         25,  26,  27,  28,  29,  30,  31,  32,  34,  35,  36,  37,  38,  39,
         40,  41,  42,  43,  44,  45,  47,  48,  49,  50,  51,  52,  53,  54,
         55,  56,  57,  58,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
         70,  71,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  83,  84,
         86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,  99, 100,
        101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 112, 113, 114, 115,
        116, 117, 118, 119, 120, 121, 122, 123, 125, 126, 127, 128, 129, 130,
        131, 132, 133, 134, 135, 136, 138, 139, 140, 141, 142, 143, 144, 145,
        146, 147, 148, 149, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160,
        161, 162, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
        177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 190, 191,
        192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 203, 204, 205, 206,
        207, 208, 209, 210, 211, 212, 213, 214, 216, 217, 218, 219, 220, 221,
        222, 223, 224, 225, 226, 227, 229, 230, 231, 232, 233, 234, 235, 236,
        237, 238, 239, 240, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251,
        252, 253, 253,
    };
    unsigned int d;

    if (!vp9_bool_bit(c, 128)) {
        d = vp9_bool_lit(c, 4);
    } else if (!vp9_bool_bit(c, 128)) {
        d = vp9_bool_lit(c, 4) + 16;
    } else if (!vp9_bool_bit(c, 128)) {
        d = vp9_bool_lit(c, 5) + 32;
    } else {
        d = vp9_bool_lit(c, 7);
        if (d >= 65)
            d = (d << 1) - 65 + vp9_bool_bit(c, 128);
        d += 64;
        if (d >= 255)
            d = 254;
    }

    return inv_map_table[d];
}

#define VP9_UPDATE(c, target) \
    do { if (vp9_bool_bit(c, 252)) (target) = vp9_read_prob_delta(c); } while (0)
#define VP9_MV_UPDATE(c, target) \
    do { if (vp9_bool_bit(c, 252)) (target) = (vp9_bool_lit(c, 7) << 1) | 1; } while (0)

static bool vp9_parse_compressed_header(struct vp9_context *codec,
                    const uint8_t *data, size_t size)
{
    struct v4l2_ctrl_vp9_compressed_hdr *ctrl = &codec->compressed_hdr;
    const struct vp9_frame_header *hdr = &codec->hdr;
    const VADecPictureParameterBufferVP9 *pic = &codec->va_pic;
    bool inter = !hdr->keyframe && !hdr->intra_only;
    bool allow_comp_inter;
    unsigned int comp_pred_mode = 0; /* SINGLE */
    struct vp9_bool c;

    memset(ctrl, 0, sizeof(*ctrl));
    codec->reference_mode = 0;

    if (!data || !size)
        return false;

    vp9_bool_init(&c, data, size);

    if (vp9_bool_bit(&c, 128))      /* marker bit must be 0 */
        return false;

    if (hdr->lossless) {
        ctrl->tx_mode = V4L2_VP9_TX_MODE_ONLY_4X4;
    } else {
        ctrl->tx_mode = vp9_bool_lit(&c, 2);
        if (ctrl->tx_mode == V4L2_VP9_TX_MODE_ALLOW_32X32)
            ctrl->tx_mode += vp9_bool_bit(&c, 128);

        if (ctrl->tx_mode == V4L2_VP9_TX_MODE_SELECT) {
            for (int i = 0; i < 2; i++)
                VP9_UPDATE(&c, ctrl->tx8[i][0]);
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++)
                    VP9_UPDATE(&c, ctrl->tx16[i][j]);
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 3; j++)
                    VP9_UPDATE(&c, ctrl->tx32[i][j]);
        }
    }

    /* coefficient probability updates */
    for (unsigned int i = 0; i < 4; i++) {
        if (vp9_bool_bit(&c, 128)) {
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 2; k++)
                    for (int l = 0; l < 6; l++)
                        for (int m = 0; m < 6; m++) {
                            if (m >= 3 && l == 0)
                                break;
                            for (int n = 0; n < 3; n++)
                                VP9_UPDATE(&c, ctrl->coef[i][j][k][l][m][n]);
                        }
        }
        if (ctrl->tx_mode == i)
            break;
    }

    for (int i = 0; i < 3; i++)
        VP9_UPDATE(&c, ctrl->skip[i]);

    if (!inter)
        return vp9_bool_valid(&c);

    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 3; j++)
            VP9_UPDATE(&c, ctrl->inter_mode[i][j]);

    if (hdr->switchable_filter)
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 2; j++)
                VP9_UPDATE(&c, ctrl->interp_filter[i][j]);

    for (int i = 0; i < 4; i++)
        VP9_UPDATE(&c, ctrl->is_inter[i]);

    allow_comp_inter =
        (pic->pic_fields.bits.last_ref_frame_sign_bias !=
         pic->pic_fields.bits.golden_ref_frame_sign_bias) ||
        (pic->pic_fields.bits.last_ref_frame_sign_bias !=
         pic->pic_fields.bits.alt_ref_frame_sign_bias);

    if (allow_comp_inter) {
        comp_pred_mode = vp9_bool_bit(&c, 128);
        if (comp_pred_mode)
            comp_pred_mode += vp9_bool_bit(&c, 128);
        if (comp_pred_mode == 2 /* SWITCHABLE */)
            for (int i = 0; i < 5; i++)
                VP9_UPDATE(&c, ctrl->comp_mode[i]);
    }

    if (comp_pred_mode != 1 /* not compound-only */) {
        for (int i = 0; i < 5; i++) {
            VP9_UPDATE(&c, ctrl->single_ref[i][0]);
            VP9_UPDATE(&c, ctrl->single_ref[i][1]);
        }
    }

    if (comp_pred_mode != 0 /* not single-only */) {
        for (int i = 0; i < 5; i++)
            VP9_UPDATE(&c, ctrl->comp_ref[i]);
    }

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 9; j++)
            VP9_UPDATE(&c, ctrl->y_mode[i][j]);

    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 3; k++)
                VP9_UPDATE(&c, ctrl->partition[(i * 4) + j][k]);

    for (int i = 0; i < 3; i++)
        VP9_MV_UPDATE(&c, ctrl->mv.joint[i]);

    for (int i = 0; i < 2; i++) {
        VP9_MV_UPDATE(&c, ctrl->mv.sign[i]);
        for (int j = 0; j < 10; j++)
            VP9_MV_UPDATE(&c, ctrl->mv.classes[i][j]);
        VP9_MV_UPDATE(&c, ctrl->mv.class0_bit[i]);
        for (int j = 0; j < 10; j++)
            VP9_MV_UPDATE(&c, ctrl->mv.bits[i][j]);
    }

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 3; k++)
                VP9_MV_UPDATE(&c, ctrl->mv.class0_fr[i][j][k]);
        for (int j = 0; j < 3; j++)
            VP9_MV_UPDATE(&c, ctrl->mv.fr[i][j]);
    }

    if (hdr->allow_high_precision_mv) {
        for (int i = 0; i < 2; i++) {
            VP9_MV_UPDATE(&c, ctrl->mv.class0_hp[i]);
            VP9_MV_UPDATE(&c, ctrl->mv.hp[i]);
        }
    }

    codec->reference_mode = comp_pred_mode;
    return vp9_bool_valid(&c);
}

/* --- control fill --- */

static void vp9_fill_frame(struct vav4l2_context *ctx, struct vp9_context *codec)
{
    const VADecPictureParameterBufferVP9 *pic = &codec->va_pic;
    const struct vp9_frame_header *hdr = &codec->hdr;
    struct v4l2_ctrl_vp9_frame *frame = &codec->frame;

    *frame = (struct v4l2_ctrl_vp9_frame) {
        .lf = {
            .level = hdr->valid ? hdr->lf_level : pic->filter_level,
            .sharpness = hdr->valid ? hdr->lf_sharpness :
                     pic->sharpness_level,
        },

        .quant = {
            .base_q_idx = hdr->base_q_idx,
            .delta_q_y_dc = hdr->delta_q_y_dc,
            .delta_q_uv_dc = hdr->delta_q_uv_dc,
            .delta_q_uv_ac = hdr->delta_q_uv_ac,
        },

        .compressed_header_size = pic->first_partition_size,
        .uncompressed_header_size = pic->frame_header_length_in_bytes,
        .frame_width_minus_1 = pic->frame_width - 1,
        .frame_height_minus_1 = pic->frame_height - 1,
        .render_width_minus_1 = pic->frame_width - 1,
        .render_height_minus_1 = pic->frame_height - 1,
        .reset_frame_context = hdr->reset_frame_context > 0 ? hdr->reset_frame_context - 1 : 0,
        .frame_context_idx = pic->pic_fields.bits.frame_context_idx,
        .profile = pic->profile,
        .bit_depth = pic->bit_depth,
        .interpolation_filter = pic->pic_fields.bits.mcomp_filter_type,
        .tile_cols_log2 = pic->log2_tile_columns,
        .tile_rows_log2 = pic->log2_tile_rows,
        .reference_mode = codec->reference_mode,
    };

    for (int i = 0; i < 4; i++)
        frame->lf.ref_deltas[i] = codec->pending.lf_ref_deltas[i];
    for (int i = 0; i < 2; i++)
        frame->lf.mode_deltas[i] = codec->pending.lf_mode_deltas[i];

    if (hdr->lf_delta_enabled)
        frame->lf.flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_ENABLED;
    if (hdr->lf_delta_update)
        frame->lf.flags |= V4L2_VP9_LOOP_FILTER_FLAG_DELTA_UPDATE;

    for (int i = 0; i < 8; i++) {
        static const uint8_t feature_map[4] = {
            V4L2_VP9_SEG_LVL_ALT_Q,
            V4L2_VP9_SEG_LVL_ALT_L,
            V4L2_VP9_SEG_LVL_REF_FRAME,
            V4L2_VP9_SEG_LVL_SKIP,
        };

        for (int j = 0; j < 4; j++) {
            if (!codec->pending.seg_feature_enabled[i][j])
                continue;
            frame->seg.feature_enabled[i] |=
                V4L2_VP9_SEGMENT_FEATURE_ENABLED(feature_map[j]);
            frame->seg.feature_data[i][feature_map[j]] =
                codec->pending.seg_feature_data[i][j];
        }
    }

    memcpy(frame->seg.tree_probs, hdr->seg_tree_probs,
           sizeof(frame->seg.tree_probs));
    memcpy(frame->seg.pred_probs, hdr->seg_pred_probs,
           sizeof(frame->seg.pred_probs));

    if (hdr->seg_enabled)
        frame->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_ENABLED;
    if (hdr->seg_update_map)
        frame->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_UPDATE_MAP;
    if (hdr->seg_temporal_update)
        frame->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_TEMPORAL_UPDATE;
    if (hdr->seg_update_data)
        frame->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_UPDATE_DATA;
    if (codec->pending.seg_abs_delta)
        frame->seg.flags |= V4L2_VP9_SEGMENTATION_FLAG_ABS_OR_DELTA_UPDATE;

    if (!pic->pic_fields.bits.frame_type)
        frame->flags |= V4L2_VP9_FRAME_FLAG_KEY_FRAME;
    if (pic->pic_fields.bits.show_frame)
        frame->flags |= V4L2_VP9_FRAME_FLAG_SHOW_FRAME;
    if (pic->pic_fields.bits.error_resilient_mode)
        frame->flags |= V4L2_VP9_FRAME_FLAG_ERROR_RESILIENT;
    if (pic->pic_fields.bits.intra_only)
        frame->flags |= V4L2_VP9_FRAME_FLAG_INTRA_ONLY;
    if (pic->pic_fields.bits.frame_type &&
        pic->pic_fields.bits.allow_high_precision_mv)
        frame->flags |= V4L2_VP9_FRAME_FLAG_ALLOW_HIGH_PREC_MV;
    if (pic->pic_fields.bits.refresh_frame_context)
        frame->flags |= V4L2_VP9_FRAME_FLAG_REFRESH_FRAME_CTX;
    if (pic->pic_fields.bits.frame_parallel_decoding_mode)
        frame->flags |= V4L2_VP9_FRAME_FLAG_PARALLEL_DEC_MODE;
    if (pic->pic_fields.bits.subsampling_x)
        frame->flags |= V4L2_VP9_FRAME_FLAG_X_SUBSAMPLING;
    if (pic->pic_fields.bits.subsampling_y)
        frame->flags |= V4L2_VP9_FRAME_FLAG_Y_SUBSAMPLING;
    if (hdr->color_range_full)
        frame->flags |= V4L2_VP9_FRAME_FLAG_COLOR_RANGE_FULL_SWING;

    frame->last_frame_ts = vav4l2_surface_ts(ctx->driver_data,
            pic->reference_frames[pic->pic_fields.bits.last_ref_frame]);
    frame->golden_frame_ts = vav4l2_surface_ts(ctx->driver_data,
            pic->reference_frames[pic->pic_fields.bits.golden_ref_frame]);
    frame->alt_frame_ts = vav4l2_surface_ts(ctx->driver_data,
            pic->reference_frames[pic->pic_fields.bits.alt_ref_frame]);

    if (pic->pic_fields.bits.last_ref_frame_sign_bias)
        frame->ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_LAST;
    if (pic->pic_fields.bits.golden_ref_frame_sign_bias)
        frame->ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_GOLDEN;
    if (pic->pic_fields.bits.alt_ref_frame_sign_bias)
        frame->ref_frame_sign_bias |= V4L2_VP9_SIGN_BIAS_ALT;
}

VAStatus vav4l2_vp9_request_parse(struct vav4l2_context *ctx,
    const VADecPictureParameterBufferVP9 *pic, const uint8_t *data, size_t size,
    struct vav4l2_vp9_state *pending, struct v4l2_ctrl_vp9_frame *frame,
    struct v4l2_ctrl_vp9_compressed_hdr *compressed)
{
    unsigned profile = ctx->profile == VAProfileVP9Profile2 ? 2 : 0;
    if (!pic || !data || !pending || !frame || !compressed ||
        (ctx->profile != VAProfileVP9Profile0 && ctx->profile != VAProfileVP9Profile2) ||
        pic->profile != profile || pic->bit_depth != (profile ? 10 : 8) ||
        !pic->pic_fields.bits.subsampling_x || !pic->pic_fields.bits.subsampling_y)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t header = pic->frame_header_length_in_bytes, partition = pic->first_partition_size;
    if (!pic->frame_width || !pic->frame_height || pic->frame_width != ctx->width || pic->frame_height != ctx->height ||
        !header || header >= size || !partition || partition >= size - header)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    struct vp9_context codec = {.va_pic = *pic, .pending = *pending};
    vp9_parse_uncompressed_header(&codec, data, header);
    if (!codec.hdr.valid || !vp9_parse_compressed_header(&codec, data + header, partition))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    vp9_fill_frame(ctx, &codec);
    *pending = codec.pending; *frame = codec.frame; *compressed = codec.compressed_hdr;
    return VA_STATUS_SUCCESS;
}
VAStatus vav4l2_vp9_translate(struct vav4l2_context *ctx, struct vav4l2_buffer **buffers, int count)
{
    struct vav4l2_collected cb;
    vav4l2_collect_decode_buffers(buffers, count, &cb);
    for (int i = 0; i < count; i++) if (buffers[i] && buffers[i]->type == VAPictureParameterBufferType &&
        buffers[i]->size < sizeof(VADecPictureParameterBufferVP9)) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!cb.pic) return VA_STATUS_ERROR_INVALID_PARAMETER;
    size_t bytes = 0;
    VAStatus status = vav4l2_packetize_slices(VAV4L2_CODEC_VP9, buffers, count, NULL, UINT32_MAX, &bytes);
    if (status != VA_STATUS_SUCCESS) return status;
    uint8_t *data = malloc(bytes);
    if (!data) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    status = vav4l2_packetize_slices(VAV4L2_CODEC_VP9, buffers, count, data, bytes, &bytes);
    struct vav4l2_vp9_state pending = ctx->vp9_state ? *ctx->vp9_state : (struct vav4l2_vp9_state){0};
    struct v4l2_ctrl_vp9_frame frame;
    struct v4l2_ctrl_vp9_compressed_hdr compressed;
    if (status == VA_STATUS_SUCCESS) status = vav4l2_vp9_request_parse(ctx, cb.pic, data, bytes, &pending, &frame, &compressed);
    if (status != VA_STATUS_SUCCESS) { free(data); return status; }
    if (((const VADecPictureParameterBufferVP9 *)cb.pic)->pic_fields.bits.frame_type && !((const VADecPictureParameterBufferVP9 *)cb.pic)->pic_fields.bits.intra_only) {
        unsigned refs[] = {((const VADecPictureParameterBufferVP9 *)cb.pic)->pic_fields.bits.last_ref_frame, ((const VADecPictureParameterBufferVP9 *)cb.pic)->pic_fields.bits.golden_ref_frame, ((const VADecPictureParameterBufferVP9 *)cb.pic)->pic_fields.bits.alt_ref_frame};
        for (unsigned i = 0; i < 3; i++) {
            VAStatus status = vav4l2_validate_reference(ctx, ((const VADecPictureParameterBufferVP9 *)cb.pic)->reference_frames[refs[i]]);
            if (status != VA_STATUS_SUCCESS) { free(data); return status; }
        }
    }
    if (!ctx->vp9_state) ctx->vp9_state = calloc(1, sizeof(*ctx->vp9_state));
    if (!ctx->vp9_state) { free(data); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    struct v4l2_ctrl_vp9_frame previous = {0};
    _Static_assert(sizeof(frame) <= sizeof(ctx->g_ctrl_payload), "grow sequence cache");
    if (ctx->g_ctrl_valid) memcpy(&previous, ctx->g_ctrl_payload, sizeof(previous));
    if (!ctx->g_ctrl_valid || frame.profile != previous.profile || frame.bit_depth != previous.bit_depth ||
        frame.frame_width_minus_1 != previous.frame_width_minus_1 || frame.frame_height_minus_1 != previous.frame_height_minus_1) {
        struct v4l2_ext_control sequence = {.id = V4L2_CID_STATELESS_VP9_FRAME, .ptr = &frame, .size = sizeof(frame)};
        struct v4l2_ext_controls global = {.count = 1, .controls = &sequence};
        if (vav4l2_codec_controls(ctx, -1, &global) < 0) { free(data); return VA_STATUS_ERROR_OPERATION_FAILED; }
        memcpy(ctx->g_ctrl_payload, &frame, sizeof(frame)); ctx->g_ctrl_valid = 1;
    }
    uint32_t format = V4L2_PIX_FMT_NV12;
    if (frame.bit_depth == 10) {
        uint32_t raw[VAV4L2_MAX_FORMATS]; int n = vav4l2_enum_capture_fourccs(ctx->v4l2_fd, raw, VAV4L2_MAX_FORMATS);
        format = 0;
        for (int i = 0; i < n; i++) if (raw[i] == V4L2_PIX_FMT_NV15) format = V4L2_PIX_FMT_NV15;
        for (int i = 0; i < n; i++) if (raw[i] == V4L2_PIX_FMT_P010) format = V4L2_PIX_FMT_P010;
        if (!format) { free(data); return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT; }
    }
    if (vav4l2_ensure_capture(ctx, frame.frame_width_minus_1 + 1, frame.frame_height_minus_1 + 1, format) < 0 ||
        !ctx->n_free_out || bytes > ctx->output_buf_size) { free(data); return VA_STATUS_ERROR_OPERATION_FAILED; }
    if (!ctx->streamed) {
        if (vav4l2_streamon(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) < 0 ||
            vav4l2_streamon(ctx->v4l2_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
            free(data); return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        ctx->streamed = 1;
    }
    struct v4l2_ext_control controls[] = {
        {.id = V4L2_CID_STATELESS_VP9_FRAME, .ptr = &frame, .size = sizeof(frame)},
        {.id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR, .ptr = &compressed, .size = sizeof(compressed)},
    };
    struct v4l2_query_ext_ctrl query = {.id = V4L2_CID_STATELESS_VP9_COMPRESSED_HDR};
    int has_compressed = vav4l2_xioctl(ctx->v4l2_fd, VIDIOC_QUERY_EXT_CTRL, &query) == 0 &&
        !(query.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY));
    struct v4l2_ext_controls ctrls = {.controls = controls, .count = has_compressed ? 2 : 1};
    if (vav4l2_codec_controls(ctx, ctx->request_fd, &ctrls) < 0) { free(data); return VA_STATUS_ERROR_OPERATION_FAILED; }
    int index = ctx->free_out_bufs[--ctx->n_free_out];
    if (!ctx->output_buf_ptr[index]) { free(data); vav4l2_out_pool_push(ctx, index); return VA_STATUS_ERROR_OPERATION_FAILED; }
    memcpy(ctx->output_buf_ptr[index], data, bytes); free(data);
    int done = vav4l2_decode_submit(ctx, index, bytes, ctx->current_surface ? ctx->current_surface->timestamp : 0);
    if (done < 0) return done == -2 ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_ERROR_OPERATION_FAILED;
    if (ctx->current_surface && vav4l2_surface_pull_capture(ctx, ctx->current_surface, done) < 0) {
        if (!vav4l2_surface_capture_matches(ctx, ctx->current_surface) || ctx->current_surface->buf_index != done)
            vav4l2_cap_pool_push(ctx, done);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!ctx->current_surface) vav4l2_cap_pool_push(ctx, done);
    *ctx->vp9_state = pending;
    return VA_STATUS_SUCCESS;
}
