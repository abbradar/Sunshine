# Lossless refinement harness for NVIDIA Linux machines

Runs Sunshine's own NVENC encoder (`src/nvenc/nvenc_base.cpp`) with the lossless refinement layer
(`src/lossless_refinement.cpp`) over CUDA on Linux, on a scripted 1080p60 desktop, and checks every block the layer
asks to encode losslessly against the source after decoding with FFmpeg. Not part of the build; untracked.

Scenario (`harness.cpp`, 24 s): a text-like desktop refined from scratch, a pointer sweep, a blinking caret, a window
that appears, a small video with occasional pointer moves, a full change with three lost frames during refinement
(reference frame invalidation), a forced IDR frame, then full-range noise refined from scratch (worst case for band
sizes) with a pointer moving over it and two more lost frames. Damage is reported the way Desktop Duplication does.

## Build (on the NixOS laptop)

    nix shell nixpkgs#gcc -c ./build.sh   # SDK=1200 by default, matches driver 535

Needs nv-codec-headers (`$S/sdk/nv-codec-headers-n12.0.16.1`), the pinned build-deps FFmpeg for CBS
(`$S/ffmpeg-builddeps/ffmpeg`, see the NixOS build notes) and the include paths in `$S/harness/env.sh`
(`env.sh.example`). Produces `harness.tar.gz`: the binary plus every library it loads, including glibc, so it runs on
old distributions through the bundled `ld-linux`.

## Run (on the NVIDIA machine)

    scp harness.tar.gz root@HOST: && ssh root@HOST 'mkdir -p t4 && tar xzf harness.tar.gz -C t4'
    ./run.sh [filter]   # codec, chroma, slices, reference frames and bitrate matrix
    ./opts.sh           # AQ, CAVLC, P7 with full-resolution two-pass, weighted prediction, min QP
    ./gapcmp.sh NAME h264|hevc   # decode with lost frames against the complete stream

`run.sh` expects an FFmpeg binary at `$FFMPEG` (default: the BtbN static build under `/root/dl/ffmpeg`).
Environment switches of `harness encode`: `LOSSLESS=0`, `LOSS_FRAME=N`, `AQ`, `CAVLC`, `WP`, `MIN_QP`,
`NVENC_PRESET=N`, `FULL_TWO_PASS`.

Known: FFmpeg can't decode H.264 after losing frames that include `frame_num` 0, with or without refinement, so the
scripted losses avoid those frames unless `LOSS_FRAME` is set.
