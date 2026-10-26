/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Validated storage export and capability-selected compatibility providers. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "va_v4l2.h"
#include "probe.h"
#include "compat/compat.h"
struct storage_node { char path[64]; struct vav4l2_compat compat; };
static struct storage_node storage_nodes[VAV4L2_PROBE_MAX_NODES];
static unsigned num_storage_nodes;
static int scanned;
void vav4l2_allocator_cleanup(void) { num_storage_nodes = 0; scanned = 0; }
static void trace_allocation(const char *node, const char *stage, unsigned bytes, unsigned rows, unsigned size, unsigned allocated, int error, const struct v4l2_format *fmt)
{
    const char *path = getenv("VAV4L2_ALLOC_TRACE"); if (!path || !*path) return;
    FILE *file = fopen(path, "a"); if (!file) { perror("allocation trace"); return; }
    if (!ftell(file)) fprintf(file, "NODE\tSTAGE\tWIDTH_BYTES\tROWS\tSIZE\tALLOCATED\tERRNO\tFORMAT\tALLOC_WIDTH\tALLOC_HEIGHT\tSTRIDE\tIMAGE\n");
    fprintf(file, "%s\t%s\t%u\t%u\t%u\t%u\t%d\t", node, stage, bytes, rows, size, allocated, error);
    const struct v4l2_pix_format_mplane *p = &fmt->fmt.pix_mp;
    fprintf(file, "%08x\t%u\t%u\t%u\t%u\n", p->pixelformat, p->width, p->height, p->plane_fmt[0].bytesperline, p->plane_fmt[0].sizeimage);
    if (fclose(file)) perror("close allocation trace");
}
static int export_storage(const char *node, struct v4l2_format *fmt, unsigned bytes, unsigned rows, int exact, const char *tag)
{
    uint64_t need = (uint64_t)bytes * rows;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return -1;
    int fd = vav4l2_open_device(node); if (fd < 0) return -1;
    const char *stage = "S_FMT"; int result = -1, error = EINVAL; unsigned allocated = 0;
    uint32_t wanted = fmt->fmt.pix_mp.pixelformat;
    if (vav4l2_xioctl(fd, VIDIOC_S_FMT, fmt) < 0) { error = errno; goto done; }
    if (fmt->fmt.pix_mp.pixelformat != wanted || fmt->fmt.pix_mp.num_planes != 1 ||
        fmt->fmt.pix_mp.plane_fmt[0].sizeimage < need ||
        (exact && fmt->fmt.pix_mp.plane_fmt[0].sizeimage > ((need + page - 1) / page) * page)) goto done;
    struct v4l2_requestbuffers req = {.type = fmt->type, .memory = V4L2_MEMORY_MMAP, .count = 1};
    stage = "REQBUFS";
    if (vav4l2_xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { error = errno; goto done; }
    if (!req.count || (req.flags & V4L2_MEMORY_FLAG_NON_COHERENT)) goto done;
    struct v4l2_plane plane = {0};
    struct v4l2_buffer buffer = {.type = fmt->type, .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &plane};
    stage = "QUERYBUF";
    if (vav4l2_xioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) { error = errno; goto done; }
    if (buffer.length != 1 || buffer.index || buffer.memory != V4L2_MEMORY_MMAP ||
        plane.length < need || plane.length < fmt->fmt.pix_mp.plane_fmt[0].sizeimage) goto done;
    struct v4l2_exportbuffer exp = {.type = fmt->type, .flags = O_RDWR | O_CLOEXEC};
    stage = "EXPBUF";
    if (vav4l2_xioctl(fd, VIDIOC_EXPBUF, &exp) < 0) { error = errno; goto done; }
    unsigned size; stage = "span";
    int fd_flags = fcntl(exp.fd, F_GETFD);
    if (fd_flags < 0 || !(fd_flags & FD_CLOEXEC) ||
        vav4l2_export_fd_validate(exp.fd, need, &size) < 0 || size < plane.length) { close(exp.fd); goto done; }
    /* Exact providers must not expose pages beyond the requested span. */
    if (exact && size > ((need + page - 1) / page) * page) { close(exp.fd); goto done; }
    allocated = size; result = exp.fd; error = 0;
    stage = tag ? tag : "exported";
done:
    trace_allocation(node, stage, bytes, rows, need, allocated, error, fmt);
    vav4l2_queue_forget(fd); close(fd); return result;
}

static void scan_storage(void)
{
    char paths[VAV4L2_PROBE_MAX_NODES][64];
    int count = vav4l2_list_video_nodes(paths, VAV4L2_PROBE_MAX_NODES);
    if (count < 0) return;
    for (int i = 0; i < count; i++) {
        int fd = open(paths[i], O_RDWR | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        struct v4l2_capability cap = {0};
        int ret = vav4l2_xioctl(fd, VIDIOC_QUERYCAP, &cap); close(fd);
        if (ret || !vav4l2_node_is_m2m(&cap)) continue;
        struct vav4l2_compat_identity id; vav4l2_compat_identity(&cap, &id);
        struct vav4l2_compat compat;
        if (vav4l2_compat_resolve(&id, &compat) < 0) continue;
        unsigned providers = 0;
        for (unsigned j = 0; j < compat.count; j++) providers += !!compat.modules[j]->allocate;
        if (!providers) continue;
        struct storage_node *node = &storage_nodes[num_storage_nodes++];
        memcpy(node->path, paths[i], sizeof(node->path)); node->compat = compat;
    }
    scanned = 1;
}
static int alloc_storage(unsigned bytes, unsigned rows, int coherent)
{
    uint64_t need = (uint64_t)bytes * rows;
    if (!bytes || !rows || need > UINT32_MAX) { errno = EINVAL; return -1; }
    if (!scanned) scan_storage();
    struct candidate { struct storage_node *node; const struct vav4l2_compat_module *module; int score; };
    struct candidate candidates[VAV4L2_PROBE_MAX_NODES * VAV4L2_COMPAT_MAX_MATCHES];
    unsigned count = 0;
    for (unsigned i = 0; i < num_storage_nodes; i++) {
        struct storage_node *node = &storage_nodes[i];
        for (unsigned j = 0; j < node->compat.count; j++) {
            const struct vav4l2_compat_module *module = node->compat.modules[j];
            if (!module->allocate) continue;
            int score = module->storage_score(&node->compat.identity, bytes, rows, coherent);
            if (score < 0) continue;
            unsigned at = count++;
            while (at && candidates[at - 1].score < score) { candidates[at] = candidates[at - 1]; at--; }
            candidates[at] = (struct candidate){node, module, score};
        }
    }
    const struct vav4l2_storage_ops ops = {.export_capture = export_storage};
    for (unsigned i = 0; i < count; i++) {
        const struct candidate *candidate = &candidates[i];
        int fd = candidate->module->allocate(&candidate->node->compat.identity, candidate->node->path, bytes, rows, coherent, &ops);
        if (fd < 0) continue;
        uint32_t actual;
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC) && vav4l2_export_fd_validate(fd, need, &actual) == 0) return fd;
        close(fd);
    }
    errno = ENOTSUP;
    return -1;
}
int vav4l2_alloc_shared_storage(unsigned bytes, unsigned rows)
{
    const char *storage = getenv("VAV4L2_SHARED_STORAGE");
    return alloc_storage(bytes, rows, storage && !strcmp(storage, "coherent"));
}
int vav4l2_alloc_coherent_storage(unsigned bytes, unsigned rows) { return alloc_storage(bytes, rows, 1); }
