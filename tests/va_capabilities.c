/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_str.h>

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror(argv[1]); return 77; }
    VADisplay display = vaGetDisplayDRM(fd);
    int major, minor;
    if (!display || vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) { close(fd); return 1; }
    int capacity = vaMaxNumProfiles(display), count = 0, first = 1, result = 1;
    VAProfile *profiles = capacity > 0 ? calloc(capacity, sizeof(*profiles)) : NULL;
    VAEntrypoint *entries = calloc(vaMaxNumEntrypoints(display), sizeof(*entries));
    if (!profiles || !entries || vaQueryConfigProfiles(display, profiles, &count) != VA_STATUS_SUCCESS) goto done;
    printf("{\"profiles\":[");
    for (int i = 0; i < count; i++) {
        int n = 0;
        if (vaQueryConfigEntrypoints(display, profiles[i], entries, &n) != VA_STATUS_SUCCESS) goto done;
        for (int j = 0; j < n; j++) {
            printf("%s{\"profile\":\"%s\",\"entrypoint\":\"%s\"}", first ? "" : ",", vaProfileStr(profiles[i]), vaEntrypointStr(entries[j]));
            first = 0;
        }
    }
    printf("]}\n"); result = 0;
done:
    free(profiles); free(entries);
    if (vaTerminate(display) != VA_STATUS_SUCCESS) result = 1;
    close(fd); return result;
}
