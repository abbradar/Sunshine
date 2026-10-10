#!/bin/bash
# Run the harness matrix on the NVIDIA machine: encode, decode with FFmpeg, check lossless areas.
# Usage: run.sh [name filter]
set -u
cd "$(dirname "$0")"
FFMPEG=${FFMPEG:-/root/dl/ffmpeg/ffmpeg-master-latest-linux64-gpl/bin/ffmpeg}
H() { ./lib/ld-linux-x86-64.so.2 --library-path "$PWD/lib:/usr/lib64" ./harness "$@"; }
mkdir -p out

run() {
  local name=$1 codec=$2 chroma=$3 slices=$4 refs=$5 bitrate=$6 packet=$7
  [[ $name == *${FILTER:-}* ]] || return 0
  echo "=== $name: $codec $chroma, $slices slices, $refs refs, $bitrate kbps, $packet-byte packets"
  if ! H encode "$codec" "$chroma" "$slices" "$refs" "$bitrate" "$packet" "out/$name" > "out/$name.log" 2>&1; then
    tail -20 "out/$name.log"
    return
  fi
  grep -vE "encoded frame sizes" "out/$name.log" | grep -E "NvEnc|lossless|bands|frames|band size|rfi" | grep -v "lossless block areas" | head -20
  local pix=yuv444p
  [ "$chroma" = 420 ] && pix=yuv420p
  "$FFMPEG" -v error -f "$codec" -i "out/$name.bit" -c copy -bsf:v trace_headers -f null - 2>&1 \
    | grep -m1 -E "transquant_bypass_enabled_flag|qpprime_y_zero_transform_bypass_flag"
  "$FFMPEG" -v error -f "$codec" -i "out/$name.bit" -fps_mode passthrough -f rawvideo -pix_fmt "$pix" - 2> "out/$name.decode.log" \
    | H check "$chroma" "out/$name"
  [ -s "out/$name.decode.log" ] && { echo "  decoder messages:"; sort "out/$name.decode.log" | uniq -c | head -5; }
}

FILTER=${1:-}
run h264_444 h264 444 1 0 20000 1024
run hevc_444 hevc 444 1 0 20000 1024
run hevc_420 hevc 420 1 0 20000 1024
run hevc_420_slices hevc 420 4 0 20000 1024
run h264_444_slices h264 444 4 0 20000 1024
run hevc_444_norfi hevc 444 1 1 20000 1024
run h264_444_80m h264 444 1 0 80000 1392
