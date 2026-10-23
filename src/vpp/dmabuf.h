/* Included by the VPP implementation: synchronous contiguous or two-plane queue transport. */
static void close_prime(VADRMPRIMESurfaceDescriptor *d)
{
    for (unsigned i = 0; i < d->num_objects; i++) close(d->objects[i].fd);
    d->num_objects = 0;
}

/* Opaque-only RGB processing needs a read-only alpha check, with no pixel
 * upload/download or CPU processing substitute. Only the input crop is read. */
static int prime_opaque(const struct vareq_surface *s, const VADRMPRIMESurfaceDescriptor *d, const VARectangle *rect)
{
    if (rect->x < 0 || rect->y < 0 || (unsigned)rect->x + rect->width > s->width ||
        (unsigned)rect->y + rect->height > s->height) { errno = EINVAL; return -1; }
    int fd = d->objects[0].fd;
    if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0) return -1;
    unsigned char *map = mmap(NULL, d->objects[0].size, PROT_READ, MAP_SHARED, fd, 0);
    int opaque = map == MAP_FAILED ? -1 : 1, error = errno;
    if (map != MAP_FAILED) {
        unsigned alpha = s->format == VA_FOURCC_ARGB ? 0 : 3, pitch = d->layers[0].pitch[0];
        for (unsigned y = rect->y; opaque && y < (unsigned)rect->y + rect->height; y++)
            for (unsigned x = rect->x; x < (unsigned)rect->x + rect->width; x++)
                if (map[(size_t)y * pitch + x * 4 + alpha] != 255) { opaque = 0; break; }
        if (munmap(map, d->objects[0].size) < 0) { opaque = -1; error = errno; }
    }
    if (vareq_dmabuf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ) < 0) { opaque = -1; error = errno; }
    errno = error; return opaque;
}

static int queue_prime(struct vareq_surface *s, VADRMPRIMESurfaceDescriptor *d, uint32_t *raw, int overwrite, const VARectangle *read_region)
{
    /* These public layouts match the kernel's contiguous raw-plane layout.
     * Y/Cr/Cb descriptors may place Cb before Cr physically. Canonical external
     * Y/Cr/Cb storage still uses staging; native YVU420 output is unreliable. */
    switch (s->format) {
    case VA_FOURCC_NV12: case VA_FOURCC_NV21: case VA_FOURCC_I420:
    case VA_FOURCC_YUY2: case VA_FOURCC_UYVY: case VA_FOURCC_VYUY:
    case VA_FOURCC_YVYU: case VA_FOURCC_BGRX: case VA_FOURCC_YV12:
    case VA_FOURCC_422H: case VA_FOURCC_YV16: case VA_FOURCC_ARGB: case VA_FOURCC_BGRA:
    case VA_FOURCC_RGB565: case VA_FOURCC_RGBA: case VA_FOURCC_RGBX: break;
    default: return 0;
    }
    VAStatus st;
    if (s->prime) st = vareq_prime_export_backing(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, d, overwrite);
    else if (s->gbm_bo && (!s->gbm_dirty || overwrite)) st = vareq_surface_fill_prime_gbm(s, VA_EXPORT_SURFACE_COMPOSED_LAYERS, d);
    else return 0;
    if (st != VA_STATUS_SUCCESS) return -1;
    *raw = va_to_v4l2_raw(s->format);
    if (s->format == VA_FOURCC_422H || s->format == VA_FOURCC_YV16) *raw = V4L2_PIX_FMT_YUV422P;
    if ((s->format == VA_FOURCC_NV12 || s->format == VA_FOURCC_NV21) && d->num_objects == 2 &&
        d->num_layers == 1 && d->layers[0].num_planes == 2 && !d->layers[0].offset[0] && !d->layers[0].offset[1]) {
        *raw = s->format == VA_FOURCC_NV12 ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_NV21M;
        for (unsigned i = 0; i < 2; i++) {
            unsigned object = d->layers[0].object_index[i], pitch = d->layers[0].pitch[i];
            if (object >= 2 || pitch < s->width || d->objects[object].drm_format_modifier != DRM_FORMAT_MOD_LINEAR ||
                (uint64_t)pitch * (i ? s->height / 2 : s->height) > d->objects[object].size) {
                close_prime(d); return 0;
            }
        }
        return 2;
    }
    struct vareq_prime_layout l;
    int ok = !vareq_prime_layout(s->format, s->width, s->height, &l) &&
        d->num_objects == 1 && d->num_layers == 1 && d->layers[0].num_planes == l.planes &&
        d->layers[0].drm_format == l.drm_format && d->objects[0].drm_format_modifier == DRM_FORMAT_MOD_LINEAR;
    uint64_t offset = 0;
    for (unsigned i = 0; ok && i < l.planes; i++) {
        unsigned p = i && (s->format == VA_FOURCC_YV12 || s->format == VA_FOURCC_YV16) ? 3 - i : i;
        unsigned pitch = i && l.planes == 3 ? d->layers[0].pitch[0] / 2 : d->layers[0].pitch[0];
        if ((l.planes == 3 && (d->layers[0].pitch[0] & 1)) || pitch < l.row[i] ||
            d->layers[0].object_index[p] || d->layers[0].offset[p] != offset || d->layers[0].pitch[p] != pitch) ok = 0;
        offset += (uint64_t)pitch * l.rows[i];
    }
    if (!ok || offset > d->objects[0].size) { close_prime(d); return 0; }
    if (read_region && (s->format == VA_FOURCC_ARGB || s->format == VA_FOURCC_BGRA || s->format == VA_FOURCC_RGBA)) {
        int opaque = prime_opaque(s, d, read_region);
        if (opaque != 1) { close_prime(d); return opaque; }
    }
    return 1;
}

