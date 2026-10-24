#include <dlfcn.h>
#include <unistd.h>
#include "va_v4l2.h"
static void *fixture_resolve(void *handle, const char *name);
#define dlsym fixture_resolve
#include "hevc_slice_interpose.c"
#undef dlsym

static struct vav4l2_buffer fixture[512];
static struct vav4l2_buffer *submitted[256];
static unsigned next_id, submitted_count, creations, destructions, ended, context_destroyed;
static int failures, fail_create, fail_map, fail_unmap, fail_render, fail_end, wrong_vendor;
static VASliceParameterBufferHEVC original[2];
static VASliceParameterBufferVP8 original_vp8;
static VASliceParameterBufferMPEG2 original_mpeg2;
static int vp8_fixture;
static void expect(int ok, const char *tag)
{
    if (!ok) { fprintf(stderr, "FAIL %s mode=%s\n", tag, mode_name ? mode_name : "unset"); failures++; }
}
VAStatus vaCreateBuffer(VADisplay display, VAContextID context, VABufferType type,
                         unsigned size, unsigned elements, void *data, VABufferID *id)
{
    (void)display; (void)context;
    if (++creations == (unsigned)fail_create || next_id >= 511) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    *id = ++next_id;
    fixture[*id] = (struct vav4l2_buffer){.type = type, .size = size, .num_elements = elements,
        .allocated_elements = elements, .data = malloc((size_t)size * elements)};
    if (!fixture[*id].data) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    memcpy(fixture[*id].data, data, (size_t)size * elements);
    return VA_STATUS_SUCCESS;
}
VAStatus vaBufferInfo(VADisplay display, VAContextID context, VABufferID id, VABufferType *type, unsigned *size, unsigned *elements)
{
    (void)display; (void)context;
    if (id > next_id || !fixture[id].data) return VA_STATUS_ERROR_INVALID_BUFFER;
    *type = fixture[id].type; *size = fixture[id].size; *elements = fixture[id].num_elements;
    return VA_STATUS_SUCCESS;
}
VAStatus vaMapBuffer(VADisplay display, VABufferID id, void **data)
{
    (void)display;
    if (id == (unsigned)fail_map) return VA_STATUS_ERROR_OPERATION_FAILED;
    fixture[id].mmapped++; *data = fixture[id].data; return VA_STATUS_SUCCESS;
}
VAStatus vaUnmapBuffer(VADisplay display, VABufferID id)
{
    (void)display;
    fixture[id].mmapped--;
    return id == (unsigned)fail_unmap ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
}
VAStatus vaDestroyBuffer(VADisplay display, VABufferID id)
{
    (void)display;
    expect(id <= next_id && fixture[id].data && !fixture[id].mmapped, "destroy-only-owned-unmapped-live-buffer");
    if (id > next_id || !fixture[id].data) return VA_STATUS_ERROR_INVALID_BUFFER;
    free(fixture[id].data); fixture[id].data = NULL; destructions++; return VA_STATUS_SUCCESS;
}
const char *vaQueryVendorString(VADisplay display)
{
    (void)display; return wrong_vendor ? "another-driver" : "libva-v4l2 0.1.0";
}
static VAStatus fixture_render(VADisplay display, VAContextID context, VABufferID *ids, int count)
{
    (void)display; (void)context;
    if (fail_render && fixture[ids[0]].type == VASliceParameterBufferType) return VA_STATUS_ERROR_OPERATION_FAILED;
    for (int i = 0; i < count; i++) {
        expect(submitted_count < 256 && fixture[ids[i]].data, "render-retains-live-buffer");
        submitted[submitted_count++] = &fixture[ids[i]];
        if (mode && fixture[ids[i]].type == VASliceParameterBufferType) {
            if (vp8_fixture) {
                const uint8_t *params = fixture[ids[i]].data;
                unsigned size = vp8_fixture == 2 ? sizeof(original_mpeg2) : sizeof(original_vp8);
                const void *original_raw = vp8_fixture == 2 ? (const void *)&original_mpeg2 : (const void *)&original_vp8;
                for (unsigned j = 0; j < fixture[ids[i]].num_elements; j++)
                    expect(!memcmp(params + j * size + 12, (const uint8_t *)original_raw + 12,
                        size - 12), "preserve-vp8-partition-and-macroblock-metadata");
                continue;
            }
            VASliceParameterBufferHEVC *params = fixture[ids[i]].data;
            for (unsigned j = 0; j < fixture[ids[i]].num_elements; j++) {
                const VASliceParameterBufferHEVC *source = &original[params[j].slice_segment_address == 9];
                expect(!memcmp((uint8_t *)&params[j] + 12, (const uint8_t *)source + 12, sizeof(*source) - 12),
                       "preserve-ffmpeg-hevc-syntax-fields");
                expect(params[j].slice_data_offset >= 11, "nonzero-offset-is-actually-submitted");
            }
        }
    }
    return VA_STATUS_SUCCESS;
}
static VAStatus fixture_end(VADisplay display, VAContextID context)
{
    (void)display; (void)context; ended++;
    if (vp8_fixture) {
        uint8_t out[32], want[] = {0, 0, 1, 0xaa, 0xbb}; size_t bytes = 0;
        if (vp8_fixture == 2) want[3] = 1;
        expect(vav4l2_packetize_slices(vp8_fixture == 2 ? VAV4L2_CODEC_MPEG2 : VAV4L2_CODEC_VP8, submitted, submitted_count, out, sizeof(out), &bytes) == VA_STATUS_SUCCESS &&
            bytes == sizeof(want) && !memcmp(out, want, sizeof(want)), "vp8-adapter-preserves-raw-payload-even-when-it-resembles-annexb");
        expect(!destructions, "vp8-generated-buffers-stay-live-through-end");
        return VA_STATUS_SUCCESS;
    }
    uint8_t out[64], want[64], prefix[] = {0, 0, 0, 1}, body[][4] = {{0x26, 1, 0xa1, 0xb2}, {2, 1, 0xd1, 0xe2}};
    size_t size = 0, bytes = 0;
    for (int i = 0; i < 2; i++) {
        unsigned len = mode == 4 || (!mode && i) ? 3 : 4;
        memcpy(want + size, prefix + 4 - len, len); size += len;
        memcpy(want + size, body[i], 4); size += 4;
    }
    expect(vav4l2_packetize_slices(VAV4L2_CODEC_HEVC, submitted, submitted_count, out, sizeof(out), &bytes) == VA_STATUS_SUCCESS &&
           bytes == size && !memcmp(out, want, size), "adapter-submission-reassembles-original-nals-exactly");
    expect(!destructions, "generated-buffers-stay-alive-through-real-EndPicture");
    return fail_end ? VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
}
static VAStatus fixture_destroy(VADisplay display, VAContextID context)
{
    (void)display; (void)context; context_destroyed++; return VA_STATUS_SUCCESS;
}
static void *fixture_resolve(void *handle, const char *name)
{
    (void)handle;
    if (!strcmp(name, "vaRenderPicture")) return fixture_render;
    if (!strcmp(name, "vaEndPicture")) return fixture_end;
    if (!strcmp(name, "vaDestroyContext")) return fixture_destroy;
    return NULL;
}
static void scenario(const char *name, int fault)
{
    vp8_fixture = 0;
    char path[] = "hevc-adapter.XXXXXX"; int fd = mkstemp(path);
    expect(fd >= 0, "trace-fixture"); if (fd < 0) return; close(fd);
    setenv("VAV4L2_HEVC_PACKET_MODE", name, 1); setenv("VAV4L2_HEVC_PACKET_LOG", path, 1);
    initialize_once = (pthread_once_t)PTHREAD_ONCE_INIT; mode = -1; trace = NULL; pending = NULL;
    next_id = submitted_count = creations = destructions = ended = context_destroyed = picture_number = 0;
    fail_create = fail_map = fail_unmap = fail_render = fail_end = wrong_vendor = 0;
    memset(fixture, 0, sizeof(fixture)); memset(original, 0, sizeof(original));
    uint8_t payload[] = {0xee, 0x26, 1, 0xa1, 0xb2, 0xff, 0, 0, 1, 2, 1, 0xd1, 0xe2, 0xff};
    for (int i = 0; i < 2; i++) {
        original[i].slice_data_offset = i ? 6 : 1; original[i].slice_data_size = i ? 7 : 4;
        original[i].slice_segment_address = i ? 9 : 0; original[i].slice_data_byte_offset = 3 + i;
        original[i].slice_qp_delta = -2; original[i].LongSliceFlags.fields.LastSliceOfPic = i;
    }
    VADisplay display = (void *)1; VABufferID ids[2];
    vaCreateBuffer(display, 2, VASliceParameterBufferType, sizeof(original[0]), 2, original, &ids[0]);
    vaCreateBuffer(display, 2, VASliceDataBufferType, sizeof(payload), 1, payload, &ids[1]);
    if (fault == 1) fail_create = creations + 2;
    if (fault == 2) fail_map = ids[1];
    if (fault == 3) fail_unmap = ids[1];
    if (fault == 4) fail_render = 1;
    if (fault == 5) fail_end = 1;
    if (fault == 6) wrong_vendor = 1;
    if (fault == 7) ((VASliceParameterBufferHEVC *)fixture[ids[0]].data)->slice_data_size = UINT32_MAX;
    VAStatus rendered = vaRenderPicture(display, 2, ids, 2);
    expect(rendered == VA_STATUS_SUCCESS ? fault == 0 || fault == 5 || fault == 6 : fault != 0, "adapter-render-status-propagation");
    expect(!fixture[ids[0]].mmapped && !fixture[ids[1]].mmapped, "original-buffers-unmapped-after-render");
    if (fault != 7) expect(!memcmp(fixture[ids[0]].data, original, sizeof(original)), "original-parameters-not-modified");
    VAStatus status = vaEndPicture(display, 2);
    expect((status == VA_STATUS_SUCCESS) == (fault == 0), "adapter-end-status-propagation");
    expect(ended == (unsigned)(fault == 0 || fault == 5), "failed-adaptation-cannot-submit-partial-picture");
    expect(!pending && fixture[ids[0]].data && fixture[ids[1]].data, "cleanup-keeps-client-owned-original-buffers");
    for (unsigned i = 3; i <= next_id; i++) expect(!fixture[i].data, "generated-buffer-cleanup");
    if (trace && fault == 0) {
        char header[256], row[256] = {0}; FILE *saved = fopen(path, "r");
        expect(saved && fgets(header, sizeof(header), saved) && fgets(row, sizeof(row), saved), "trace-records-completed-picture");
        char expected_row[128]; snprintf(expected_row, sizeof(expected_row), "%s\t0\t2\t%d\t0\t", name, mode <= 1 ? 2 : 6);
        expect(!strncmp(row, expected_row, strlen(expected_row)), "trace-mode-picture-and-transformation-count");
        if (saved) fclose(saved);
    }
    vaDestroyBuffer(display, ids[0]); vaDestroyBuffer(display, ids[1]);
    if (trace) fclose(trace);
    unlink(path);
}
static void aborted_context(void)
{
    scenario("fragments", 0);
    /* A client may abandon a partially rendered picture without EndPicture. */
    char path[] = "hevc-abort.XXXXXX"; int fd = mkstemp(path); if (fd < 0) { failures++; return; } close(fd);
    setenv("VAV4L2_HEVC_PACKET_MODE", "offset", 1); setenv("VAV4L2_HEVC_PACKET_LOG", path, 1);
    initialize_once = (pthread_once_t)PTHREAD_ONCE_INIT; mode = -1; trace = NULL;
    uint8_t payload[] = {0x26, 1, 0xaa}; VASliceParameterBufferHEVC sp = {.slice_data_size = sizeof(payload)};
    original[0] = sp;
    VABufferID ids[2]; vaCreateBuffer((void *)1, 2, VASliceParameterBufferType, sizeof(sp), 1, &sp, &ids[0]);
    vaCreateBuffer((void *)1, 2, VASliceDataBufferType, sizeof(payload), 1, payload, &ids[1]);
    expect(vaRenderPicture((void *)1, 2, ids, 2) == VA_STATUS_SUCCESS && pending, "aborted-context-has-generated-buffers");
    expect(vaDestroyContext((void *)1, 2) == VA_STATUS_SUCCESS && !pending && context_destroyed == 1, "context-destruction-cleans-abandoned-adapter-buffers");
    vaDestroyBuffer((void *)1, ids[0]); vaDestroyBuffer((void *)1, ids[1]);
    if (trace) fclose(trace);
    unlink(path);
}
static void raw_scenario(const char *name, int mpeg2)
{
    char path[] = "vp8-adapter.XXXXXX"; int fd = mkstemp(path);
    expect(fd >= 0, "vp8-trace-fixture"); if (fd < 0) return; close(fd);
    setenv("VAV4L2_HEVC_PACKET_MODE", name, 1); setenv("VAV4L2_HEVC_PACKET_LOG", path, 1);
    initialize_once = (pthread_once_t)PTHREAD_ONCE_INIT; mode = -1; trace = NULL; pending = NULL; vp8_fixture = mpeg2 ? 2 : 1;
    next_id = submitted_count = creations = destructions = ended = context_destroyed = picture_number = 0;
    fail_create = fail_map = fail_unmap = fail_render = fail_end = wrong_vendor = 0;
    memset(fixture, 0, sizeof(fixture));
    original_vp8 = (VASliceParameterBufferVP8){.slice_data_offset = 1, .slice_data_size = 5, .macroblock_offset = 17,
        .num_of_partitions = 2, .partition_size = {2, 3}};
    uint8_t payload[] = {0xee, 0, 0, 1, 0xaa, 0xbb, 0xff}; VABufferID ids[2];
    original_mpeg2 = (VASliceParameterBufferMPEG2){.slice_data_offset = 1, .slice_data_size = 5, .macroblock_offset = 37,
        .slice_vertical_position = 2, .quantiser_scale_code = 9};
    if (mpeg2) payload[4] = 1;
    vaCreateBuffer((void *)1, 2, VASliceParameterBufferType, mpeg2 ? sizeof(original_mpeg2) : sizeof(original_vp8), 1,
        mpeg2 ? (void *)&original_mpeg2 : (void *)&original_vp8, &ids[0]);
    vaCreateBuffer((void *)1, 2, VASliceDataBufferType, sizeof(payload), 1, payload, &ids[1]);
    expect(vaRenderPicture((void *)1, 2, ids, 2) == VA_STATUS_SUCCESS && vaEndPicture((void *)1, 2) == VA_STATUS_SUCCESS && ended == 1,
        "vp8-real-adapter-to-packetizer-submission");
    expect(!pending && !memcmp(fixture[ids[0]].data, mpeg2 ? (void *)&original_mpeg2 : (void *)&original_vp8, mpeg2 ? sizeof(original_mpeg2) : sizeof(original_vp8)), "vp8-original-parameters-and-generated-cleanup");
    for (unsigned i = 3; i <= next_id; i++) expect(!fixture[i].data, "vp8-no-leaked-generated-buffers");
    vaDestroyBuffer((void *)1, ids[0]); vaDestroyBuffer((void *)1, ids[1]);
    if (trace) fclose(trace);
    unlink(path);
}

int main(void)
{
    const char *names[] = {"whole", "offset", "fragments", "array", "split-prefix"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) scenario(names[i], 0);
    for (int fault = 1; fault <= 7; fault++) scenario("fragments", fault);
    aborted_context();
    for (unsigned i = 0; i < 4; i++) { raw_scenario(names[i], 0); raw_scenario(names[i], 1); }
    return failures != 0;
}
