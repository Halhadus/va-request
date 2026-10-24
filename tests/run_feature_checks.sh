#!/usr/bin/env bash
# Public libva checks and repeated JPEG encode; stderr remains on the terminal.
set -uo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd) || exit 2
cd "$root" || exit 2
build=${VAV4L2_BUILD_DIR:-builddir}
render=${VAV4L2_RENDER_NODE:-/dev/dri/renderD128}
if [[ "$build" != /* ]]; then build="$root/$build"; fi
export LIBVA_DRIVER_NAME=v4l2 LIBVA_DRIVERS_PATH="$build" VAV4L2_ADVERTISE_AV1=0
mkdir -p verify/features || exit 2
run=$(mktemp -d "$root/verify/features/run.XXXXXX") || exit 2
if ! python3 tests/feature_source.py "$build" "$run/source.json"; then
  printf 'PASS=0 FAIL=1 COMPLETE=0 EXIT=1 STAGE=source\nRUN=%s\n' "$run" | tee verify/features/summary.txt
  exit 1
fi
export VAV4L2_VPP_TRACE="$run/vpp-trace.tsv" VAV4L2_DMABUF_TRACE="$run/dmabuf-trace.tsv"
"$build/va_feature_client" "$render" "$run/results.tsv" | tee "$run/client.log"
statuses=("${PIPESTATUS[@]}")
client_exit=${statuses[0]}
complete=0
if [[ -f "$run/results.tsv" ]] && awk -F '\t' '$1 == "COMPLETE" && $2 == "1" {found=1} END {exit !found}' "$run/results.tsv"; then complete=1; fi
if python3 tests/check_dmabuf_trace.py "$run/results.tsv" "$run/vpp-trace.tsv"; then
  printf 'PASS\tdmabuf-direct-queue-evidence\n' >> "$run/results.tsv"
else
  printf 'FAIL\tdmabuf-direct-queue-evidence\n' >> "$run/results.tsv"
fi
jpeg_ok=1
if ! ffmpeg -nostdin -hide_banner -v warning -vaapi_device "$render" -f lavfi -i 'testsrc2=size=320x240:rate=5:duration=1.6' -vf 'format=nv12,hwupload' -c:v mjpeg_vaapi -frames:v 8 -y "$run/jpeg-%02d.jpg"; then jpeg_ok=0; fi
shopt -s nullglob
images=("$run"/jpeg-*.jpg)
if [[ ${#images[@]} != 8 ]]; then jpeg_ok=0; fi
if [[ $jpeg_ok == 1 ]] && ! ffmpeg -nostdin -v error -xerror -framerate 5 -start_number 1 -i "$run/jpeg-%02d.jpg" -frames:v 8 -f null -; then jpeg_ok=0; fi
if [[ $jpeg_ok == 1 ]]; then printf 'PASS\tjpeg-eight-frames\n' >> "$run/results.tsv"; else printf 'FAIL\tjpeg-eight-frames\n' >> "$run/results.tsv"; fi
native_ok=1
native_images=("$run"/results.tsv.native-*.jpg)
if [[ ${#native_images[@]} != 3 ]]; then native_ok=0; fi
for file in "${native_images[@]}"; do
  if ! ffmpeg -nostdin -v error -xerror -i "$file" -frames:v 1 -f null -; then native_ok=0; fi
done
if [[ $native_ok == 1 ]]; then printf 'PASS\tjpeg-native-software-decode\n' >> "$run/results.tsv"; else printf 'FAIL\tjpeg-native-software-decode\n' >> "$run/results.tsv"; fi
layouts_ok=1
for frame in {0..7}; do
  reference="$run/results.tsv.layout-$frame-ref.jpg"
  candidate="$run/results.tsv.layout-$frame-test.jpg"
  if [[ ! -s "$reference" || ! -s "$candidate" ]] ||
     ! ffmpeg -nostdin -v error -xerror -i "$reference" -pix_fmt yuv420p -frames:v 1 -f framemd5 -y "$run/layout-$frame-ref.md5" ||
     ! ffmpeg -nostdin -v error -xerror -i "$candidate" -pix_fmt yuv420p -frames:v 1 -f framemd5 -y "$run/layout-$frame-test.md5" ||
     ! python3 tests/compare_framemd5.py 1 "$run/layout-$frame-ref.md5" "$run/layout-$frame-test.md5"; then layouts_ok=0; fi
done
if [[ $layouts_ok == 1 ]]; then printf 'PASS\tjpeg-420-layout-pixels\n' >> "$run/results.tsv"; else printf 'FAIL\tjpeg-420-layout-pixels\n' >> "$run/results.tsv"; fi
pass=$(awk -F '\t' '$1 == "PASS" {n++} END {print n+0}' "$run/results.tsv")
fail=$(awk -F '\t' '$1 == "FAIL" {n++} END {print n+0}' "$run/results.tsv")
exit_code=0
if [[ $client_exit != 0 || ${statuses[1]} != 0 || $complete != 1 || $jpeg_ok != 1 || $native_ok != 1 || $layouts_ok != 1 || $fail != 0 ]]; then exit_code=1; fi
printf 'PASS=%s FAIL=%s COMPLETE=%s EXIT=%s\nRUN=%s\n' "$pass" "$fail" "$complete" "$exit_code" "$run" | tee verify/features/summary.txt
exit "$exit_code"
