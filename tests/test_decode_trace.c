/* Trace actual queue failure paths without a decoder or writable procfs. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "va_v4l2.h"

static int failures, storage = -1, opens, fail_open, fail_write, trace_cloexec, last_fd = -1;
static void expect(int ok, const char *tag) { if (!ok) { fprintf(stderr, "FAIL decode trace %s\n", tag); failures++; } }
int __real_open(const char *, int, ...);
int __real_open64(const char *, int, ...);
static int trace_open(const char *path, int flags, mode_t mode, int large)
{
    if (strcmp(path, "decode-trace-fixture")) return large ? __real_open64(path, flags, mode) : __real_open(path, flags, mode);
    opens++; trace_cloexec = !!(flags & O_CLOEXEC);
    if (fail_open) { errno = ENOSPC; return -1; }
    return last_fd = fcntl(storage, F_DUPFD_CLOEXEC, 0);
}
ssize_t __real_write(int fd, const void *data, size_t size);
ssize_t __wrap_write(int fd, const void *data, size_t size) { if (fail_write && fd == last_fd) { errno = ENOSPC; return -1; } return __real_write(fd, data, size); }
int __wrap_open(const char *path, int flags, ...) { mode_t mode = 0; if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); } return trace_open(path, flags, mode, 0); }
int __wrap_open64(const char *path, int flags, ...) { mode_t mode = 0; if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); } return trace_open(path, flags, mode, 1); }
static int fault_ioctl(int fd, unsigned long request, void *arg) { (void)fd; (void)request; (void)arg; errno = EIO; return -1; }
int main(void)
{
    storage = memfd_create("decode trace", MFD_CLOEXEC); if (storage < 0) return 1;
    struct vav4l2_surface surface = {.surface_id = 7, .buf_index = -1};
    struct vav4l2_context ctx = {.context_id = 3, .codec = VAV4L2_CODEC_HEVC, .current_surface = &surface,
        .v4l2_fd = 17, .request_fd = 18, .width = 3840, .height = 2160, .frame_count = 5,
        .output_bufs_allocd = 1, .output_buf_size = 64, .capture_bufs_allocd = 4};
    unsetenv("VAV4L2_DECODE_TRACE"); errno = EBUSY;
    vav4l2_decode_trace(&ctx, "disabled", -1, 0); expect(!opens && errno == EBUSY, "disabled-no-open-and-errno");
    setenv("VAV4L2_DECODE_TRACE", "decode-trace-fixture", 1);
    expect(vav4l2_decode_submit(&ctx, 0, 8, 1000) == -1 && ctx.n_free_out == 1 && !ctx.n_free_cap, "empty-pool-stays-failed-with-output-recovered");
    vav4l2_set_ioctl_hook(fault_ioctl);
    errno = 0; expect(vav4l2_ioctl_once(17, VIDIOC_QBUF, NULL) == -1 && errno == EIO, "ioctl-error-preserved");
    fail_open = 1; errno = 0; expect(vav4l2_ioctl_once(17, VIDIOC_QBUF, NULL) == -1 && errno == EIO, "trace-open-failure-cannot-replace-ioctl-errno");
    fail_open = 0; expect(trace_cloexec, "trace-FD-CLOEXEC");
    fail_write = 1; errno = 0; expect(vav4l2_ioctl_once(17, VIDIOC_QBUF, NULL) == -1 && errno == EIO, "trace-write-failure-cannot-replace-ioctl-errno");
    fail_write = 0; expect(fcntl(last_fd, F_GETFD) < 0 && errno == EBADF, "trace-FD-closes-after-write-failure");
    char log[8192]; ssize_t n = pread(storage, log, sizeof(log) - 1, 0); expect(n > 0, "trace-has-evidence");
    if (n > 0) { log[n] = 0; expect(strstr(log, "STAGE=capture-empty") && strstr(log, "ALLOCATED=4\tFREE=0") && strstr(log, "WIDTH=3840\tHEIGHT=2160") && strstr(log, "FRAME=5\tSURFACE=7"), "queue-context-is-recorded"); expect(strstr(log, "STAGE=ioctl") && strstr(log, "STATUS=-1") && strstr(log, "ERRNO=5"), "raw-ioctl-error-is-recorded"); }
    vav4l2_set_ioctl_hook(NULL); unsetenv("VAV4L2_DECODE_TRACE"); close(storage);
    printf("decode trace:%s\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
