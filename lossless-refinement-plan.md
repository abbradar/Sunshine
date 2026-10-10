# Lossless refinement: HEVC, lossless pointer updates and rate-control pacing

Plan for the next changes on `feat/nvenc-lossless-refinement`. The measurements behind it come from probes run
against NVENC on a Tesla T4 (Turing) with driver 535.129.03 (NVENC API 12.1) on Linux, using synthetic 1080p
content; see "Evidence" at the end.

## Status (2026-10-10)

Implemented, with the fixes of two code reviews (see "Review follow-ups"), uncommitted. Verified:

- On the Tesla T4 (driver 535, NVENC API 12.1) with Sunshine's own `nvenc_base.cpp` and the refinement layer, driven
  through the scripted desktop of the harness in `lossless-refinement-t4/` and decoded with FFmpeg: every block the
  layer asked to encode losslessly was bit-exact in 15 configurations (H.264 4:4:4, HEVC 4:4:4 and 4:2:0; 1 and 4
  slices; 5-frame DPB with reference frame invalidation and a 1-frame DPB falling back to IDR; 20 and 80 Mbps;
  spatial AQ, CAVLC, P7 with full-resolution two-pass, weighted prediction, minimum QP), including pointer and caret
  damage, a new window, a small video, losses with invalidation, a forced IDR and full-range noise. No frame with
  lossless areas exceeded the FEC limit. The H.264 SPS and HEVC PPS checks passed on NVENC's real parameter sets.
- 112 unit tests of the layer and the parameter set checks pass in a standalone gtest harness (GCC 16, project
  warning flags, no warnings), with full line coverage of `src/lossless_refinement.h`, and of
  `src/lossless_refinement.cpp` and the new CBS code except allocation failure edges.
- The Windows-only damage helpers (rotation of Desktop Duplication rectangles, damage composition, cursor area)
  were run natively against shim types: the rotation inverts the cursor viewport for all four rotations.
- Cross-compiled for Windows (MinGW-w64 GCC 16, `-fsyntax-only -Wall`) without warnings: `display_vram.cpp`,
  `display_base.cpp`, `video.cpp`, `cbs.cpp`, `lossless_refinement.cpp`, `nvenc_base.cpp` against SDK 11.0, 12.0,
  13.0 and 13.1, and the changed tests including `tests/unit/platform/windows/test_display_damage.cpp`.
- Formatted with the pinned clang-format 23.1.0.

Still to do: Windows full build, `test_sunshine`, and the streaming tests listed under "Verification".

## Goals

1. **HEVC support.** Lossless refinement for HEVC 8-bit streams, 4:4:4 (Main 4:4:4) and 4:2:0 (Main). Unlike the
   H.264 High 4:4:4 Predictive stream, these decode on common hardware decoders (verified bit-exact with Intel VA-API).
2. **Lossless pointer updates.** When only the mouse pointer moves (DDX pointer-only updates, which already report
   the old and new cursor rectangles as damage), encode exactly the blocks under those rectangles losslessly in the
   same frame instead of dropping the bands to lossy and re-refining them later.
3. **Rate-control pacing.** Keep NVENC's CBR model in sync with wall-clock time during a refinement pass, so motion
   right after a pass isn't encoded at a much higher QP.

## Design

### 1. HEVC: enabling transquant bypass in a low-latency CBR session

NVENC exposes no per-block lossless switch for HEVC. HEVC QP 0 is not lossless; only CUs with
`cu_transquant_bypass_flag` are, which needs `transquant_bypass_enabled_flag` in the PPS. NVENC sets that flag only
in sessions with `NV_ENC_TUNING_INFO_LOSSLESS`, which reject CBR and lose ~33% efficiency on lossy content.

Measured workaround (undocumented driver behaviour, so it is verified at runtime):

1. Initialize the session normally (ultra-low-latency tuning, CBR, QP delta map mode).
2. Before the first frame, `NvEncReconfigureEncoder` to lossless tuning with the lossless preset's rate control,
   then straight back to the original configuration.
3. The PPS keeps `transquant_bypass_enabled_flag = 1` (and chroma QP offsets 0) for the rest of the session,
   including later forced IDRs. From then on, CTBs whose QP delta map entry brings QP to 0 are coded with transquant
   bypass, so they are bit-exact; everything else stays under CBR rate control.
4. Read the parameter sets with `nvEncGetSequenceParams` and check the flag with FFmpeg's coded bitstream reader
   (`cbs::hevc_transquant_bypass_enabled()`). If any step fails or the flag isn't set, fail the lossless attempt;
   `create_encoder()` already retries without refinement.

