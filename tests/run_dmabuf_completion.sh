#!/usr/bin/env bash
# Hardware acceptance groups. Run from the repository root.
set -uo pipefail
groups=("$@")
group_selected() {
  if [[ ${#groups[@]} == 0 ]]; then return 0; fi
  local group; for group in "${groups[@]}"; do [[ $group == "$1" ]] && return 0; done
  return 1
}
root=$PWD
if [[ ! -f tests/va_dmabuf_extended.c || ! -f tests/vpp_dmabuf_probe.c || ! -f tests/check_dmabuf_allocation.py || ! -f tests/va_dmabuf_rows.c || ! -f tests/check_dmabuf_rows.py || ! -f tests/va_dmabuf_complete.c || ! -f tests/check_dmabuf_complete.py ]]; then
  echo 'Run this script from the va-request repository root.'; exit 2
fi
build=${VAREQ_BUILD_DIR:-builddir}
if [[ "$build" != /* ]]; then build="$root/$build"; fi
render=${VAREQ_RENDER_NODE:-/dev/dri/renderD128}
ffmpeg=${FF:-/usr/bin/ffmpeg}
export LIBVA_DRIVER_NAME=v4l2request LIBVA_DRIVERS_PATH="$build" VAREQ_ADVERTISE_AV1=0 VAREQ_VPP_DMABUF=auto VAREQ_CAPTURE_DMABUF=0 VAREQ_PRIME_CANONICAL=0 VAREQ_VPP_DMABUF_SESSION=default VAREQ_SHARED_STORAGE=auto VAREQ_JPEG_DMABUF=auto VAREQ_CAPTURE_POOL_DMABUF=auto VAREQ_TEST_CAPTURE_POOL=1
mkdir -p verify/dmabuf-completion || exit 2
run=$(mktemp -d "$root/verify/dmabuf-completion/run.XXXXXX") || exit 2
pass=0 fail=0 skip=0 complete=0 stage=build
printf 'NAME\tSTATUS\tEXIT_CODE\n' > "$run/results.tsv"
report=${VAREQ_REPORT_FILE:-$root/verify/hardware-report.tar.gz}
finish() {
  local code=$?
  if [[ $code != 0 && $fail == 0 ]]; then fail=1; fi
  printf 'PASS=%s FAIL=%s SKIP=%s COMPLETE=%s EXIT=%s STAGE=%s\nRUN=%s\n' "$pass" "$fail" "$skip" "$complete" "$code" "$stage" "$run" | tee "$run/summary.txt"
  mkdir -p "$(dirname "$report")" || exit 1
  if tar --exclude='*.raw' --exclude='*.yuv' -czf "$report" -C "$root/verify/dmabuf-completion" "${run##*/}"; then printf 'REPORT=%s\n' "$report"; else exit 1; fi
}
trap finish EXIT
run_case() {
  local name=$1; shift
  stage=$name
  if [[ $name == matrix-* && -r /proc/meminfo ]]; then awk '/^(MemAvailable|MemFree|CmaTotal|CmaFree):/' /proc/meminfo > "$run/$name-memory-before.txt"; fi
  "$@" | tee "$run/$name.log"
  local codes=("${PIPESTATUS[@]}")
  if [[ $name == matrix-* && -r /proc/meminfo ]]; then awk '/^(MemAvailable|MemFree|CmaTotal|CmaFree):/' /proc/meminfo > "$run/$name-memory-after.txt"; fi
  if [[ ${codes[0]} == 0 && ${codes[1]} == 0 ]]; then pass=$((pass+1)); printf '%s\tPASS\t0\n' "$name" >> "$run/results.tsv"; return 0; fi
  if [[ ${codes[0]} == 77 && ${codes[1]} == 0 ]]; then skip=$((skip+1)); printf '%s\tSKIP\t77\n' "$name" >> "$run/results.tsv"; return 0; fi
  fail=$((fail+1)); printf '%s\tFAIL\t1\n' "$name" >> "$run/results.tsv"; return 1
}
run_case build meson compile -C "$build" || exit 1
unit_ok=1
run_case unit meson test -C "$build" --print-errorlogs || unit_ok=0
for name in testlog.txt testlog.json; do
  if [[ -f "$build/meson-logs/$name" ]]; then cp -- "$build/meson-logs/$name" "$run/unit-$name" || exit 1; fi
done
[[ $unit_ok == 1 ]] || exit 1
export VAREQ_ALLOC_TRACE="$run/allocation.tsv"
python3 tests/collect_dmabuf_environment.py "$run/environment" "$build" || exit 1
run_case extended-identity sha256sum "$build/v4l2request_drv_video.so" "$build/va_dmabuf_extended" "$build/va_dmabuf_rows" "$build/va_dmabuf_complete" || exit 1
run_extended() {
  local tag=$1 mode=$2 code=0
  env VAREQ_EXPBUF_EXPORT="$mode" VAREQ_ALLOC_TRACE="$run/extended-$tag-allocation.tsv" VAREQ_VPP_TRACE="$run/extended-$tag-vpp.tsv" VAREQ_DMABUF_TRACE="$run/extended-$tag-dma.tsv" \
    "$build/va_dmabuf_extended" "$render" "$run/extended-$tag.tsv" || code=1
  python3 tests/check_extended_dmabuf.py "$run/extended-$tag.tsv" "$run/extended-$tag-vpp.tsv" || code=1
  python3 tests/check_dmabuf_allocation.py "$run/extended-$tag-allocation.tsv" || code=1
  return "$code"
}
run_rows() {
  local tag=$1 mode=$2 code=0
  env VAREQ_EXPBUF_EXPORT="$mode" VAREQ_ALLOC_TRACE="$run/rows-$tag-allocation.tsv" VAREQ_VPP_TRACE="$run/rows-$tag-vpp.tsv" VAREQ_DMABUF_TRACE="$run/rows-$tag-dma.tsv" \
    "$build/va_dmabuf_rows" "$render" "$run/rows-$tag.tsv" || code=1
  python3 tests/check_dmabuf_rows.py "$run/rows-$tag.tsv" "$run/rows-$tag-vpp.tsv" || code=1
  return "$code"
}
run_complete() {
  local tag=$1 mode=$2 code=0 frame kind
  env VAREQ_EXPBUF_EXPORT="$mode" VAREQ_ALLOC_TRACE="$run/complete-$tag-allocation.tsv" VAREQ_VPP_TRACE="$run/complete-$tag-vpp.tsv" VAREQ_DMABUF_TRACE="$run/complete-$tag-dma.tsv" VAREQ_JPEG_TRACE="$run/complete-$tag-jpeg.tsv" \
    "$build/va_dmabuf_complete" "$render" "$run/complete-$tag.tsv" || code=1
  python3 tests/check_dmabuf_complete.py "$run/complete-$tag.tsv" "$run/complete-$tag-vpp.tsv" "$run/complete-$tag-jpeg.tsv" || code=1
  for ((frame=0; frame<12; frame++)); do
    for kind in ref test; do
      "$ffmpeg" -nostdin -v error -xerror -i "$run/complete-$tag.tsv.layout-$frame-$kind.jpg" -pix_fmt yuv420p -frames:v 1 -f framemd5 -y "$run/complete-$tag-$frame-$kind.md5" || code=1
    done
    python3 tests/compare_framemd5.py 1 "$run/complete-$tag-$frame-test.md5" "$run/complete-$tag-$frame-ref.md5" || code=1
  done
  return "$code"
}
run_direct_import() {
  local tag=$1 mode=$2
  env VAREQ_EXPBUF_EXPORT="$mode" VAREQ_CAPTURE_DMABUF=1 VAREQ_PRIME_CANONICAL=1 VAREQ_CAPTURE_TRACE="$run/capture-$tag.tsv" \
    python3 tests/dmabuf_decode_checks.py "$build" "$run/direct-imported-$tag" "$ffmpeg" "$render" && python3 tests/check_capture_trace.py "$run/capture-$tag.tsv"
}
run_feature_phase() {
  local name=$1 mode=$2 session=$3 previous= feature_run= code=0
  if [[ -f verify/features/summary.txt ]]; then previous=$(sed -n 's/^RUN=//p' verify/features/summary.txt); fi
  run_case "$name" env VAREQ_BUILD_DIR="$build" VAREQ_EXPBUF_EXPORT="$mode" VAREQ_VPP_DMABUF_SESSION="$session" bash tests/run_feature_checks.sh || code=1
  if [[ -f verify/features/summary.txt ]]; then
    feature_run=$(sed -n 's/^RUN=//p' verify/features/summary.txt)
    if [[ "$feature_run" == "$root/verify/features/run."* && -d "$feature_run" && "$feature_run" != "$previous" ]]; then
      cp -a -- "$feature_run" "$run/$name" || exit 1
      cp -- verify/features/summary.txt "$run/$name/summary.txt" || exit 1
    else fail=$((fail+1)); code=1; fi
  else fail=$((fail+1)); code=1; fi
  return "$code"
}
for mode in 1 0; do
  if [[ $mode == 1 ]]; then tag=default; else tag=gbm; fi
  if group_selected features && ! run_feature_phase "features-$tag" "$mode" ""; then
    run_feature_phase "features-$tag-reconfigure" "$mode" reconfigure || true
    run_feature_phase "features-$tag-isolated" "$mode" isolated || true
  fi
  if group_selected extended; then run_case "extended-$tag" run_extended "$tag" "$mode" || true; fi
  if group_selected rows; then run_case "rows-$tag" run_rows "$tag" "$mode" || true; fi
  if group_selected complete; then run_case "complete-$tag" run_complete "$tag" "$mode" || true; fi
  if group_selected matrix; then
  run_case "matrix-$tag" env BUILDDIR="$build" OUTDIR="$run/matrix-$tag" DRM_DEVICE="$render" VAREQ_EXPBUF_EXPORT="$mode" bash tests/run_full_matrix.sh ||
    run_case "matrix-$tag-coherent" env BUILDDIR="$build" OUTDIR="$run/matrix-$tag-coherent" DRM_DEVICE="$render" VAREQ_EXPBUF_EXPORT="$mode" VAREQ_SHARED_STORAGE=coherent bash tests/run_full_matrix.sh || true
  fi
  if group_selected imported; then
  run_case "imported-$tag" env VAREQ_EXPBUF_EXPORT="$mode" python3 tests/dmabuf_decode_checks.py "$build" "$run/imported-$tag" "$ffmpeg" "$render" ||
    run_case "imported-$tag-coherent" env VAREQ_EXPBUF_EXPORT="$mode" VAREQ_SHARED_STORAGE=coherent python3 tests/dmabuf_decode_checks.py "$build" "$run/imported-$tag-coherent" "$ffmpeg" "$render" || true
  fi
  if group_selected direct-capture; then
  run_case "direct-capture-$tag" run_direct_import "$tag" "$mode" ||
    VAREQ_SHARED_STORAGE=coherent run_case "direct-capture-$tag-coherent" run_direct_import "$tag-coherent" "$mode" || true
  fi
  if group_selected renderer; then
  run_case "renderer-$tag" env VAREQ_EXPBUF_EXPORT="$mode" "$build/va_dmabuf_renderer" "$render" "$run/renderer-$tag.tsv" ||
    run_case "renderer-$tag-coherent" env VAREQ_EXPBUF_EXPORT="$mode" VAREQ_SHARED_STORAGE=coherent "$build/va_dmabuf_renderer" "$render" "$run/renderer-$tag-coherent.tsv" || true
  fi
done
if group_selected transport; then
python3 tests/collect_dmabuf_kernel.py "$run/transport-kernel-before" || true
run_case transport-probe env VAREQ_ALLOC_TRACE="$run/allocation-probe.tsv" "$build/vpp_dmabuf_probe" "$render" "$run/transport-probe.tsv" ||
  run_case transport-probe-coherent env VAREQ_SHARED_STORAGE=coherent VAREQ_ALLOC_TRACE="$run/allocation-probe-coherent.tsv" "$build/vpp_dmabuf_probe" "$render" "$run/transport-probe-coherent.tsv" || true
python3 tests/collect_dmabuf_kernel.py "$run/transport-kernel-after" || true
fi
complete=1 stage=done
[[ $fail == 0 ]]
