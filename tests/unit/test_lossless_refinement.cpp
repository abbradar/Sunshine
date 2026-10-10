/**
 * @file tests/unit/test_lossless_refinement.cpp
 * @brief Test src/lossless_refinement.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

// local includes
#include <src/lossless_refinement.h>
#include <src/platform/common.h>

using namespace std::literals;

/**
 * @brief Timing shared by the lossless refinement tests.
 */
struct LosslessTimingFixture: testing::Test {
  using clock = video::lossless_band_schedule_t::clock;

  static constexpr clock::duration delay = 100ms;  ///< Time without new content before refinement starts.
  static constexpr clock::duration frame_interval = 16ms;  ///< Minimum time between two bands.
  static constexpr int bitrate_kbps = 8000;  ///< Refinement bitrate, sending 1 kB takes 1 ms.
  static constexpr clock::duration max_wait = 33ms;  ///< Longest wait allowed by the minimum framerate.

  const clock::time_point start = clock::time_point {} + 1h;  ///< Time refinement is created.
};

/**
 * @brief Coverage for scheduling lossless refinement bands of static content.
 */
struct LosslessBandScheduleTest: LosslessTimingFixture {
  static constexpr std::uint32_t band_count = 3;  ///< Bands per refinement pass.

  video::lossless_band_schedule_t refinement {band_count, delay, frame_interval, bitrate_kbps, start};  ///< Schedule under test.

  /**
   * @brief Encode every pending band, one per frame interval and frame.
   *
   * @param now Time the first pending band is due.
   * @param frame Frame the first pending band is encoded in.
   * @return Time after the last band.
   */
  clock::time_point finish_pass(clock::time_point now, std::int64_t frame) {
    while (refinement.next_band(now, frame++)) {
      now += frame_interval;
    }
    return now;
  }
};

TEST_F(LosslessBandScheduleTest, DisabledWithoutBands) {
  video::lossless_band_schedule_t disabled {0, delay, frame_interval, bitrate_kbps, start};
  EXPECT_EQ(0U, disabled.band_count());
  EXPECT_EQ(max_wait, disabled.wait_time(start + 1h, max_wait));
  EXPECT_EQ(std::nullopt, disabled.next_band(start + 1h, 0));
}

TEST_F(LosslessBandScheduleTest, WaitsForDelayBeforeFirstBand) {
  EXPECT_EQ(max_wait, refinement.wait_time(start, max_wait));
  EXPECT_EQ(10ms, refinement.wait_time(start + 90ms, max_wait));
  EXPECT_EQ(std::nullopt, refinement.next_band(start + 90ms, 0));
  // Waiting for new content may return marginally early
  EXPECT_EQ(0U, refinement.next_band(start + delay - 500us, 1));
}

TEST_F(LosslessBandScheduleTest, NeverRefinesFrameWithNewContent) {
  video::lossless_band_schedule_t no_delay {band_count, 0ms, frame_interval, bitrate_kbps, start};
  EXPECT_EQ(std::nullopt, no_delay.next_band(start, 0));
  EXPECT_EQ(0U, no_delay.next_band(start + frame_interval, 1));

  const auto change = start + 1s;
  no_delay.bands_changed(0, band_count, change);
  EXPECT_EQ(std::nullopt, no_delay.next_band(change, 2));
  EXPECT_EQ(1U, no_delay.next_band(change + frame_interval, 3));
}

