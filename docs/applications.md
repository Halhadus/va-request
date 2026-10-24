# Application settings

Select the installed driver in the application's environment:

```sh
export LIBVA_DRIVER_NAME=v4l2
vainfo --display drm --device /dev/dri/renderD128
```

Use the render node for your system. For an uninstalled build, also set `LIBVA_DRIVERS_PATH="$PWD/build"`. The application needs access to render and video devices; Request decoding also needs the associated media device. Restart it after changing the driver selection.

| Application | Enable / select | Keep at default | Check |
| --- | --- | --- | --- |
| FFmpeg | `-hwaccel vaapi -hwaccel_output_format vaapi -vaapi_device /dev/dri/renderD128` before the input | Codec/profile selection | Verbose decode log and output frame count |
| mpv | `--hwdec=auto` | Codec selection and fallback | Playback statistics and decoder log |
| VLC | Preferences → Input/Codecs → hardware decoding: Automatic or VA-API | Other decoder settings | Codec and debug information |
| Firefox | Settings → Performance → hardware acceleration; `media.hardware-video-decoding.enabled=true` | Feature blocklists, software codecs and sandbox settings | `about:support` and the active stream codec |
| Chromium / Chrome | Settings → System → graphics acceleration | Feature blocklists, driver workarounds and sandbox settings | `chrome://gpu` and `chrome://media-internals` |

Browser VA-API availability depends on the application build and distribution. Graphics acceleration alone does not establish that a stream uses hardware video decoding.

For an 8-bit 4:2:0 input, FFmpeg can produce comparable frame hashes:

```sh
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -vaapi_device /dev/dri/renderD128 -i input.mp4 -vf 'hwdownload,format=nv12' -f framemd5 hardware.md5
ffmpeg -i input.mp4 -pix_fmt nv12 -f framemd5 software.md5
```

Use P010, YUY2 or Y210 for the corresponding bit depth and chroma. MPEG-2 can have IDCT rounding differences; see [testing](testing.md).

Leave `VAV4L2_ADVERTISE_AV1` unset for normal use. AV1 is experimental and VP9 is not implemented. Applications with software fallback can use it for unavailable profiles.

Sources: [FFmpeg hardware acceleration](https://ffmpeg.org/ffmpeg.html), [mpv manual](https://mpv.io/manual/stable/), [Firefox performance settings](https://support.mozilla.org/en-US/kb/performance-settings), [Chromium GPU documentation](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/gpu/vaapi.md).
