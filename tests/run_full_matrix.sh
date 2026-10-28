#!/bin/bash
# Full VA-API → V4L2 fixture matrix. Requires the complete codec/VPP interface set.
# Success = hwdownload framemd5 vs software, except MPEG-2 which is
# compared to GStreamer v4l2slmpeg2dec (same kernel IDCT; ffmpeg SW differs).
set -uo pipefail
FF=${FF:-/usr/bin/ffmpeg}
export LIBVA_DRIVER_NAME=v4l2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLIP=${CLIPDIR:-$ROOT/verify/clips}
OUT=${OUTDIR:-$ROOT/verify/matrix}
mkdir -p "$CLIP" "$OUT" || exit 1
: > "$OUT/vpp-trace.tsv" || exit 1
fail=0
pass=0
skip=0
complete=0
stage=start
printf 'RESULT\tCASE\n' > "$OUT/results.tsv" || exit 1
record_result() { printf '%s\t%s\n' "$1" "$2" >> "$OUT/results.tsv"; }
write_summary() {
  local status=$?
  if [ "$status" -ne 0 ] && [ "$fail" -eq 0 ]; then fail=1; fi
  echo "PASS=$pass FAIL=$fail SKIP=$skip COMPLETE=$complete EXIT=$status STAGE=$stage" | tee "$OUT/summary.txt"
}
trap write_summary EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
BUILD=${BUILDDIR:-$ROOT/builddir}
export LIBVA_DRIVERS_PATH="${LIBVA_DRIVERS_PATH:-$BUILD}"
DRM=${DRM_DEVICE:-/dev/dri/renderD128}
cd "$ROOT" || exit 1
# Abort command failures where continuing could compare stale output.
must() { "$@" || { echo "COMMAND_FAILED: $*"; record_result FAIL "$stage:command"; exit 1; }; }
compare_md5() { python3 "$ROOT/tests/compare_framemd5.py" "$@"; }
if [ "${VAV4L2_ADVERTISE_AV1:-0}" = 1 ]; then av1=1; else av1=0; fi
if [ "${VAV4L2_TEST_AV1_TILES:-0}" = 1 ] && [ "$av1" -ne 1 ]; then
  echo "VAV4L2_TEST_AV1_TILES=1 requires VAV4L2_ADVERTISE_AV1=1"; exit 1
fi

enc() {
  local dest="$1"; shift
  if [ -s "$dest" ]; then
    echo "have $dest"
    return 0
  fi
  echo "ENC $dest"
  if ! "$FF" -nostdin -hide_banner -y "$@" "$dest"; then
    rm -f "$dest"
    echo "ENC_FAIL $dest"; exit 1
  fi
}

pair_sw() {
  local src="$1" n="$2" tag="$3" fmt="${4:-nv12}" pix="${5:-yuv420p}" pool_required="${6:-0}"
  stage="decode:$tag"
  echo "== SW $tag $src n=$n =="
  rm -f -- "$OUT/${tag}-hw.md5" "$OUT/${tag}-sw.md5" || { record_result FAIL "$tag:output-clear"; fail=$((fail+1)); return; }
  : > "$OUT/${tag}-decode.tsv" || { record_result FAIL "$tag:trace-create"; fail=$((fail+1)); return; }
  VAV4L2_DECODE_TRACE="$OUT/${tag}-decode.tsv" "$FF" -nostdin -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device "$DRM" -i "$src" \
    -vf "hwdownload,format=$fmt" -pix_fmt "$pix" -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-hw.md5"
  local he=$?
  "$FF" -nostdin -hide_banner -i "$src" -pix_fmt "$pix" -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-sw.md5"
  local se=$?
  printf 'HARDWARE_EXIT=%s\nSOFTWARE_EXIT=%s\n' "$he" "$se" > "$OUT/${tag}-exit.txt"
  python3 "$ROOT/tests/decode_evidence.py" "$tag" "$n" "$he" "$se" "$OUT/${tag}-decode.tsv" "$OUT/${tag}-hw.md5" "$OUT/${tag}-sw.md5" "$OUT/${tag}-decode.json" || { record_result FAIL "$tag:evidence"; fail=$((fail+1)); return; }
  if [ "$he" -ne 0 ]; then
    echo "HW_EXIT $he $tag"; record_result FAIL "$tag:hw-exit-$he"; fail=$((fail+1)); return
  fi
  if [ "$se" -ne 0 ] || ! compare_md5 "$n" "$OUT/${tag}-hw.md5" "$OUT/${tag}-sw.md5"; then
    echo "MD5_DIFF $tag"
    record_result FAIL "$tag"
    diff -u "$OUT/${tag}-sw.md5" "$OUT/${tag}-hw.md5" | head -16
    fail=$((fail+1))
  else
    if [ "$pool_required" -eq 1 ] && ! python3 "$ROOT/tests/check_capture_pool_trace.py" "$OUT/${tag}-decode.tsv" "$n"; then record_result FAIL "$tag:pool-evidence"; fail=$((fail+1)); return; fi
    echo "MD5_MATCH $tag"
    record_result PASS "$tag"
    pass=$((pass+1))
  fi
}