Loop filters: HEVC never deblocks or applies SAO to transquant-bypass CUs, but it does filter ordinary skipped CUs
next to coded ones, which disturbed already-refined bands by ±1–2. So for HEVC every band that is refined and
unchanged keeps QP delta −51 in every frame ("protected" bands). Unchanged protected CTBs are coded as bypass skip
CUs at nearly no cost. H.264 doesn't need this (no SAO, deblocking already disabled; protecting produced
byte-identical streams), but the same map policy is used for both codecs to keep one code path.

Map granularity: 32×32 CTBs for HEVC (NVENC only supports a 32×32 maximum CU size; set explicitly), 16×16
macroblocks for H.264. Band sizing uses the block size and the chroma format (3 bytes per pixel for 4:4:4,
1.5 for 4:2:0) so that a band and the largest lossless damage fit into the largest frame the stream protects with FEC
(computed from the client's packet size and `fec_percentage`, about 855 kB with 1024-byte packets and 20% FEC), even
at the worst expansion of lossless coding: full-range noise codes to 147% (H.264 CABAC), 156% (CAVLC), 132% (HEVC
4:4:4) and 121% (HEVC 4:2:0) of its uncompressed size with x264/x265, so bands assume 160%. NVENC stays far below:
104% (H.264), 109% (HEVC 4:4:4) and 95% (HEVC 4:2:0) on the T4, 115% with P7.

Both codecs read the parameter sets back after creating the session (`nvEncGetSequenceParams`, parsed with FFmpeg's
coded bitstream reader) and fall back to a session without refinement unless the H.264 SPS sets
`qpprime_y_zero_transform_bypass_flag` or the HEVC PPS sets `transquant_bypass_enabled_flag`. A minimum QP and
spatial AQ are turned off with a warning: AQ raised the QP of busy lossless blocks (off by 1–2 on noise).

Split-frame encoding (SDK 13, multi-NVENC GPUs) is disabled for HEVC lossless sessions, because the bypass
workaround and QP maps couldn't be tested with it (the T4 has one NVENC engine). When the user forces split-frame
encoding, refinement is disabled with a warning instead.

Side effect to document: the lossless PPS has chroma QP offsets 0 instead of +6, so lossy frames spend more bits
on chroma (chroma ~5 dB better at equal luma quality; at fixed QP ~21–25% larger frames; under CBR this shifts bits
from luma to chroma).

### 2. Lossless pointer updates and Desktop Duplication damage

Desktop Duplication reports what changed with each frame: dirty rectangles and the destinations of moved
rectangles, in its unrotated surface (mapped to display coordinates with the inverse of the cursor viewport rotation),
plus the old and new areas of the cursor Sunshine blends in. Each new image carries these as damage relative to the
image numbered one less. Images whose predecessor wasn't a desktop image, or whose metadata couldn't be read, carry no
damage (whole frame changed).

Per captured image with damage that follows the previous capture:

- Map each damage rectangle from display to frame coordinates (same aspect-preserving scaling and centering the
  encode device uses, in both axes, with a one-display-pixel margin for filtering) and then to a block rectangle.
- If the damaged blocks cover at most the area of two 128×128 pointers with the margin at the worst block alignment
  (`lossless_damage_max_pixels()`: 51200 pixels for macroblocks, 73728 for CTBs), the damage of earlier frames has
  been paced and no loss backoff is pending, encode them in the next frame with QP delta −51 and **don't** mark their
  bands changed. This covers pointer moves, a blinking caret and typing.
- Otherwise mark only the bands under the damage changed, so they wait for the refinement delay while other bands
  keep refining (a video or a scrolling window no longer resets every band).
- Frames with lossless damage are paced by the bitrate like bands. Damage only waits for earlier damage, by its share
  of a frame's lossless area when it shares the frame with a band; bands wait for whole frames. Bands are sized to fit
  into the FEC limit together with the largest lossless damage.
- Losing a frame with lossless damage doesn't refine its bands again: refined bands stay lossless in every frame, so
  the next frame recodes the lost damage losslessly against the older reference.

### 3. Rate-control pacing

NVENC's CBR assumes frames arrive at the configured framerate and budgets bits per frame. Between refinement bands
the encoder currently only wakes up at the minimum framerate (half the stream framerate by default), so NVENC sees
half as much time as actually passes and ends a pass with a large deficit. Measured at 40 Mbps 1080p: the first motion
frames after a pass ran at QP 32→24 instead of ~15.

Fix: while a band's pacing window is open, `lossless_refinement_t::wait_time()` also caps the wait at the stream frame
interval, so idle frames are encoded at the stream framerate. Measured: QP after the pass drops to 7→2 (H.264) and
14→2 (HEVC). Idle frames are ~12 bytes; the cost is encoder time during refinement passes only.

## Architecture

Lossless refinement is a layer above the encoder. An encoder only has to support forcing lossless coding for
rectangles of blocks of a fixed size; everything else is encoder-independent.

- `src/lossless_refinement.{h,cpp}` (namespace `video`, unit tested on all platforms)
  - Geometry: block and band math, band sizing against the FEC limit, display-to-block mapping (both axes, scaling
    margin, clamping), the damage limit, and `fill_lossless_block_map()` to turn block rectangles into any per-block
    encoder map.
  - `lossless_band_schedule_t`: when each band is refined (delay, round robin, bitrate pacing of bands and damage
    with framerate cadence, loss backoff, invalidation, refined state).
  - `lossless_refinement_t`: the per-session layer. Built from `lossless_geometry_t` (display and frame size, the
    encoder's block size, chroma format, largest FEC-protected frame), it classifies damage of captured images,
    returns the lossless block rectangles of every frame (refined bands merged into rectangles, the band refined
    now, queued damage), and records encoded frames, losses and invalidations, ignoring images delivered again
    unchanged.
- `lossless_refinement_applicable()` in `src/video.h`: which stream formats can be refined (8-bit, no intra refresh,
  HEVC or H.264 4:4:4).
- Encode loop (`encode_run()` in `src/video.cpp`): owns the layer for every encode session and wires it to captured
  images, frames, losses, invalidations and wait times, with the delay from `config::video` (still set by
  `nvenc_lossless_refinement_delay`). Sessions only report `lossless_block_size()` (zero by default) and whether an
  invalidation request invalidated frames, was already covered by an earlier one, or needs an IDR frame; `encode()`
  passes the lossless rectangles to the session and reports the encoded size.
- Encoder interface (`src/nvenc/nvenc_encoder.h`): `lossless_block_size()`, zero when unsupported, and
  `encode_frame(frame_index, force_idr, lossless_blocks)`.
- NVENC (`src/nvenc/nvenc_base.cpp`): the encoder-specific parts only. The lossless encode capability, H.264 bypass
  flag and deblocking, HEVC 32x32 CTBs, split-frame encoding off, the HEVC round trip with the PPS check (parsed by
  `cbs::hevc_transquant_bypass_enabled()` in `src/cbs.cpp`), and the QP delta map filled from the rectangles.
- Capture (`src/platform/windows/display_vram.cpp`, `display_base.cpp`): Desktop Duplication numbers each new image
  and reports its dirty and moved rectangles and the cursor areas as damage; an image it delivers again unchanged
  keeps its number and isn't modified. The pure parts (`display_rect_from_surface()`, `captured_image_damage()`,
  `blended_cursor_rect()`) are tested in `tests/unit/platform/windows/test_display_damage.cpp`.
- Stream (`src/stream.cpp`): `max_fec_frame_size()` computes the largest frame FEC protects, passed to video as
  `config_t::maxFecFrameSize`.

Another encoder needs a block size and a way to apply the rectangles (e.g. a per-block QP map) in its session and
`encode()` branch. The options are still named `nvenc_lossless_refinement*`; renaming them to generic names is left
until a second encoder supports refinement.

- Also changed: docs, web UI text, `tools/lossless_check` (HEVC PPS flag), tests in
  `tests/unit/test_lossless_refinement.cpp` (geometry, schedule, layer), `tests/unit/test_cbs.cpp` (PPS check),
  `tests/unit/test_stream.cpp` (FEC frame size) and `tests/unit/test_nvenc_dynamic_factory.cpp` (mock).

## Review follow-ups

- Desktop Duplication no longer writes damage or sequence numbers into an image it forwards again while encoders may
  read it; numbering moved from the capture thread into the display. (`frame_timestamp` is still rewritten on
  forwarded images, as before this branch.)
- Repeated invalidation requests that NVENC reports as already handled no longer reset bands or double the backoff.
- `create_encoder()` only retries without refinement when creation failed after lossless refinement was configured.
- A configured minimum QP is reported when lossless refinement overrides it (it isn't a user option today).
- `lossless_check` stops tracing headers once the parameter set was seen and treats a missing flag as 0.
- Not covered by unit tests: `duplication_t::changed_rects()` and the snapshot code around the damage helpers (they
  need Desktop Duplication), the wiring in `encode_run()` (needs a display and an encoder; the T4 harness drives the
  same calls), and the `SUNSHINE_DEBUG_VIDEO_DUMP` dump.
- Second review: a band and damage in one frame no longer hold back damage by the band's size (damage is charged by
  its share of the lossless area) and fit into the FEC limit (bands reserve room for the largest damage); damage
  waits for the loss backoff; H.264 sessions verify the SPS flag; forced split-frame encoding disables HEVC
  refinement with a warning; `lossless_refinement.cpp` no longer includes `video.h`; damage handling no longer
  supports several captures per frame, which the encode loop never does.
- The T4 run found that spatial AQ makes lossless blocks lossy; refinement now turns it off.

## Verification

- Unit tests for all SDK-neutral code, including the SPS and PPS checks with real NVENC parameter sets captured on
  the T4 (flag 0 and 1; HEVC 4:2:0 and 4:4:4), a parameter set without the one it refers to, and truncated input;
  Windows unit tests for the Desktop Duplication damage helpers.
- Build and run the SDK-neutral unit tests on Linux; cross-compile the Windows code with MinGW.
- Integration run on the T4 with Sunshine's own NVENC code and configuration, decoding the stream and comparing every
  lossless area with the source: done, see "Status" and `lossless-refinement-t4/README.md`.
- `uv run --locked lb-update-clang-format`.
- To do on Windows: full build, `test_sunshine`, streaming with Moonlight for H.264 and HEVC (4:4:4 and 4:2:0),
  checking dumps with `lossless_check`, and confirming the PPS check passes on the target GPU and driver.

## Decisions taken (easy to change)

- HEVC 4:2:0 is included; it is lossless relative to the 4:2:0 image.
- 10-bit and AV1 stay unsupported (untested).
- Split-frame encoding is disabled for HEVC lossless sessions; forcing it disables refinement instead.
- A minimum QP and spatial AQ are turned off for refinement.
- Damage from Desktop Duplication's dirty rectangles goes through the same lossless-in-place path as pointer moves.
- Lossless pointer damage is limited to two 128x128 pointers per frame, at any block alignment.
- Bands assume lossless coding expands content to at most 160% (NVENC's worst case wasn't measured; x264/x265 reach 156%).
- The protected-band map is used for H.264 too.

## Risks

- Desktop Duplication's dirty rectangles are trusted. A change they miss inside a refined band is still encoded
  losslessly (refined bands are lossless in every frame), so the image stays correct, but such a frame isn't paced
  and could exceed the FEC limit.
- Small animations (spinners, small videos) now take the lossless damage path whenever pacing allows, and are encoded
  lossy in between, so their quality may alternate.
- Not caused by this change, found on the T4: with H.264 reference frame invalidation, FFmpeg can't decode the
  frames after losing frames that include `frame_num` 0 (it drops them until the next IDR), with or without
  refinement. Losses elsewhere decode identically to the complete stream. Worth an upstream issue.

- The HEVC round trip relies on undocumented driver behaviour, verified only on Turing with driver 535. The runtime
  PPS check falls back to a normal session if the flag isn't set, but a driver could set the flag and still
  misbehave; the Windows test on newer GPUs matters.
- Reconfiguring may be rejected in combination with other options (weighted prediction, two-pass); that also falls
  back to a normal session and logs why.
- HEVC lossless blocks can be expensive on pathological content (57 kB for a cursor over a one-pixel checkerboard).

## Evidence (T4, driver 535, 1080p synthetic content)

| Experiment | Result |
|---|---|
| HEVC normal session, QP delta −51 | Not lossless (max error 11), PPS bypass flag 0 |
| HEVC lossless tuning session | Bypass flag 1, CBR rejected, lossy frames ~33% larger at equal quality |
| Reconfigure per refinement phase | Stream corrupt: NVENC never re-sends the PPS |
| Round trip before the IDR, −51 only on the current band | Earlier bands drift by ±1–2 (SAO/deblocking) |
| Round trip before the IDR, refined bands kept at −51 | Bit-exact 4:4:4 and 4:2:0; stays exact through motion in other bands; forced IDR keeps the PPS; Intel VA-API decode identical to software decode |
| H.264, refined bands kept at −51 vs. only the current band | Byte-identical streams |
| Idle frames at stream framerate during the pass (40 Mbps) | QP after the pass 7→2 instead of 32→24 (H.264) |
| Cursor blocks at −51 | 60/60 frames bit-exact for both codecs |
| Harness with Sunshine's `nvenc_base.cpp`, 15 configurations | All lossless areas bit-exact; spatial AQ was off by 1–2 on noise until refinement turned it off |
| Worst lossless band on full-range noise | 104% (H.264), 109% (HEVC 4:4:4), 95% (HEVC 4:2:0) of raw size; 115% with P7 |