static void trace_dma_attempt(const struct vareq_context *ctx, const char *stage, int error,
                               const VADRMPRIMESurfaceDescriptor *src, const VADRMPRIMESurfaceDescriptor *dst)
{
    const char *path = getenv("VAREQ_DMABUF_TRACE");
    if (!path || !*path) return;
    FILE *file = fopen(path, "a");
    if (!file) { perror("DMA-BUF trace"); return; }
    if (!ftell(file)) fprintf(file, "NODE\tSTAGE\tERRNO\tSRC_FORMAT\tDST_FORMAT\tSRC_SIZE\tDST_SIZE\tSRC_OBJECTS\tDST_OBJECTS\tSRC_PITCH\tDST_PITCH\tSESSION\tSTREAMING\tRAW_OUTPUT\tRAW_CAPTURE\n");
    fprintf(file, "%s\t%s\t%d\t%08x\t%08x\t%ux%u\t%ux%u\t", ctx->device_path ? ctx->device_path : "-", stage, error,
        src->fourcc, dst->fourcc, src->width, src->height, dst->width, dst->height);
    const VADRMPRIMESurfaceDescriptor *descs[] = {src, dst};
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned o = 0; o < descs[i]->num_objects; o++) fprintf(file, "%s%u", o ? "," : "", descs[i]->objects[o].size);
        fputc('\t', file);
    }
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned p = 0; p < descs[i]->layers[0].num_planes; p++) fprintf(file, "%s%u@%u", p ? "," : "", descs[i]->layers[0].pitch[p], descs[i]->layers[0].offset[p]);
        fputc('\t', file);
    }
    const char *session = getenv("VAREQ_VPP_DMABUF_SESSION");
    fprintf(file, "%s\t%u\t%08x\t%08x\n", session && *session ? session : "default", ctx->vpp_q.streaming,
        ctx->vpp_q.ofmt.fmt.pix_mp.pixelformat, ctx->vpp_q.cfmt.fmt.pix_mp.pixelformat);
    if (fclose(file)) perror("close DMA-BUF trace");
}

/* Start each queue once; policy selects whether this precedes QBUF. */
static int start_dma_streams(int fd, struct vareq_m2m_state *q, const char **stage)
{
    for (unsigned i = 0; i < 2; i++) {
        if (q->streaming & (1u << i)) continue;
        enum v4l2_buf_type type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        *stage = i ? "STREAMON-capture" : "STREAMON-output";
        if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) return -1;
        q->streaming |= 1u << i;
    }
    return 0;
}

/* 1 = completed, 0 = safely use MMAP staging, -1 = operation failed.
 * Fallback/retry is allowed only before the new job starts, after queued buffers are cancelled. */
