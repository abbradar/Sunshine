#!/bin/bash
# Run lossless refinement with Sunshine's other NVENC options.
set -u
cd "$(dirname "$0")"
FFMPEG=${FFMPEG:-/root/dl/ffmpeg/ffmpeg-master-latest-linux64-gpl/bin/ffmpeg}
H() { ./lib/ld-linux-x86-64.so.2 --library-path "$PWD/lib:/usr/lib64" ./harness "$@"; }
opt() {
  local name=$1 env=$2 codec=$3 chroma=$4
  echo "=== $name: $env $codec $chroma"
  if ! env $env ./lib/ld-linux-x86-64.so.2 --library-path "$PWD/lib:/usr/lib64" ./harness encode "$codec" "$chroma" 1 0 20000 1024 "out/$name" > "out/$name.log" 2>&1; then
    tail -5 "out/$name.log"; return
  fi
  grep -E "created encoder|warning|error|^frames|band size" "out/$name.log" | grep -v "encoded frame sizes" | sed 's/^\[.*\] *//'
  local pix=yuv444p; [ "$chroma" = 420 ] && pix=yuv420p
  "$FFMPEG" -v error -f "$codec" -i "out/$name.bit" -fps_mode passthrough -f rawvideo -pix_fmt "$pix" - | H check "$chroma" "out/$name" | tail -3
}
opt h264_aq AQ=1 h264 444
opt hevc_aq AQ=1 hevc 444
opt h264_cavlc CAVLC=1 h264 444
opt h264_p7 "NVENC_PRESET=7 FULL_TWO_PASS=1" h264 444
opt hevc_p7 "NVENC_PRESET=7 FULL_TWO_PASS=1" hevc 420
opt hevc_wp WP=1 hevc 420
opt h264_minqp MIN_QP=1 h264 444
opt hevc_minqp MIN_QP=1 hevc 444