TEST_F(LosslessBandScheduleTest, PacesSmallBandsByFrameInterval) {
  const auto first = start + delay;
  EXPECT_EQ(0U, refinement.next_band(first, 0));
  refinement.lossless_frame_encoded(first, 1'000, 0);

  EXPECT_EQ(frame_interval, refinement.wait_time(first, max_wait));
  EXPECT_EQ(std::nullopt, refinement.next_band(first + 10ms, 1));
  EXPECT_EQ(1U, refinement.next_band(first + frame_interval, 1));
}

TEST_F(LosslessBandScheduleTest, PacesLargeBandsByBitrate) {
  const auto first = start + delay;
  EXPECT_EQ(0U, refinement.next_band(first, 0));
  refinement.lossless_frame_encoded(first, 100'000, 0);

  EXPECT_EQ(10ms, refinement.wait_time(first + 90ms, 1s));
  EXPECT_EQ(std::nullopt, refinement.next_band(first + frame_interval, 1));
  EXPECT_EQ(1U, refinement.next_band(first + 100ms, 1));
}

TEST_F(LosslessBandScheduleTest, KeepsEncodingAtFramerateWhilePacing) {
  auto now = start + delay;
  EXPECT_EQ(0U, refinement.next_band(now, 0));
  now += frame_interval;
  EXPECT_EQ(1U, refinement.next_band(now, 1));
  now += frame_interval;
  EXPECT_EQ(2U, refinement.next_band(now, 2));
  refinement.lossless_frame_encoded(now, 500'000, 0);

  // The pass is complete, but rate control has to see the 500 ms it takes to send the last band
  EXPECT_EQ(frame_interval, refinement.wait_time(now, max_wait));
  EXPECT_EQ(frame_interval, refinement.wait_time(now + 480ms, max_wait));
  EXPECT_EQ(max_wait, refinement.wait_time(now + 500ms, max_wait));
}

TEST_F(LosslessBandScheduleTest, PacesDamageByItsShareOfFrame) {
  const auto first = start + delay;
  EXPECT_EQ(0U, refinement.next_band(first, 0));
  // A band and damage, of which the damage takes 10 ms to send
  refinement.lossless_frame_encoded(first, 100'000, 10'000);

  EXPECT_FALSE(refinement.damage_allowed(first + 5ms));
  EXPECT_TRUE(refinement.damage_allowed(first + 10ms));
  EXPECT_EQ(std::nullopt, refinement.next_band(first + 90ms, 1));
  EXPECT_EQ(1U, refinement.next_band(first + 100ms, 1));
}

TEST_F(LosslessBandScheduleTest, PacesOnlyByFrameIntervalWithoutBitrate) {
  video::lossless_band_schedule_t unlimited {band_count, delay, frame_interval, 0, start};
  const auto first = start + delay;
  EXPECT_EQ(0U, unlimited.next_band(first, 0));
  unlimited.lossless_frame_encoded(first, 1'000'000, 1'000'000);

  EXPECT_EQ(1U, unlimited.next_band(first + frame_interval, 1));
  EXPECT_TRUE(unlimited.damage_allowed(first));
}

TEST_F(LosslessBandScheduleTest, NewContentContinuesRoundRobin) {
  const auto first = start + delay;
  EXPECT_EQ(0U, refinement.next_band(first, 0));
  EXPECT_EQ(1U, refinement.next_band(first + frame_interval, 1));

  const auto change = first + 2 * frame_interval;
  refinement.bands_changed(0, band_count, change);
  EXPECT_EQ(std::nullopt, refinement.next_band(change + delay - 2ms, 2));

  // All bands are pending again, starting with the one that wasn't reached
  auto now = change + delay;
  for (const std::uint32_t band : {2U, 0U, 1U}) {
    EXPECT_EQ(band, refinement.next_band(now, 3));
    now += frame_interval;
  }
  EXPECT_EQ(std::nullopt, refinement.next_band(now, 4));
}

TEST_F(LosslessBandScheduleTest, NewContentKeepsPacingOfLargeBand) {
  const auto first = start + delay;
  EXPECT_EQ(0U, refinement.next_band(first, 0));
  refinement.lossless_frame_encoded(first, 500'000, 0);

  refinement.bands_changed(0, band_count, first + 10ms);
  EXPECT_EQ(std::nullopt, refinement.next_band(first + 200ms, 1));
  EXPECT_EQ(1U, refinement.next_band(first + 500ms, 1));
}

TEST_F(LosslessBandScheduleTest, InvalidationRepeatsOnlyLostBandsAfterBackoff) {
  // Bands 0, 1 and 2 are encoded in frames 10, 11 and 12
  const auto now = finish_pass(start + delay, 10);

  refinement.frames_invalidated(12, now);
  EXPECT_FALSE(refinement.damage_allowed(now + 200ms));
  EXPECT_EQ(std::nullopt, refinement.next_band(now + 200ms, 20));
  EXPECT_TRUE(refinement.damage_allowed(now + 250ms));
  EXPECT_EQ(2U, refinement.next_band(now + 250ms, 20));
  EXPECT_EQ(std::nullopt, refinement.next_band(now + 250ms + frame_interval, 21));
}

TEST_F(LosslessBandScheduleTest, InvalidationOfLaterFramesKeepsPass) {
  const auto now = finish_pass(start + delay, 10);

  refinement.frames_invalidated(13, now);
  EXPECT_EQ(max_wait, refinement.wait_time(now, max_wait));
  EXPECT_EQ(std::nullopt, refinement.next_band(now + 1s, 20));
}

TEST_F(LosslessBandScheduleTest, BackoffDoublesForRepeatedLossesAndResets) {
  auto now = start + 1s;
  for (const auto backoff : {250ms, 500ms, 1000ms, 2000ms, 4000ms, 8000ms, 8000ms}) {
    refinement.frames_invalidated(0, now);
    EXPECT_EQ(backoff, refinement.wait_time(now, 1h));
    now += 1s;
  }

  now += 17s;
  refinement.frames_invalidated(0, now);
  EXPECT_EQ(250ms, refinement.wait_time(now, 1h));
}

TEST_F(LosslessBandScheduleTest, PartialChangeDelaysOnlyChangedBands) {
  const auto now = finish_pass(start + delay, 0);

  refinement.bands_changed(1, 2, now);
  EXPECT_EQ(delay, refinement.wait_time(now, 1s));
  EXPECT_EQ(std::nullopt, refinement.next_band(now + delay - 2ms, 10));
  EXPECT_EQ(1U, refinement.next_band(now + delay, 10));
  EXPECT_EQ(std::nullopt, refinement.next_band(now + delay + frame_interval, 11));
}

TEST_F(LosslessBandScheduleTest, PartialChangeLetsOtherBandsRefine) {
  const auto first = start + delay;
  EXPECT_EQ(0U, refinement.next_band(first, 0));

  // A pointer moving over band 1 doesn't hold back band 2
  const auto move = first + 10ms;
  refinement.bands_changed(1, 2, move);
  EXPECT_EQ(2U, refinement.next_band(first + frame_interval, 1));
  EXPECT_EQ(std::nullopt, refinement.next_band(first + 2 * frame_interval, 2));
  EXPECT_EQ(1U, refinement.next_band(move + delay, 3));
}

TEST_F(LosslessBandScheduleTest, PartialChangeIgnoresBandsOutOfRange) {
  const auto now = finish_pass(start + delay, 0);

  refinement.bands_changed(2, 10, now);
  EXPECT_EQ(2U, refinement.next_band(now + delay, 10));
  EXPECT_EQ(std::nullopt, refinement.next_band(now + delay + frame_interval, 11));
}

using LosslessBandCountParam = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, bool, std::uint64_t, std::uint32_t>;

/**
 * @brief Coverage for sizing lossless refinement bands to fit into one frame.
 */
struct LosslessBandCountTest: testing::TestWithParam<LosslessBandCountParam> {};

TEST_P(LosslessBandCountTest, KeepsBandsBelowSizeLimit) {
  const auto &[block_columns, block_rows, block_size, yuv444, max_frame_bytes, expected] = GetParam();
  const auto band_count = video::lossless_band_count(block_columns, block_rows, block_size, yuv444, max_frame_bytes);
  EXPECT_EQ(expected, band_count);

  // Together with the largest damage, even at the worst expansion of lossless coding
  const std::uint64_t block_row_bytes = std::uint64_t {block_columns} * block_size * block_size * (yuv444 ? 6 : 3) / 2;
  const std::uint64_t damage_bytes = video::lossless_damage_max_pixels(block_size) * (yuv444 ? 6 : 3) / 2;
  for (std::uint32_t band = 0; band < band_count; ++band) {
    const auto [first, last] = video::lossless_band_rows(std::max(block_rows, 1U), band, band_count);
    if (last - first > 1) {
      EXPECT_LE(((last - first) * block_row_bytes + damage_bytes) * video::lossless_max_expansion_percent / 100, max_frame_bytes) << "band " << band;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
  LosslessBandCounts,
  LosslessBandCountTest,
  testing::Values(
    // 700 kB of uncompressed content, minus the largest damage: 154 kB for 4:4:4 and 77 kB for 4:2:0 with
    // macroblocks, 221 kB with coding tree blocks
    LosslessBandCountParam {0, 0, 16, true, 1'120'000, 1},  // empty frame still gets one band
    LosslessBandCountParam {120, 68, 16, true, 1'120'000, 14},  // 1080p, 68 rows of 92 kB, 5 per band
    LosslessBandCountParam {120, 68, 16, false, 1'120'000, 6},  // 1080p, 68 rows of 46 kB, 13 per band
    LosslessBandCountParam {6250, 1, 16, true, 1'120'000, 1},  // can't split a single block row
    LosslessBandCountParam {60, 34, 32, true, 1'120'000, 17},  // 1080p, 34 rows of 184 kB, 2 per band
    // Without room for bands next to the largest damage, or without a size, every block row is a band
    LosslessBandCountParam {120, 68, 16, true, 200'000, 68},
    LosslessBandCountParam {120, 68, 16, true, 0, 68}
  )
);

/**
 * @brief Coverage for splitting block rows into bands and finding the bands of block rows.
 */
struct LosslessBandRowsTest: testing::TestWithParam<std::tuple<std::uint32_t, std::uint32_t>> {};

TEST_P(LosslessBandRowsTest, CoversAllRowsEvenly) {
  const auto &[block_rows, band_count] = GetParam();

  std::uint32_t next_row = 0;
  for (std::uint32_t band = 0; band < band_count; ++band) {
    const auto [first, last] = video::lossless_band_rows(block_rows, band, band_count);
    EXPECT_EQ(next_row, first) << "band " << band;
    EXPECT_GE(last - first, block_rows / band_count) << "band " << band;
    EXPECT_LE(last - first, (block_rows + band_count - 1) / band_count) << "band " << band;
    next_row = last;
  }
  EXPECT_EQ(block_rows, next_row);
}

INSTANTIATE_TEST_SUITE_P(
  LosslessBandRows,
  LosslessBandRowsTest,
  testing::Values(
    std::make_tuple(1U, 1U),
    std::make_tuple(68U, 7U),
    std::make_tuple(135U, 25U),
    std::make_tuple(7U, 7U)
  )
);

TEST_P(LosslessBandRowsTest, FindsBandOfEveryRow) {
  const auto &[block_rows, band_count] = GetParam();

  for (std::uint32_t band = 0; band < band_count; ++band) {
    const auto [first, last] = video::lossless_band_rows(block_rows, band, band_count);
    for (auto row = first; row < last; ++row) {
      EXPECT_EQ(std::pair(band, band + 1), video::lossless_bands_for_blocks({0, row, 1, 1}, block_rows, band_count)) << "row " << row;
    }
  }
}

namespace {

  /**
   * @brief H.264 macroblocks of a 1080p frame captured from a 1080p display.
   */
  constexpr video::lossless_geometry_t macroblocks_1080p {1920, 1080, 1920, 1080, 16, true, 0};

}  // namespace

using LosslessBlocksForDisplayRectParam = std::tuple<video::lossless_geometry_t, platf::damage_rect_t, std::optional<video::lossless_block_rect_t>>;

/**
 * @brief Coverage for mapping display rectangles to the blocks of the encoded frame.
 */
struct LosslessBlocksForDisplayRectTest: testing::TestWithParam<LosslessBlocksForDisplayRectParam> {};

TEST_P(LosslessBlocksForDisplayRectTest, CoversScaledRect) {
  const auto &[geometry, rect, expected] = GetParam();
  EXPECT_EQ(expected, video::lossless_blocks_for_display_rect(geometry, rect));
}

INSTANTIATE_TEST_SUITE_P(
  LosslessBlocksForDisplayRect,
  LosslessBlocksForDisplayRectTest,
  testing::Values(
    // Pixels 99-133 with the margin, macroblocks 6-8
    LosslessBlocksForDisplayRectParam {macroblocks_1080p, {100, 100, 32, 32}, video::lossless_block_rect_t {6, 6, 3, 3}},
    // Same pixels in coding tree blocks 3-4
    LosslessBlocksForDisplayRectParam {{1920, 1080, 1920, 1080, 32, true}, {100, 100, 32, 32}, video::lossless_block_rect_t {3, 3, 2, 2}},
    // Pointer partly left of the display, pixels 0-23
    LosslessBlocksForDisplayRectParam {macroblocks_1080p, {-10, 100, 32, 32}, video::lossless_block_rect_t {0, 6, 2, 3}},
    // Clipped at the bottom right, pixels 1899-1920 and 1059-1080
    LosslessBlocksForDisplayRectParam {macroblocks_1080p, {1900, 1060, 40, 40}, video::lossless_block_rect_t {118, 66, 2, 2}},
    // Outside of the frame, or empty
    LosslessBlocksForDisplayRectParam {macroblocks_1080p, {2000, 2000, 32, 32}, std::nullopt},
    LosslessBlocksForDisplayRectParam {macroblocks_1080p, {100, 100, 0, 32}, std::nullopt},
    // 4K display scaled to 1080p, pixels 199-217
    LosslessBlocksForDisplayRectParam {{3840, 2160, 1920, 1080, 16, true}, {400, 400, 32, 32}, video::lossless_block_rect_t {12, 12, 2, 2}},
    // 1920x1200 pillarboxed into 1920x1080 at 90% with 96 pixel bars on the sides, pixels 95-126 and 0-30
    LosslessBlocksForDisplayRectParam {{1920, 1200, 1920, 1080, 16, true}, {0, 0, 32, 32}, video::lossless_block_rect_t {5, 0, 3, 2}},
    // Unknown display size or no blocks
    LosslessBlocksForDisplayRectParam {{0, 0, 1920, 1080, 16, true}, {0, 0, 32, 32}, std::nullopt},
    LosslessBlocksForDisplayRectParam {{1920, 1080, 1920, 1080, 0, true}, {0, 0, 32, 32}, std::nullopt}
  )
);

using LosslessBandsForBlocksParam = std::tuple<video::lossless_block_rect_t, std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>;

/**
 * @brief Coverage for finding the bands that contain a block rectangle.
 */
struct LosslessBandsForBlocksTest: testing::TestWithParam<LosslessBandsForBlocksParam> {};

TEST_P(LosslessBandsForBlocksTest, CoversBlockRows) {
  const auto &[blocks, band_count, expected] = GetParam();
  // 68 macroblock rows of 1080p, in 10 bands starting at rows 0, 6, 13, 20, 27, 34, 40, 47, 54, 61
  EXPECT_EQ(expected, video::lossless_bands_for_blocks(blocks, 68, band_count));
}

INSTANTIATE_TEST_SUITE_P(
  LosslessBandsForBlocks,
  LosslessBandsForBlocksTest,
  testing::Values(
    LosslessBandsForBlocksParam {{0, 6, 3, 3}, 10, {1, 2}},  // rows 6-8
    LosslessBandsForBlocksParam {{0, 12, 3, 3}, 10, {1, 3}},  // rows 12-14 across bands 1 and 2
    LosslessBandsForBlocksParam {{0, 66, 3, 5}, 10, {9, 10}},  // clipped at the bottom
    LosslessBandsForBlocksParam {{0, 68, 3, 3}, 10, {0, 0}},  // below the frame
    LosslessBandsForBlocksParam {{0, 6, 3, 0}, 10, {0, 0}},  // no rows
    LosslessBandsForBlocksParam {{0, 6, 3, 3}, 0, {0, 0}}  // no bands
  )
);

TEST(LosslessDamageMaxPixelsTest, CoversTwoLargestPointersAtAnyAlignment) {
  // 128 pixels and the margin cover up to 10 macroblocks or 6 coding tree blocks
  EXPECT_EQ(2U * 160 * 160, video::lossless_damage_max_pixels(16));
  EXPECT_EQ(2U * 192 * 192, video::lossless_damage_max_pixels(32));
}

TEST(LosslessBlockMapTest, FillsAndClipsBlockRects) {
  constexpr std::int8_t L = -51;
  std::vector<std::int8_t> map(12, 7);  // 3 columns, 4 rows
  const std::vector<video::lossless_block_rect_t> blocks {{1, 1, 2, 2}, {2, 3, 5, 5}, {5, 5, 1, 1}};
  video::fill_lossless_block_map(std::span {map}, 3, blocks, L);
  EXPECT_EQ((std::vector<std::int8_t> {0, 0, 0, 0, L, L, 0, L, L, 0, 0, L}), map);
}

TEST(LosslessBlockMapTest, IgnoresZeroWidth) {
  std::vector<std::int8_t> map;
  const std::vector<video::lossless_block_rect_t> blocks {{0, 0, 1, 1}};
  video::fill_lossless_block_map(std::span {map}, 0, blocks, std::int8_t {-51});
  EXPECT_TRUE(map.empty());
}

/**
 * @brief Coverage for the encoder-independent lossless refinement of a session.
 *
 * Uses 1080p with H.264 macroblocks: 120x68 blocks in 10 bands starting at rows 0, 6, 13, 20, 27, 34, 40, 47, 54, 61.
 */
struct LosslessRefinementTest: LosslessTimingFixture {
  static constexpr video::lossless_geometry_t geometry {
    .display_width = 1920,
    .display_height = 1080,
    .frame_width = 1920,
    .frame_height = 1080,
    .block_size = 16,
    .yuv444 = true,
    .max_frame_bytes = 1'280'000,
  };  ///< Sizes of the session, with 800 kB of uncompressed content, 646 kB for bands next to the largest damage.
  static constexpr platf::damage_rect_t pointer {100, 100, 32, 32};  ///< Pointer area, pixels 99-133 with the margin.
  static constexpr video::lossless_block_rect_t pointer_blocks {6, 6, 3, 3};  ///< Macroblocks of the pointer area, in band 1.

  const std::vector<video::lossless_block_rect_t> all_bands {{0, 0, 120, 68}};  ///< Blocks of all bands.
  video::lossless_refinement_t refinement {geometry, delay, frame_interval, bitrate_kbps, start};  ///< Refinement under test.
  std::int64_t frame = 0;  ///< Index of the next frame.

  /**
   * @brief Report a captured image.
   *
   * @param sequence Capture sequence number.
   * @param damage Damaged areas, or `std::nullopt` when the whole frame may have changed.
   * @param now Current time.
   */
  void capture(std::uint64_t sequence, std::optional<std::vector<platf::damage_rect_t>> damage, clock::time_point now) {
    platf::img_t img;
    img.capture_sequence = sequence;
    img.damage = std::move(damage);
    refinement.image_captured(img, now);
  }

  /**
   * @brief Encode the next frame.
   *
   * @param now Current time.
   * @param frame_size Size of the encoded frame in bytes.
   * @return Lossless areas of the frame.
   */
  video::lossless_frame_t encode(clock::time_point now, std::size_t frame_size = 1'000) {
    auto lossless = refinement.next_frame(now, frame++);
    refinement.frame_encoded(frame_size);
    return lossless;
  }

  /**
   * @brief Capture the first image and refine every band, in frames 0 to 9.
   *
   * @return Time after the last band.
   */
  clock::time_point finish_pass() {
    capture(1, std::nullopt, start);
    auto now = start + delay;
    while (encode(now).refined_band) {
      now += frame_interval;
    }
    return now;
  }
};

TEST_F(LosslessRefinementTest, DisabledWithoutBlockSize) {
  auto disabled_geometry = geometry;
  disabled_geometry.block_size = 0;
  video::lossless_refinement_t disabled {disabled_geometry, delay, frame_interval, bitrate_kbps, start};

  platf::img_t img;
  img.capture_sequence = 1;
  disabled.image_captured(img, start);
  const auto lossless = disabled.next_frame(start + 1h, 0);
  disabled.frame_encoded(1'000);

  EXPECT_EQ(0U, disabled.band_count());
  EXPECT_TRUE(lossless.blocks.empty());
  EXPECT_EQ(std::nullopt, lossless.refined_band);
  EXPECT_EQ(max_wait, disabled.wait_time(start + 1h, max_wait));
}

TEST_F(LosslessRefinementTest, RefinesFirstBandAfterDelay) {
  capture(1, std::nullopt, start);

  const auto early = encode(start + delay - 10ms);
  EXPECT_EQ(std::nullopt, early.refined_band);
  EXPECT_TRUE(early.blocks.empty());

  const auto first = encode(start + delay);
  EXPECT_EQ(0U, first.refined_band);
  EXPECT_EQ((std::vector<video::lossless_block_rect_t> {{0, 0, 120, 6}}), first.blocks);
}

TEST_F(LosslessRefinementTest, KeepsRefinedBandsLossless) {
  capture(1, std::nullopt, start);
  EXPECT_EQ(0U, encode(start + delay).refined_band);

  // Band 0 stays lossless next to the newly refined band 1, in one rectangle
  const auto second = encode(start + delay + frame_interval);
  EXPECT_EQ(1U, second.refined_band);
  EXPECT_EQ((std::vector<video::lossless_block_rect_t> {{0, 0, 120, 13}}), second.blocks);
}

TEST_F(LosslessRefinementTest, EncodesPointerDamageLosslesslyInPlace) {
  const auto now = finish_pass();

  // Old and new pointer area, pixels 99-163 and 99-143 with the margin
  capture(2, std::vector<platf::damage_rect_t> {pointer, {130, 110, 32, 32}}, now);
  const auto lossless = encode(now);
  const std::vector<video::lossless_block_rect_t> expected {{0, 0, 120, 68}, pointer_blocks, {8, 6, 3, 3}};
  EXPECT_EQ(expected, lossless.blocks);
  EXPECT_EQ(std::nullopt, lossless.refined_band);

  // The damage is only encoded once, and the bands stay refined
  EXPECT_EQ(all_bands, encode(now + delay).blocks);
}

TEST_F(LosslessRefinementTest, EmptyDamageKeepsBandsRefined) {
  const auto now = finish_pass();

  capture(2, std::vector<platf::damage_rect_t> {}, now);
  EXPECT_EQ(all_bands, encode(now).blocks);

  // Damage below the frame covers no blocks either
  capture(3, std::vector<platf::damage_rect_t> {{0, 2000, 32, 32}}, now);
  EXPECT_EQ(all_bands, encode(now).blocks);
}

TEST_F(LosslessRefinementTest, EncodesTwoLargestPointersInPlace) {
  const auto now = finish_pass();

  // Two 128x128 pointers that cover 10x10 macroblocks each with the margin
  std::vector<platf::damage_rect_t> damage {{16, 16, 128, 128}, {512, 512, 128, 128}};
  capture(2, damage, now);
  const std::vector<video::lossless_block_rect_t> expected {{0, 0, 120, 68}, {0, 0, 10, 10}, {31, 31, 10, 10}};
  EXPECT_EQ(expected, encode(now).blocks);

  // Two more macroblocks exceed the limit, so bands 0, 1, 4, 5, 6 and 7 are encoded lossy
  damage.push_back({1000, 800, 1, 1});
  capture(3, damage, now);
  const std::vector<video::lossless_block_rect_t> lossy {{0, 13, 120, 14}, {0, 54, 120, 14}};
  EXPECT_EQ(lossy, encode(now).blocks);
}

TEST_F(LosslessRefinementTest, PacesDamageByBitrate) {
  const auto now = finish_pass();

  // Sending the frame with the pointer damage takes 100 ms
  capture(2, std::vector {pointer}, now);
  encode(now, 100'000);

  // Until then, more damage is encoded lossy, and band 1 waits for the delay
  capture(3, std::vector {pointer}, now + 50ms);
  const std::vector<video::lossless_block_rect_t> lossy {{0, 0, 120, 6}, {0, 13, 120, 55}};
  EXPECT_EQ(lossy, encode(now + 50ms).blocks);

  capture(4, std::vector {pointer}, now + 100ms);
  const std::vector<video::lossless_block_rect_t> lossless {{0, 0, 120, 6}, {0, 13, 120, 55}, pointer_blocks};
  EXPECT_EQ(lossless, encode(now + 100ms).blocks);
}

TEST_F(LosslessRefinementTest, PacesDamageByItsShareOfFrameWithBand) {
  capture(1, std::nullopt, start);
  encode(start + delay);

  // Band 1 and the pointer damage share a frame that takes 100 ms to send, of which the damage takes 1 ms
  auto now = start + delay + frame_interval;
  capture(2, std::vector {pointer}, now);
  const auto shared = encode(now, 100'000);
  EXPECT_EQ(1U, shared.refined_band);
  const std::vector<video::lossless_block_rect_t> expected {{0, 0, 120, 13}, pointer_blocks};
  EXPECT_EQ(expected, shared.blocks);

  // So more damage is encoded losslessly in the next frame, while band 2 waits until band 1 is sent
  now += frame_interval;
  capture(3, std::vector {pointer}, now);
  const auto next = encode(now);
  EXPECT_EQ(std::nullopt, next.refined_band);
  EXPECT_EQ(expected, next.blocks);
}

TEST_F(LosslessRefinementTest, EncodesDamageLossyDuringLossBackoff) {
  const auto now = finish_pass();

  // Only later frames are lost, so every band stays refined, but the pointer is encoded lossy during the backoff
  refinement.frames_invalidated(frame, now);
  capture(2, std::vector {pointer}, now);
  const std::vector<video::lossless_block_rect_t> lossy {{0, 0, 120, 6}, {0, 13, 120, 55}};
  EXPECT_EQ(lossy, encode(now).blocks);
}

TEST_F(LosslessRefinementTest, ImageDeliveredAgainChangesNothing) {
  const auto now = finish_pass();

  capture(1, std::nullopt, now);
  EXPECT_EQ(all_bands, encode(now).blocks);

  capture(2, std::vector {pointer}, now);
  const std::vector<video::lossless_block_rect_t> expected {{0, 0, 120, 68}, pointer_blocks};
  EXPECT_EQ(expected, encode(now).blocks);
}

TEST_F(LosslessRefinementTest, DelaysBandsOfLargeDamage) {
  const auto now = finish_pass();

  // Pixels 255-657, macroblock rows 15-41 in bands 2 to 6
  capture(2, std::vector<platf::damage_rect_t> {{0, 256, 400, 400}}, now);
  const auto lossy = encode(now);
  const std::vector<video::lossless_block_rect_t> expected {{0, 0, 120, 13}, {0, 47, 120, 21}};
  EXPECT_EQ(expected, lossy.blocks);

  EXPECT_EQ(std::nullopt, encode(now + delay - 2ms).refined_band);
  EXPECT_EQ(2U, encode(now + delay).refined_band);
}

TEST_F(LosslessRefinementTest, DamageOfSkippedCaptureChangesWholeFrame) {
  const auto now = finish_pass();

  // Capture 2 went to another session, so the damage of capture 3 doesn't cover everything that changed
  capture(3, std::vector {pointer}, now);
  EXPECT_TRUE(encode(now).blocks.empty());
}

TEST_F(LosslessRefinementTest, InvalidatedDamageKeepsRefinedBands) {
  const auto now = finish_pass();

  // Pointer damage in band 1 is encoded in frame 20, then frames from 15 on are invalidated
  frame = 20;
  capture(2, std::vector {pointer}, now);
  encode(now);
  refinement.frames_invalidated(15, now);

  // Band 1 was refined in frame 1, so it stays lossless and the next frame recodes the lost damage
  EXPECT_EQ(all_bands, encode(now).blocks);
}

TEST_F(LosslessRefinementTest, InvalidationRefinesBandsOfLostFramesAgain) {
  const auto now = finish_pass();

  // Bands 5 to 9 were refined in frames 5 to 9
  refinement.frames_invalidated(5, now);
  EXPECT_EQ((std::vector<video::lossless_block_rect_t> {{0, 0, 120, 34}}), encode(now).blocks);
  EXPECT_EQ(5U, encode(now + 250ms).refined_band);
}

TEST_F(LosslessRefinementTest, LossRefinesEverythingAgain) {
  const auto now = finish_pass();

  refinement.frames_lost(now);
  EXPECT_TRUE(encode(now).blocks.empty());
  EXPECT_EQ(0U, encode(now + 250ms).refined_band);
}
