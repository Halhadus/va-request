/* SPDX-License-Identifier: MIT */
#ifndef VA_REQUEST_API_INTERNAL_H
#define VA_REQUEST_API_INTERNAL_H
#include "va_request.h"
extern pthread_mutex_t vareq_api_lock;
void vareq_free_buffer_locked(struct vareq_buffer *buffer);
struct vareq_context *vareq_context_for_surface(struct vareq_driver_data *dd, VASurfaceID surface);
VAStatus vareq_query_image_formats(VADriverContextP ctx,
                           VAImageFormat *format_list,
                           int *num_formats);
VAStatus vareq_derive_image(VADriverContextP ctx,
                    VASurfaceID surface,
                    VAImage *image);
VAStatus vareq_destroy_image(VADriverContextP ctx, VAImageID image_id);
VAStatus vareq_create_image(VADriverContextP ctx, VAImageFormat *format,
                    int width, int height, VAImage *image);
VAStatus vareq_surface_read_image_locked(struct vareq_driver_data *driver_data,
                                          struct vareq_surface *surf, struct vareq_buffer *ib,
                                          int x, int y, unsigned width, unsigned height);
VAStatus vareq_get_image(VADriverContextP ctx, VASurfaceID surface,
                 int x, int y, unsigned int width, unsigned int height,
                 VAImageID image_id);
VAStatus vareq_put_image(VADriverContextP ctx,
                 VASurfaceID surface,
                 VAImageID image,
                 int src_x, int src_y,
                 unsigned int src_width, unsigned int src_height,
                 int dest_x, int dest_y,
                 unsigned int dest_width, unsigned int dest_height);
VAStatus vareq_put_surface(VADriverContextP ctx,
                   VASurfaceID surface,
                   void *draw,
                   short src_x, short src_y,
                   unsigned short src_w, unsigned short src_h,
                   short dest_x, short dest_y,
                   unsigned short dest_w, unsigned short dest_h,
                   VARectangle *cliprects,
                   unsigned int number_cliprects,
                   unsigned int flags);
VAStatus vareq_set_image_palette(
    VADriverContextP ctx,
    VAImageID image,
    unsigned char *palette);
VAStatus vareq_query_subpicture_formats(
    VADriverContextP ctx,
    VAImageFormat *format_list,
    unsigned int *flags,
    unsigned int *num_formats);
VAStatus vareq_create_subpicture(
    VADriverContextP ctx,
    VAImageID image,
    VASubpictureID *subpicture);
VAStatus vareq_destroy_subpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture);
VAStatus vareq_set_subpicture_image(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VAImageID image);
VAStatus vareq_set_subpicture_chromakey(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    unsigned int chromakey_min,
    unsigned int chromakey_max,
    unsigned int chromakey_mask);
VAStatus vareq_set_subpicture_alpha(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    float global_alpha);
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
    unsigned int flags);
VAStatus vareq_deassociate_subpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces);
VAStatus vareq_query_display_attributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int *num_attributes);
VAStatus vareq_display_attributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes);
#endif