pair_gst_mpeg2() {
  local src="$1" n="$2" tag="$3"
  stage="decode:$tag"
  echo "== GST $tag $src n=$n =="
  "$FF" -nostdin -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device "$DRM" -i "$src" \
    -vf "hwdownload,format=nv12" -pix_fmt yuv420p -frames:v "$n" \
    -f framemd5 -y "$OUT/${tag}-hw.md5"
  local he=$?
  if [ "$he" -ne 0 ]; then
    echo "HW_EXIT $he $tag"; record_result FAIL "$tag:hw-exit-$he"; fail=$((fail+1)); return
  fi
  local yuv="$OUT/${tag}-gst.yuv" reference="$src" dim w h coded_h
  dim=$(awk '/^#dimensions 0:/{print $3; exit}' "$OUT/${tag}-hw.md5")
  w=${dim%x*}; h=${dim#*x}
  if ! [[ "$w" =~ ^[0-9]+$ && "$h" =~ ^[0-9]+$ ]] || [ "$w" -eq 0 ] || [ "$h" -eq 0 ]; then
    echo "GST_FAIL $tag invalid dimensions"; record_result FAIL "$tag:dimensions"; fail=$((fail+1)); return
  fi
  coded_h=$(( (h + 15) / 16 * 16 ))
  if [ "$coded_h" -ne "$h" ]; then
    # Keep Hantro/GStreamer on the same coded macroblock grid. Decode the
    # reference at its aligned height, then crop; never rescale to 1080.
    reference="$OUT/${tag}-reference.m2v"
    if ! "$FF" -nostdin -hide_banner -loglevel error -i "$src" -map 0:v:0 -c:v copy \
         -f mpeg2video -y "$OUT/${tag}-elementary.m2v" ||
       ! python3 tests/mpeg2_reference.py "$OUT/${tag}-elementary.m2v" "$reference" "$w" "$h" ||
       ! "$FF" -nostdin -hide_banner -loglevel error -i "$src" -pix_fmt yuv420p -frames:v "$n" \
         -f framemd5 -y "$OUT/${tag}-original-sw.md5" ||
       ! "$FF" -nostdin -hide_banner -loglevel error -i "$reference" -vf "crop=$w:$h:0:0" \
         -pix_fmt yuv420p -frames:v "$n" -f framemd5 -y "$OUT/${tag}-reference-sw.md5" ||
       ! compare_md5 "$n" "$OUT/${tag}-original-sw.md5" "$OUT/${tag}-reference-sw.md5"; then
      echo "GST_FAIL $tag reference changed visible pixels"; record_result FAIL "$tag:reference"; fail=$((fail+1)); return
    fi
    echo "MPEG2_REFERENCE_EQUIVALENT $tag"
  fi
  gst-launch-1.0 -q filesrc location="$reference" ! parsebin ! v4l2slmpeg2dec \
    ! videoconvert ! video/x-raw,format=I420 ! filesink location="$yuv" \
    || { echo "GST_FAIL $tag"; record_result FAIL "$tag:gst"; fail=$((fail+1)); return; }
  "$FF" -nostdin -hide_banner -f rawvideo -s "${w}x${coded_h}" -pix_fmt yuv420p -i "$yuv" \
    -vf "crop=$w:$h:0:0" -frames:v "$n" -f framemd5 -y "$OUT/${tag}-gst.md5"
  # Compare ordered payload hashes, with an exact frame count.
  local ge=$?
  if [ "$ge" -eq 0 ] && compare_md5 "$n" "$OUT/${tag}-hw.md5" "$OUT/${tag}-gst.md5"
  then
    echo "GST_MATCH $tag"
    record_result PASS "$tag"
    pass=$((pass+1))
  else
    echo "GST_DIFF $tag"
    record_result FAIL "$tag"
    diff -u "$OUT/${tag}-gst.md5" "$OUT/${tag}-hw.md5" | head -16
    fail=$((fail+1))
  fi
}

stage=unit
echo "===== unit ====="
unit_status=0
meson test -C "$BUILD" --print-errorlogs || unit_status=$?
printf '%s\n' "$unit_status" > "$OUT/unit-exit.txt" || exit 1
for log in testlog.txt testlog.json; do
  if [ -f "$BUILD/meson-logs/$log" ]; then cp -- "$BUILD/meson-logs/$log" "$OUT/unit-$log" || exit 1; fi
done
if [ "$unit_status" -eq 0 ] && "$BUILD/test_video_probe" --live | tee "$OUT/live-probe.log"; then
  echo "UNIT_PASS"; record_result PASS unit; pass=$((pass+1))
else
  echo "UNIT_FAIL"; record_result FAIL unit; exit 1
fi

H264_NODE=$(awk '/^live H\.264[[:space:]]+->/{print $4; exit}' "$OUT/live-probe.log")
stage=gbm-probe
echo "===== gbm-probe: platform gate for GBM display surfaces ====="
cc -O2 -Wall -o "$OUT/gbm_probe" tests/gbm_probe.c \
  -lgbm -lEGL -lGLESv2 || exit 1
if [ -x "$OUT/gbm_probe" ]; then
  if "$OUT/gbm_probe" 1280 720 &&
     "$OUT/gbm_probe" 1920 1080; then
    echo "PASS gbm-probe"; record_result PASS gbm-probe; pass=$((pass+1))
  else
    echo "FAIL gbm-probe"; record_result FAIL gbm-probe; fail=$((fail+1))
  fi
fi

stage=vainfo
echo "===== vainfo ====="
if ! vainfo --display drm --device "$DRM" | tee "$OUT/vainfo.log"; then
  echo "VAINFO_FAILED"; record_result FAIL vainfo; exit 1
fi
if ! grep -q "libva-v4l2 0.1.0" "$OUT/vainfo.log"; then
  echo "vainfo did not load driver"; record_result FAIL vainfo-driver; fail=$((fail+1))
else
  # AV1 remains opt-in until multi-tile hardware validation is complete.
  for p in VAProfileH264ConstrainedBaseline VAProfileH264Main VAProfileH264High \
           VAProfileH264High10 VAProfileH264High422 \
           VAProfileHEVCMain VAProfileHEVCMain10 \
           VAProfileVP8Version0_3 VAProfileMPEG2Simple VAProfileMPEG2Main \
           VAProfileJPEGBaseline VAProfileNone; do
    if grep -q "$p" "$OUT/vainfo.log"; then
      echo "VAINFO_OK $p"
    else
      echo "VAINFO_MISS $p"; record_result FAIL "vainfo:$p"; fail=$((fail+1))
    fi
  done
  if [ "$av1" -eq 1 ]; then
    grep -q VAProfileAV1Profile0 "$OUT/vainfo.log" || { echo "VAINFO_MISS AV1"; record_result FAIL vainfo:AV1; fail=$((fail+1)); }
  elif grep -q VAProfileAV1Profile0 "$OUT/vainfo.log"; then
    echo "VAINFO_UNEXPECTED AV1"; record_result FAIL vainfo:unexpected-AV1; fail=$((fail+1))
  fi
  record_result PASS vainfo
  pass=$((pass+1))
fi

stage=generate-clips
echo "===== generate clips ====="
# H.264
enc "$CLIP/h264_high_b.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 2
enc "$CLIP/h264_allp.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 0
enc "$CLIP/h264_slices.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 0 -slices 4
enc "$CLIP/h264_baseline.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v baseline -g 30 -bf 0
enc "$CLIP/h264_mbaff.mp4" -f lavfi -i testsrc2=size=320x240:rate=8:duration=1 \
  -pix_fmt yuv420p -c:v libx264 -preset medium -flags +ilme+ildct -x264-params "interlaced=1:tff=1" -frames:v 8
enc "$CLIP/h264_main.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v main -g 30 -bf 2
enc "$CLIP/h264_4k.mp4" -f lavfi -i testsrc=size=3840x2160:rate=30:duration=1 \
  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 30 -bf 0 -frames:v 8
	enc "$CLIP/h264_qcif.mp4" -f lavfi -i testsrc=size=320x240:rate=25:duration=2 \
	  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -profile:v high -g 25 -bf 0
	enc "$CLIP/h264_10.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv420p10le -c:v libx264 -preset ultrafast -profile:v high10 -frames:v 8
	enc "$CLIP/h264_422_idr10.h264" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv422p10le -c:v libx264 -preset ultrafast -g 1 -keyint_min 1 -sc_threshold 0 -frames:v 8 -f h264
	enc "$CLIP/h264_422_idr8.h264" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv422p -c:v libx264 -preset ultrafast -g 1 -keyint_min 1 -sc_threshold 0 -frames:v 8 -f h264
	enc "$CLIP/h264_idr_nv12.h264" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
	  -pix_fmt yuv420p -c:v libx264 -preset ultrafast -g 1 -keyint_min 1 -sc_threshold 0 -frames:v 8 -f h264

# HEVC
x265_pool=$(python3 "$ROOT/tests/encoder_pool.py" --auto) || exit 1
enc "$CLIP/hevc_main.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx265 -preset ultrafast -x265-params "pools=$x265_pool:log-level=error" -g 30
enc "$CLIP/hevc_wpp.mp4" -f lavfi -i testsrc=size=1920x1080:rate=30:duration=4 \
  -pix_fmt yuv420p -c:v libx265 -preset ultrafast -x265-params "pools=$x265_pool:log-level=error:wpp=1" -g 30
enc "$CLIP/hevc_4k.mp4" -f lavfi -i testsrc=size=3840x2160:rate=30:duration=1 \
  -pix_fmt yuv420p -c:v libx265 -preset ultrafast -x265-params "pools=$x265_pool:log-level=error" -g 30 -frames:v 8
enc "$CLIP/hevc10.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=1 \
  -pix_fmt yuv420p10le -c:v libx265 -preset ultrafast -x265-params "pools=$x265_pool:log-level=error" -frames:v 8

if [ "$av1" -eq 1 ]; then
enc "$CLIP/av1_aom_1tile.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libaom-av1 -tile-columns 0 -tile-rows 0 -cpu-used 8 -crf 32 -b:v 0 -g 30 -usage realtime
enc "$CLIP/av1_svt_1tile.mp4" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libsvtav1 -svtav1-params tile-columns=0:tile-rows=0 -preset 10 -g 32 -b:v 1M
enc "$CLIP/av1_4k_1tile.mp4" -f lavfi -i testsrc=size=3840x2160:rate=30:duration=1 \
  -pix_fmt yuv420p -c:v libaom-av1 -tile-columns 0 -tile-rows 0 -cpu-used 8 -crf 36 -b:v 0 -g 8 -usage realtime -frames:v 8
enc "$CLIP/av1_default_1tile.mp4" -f lavfi -i testsrc=size=640x360:rate=30:duration=1 \
  -pix_fmt yuv420p -c:v libaom-av1 -tile-columns 0 -tile-rows 0 -cpu-used 8 -crf 36 -b:v 0 -g 30

fi

# VP8
enc "$CLIP/vp8_480.webm" -f lavfi -i testsrc=size=640x480:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v libvpx -b:v 500k -g 12 -auto-alt-ref 0
enc "$CLIP/vp8_720.webm" -f lavfi -i testsrc=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v libvpx -b:v 1M -g 15 -auto-alt-ref 1

# MPEG-2
enc "$CLIP/mpeg2_ip.mpg" -f lavfi -i testsrc=size=720x480:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v mpeg2video -q:v 5 -g 12 -bf 0
enc "$CLIP/mpeg2_b.mpg" -f lavfi -i testsrc=size=720x480:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v mpeg2video -q:v 5 -g 12 -bf 2
enc "$CLIP/mpeg2_1080.mpg" -f lavfi -i testsrc=size=1920x1080:rate=25:duration=2 \
  -pix_fmt yuv420p -c:v mpeg2video -q:v 8 -g 12 -bf 2

export VAV4L2_STORAGE_TRACE="$OUT/storage-trace.tsv" VAV4L2_DECODE_TRACE="$OUT/setup-decode.tsv"
: > "$VAV4L2_STORAGE_TRACE"
: > "$VAV4L2_DECODE_TRACE"
stage=decode
echo "===== decode matrix ====="
pair_sw "$CLIP/h264_high_b.mp4" 120 h264-high-b
pair_sw "$CLIP/h264_allp.mp4" 120 h264-allp
pair_sw "$CLIP/h264_slices.mp4" 120 h264-slices
pair_sw "$CLIP/h264_baseline.mp4" 50 h264-baseline
pair_sw "$CLIP/h264_main.mp4" 50 h264-main
pair_sw "$CLIP/h264_4k.mp4" 8 h264-4k
	pair_sw "$CLIP/h264_qcif.mp4" 40 h264-qcif

pair_sw "$CLIP/h264_10.mp4" 8 h26410 p010le p010le

	# H.264 High 4:2:2 — stock ffmpeg cannot reach the bridge for 4:2:2
	# (h264 get_pixel_format only offers VAAPI for 4:2:0, and the vaapi
	# profile map has no H264_HIGH_422 entry), so drive the VA-API path
	# with tests/va_h264422_client.c and the kernel path with GST.
	cc -O2 -Wall -o "$OUT/va422" tests/va_h264422_client.c \
	  $(pkg-config --cflags --libs libva libva-drm) || exit 1
	for depth in 10:Y210:y210le:NV20 8:YUY2:yuyv422:NV16; do
	  d="${depth%%:*}"; rest="${depth#*:}"
	  img="${rest%%:*}"; rest="${rest#*:}"
	  pfmt="${rest%%:*}"; cap="${rest##*:}"
	  clip="$CLIP/h264_422_idr${d}.h264"
	  LIBVA_DRIVER_NAME=v4l2 "$OUT/va422" "$DRM" "$clip" "$d" \
	    >"$OUT/h264422${d}-va.raw"
	  v4=$?
	  if [ "$v4" -ne 0 ]; then echo "VA422_EXIT $v4 h264422-$d"; record_result FAIL "h264422-$d-va:exit-$v4"; fail=$((fail+1)); continue; fi
	  must "$FF" -nostdin -hide_banner -loglevel error -f rawvideo -framerate 30 -s 1280x720 \
	    -pix_fmt "$pfmt" -i "$OUT/h264422${d}-va.raw" -frames:v 8 \
	    -f framemd5 -y "$OUT/h264422${d}-va.md5"
	  must "$FF" -nostdin -hide_banner -loglevel error -i "$clip" -pix_fmt "$pfmt" -frames:v 8 \
	    -f framemd5 -y "$OUT/h264422${d}-sw.md5"
	  if compare_md5 8 "$OUT/h264422${d}-va.md5" "$OUT/h264422${d}-sw.md5"; then
	    echo "MD5_MATCH h264422-$d-va"; record_result PASS "h264422-$d-va"; pass=$((pass+1))
	  else echo "MD5_DIFF h264422-$d-va"; record_result FAIL "h264422-$d-va"; fail=$((fail+1)); fi
	done
stage=h264422-vpp
LIBVA_DRIVER_NAME=v4l2 "$OUT/va422" "$DRM" "$CLIP/h264_422_idr8.h264" 8 vpp >"$OUT/h2644228-vpp.raw"
v4=$?
if [ "$v4" -ne 0 ]; then
  record_result FAIL "h264422-8-vpp:exit-$v4"; fail=$((fail+1))
else
  must "$FF" -nostdin -hide_banner -loglevel error -f rawvideo -framerate 30 -s 1280x720 \
    -pix_fmt yuyv422 -i "$OUT/h2644228-vpp.raw" -frames:v 8 -f framemd5 -y "$OUT/h2644228-vpp.md5"
  if compare_md5 8 "$OUT/h2644228-vpp.md5" "$OUT/h2644228-sw.md5"; then
    record_result PASS h264422-8-vpp; pass=$((pass+1))
  else
    record_result FAIL h264422-8-vpp; fail=$((fail+1))
  fi
fi
# --- va-export: exported dma-buf content == vaGetImage content ----------
stage=h264422-planar
must "$FF" -nostdin -hide_banner -loglevel error -i "$CLIP/h264_422_idr8.h264" -pix_fmt yuv422p -frames:v 8 -f framemd5 -y "$OUT/h2644228-planar-sw.md5"
for layout in 422H YV16 vpp-422H vpp-YV16; do
  LIBVA_DRIVER_NAME=v4l2 "$OUT/va422" "$DRM" "$CLIP/h264_422_idr8.h264" 8 "$layout" >"$OUT/h2644228-$layout.raw"
  v4=$?
  if [ "$v4" -ne 0 ]; then
    record_result FAIL "h264422-8-$layout:exit-$v4"; fail=$((fail+1)); continue
  fi
  must "$FF" -nostdin -hide_banner -loglevel error -f rawvideo -framerate 30 -s 1280x720 -pix_fmt yuv422p -i "$OUT/h2644228-$layout.raw" -frames:v 8 -f framemd5 -y "$OUT/h2644228-$layout.md5"
  if compare_md5 8 "$OUT/h2644228-$layout.md5" "$OUT/h2644228-planar-sw.md5"; then
    record_result PASS "h264422-8-$layout"; pass=$((pass+1))
  else
    record_result FAIL "h264422-8-$layout"; fail=$((fail+1))
  fi
done
must meson compile -C "$BUILD" va_export_client
if [ -x "$BUILD/va_export_client" ]; then
  LIBVA_DRIVER_NAME=v4l2 "$BUILD/va_export_client" \
    "$DRM" "$CLIP/h264_idr_nv12.h264" | tee "$OUT/va_export.log"
  ve=$?
  if [ "$ve" -eq 0 ] && grep -q "EXPORT_EXACT 7" "$OUT/va_export.log" \
     && grep -q "LAZY_EXACT 7" "$OUT/va_export.log" \
     && grep -q '^RETAINED_LIFETIME_EXACT$' "$OUT/va_export.log" \
     && awk '/^RETAINED_EXACT / { if (NF != 2 || $2 != n) exit 1; n++ } END { if (n != 8) exit 1 }' "$OUT/va_export.log" \
     && awk '/^PACKET_EXACT / { if (NF != 3 || $2 != int(n/8)+1 || $3 != n%8) exit 1; n++ } END { if (n != 32) exit 1 }' "$OUT/va_export.log"; then
    echo "PASS va-export"; record_result PASS va-export; pass=$((pass+1))
  else
    echo "FAIL va-export (exit $ve)"; record_result FAIL "va-export:exit-$ve"; fail=$((fail+1))
  fi
else
  echo "FAIL va-export (not built)"; record_result FAIL va-export:not-built; fail=$((fail+1))
fi

# Select the GStreamer factory bound to the same video node as the bridge.
# Each V4L2 device can register a different factory with different caps.
pick_gst_h264() {
  if [ -n "${GST_H264_DECODER:-}" ]; then
    gst-inspect-1.0 "$GST_H264_DECODER" >/dev/null || return 1
    printf '%s\n' "$GST_H264_DECODER"; return 0
  fi
  local factories factory node
  factories=$(LC_ALL=C gst-inspect-1.0 v4l2codecs) || return 1
  for factory in $(printf '%s\n' "$factories" | awk '$1 ~ /^v4l2sl.*h264dec:$/ {sub(/:$/, "", $1); print $1}'); do
    node=$(LC_ALL=C gst-inspect-1.0 "$factory" | awk '
      /^[[:space:]]*video-device[[:space:]]*:/ {property=1; next}
      property && /Default:/ {gsub(/"/, ""); node=$NF; property=0}
      END {print node}') || continue
    if [ "$node" = "$H264_NODE" ]; then printf '%s\n' "$factory"; return 0; fi
  done
  return 1
}

gst_h264422() (
  local depth="$1" rawfmt pixfmt frame_size
  local convert=()
  stage="h264422-gst-$depth"
  if [ "$depth" = 8 ]; then rawfmt=Y42B; pixfmt=yuv422p; else rawfmt=NV16_10LE40; pixfmt=yuv422p10le; fi
  if [ "$depth" = 8 ]; then convert=(videoconvert "!"); fi
  must gst-launch-1.0 -q filesrc location="$CLIP/h264_422_idr${depth}.h264" ! parsebin ! "$GST_H264" \
    ! "${convert[@]}" video/x-raw,format="$rawfmt" ! filesink location="$OUT/h264422-${depth}gst.raw"
  if [ "$depth" = 8 ]; then
    must gst-launch-1.0 -q filesrc location="$CLIP/h264_422_idr8.h264" ! parsebin ! avdec_h264 \
      ! videoconvert ! video/x-raw,format=Y42B ! filesink location="$OUT/h264422-8sw.raw"
  else
    must gst-launch-1.0 -q filesrc location="$CLIP/h264_422_idr10.h264" ! parsebin ! avdec_h264 \
      ! video/x-raw,format=I422_10LE ! filesink location="$OUT/h264422-10sw.raw"
    frame_size=$(python3 -c 'import os,sys; n=os.path.getsize(sys.argv[1]); assert n and n % 8 == 0, "invalid 8-frame dump"; print(n // 8)' "$OUT/h264422-10gst.raw") || exit 1
    must python3 tests/nv20_unpack.py "$OUT/h264422-10gst.raw" "$OUT/h264422-10up.raw" 8 1280 720 "$frame_size"
    must cp "$OUT/h264422-10up.raw" "$OUT/h264422-10gst.raw"
  fi
  must "$FF" -nostdin -hide_banner -loglevel error -f rawvideo -s 1280x720 -pix_fmt "$pixfmt" \
    -i "$OUT/h264422-${depth}gst.raw" -frames:v 8 -f framemd5 -y "$OUT/h264422-${depth}gst.md5"
  must "$FF" -nostdin -hide_banner -loglevel error -f rawvideo -s 1280x720 -pix_fmt "$pixfmt" \
    -i "$OUT/h264422-${depth}sw.raw" -frames:v 8 -f framemd5 -y "$OUT/h264422-${depth}sw.md5"
  compare_md5 8 "$OUT/h264422-${depth}gst.md5" "$OUT/h264422-${depth}sw.md5"
)
if GST_H264=$(pick_gst_h264); then
  echo "GST_H264=$GST_H264 NODE=$H264_NODE"
  for depth in 8 10; do
    stage="h264422-gst-$depth"
    if gst_h264422 "$depth"; then
      echo "GST_MATCH h264422-$depth"; record_result PASS "h264422-$depth-gst"; pass=$((pass+1))
    else
      echo "GST_FAIL h264422-$depth"; record_result FAIL "h264422-$depth-gst"; fail=$((fail+1))
    fi
  done
else
  echo "GST_FAIL no H.264 factory for $H264_NODE"; record_result FAIL h264422-gst:factory; fail=$((fail+2))
fi

pair_sw "$CLIP/hevc_main.mp4" 120 hevc-main
pair_sw "$CLIP/hevc_wpp.mp4" 120 hevc-wpp
pair_sw "$CLIP/hevc_4k.mp4" 8 hevc-4k
# Forced internal CAPTURE pool hardware checks;never substitute native output.
if [ "${VAV4L2_TEST_CAPTURE_POOL:-0}" = 1 ]; then
  VAV4L2_CAPTURE_POOL_DMABUF=required pair_sw "$CLIP/h264_4k.mp4" 8 h264-4k-pool nv12 yuv420p 1
  VAV4L2_CAPTURE_POOL_DMABUF=required pair_sw "$CLIP/hevc_4k.mp4" 8 hevc-4k-pool nv12 yuv420p 1
fi
# End forced internal CAPTURE pool checks.

pair_sw "$CLIP/hevc10.mp4" 8 hevc10 p010le p010le
pair_sw "$CLIP/h264_mbaff.mp4" 8 h264-mbaff

stage=hevc-packets
echo "===== HEVC/VP8 packetization ====="
must meson compile -C "$BUILD" hevc_slice_interpose
if python3 "$ROOT/tests/hevc_slice_checks.py" "$BUILD" "$OUT/hevc-slices" "$FF" "$DRM"; then
  echo "PASS hevc-packets"; record_result PASS hevc-packets; pass=$((pass+1))
else
  echo "FAIL hevc-packets"; record_result FAIL hevc-packets; fail=$((fail+1))
fi

if [ "$av1" -eq 1 ]; then
  pair_sw "$CLIP/av1_aom_1tile.mp4" 60 av1-aom-full
  pair_sw "$CLIP/av1_svt_1tile.mp4" 60 av1-svt-full
  pair_sw "$CLIP/av1_4k_1tile.mp4" 8 av1-4k
  pair_sw "$CLIP/av1_default_1tile.mp4" 30 av1-default-full
else
  echo "SKIP AV1 (profile disabled)"; skip=$((skip+4))
  for tag in av1-aom-full av1-svt-full av1-4k av1-default-full; do record_result SKIP "$tag"; done
fi
if [ "${VAV4L2_TEST_AV1_TILES:-0}" = 1 ]; then
  for grid in 2x1:1:0 4x1:2:0 8x1:3:0 2x2:1:1; do
    name=${grid%%:*}; rest=${grid#*:}; cols=${rest%%:*}; rows=${rest##*:}
    enc "$CLIP/av1_tiles_${name}.mkv" -f lavfi -i testsrc2=size=1920x1080:rate=30 \
      -frames:v 60 -pix_fmt yuv420p -c:v libaom-av1 -cpu-used 8 -g 30 \
      -tile-columns "$cols" -tile-rows "$rows"
    pair_sw "$CLIP/av1_tiles_${name}.mkv" 60 "av1-tiles-$name"
  done
fi

pair_sw "$CLIP/vp8_480.webm" 40 vp8-480
pair_sw "$CLIP/vp8_720.webm" 50 vp8-720

pair_gst_mpeg2 "$CLIP/mpeg2_ip.mpg" 40 mpeg2-ip
pair_gst_mpeg2 "$CLIP/mpeg2_b.mpg" 40 mpeg2-b
pair_gst_mpeg2 "$CLIP/mpeg2_1080.mpg" 40 mpeg2-1080

stage=jpeg
echo "== JPEG mjpeg_vaapi =="
# A failed encoder may leave an older va.jpg untouched. Validate a unique
# output from this run, then publish va.jpg only after successful decoding.
jpeg_current=$(mktemp "$OUT/jpeg-current.XXXXXX") || exit 1
VAV4L2_DEBUG=1 "$FF" -nostdin -hide_banner -vaapi_device "$DRM" -f lavfi \
  -i testsrc=size=320x240:rate=5:duration=1 -vf "format=nv12,hwupload" \
  -c:v mjpeg_vaapi -frames:v 1 -f image2 -y "$jpeg_current"
jpeg_exit=$?
printf '%s\n' "$jpeg_exit" > "$OUT/jpeg-exit.txt"
if [ "$jpeg_exit" -eq 0 ] && \
   ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height \
     -of csv=p=0 "$jpeg_current" | grep -qx 'mjpeg,320,240' && \
   "$FF" -nostdin -v error -xerror -i "$jpeg_current" -frames:v 1 -f null -; then
  must mv -f -- "$jpeg_current" "$OUT/va.jpg"
  echo "JPEG_OK"; record_result PASS jpeg; pass=$((pass+1))
else
  echo "JPEG_FAIL exit=$jpeg_exit output=$jpeg_current"; record_result FAIL jpeg; fail=$((fail+1))
fi

stage=vpp
echo "== VPP scale_vaapi =="
export VAV4L2_VPP_TRACE="$OUT/vpp-trace.tsv"
VAV4L2_DEBUG=1 "$FF" -nostdin -hide_banner -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device "$DRM" -i "$CLIP/h264_qcif.mp4" \
  -vf "scale_vaapi=w=160:h=120,hwdownload,format=nv12" -frames:v 8 \
  -f framemd5 -y "$OUT/vpp.md5"
if [ "$?" -eq 0 ] && compare_md5 8 "$OUT/vpp.md5" --min-unique 2 &&
   grep -q '^#dimensions 0: 160x120$' "$OUT/vpp.md5"; then
  echo "VPP_OK"; record_result PASS vpp; pass=$((pass+1))
else
  echo "VPP_FAIL"; record_result FAIL vpp; fail=$((fail+1))
fi

complete=1
stage=done
if [ "$fail" -ne 0 ]; then
  echo "MATRIX_FAILED"
  exit 1
fi
echo "MATRIX_ALL_PASS"
exit 0
