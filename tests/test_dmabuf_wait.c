/* Deadline/fence fault injection; fake time avoids real multi-second waits. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <linux/dma-buf.h>
#include "va_request.h"
static unsigned failures, polls, clocks, calls, interrupts, again, successes, clock_fail;
static int poll_result, ioctl_error, budgets[8];
static short readiness, requested;
static int64_t tick, step;
static void expect(int ok, const char *name) { if (!ok) { fprintf(stderr, "FAIL DMA deadline: %s\n", name); failures++; } }
int __wrap_clock_gettime(clockid_t clock, struct timespec *t)
{
    expect(clock == CLOCK_MONOTONIC, "monotonic deadline"); clocks++;
    if (clock_fail && clocks == clock_fail) { errno = EIO; return -1; }
    t->tv_sec = tick / 1000000000; t->tv_nsec = tick % 1000000000; tick += step; return 0;
}
int __wrap_poll(struct pollfd *p, nfds_t n, int timeout)
{
    expect(n == 1, "single DMA fence"); requested = p->events;
    if (polls < 8) budgets[polls] = timeout;
    polls++;
    if (interrupts) { interrupts--; p->revents = POLLERR; errno = EINTR; return -1; }
    p->revents = readiness; if (poll_result < 0) errno = EACCES; return poll_result;
}
static int sync_ioctl(int fd, unsigned long request, void *arg)
{
    (void)fd; (void)arg; calls++;
    /* A cap also keeps the deliberately old unbounded helper test finite. */
    if (calls > 8) { errno = ELOOP; return -1; }
    if (interrupts) { interrupts--; errno = EINTR; return -1; }
    if (again) { again--; errno = EAGAIN; return -1; }
    if (ioctl_error) { errno = ioctl_error; return -1; }
    expect(request == DMA_BUF_IOCTL_SYNC || request == VIDIOC_QUERYCAP, "expected ioctl"); successes++; return 0;
}
static void reset(void)
{
    polls = clocks = calls = interrupts = again = successes = clock_fail = 0;
    tick = 0; step = 800000000; readiness = POLLIN | POLLOUT; poll_result = 1; ioctl_error = 0;
    memset(budgets, 0, sizeof(budgets)); errno = EDOM;
}
int main(void)
{
    vareq_set_ioctl_hook(sync_ioctl);
    for (unsigned write = 0; write < 2; write++) {
        reset(); expect(!vareq_dmabuf_wait(7, write) && requested == (write ? POLLOUT : POLLIN), "read/write reservation events");
    }
    reset(); poll_result = 0; expect(vareq_dmabuf_wait(7, 0) < 0 && errno == ETIMEDOUT && !calls, "timeout reports ETIMEDOUT without CPU access");
    const short bad[] = {POLLERR, POLLHUP, POLLNVAL, POLLPRI, POLLIN | POLLHUP};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        reset(); readiness = bad[i]; expect(vareq_dmabuf_wait(7, 0) < 0 && errno == (bad[i] & POLLNVAL ? EBADF : EIO), "invalid/error/hangup/wrong readiness reports its cause");
    }
    reset(); poll_result = -1; expect(vareq_dmabuf_wait(7, 0) < 0 && errno == EACCES, "poll syscall errno preserved");
    struct pollfd p = {.fd = 7, .events = POLLIN};
    reset(); interrupts = 3;
    expect(!vareq_poll_intr(&p, 1, 2000) && polls == 3 && budgets[0] == 2000 && budgets[1] == 1200 && budgets[2] == 400 && !p.revents,
        "signal storm consumes one timeout budget and clears stale readiness");
    reset(); interrupts = 1;
    expect(vareq_poll_intr(&p, 1, 2000) == 1 && polls == 2 && budgets[1] == 1200, "transient signal keeps only remaining time");
    reset(); interrupts = 2;
    expect(vareq_poll_intr(&p, 1, -1) == 1 && polls == 3 && !clocks && budgets[2] == -1, "explicit infinite wait retains its contract");
    reset(); clock_fail = 1; expect(vareq_poll_intr(&p, 1, 2000) < 0 && errno == EIO && !polls, "clock failure cannot silently extend wait");
    reset(); interrupts = 1; clock_fail = 2; expect(vareq_poll_intr(&p, 1, 2000) < 0 && errno == EIO, "retry clock failure propagated");
    reset(); interrupts = 1; expect(!vareq_poll_intr(&p, 1, 0) && polls == 1, "zero timeout stays nonblocking after EINTR");
    /* END skips reservation polling, so EINTR injection belongs to the ioctl. */
    for (unsigned transient = 0; transient < 2; transient++) {
        reset(); step = 100000000;
        if (transient) interrupts = 2; else again = 2;
        expect(!vareq_dmabuf_sync(7, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) && calls == 3 && successes == 1 && !polls,
            "transient END EINTR/EAGAIN recovers without a new fence wait");
        reset(); if (transient) interrupts = 100; else again = 100;
        expect(vareq_dmabuf_sync(7, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0 && errno == ETIMEDOUT && calls == 3 && !polls,
            "permanent END EINTR/EAGAIN stops at the original deadline");
    }
    reset(); ioctl_error = EACCES;
    expect(vareq_dmabuf_sync(7, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ) < 0 && errno == EACCES && polls == 1 && calls == 1,
        "START failure preserves errno after reservation wait");
    reset(); interrupts = 2;
    expect(!vareq_xioctl(7, VIDIOC_QUERYCAP, NULL) && calls == 3, "ordinary ioctl EINTR retry unchanged");
    vareq_set_ioctl_hook(NULL);
    printf("DMA deadline: %s (21 timeout/fence/retry cases)\n", failures ? "FAIL" : "PASS"); return failures != 0;
}