static int run_dmabuf(struct vareq_context *ctx, struct vareq_surface *src,
                      struct vareq_surface *dst, int rotate, int hflip, int vflip,
                      const struct v4l2_pix_format_mplane *in, const struct v4l2_pix_format_mplane *out,
                      const VARectangle *input, const VARectangle *output)
{
    const char *mode = getenv("VAREQ_VPP_DMABUF");
    if ((mode && !strcmp(mode, "0")) || !ctx->device_path) return 0;
    int required = mode && !strcmp(mode, "required");
    VADRMPRIMESurfaceDescriptor d[2] = {{0}};
    uint32_t raw[2] = {0}; unsigned planes[2] = {0}, excluded = 0;
    const char *stage = "layout";
    int result = 0, submitted = 0, job_started = 0, completed = 0, had_picture = dst->has_pic;
    struct vareq_m2m_state *q = &ctx->vpp_q;
    struct vareq_surface *surfaces[] = {src, dst};
    for (unsigned i = 0; i < 2; i++) {
        int ret = queue_prime(surfaces[i], &d[i], &raw[i], i && !output->x && !output->y && output->width == dst->width && output->height == dst->height, i ? NULL : input);
        if (ret <= 0) { result = ret; goto done; }
        planes[i] = ret;
    }
    uint32_t key[8] = {src->width, src->height, src->format, dst->width, dst->height, dst->format,
        (uint32_t)(rotate / 90) | ((uint32_t)hflip << 8) | ((uint32_t)vflip << 16),
        in->ycbcr_enc | (in->quantization << 4) | (out->ycbcr_enc << 8) | (out->quantization << 12)};
    /* Distinct VA IDs may still alias the same allocation. Snapshot input
     * through staging when any input/output object aliases. */
    for (unsigned i = 0; i < d[0].num_objects; i++) for (unsigned j = 0; j < d[1].num_objects; j++) {
        struct stat a, b;
        if (fstat(d[0].objects[i].fd, &a) < 0 || fstat(d[1].objects[j].fd, &b) < 0) { result = -1; goto done; }
        if (a.st_dev == b.st_dev && a.st_ino == b.st_ino) goto done;
    }
retry:
    stage = "select";
    struct v4l2_format *formats[] = {&q->ofmt, &q->cfmt};
    const struct v4l2_pix_format_mplane *colors[] = {in, out};
    int warm = q->valid && q->out_memory == V4L2_MEMORY_DMABUF && q->cap_memory == V4L2_MEMORY_DMABUF && !memcmp(q->key, key, sizeof(key));
    for (unsigned i = 0; warm && i < 2; i++) {
        const struct v4l2_pix_format_mplane *f = &formats[i]->fmt.pix_mp;
        warm = f->pixelformat == raw[i] && f->num_planes == planes[i] && color_matches(f, colors[i]);
        for (unsigned p = 0; warm && p < planes[i]; p++) {
            unsigned o = planes[i] == 1 ? 0 : d[i].layers[0].object_index[p];
            warm = f->plane_fmt[p].bytesperline == d[i].layers[0].pitch[p] && f->plane_fmt[p].sizeimage <= d[i].objects[o].size;
        }
    }
    if (select_device(ctx, raw[0], raw[1], src->width, src->height, dst->width, dst->height,
        rotate, hflip, vflip, input->x || input->y,
        output->x || output->y || output->width != dst->width || output->height != dst->height, warm, excluded, in->quantization, out->quantization) < 0) { result = 0; goto done; }
    warm &= q->valid; /* Selecting another node tears down the old queue. */
    /* Diagnostic sessions preserve the same DMA transport and pixel checks.
     * They distinguish queue reconfiguration from a fresh file context. */
    const char *session = getenv("VAREQ_VPP_DMABUF_SESSION");
    if (session && (!strcmp(session, "reconfigure") || !strcmp(session, "isolated"))) {
        warm = 0;
        if (!strcmp(session, "isolated")) {
            vareq_m2m_teardown(ctx, q);
            if (ctx->v4l2_fd < 0) { result = -1; goto done; }
            close(ctx->v4l2_fd); ctx->v4l2_fd = open(ctx->device_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        }
    }
    if (!warm) vareq_m2m_teardown(ctx, q);
    if (ctx->v4l2_fd < 0) { result = -1; goto done; }
    q->valid = 1;
    q->out_planes = planes[0];
    int fd = ctx->v4l2_fd;
    if (warm) goto spans;
    for (unsigned i = 0; i < 2; i++) {
        struct v4l2_format *f = formats[i];
        f->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        f->fmt.pix_mp = *colors[i];
        f->fmt.pix_mp.width = surfaces[i]->width; f->fmt.pix_mp.height = surfaces[i]->height;
        f->fmt.pix_mp.pixelformat = raw[i];
        f->fmt.pix_mp.field = V4L2_FIELD_NONE; f->fmt.pix_mp.num_planes = planes[i];
        for (unsigned p = 0; p < planes[i]; p++) f->fmt.pix_mp.plane_fmt[p].bytesperline = d[i].layers[0].pitch[p];
        stage = i ? "S_FMT-capture" : "S_FMT-output";
        if (xioctl(fd, VIDIOC_S_FMT, f) < 0) {
            result = errno == EINVAL || errno == ENOTTY ? 0 : -1; goto release;
        }
        if (f->fmt.pix_mp.width != surfaces[i]->width || f->fmt.pix_mp.height != surfaces[i]->height ||
            f->fmt.pix_mp.pixelformat != raw[i] || f->fmt.pix_mp.num_planes != planes[i] ||
            f->fmt.pix_mp.field != V4L2_FIELD_NONE || !color_matches(&f->fmt.pix_mp, colors[i])) goto release;
        for (unsigned p = 0; p < planes[i]; p++) {
            unsigned object = planes[i] == 1 ? 0 : d[i].layers[0].object_index[p];
            unsigned pitch = d[i].layers[0].pitch[p];
            unsigned size = planes[i] == 1 ? vareq_va_image_size(surfaces[i]->format, pitch, surfaces[i]->height) :
                pitch * (p ? surfaces[i]->height / 2 : surfaces[i]->height);
            if (f->fmt.pix_mp.plane_fmt[p].bytesperline != pitch || !size ||
                f->fmt.pix_mp.plane_fmt[p].sizeimage < size || f->fmt.pix_mp.plane_fmt[p].sizeimage > d[i].objects[object].size) goto release;
        }
    }

spans:;
    /* Some importer tables are sized from negotiated image bytes, whereas an
     * imported attachment exposes the entire object. Reject excess pages
     * before QBUF; retry another backend or use the normal staging path. */
    if (ctx->vpp_behaviors & VAREQ_COMPAT_EXACT_IMPORT_SPAN) {
        long page = sysconf(_SC_PAGESIZE);
        stage = "object-span";
        if (page <= 0) { result = -1; goto release; }
        for (unsigned i = 0; i < 2; i++) {
            uint64_t image = 0, pages = 0;
            for (unsigned p = 0; p < planes[i]; p++) {
                unsigned o = planes[i] == 1 ? 0 : d[i].layers[0].object_index[p];
                image += formats[i]->fmt.pix_mp.plane_fmt[p].sizeimage;
                pages += ((uint64_t)d[i].objects[o].size + page - 1) / page;
            }
            if (pages > (image + page - 1) / page) goto release;
        }
    }

    int partial = input->x || input->y || input->width != src->width || input->height != src->height ||
        output->x || output->y || output->width != dst->width || output->height != dst->height;
    /* Selection changes require idle queues. Both buffers were dequeued
     * before retaining a session; stop it before changing either rectangle. */
    if (q->streaming && (partial || q->selection_active)) {
        for (unsigned i = 0; i < 2; i++) {
            enum v4l2_buf_type type = formats[i]->type;
            stage = i ? "STREAMOFF-selection-capture" : "STREAMOFF-selection-output";
            if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) { result = -1; goto release; }
            q->streaming &= ~(1u << i);
        }
    }
    stage = "selection";
    if (set_regions(fd, q, input, output, partial) < 0) { result = -1; goto release; }
    if (warm) goto queue_job;
    int control_rotate = rotate, control_hflip = hflip, control_vflip = vflip;
    vareq_vpp_transform_controls(ctx->vpp_behaviors, &control_rotate, &control_hflip, &control_vflip);
    stage = "controls";
    if (set_transform(fd, ctx->vpp_controls, VAREQ_VPP_ROTATE, V4L2_CID_ROTATE, control_rotate) < 0 ||
        set_transform(fd, ctx->vpp_controls, VAREQ_VPP_HFLIP, V4L2_CID_HFLIP, control_hflip) < 0 ||
        set_transform(fd, ctx->vpp_controls, VAREQ_VPP_VFLIP, V4L2_CID_VFLIP, control_vflip) < 0) { result = -1; goto release; }
    for (unsigned i = 0; i < 2; i++) {
        struct v4l2_requestbuffers req = {.type = formats[i]->type, .memory = V4L2_MEMORY_DMABUF, .count = 1};
        stage = i ? "REQBUFS-capture" : "REQBUFS-output";
        if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
            result = errno == EINVAL || errno == ENOTTY ? 0 : -1; goto release;
        }
        if (!req.count) goto release;
        if (i) q->cap_memory = V4L2_MEMORY_DMABUF; else q->out_memory = V4L2_MEMORY_DMABUF;
    }
