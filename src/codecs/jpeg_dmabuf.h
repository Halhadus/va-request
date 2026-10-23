/* JPEG OUTPUT transport only. Hantro does not consume arbitrary DRM offsets;
 * multi-plane input therefore requires zero-offset objects for every plane. */
static void jpeg_close_prime(VADRMPRIMESurfaceDescriptor *d)
{
    for (unsigned i = 0; i < d->num_objects; i++) close(d->objects[i].fd);
    d->num_objects = 0;
}

static int jpeg_prime(struct vareq_surface *s, VADRMPRIMESurfaceDescriptor *d, unsigned *raw)
{
    if (!s || !s->has_pic || s->status == VASurfaceSkipped ||
        (s->format != VA_FOURCC_NV12 && s->format != VA_FOURCC_I420 && s->format != VA_FOURCC_YV12)) return 0;
    VAStatus status;
    if (s->prime) status = vareq_prime_export_backing(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, d, 0);
    else if (s->gbm_bo && !s->gbm_dirty && (s->gbm_imported || s->gbm_writable || s->last_writer == VAREQ_WRITER_BO))
        status = vareq_surface_fill_prime_gbm(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, d);
    else return 0;
    if (status != VA_STATUS_SUCCESS) return -1;
    unsigned planes = s->format == VA_FOURCC_NV12 ? 2 : 3;
    int ok = d->num_layers == 1 && d->layers[0].num_planes == planes;
    if (s->format == VA_FOURCC_NV12 && d->num_objects == 1) {
        ok &= !d->layers[0].object_index[0] && !d->layers[0].object_index[1] && !d->layers[0].offset[0] &&
            d->layers[0].pitch[0] == d->layers[0].pitch[1] && d->layers[0].offset[1] == (uint64_t)d->layers[0].pitch[0] * s->height;
        *raw = V4L2_PIX_FMT_NV12;
    } else {
        ok &= d->num_objects == planes;
        for (unsigned p = 0; p < planes; p++) ok &= !d->layers[0].offset[p];
        *raw = planes == 2 ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_YUV420M;
    }
    for (unsigned o = 0; o < d->num_objects; o++) ok &= d->objects[o].drm_format_modifier == DRM_FORMAT_MOD_LINEAR;
    if (!ok) jpeg_close_prime(d);
    return ok;
}

static unsigned jpeg_prime_plane(const VADRMPRIMESurfaceDescriptor *d, unsigned p)
{
    return p && d->fourcc == VA_FOURCC_YV12 ? 3 - p : p;
}

static int jpeg_dma_format(struct vareq_context *ctx, struct v4l2_format *f, const VADRMPRIMESurfaceDescriptor *d, unsigned raw)
{
    *f = (struct v4l2_format){.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    f->fmt.pix_mp.width = d->width; f->fmt.pix_mp.height = d->height;
    f->fmt.pix_mp.pixelformat = raw; f->fmt.pix_mp.field = V4L2_FIELD_NONE;
    f->fmt.pix_mp.num_planes = raw == V4L2_PIX_FMT_NV12 ? 1 : raw == V4L2_PIX_FMT_NV12M ? 2 : 3;
    for (unsigned p = 0; p < f->fmt.pix_mp.num_planes; p++) f->fmt.pix_mp.plane_fmt[p].bytesperline = d->layers[0].pitch[jpeg_prime_plane(d, p)];
    return vareq_codec_format(ctx, f);
}

static int jpeg_dma_spans(const struct v4l2_format *f, const VADRMPRIMESurfaceDescriptor *d, unsigned raw)
{
    unsigned planes = raw == V4L2_PIX_FMT_NV12 ? 1 : raw == V4L2_PIX_FMT_NV12M ? 2 : 3;
    if (f->fmt.pix_mp.pixelformat != raw || f->fmt.pix_mp.width != d->width || f->fmt.pix_mp.height != d->height ||
        f->fmt.pix_mp.field != V4L2_FIELD_NONE || f->fmt.pix_mp.num_planes != planes) return 0;
    for (unsigned p = 0; p < planes; p++) {
        unsigned logical = jpeg_prime_plane(d, p), o = d->layers[0].object_index[logical];
        unsigned pitch = d->layers[0].pitch[logical], rows = p ? d->height / 2 : d->height;
        uint64_t need = (uint64_t)pitch * rows;
        if (planes == 1) need += (uint64_t)pitch * (d->height / 2);
        if (o >= d->num_objects || pitch < (p && planes == 3 ? d->width / 2 : d->width) ||
            f->fmt.pix_mp.plane_fmt[p].bytesperline != pitch ||
            f->fmt.pix_mp.plane_fmt[p].sizeimage < need || f->fmt.pix_mp.plane_fmt[p].sizeimage > d->objects[o].size) return 0;
    }
    return 1;
}

static void jpeg_trace(const struct vareq_context *ctx, const char *stage)
{
    const char *path = getenv("VAREQ_JPEG_TRACE");
    if (!path || !*path) return;
    FILE *file = fopen(path, "a");
    if (!file) { perror("JPEG trace"); return; }
    if (!ftell(file)) fprintf(file, "STAGE\tFORMAT\tWIDTH\tHEIGHT\tPLANES\tOUTPUT_MEMORY\tCAPTURE_MEMORY\n");
    const struct vareq_m2m_state *q = &ctx->jpeg_q;
    fprintf(file, "%s\t%08x\t%u\t%u\t%d\t%u\t%u\n", stage, q->ofmt.fmt.pix_mp.pixelformat,
        q->ofmt.fmt.pix_mp.width, q->ofmt.fmt.pix_mp.height, q->out_planes,
        q->out_memory ? q->out_memory : V4L2_MEMORY_MMAP, q->cap_memory ? q->cap_memory : V4L2_MEMORY_MMAP);
    if (fclose(file)) perror("close JPEG trace");
}
