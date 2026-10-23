/* SPDX-License-Identifier: MIT */
#include "internal.h"

VAStatus vareq_put_surface(VADriverContextP ctx,
                   VASurfaceID surface,
                   void *draw,
                   short src_x, short src_y,
                   unsigned short src_w, unsigned short src_h,
                   short dest_x, short dest_y,
                   unsigned short dest_w, unsigned short dest_h,
                   VARectangle *cliprects,
                   unsigned int number_cliprects,
                   unsigned int flags)
{
    (void)ctx; (void)surface; (void)draw; (void)src_x; (void)src_y; (void)src_w; (void)src_h;
    (void)dest_x; (void)dest_y; (void)dest_w; (void)dest_h; (void)cliprects; (void)number_cliprects; (void)flags;
    /* Not needed for decode-only driver */
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* Unsupported entrypoints still need the exact libva callback prototype.
 * Casting one generic function into unrelated signatures is undefined C. */
VAStatus vareq_set_image_palette(
    VADriverContextP ctx,
    VAImageID image,
    unsigned char *palette)
{
    (void)ctx;
    (void)image;
    (void)palette;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_query_subpicture_formats(
    VADriverContextP ctx,
    VAImageFormat *format_list,
    unsigned int *flags,
    unsigned int *num_formats)
{
    (void)ctx;
    (void)format_list;
    (void)flags;
    (void)num_formats;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_create_subpicture(
    VADriverContextP ctx,
    VAImageID image,
    VASubpictureID *subpicture)
{
    (void)ctx;
    (void)image;
    (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_destroy_subpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture)
{
    (void)ctx;
    (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_set_subpicture_image(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VAImageID image)
{
    (void)ctx;
    (void)subpicture;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_set_subpicture_chromakey(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    unsigned int chromakey_min,
    unsigned int chromakey_max,
    unsigned int chromakey_mask)
{
    (void)ctx;
    (void)subpicture;
    (void)chromakey_min;
    (void)chromakey_max;
    (void)chromakey_mask;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_set_subpicture_alpha(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    float global_alpha)
{
    (void)ctx;
    (void)subpicture;
    (void)global_alpha;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_associate_subpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces,
    short src_x,
    short src_y,
    unsigned short src_width,
    unsigned short src_height,
    short dest_x,
    short dest_y,
    unsigned short dest_width,
    unsigned short dest_height,
    unsigned int flags)
{
    (void)ctx;
    (void)subpicture;
    (void)target_surfaces;
    (void)num_surfaces;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dest_x;
    (void)dest_y;
    (void)dest_width;
    (void)dest_height;
    (void)flags;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_deassociate_subpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces)
{
    (void)ctx;
    (void)subpicture;
    (void)target_surfaces;
    (void)num_surfaces;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_query_display_attributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int *num_attributes)
{
    (void)ctx;
    (void)attr_list;
    (void)num_attributes;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus vareq_display_attributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes)
{
    (void)ctx;
    (void)attr_list;
    (void)num_attributes;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
