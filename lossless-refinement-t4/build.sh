#!/usr/bin/env bash
# Build the T4 harness against Sunshine's sources and package it with its libraries.
set -euo pipefail
# Dependencies from the session that wrote this (see README.md): nv-codec-headers, the build-deps FFmpeg and the
# include paths of env.sh
S=${S:-/tmp/claude-1000/-home-abbradar-projects-sunshine/f90fac8e-c1a3-4706-a36e-aa421df82781/scratchpad}
. $S/harness/env.sh
cd "$(dirname "$0")"
SDK=${SDK:-1200}
case $SDK in 1200) HDR=n12.0.16.1;; 1201) HDR=n12.1.14.0;; 1300) HDR=n13.0.19.0;; esac
FF=$S/ffmpeg-builddeps/ffmpeg
NV="-I$S/sdk/nv-codec-headers-$HDR/include -DNVENC_NAMESPACE=nvenc_$SDK -DNVENC_SDK_VERSION=$SDK -include stdlib.h -include string.h -include stdio.h"
FLAGS="-std=c++23 -O2 -g -Wall -Wno-sign-compare -DBOOST_LOG_DYN_LINK -I$FF/include $INC"
R=$(cd .. && pwd)
mkdir -p obj
g++ $NV $FLAGS -c $R/src/nvenc/nvenc_base.cpp -o obj/nvenc_base.o
g++ $NV $FLAGS -c $R/src/nvenc/nvenc_utils.cpp -o obj/nvenc_utils.o
g++ $NV $FLAGS -c harness.cpp -o obj/harness.o
g++ $FLAGS -c $R/src/lossless_refinement.cpp -o obj/lossless_refinement.o
g++ $FLAGS -c $R/src/cbs.cpp -o obj/cbs.o
g++ $FLAGS -c $R/src/stat_trackers.cpp -o obj/stat_trackers.o
g++ $FLAGS -c log_stub.cpp -o obj/log_stub.o
BOOST=/nix/store/nb7q034gmpx0bypbxj3xgczii9jrzkfa-boost-1.91.0/lib
g++ -o harness obj/*.o \
  -Wl,--start-group $FF/lib/libcbs.a $FF/lib/libavcodec.a $FF/lib/libavutil.a $FF/lib/libva.a $FF/lib/libva-drm.a -Wl,--end-group \
  -L$BOOST -lboost_log -lboost_thread -lboost_filesystem -lpthread -lm -ldl
# Package: the binary, every library it loads, and glibc's compatibility libraries the driver needs
rm -rf pkg && mkdir -p pkg/lib
cp harness run.sh opts.sh gapcmp.sh pkg/
for lib in $(ldd harness | awk '/=>/ { print $3 } /ld-linux/ { print $1 }'); do cp -L --remove-destination "$lib" pkg/lib/; done
GLIBC=$(dirname "$(ldd harness | awk '/libc.so.6/ { print $3 }')")
for l in libdl.so.2 libpthread.so.0 librt.so.1 libm.so.6; do cp -L --remove-destination "$GLIBC/$l" pkg/lib/; done
tar czf harness.tar.gz -C pkg .
ls -la harness.tar.gz