queue_job:
    /* Writable native BOs retain a sync fd for subsequent CPU readers. */
    stage = "native-write-sync";
    if (dst->gbm_bo && vareq_gbm_enable_write(dst) < 0) { result = -1; goto release; }
    stage = "reservation-wait";
    for (unsigned i = 0; i < 2; i++) for (unsigned o = 0; o < d[i].num_objects; o++)
        if (vareq_dmabuf_wait(d[i].objects[o].fd, i) < 0) { result = -1; goto release; }
    if ((ctx->vpp_behaviors & VAREQ_COMPAT_STREAM_BEFORE_QUEUE) && start_dma_streams(fd, q, &stage) < 0) { result = -1; goto release; }
    for (int i = 1; i >= 0; i--) {
        struct v4l2_plane plane[3] = {{0}};
        for (unsigned p = 0; p < planes[i]; p++) {
            unsigned o = planes[i] == 1 ? 0 : d[i].layers[0].object_index[p];
            plane[p].m.fd = d[i].objects[o].fd; plane[p].length = d[i].objects[o].size;
            plane[p].bytesused = i ? 0 : formats[i]->fmt.pix_mp.plane_fmt[p].sizeimage;
        }
        struct v4l2_buffer buffer = {.type = formats[i]->type, .memory = V4L2_MEMORY_DMABUF,
            .length = planes[i], .m.planes = plane};
        stage = i ? "QBUF-capture" : "QBUF-output";
        if (xioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
            result = errno == EINVAL || errno == ENOTTY || errno == ENOMEM || errno == ENOSPC ? 0 : -1; goto release;
        }
        submitted = 1;
        if (!i && q->streaming == 3) job_started = 1;
    }
    if (start_dma_streams(fd, q, &stage) < 0) { result = -1; goto release; }
    job_started = 1;
    stage = "completion-wait";
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (vareq_poll_intr(&pfd, 1, 2000) <= 0 || !(pfd.revents & POLLIN) ||
        (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) { result = -1; goto release; }
    struct v4l2_plane plane[3] = {{0}};
    struct v4l2_buffer buffer = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF,
        .length = planes[1], .m.planes = plane};
    stage = "DQBUF-capture";
    if (xioctl(fd, VIDIOC_DQBUF, &buffer) < 0 || buffer.index || buffer.length != planes[1] ||
        buffer.memory != V4L2_MEMORY_DMABUF || (buffer.flags & V4L2_BUF_FLAG_ERROR)) { result = -1; goto release; }
    for (unsigned p = 0; p < planes[1]; p++) {
        unsigned o = planes[1] == 1 ? 0 : d[1].layers[0].object_index[p];
        if (plane[p].data_offset || plane[p].bytesused < q->cfmt.fmt.pix_mp.plane_fmt[p].sizeimage ||
            plane[p].bytesused > d[1].objects[o].size) { result = -1; goto release; }
    }
    stage = "DQBUF-output";
    if (vareq_m2m_finish_output(ctx, q) < 0) { result = -1; goto release; }
    /* Retain streams only when selected policy requires it. Both DQBUFs
     * have returned buffer ownership before retaining the session. */
    for (unsigned i = 0; !(ctx->vpp_behaviors & VAREQ_COMPAT_KEEP_DMA_STREAMS) && i < 2; i++) {
        enum v4l2_buf_type type = formats[i]->type;
        stage = i ? "STREAMOFF-capture" : "STREAMOFF-output";
        if (xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) { result = -1; goto release; }
        q->streaming &= ~(1u << i);
    }
    completed = 1; result = 1;
    memcpy(q->key, key, sizeof(key));
    trace_dma_attempt(ctx, "complete", 0, &d[0], &d[1]);
    /* Both buffers are dequeued; vb2 may retain object attachments until
     * queue teardown. Local descriptor duplicates close below. */
    if (dst->gbm_bo && dst->format == VA_FOURCC_NV12) {
        dst->stride = dst->gbm_stride; dst->aligned_h = dst->height; dst->cap_fourcc = V4L2_PIX_FMT_NV12;
    }
    goto done;
release:
    {
        int retryable = !result && !job_started;
        trace_dma_attempt(ctx, stage, result ? errno : EINVAL, &d[0], &d[1]);
        vareq_m2m_teardown(ctx, q);
        if (ctx->v4l2_fd < 0) result = -1;
        else if (retryable) {
            for (unsigned i = 0; i < ctx->driver_data->num_vpp_devices; i++)
                if (!strcmp(ctx->device_path, ctx->driver_data->vpp_devices[i].path)) excluded |= 1u << i;
            submitted = 0; /* STREAMOFF/REQBUFS(0) cancelled the unstarted job. */
            goto retry;
        }
    }
done:
    if (submitted) vareq_prime_device_written(dst, completed);
    else dst->has_pic = had_picture;
    close_prime(&d[0]); close_prime(&d[1]);
    return !result && required ? -1 : result;
}
