/**
 * @file src/lossless_refinement.cpp
 * @brief Definitions for encoder-independent lossless refinement of static content.
 */
// this include
#include "lossless_refinement.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <limits>

// local includes
#include "platform/common.h"

using namespace std::literals;

namespace {

  constexpr auto initial_loss_backoff = 250ms;
  constexpr auto max_loss_backoff = 8s;

  // Waiting for new content may return marginally early
  constexpr auto wake_up_slack = 1ms;

  /**
   * @brief Compute the number of blocks needed to cover a frame dimension, if the encoder has lossless blocks.
   *
   * @param pixels Frame dimension in pixels.
   * @param block_size Block size in pixels, zero when the encoder can't encode blocks losslessly.
   * @return Number of blocks, zero without blocks.
   */
  std::uint32_t frame_blocks(std::int32_t pixels, std::uint32_t block_size) {
    return block_size > 0 ? video::lossless_block_count(static_cast<std::uint32_t>(std::max(pixels, 0)), block_size) : 0;
  }

}  // namespace

namespace video {

  std::optional<lossless_block_rect_t> lossless_blocks_for_display_rect(const lossless_geometry_t &geometry, const platf::damage_rect_t &rect) {
    if (geometry.display_width <= 0 || geometry.display_height <= 0 || geometry.frame_width <= 0 || geometry.frame_height <= 0 || rect.width <= 0 || rect.height <= 0 || geometry.block_size == 0) {
      return std::nullopt;
    }

    const double scale = std::min(static_cast<double>(geometry.frame_width) / geometry.display_width, static_cast<double>(geometry.frame_height) / geometry.display_height);
    // Map a half-open span of display pixels to a half-open span of frame pixels, clamped to the frame
    const auto map_span = [scale](std::int32_t display_size, std::int32_t frame_size, std::int32_t first, std::int32_t last) {
      const double offset = (frame_size - display_size * scale) / 2;
      const double first_px = std::max(0.0, std::floor(offset + (static_cast<double>(first) - 1) * scale));
      const double last_px = std::min(static_cast<double>(frame_size), std::ceil(offset + (static_cast<double>(last) + 1) * scale));
      return std::pair {first_px, last_px};
    };
    const auto [first_x, last_x] = map_span(geometry.display_width, geometry.frame_width, rect.x, rect.x + rect.width);
    const auto [first_y, last_y] = map_span(geometry.display_height, geometry.frame_height, rect.y, rect.y + rect.height);
    if (first_x >= last_x || first_y >= last_y) {
      return std::nullopt;
    }

    const auto first_column = static_cast<std::uint32_t>(first_x) / geometry.block_size;
    const auto first_row = static_cast<std::uint32_t>(first_y) / geometry.block_size;
    return lossless_block_rect_t {
      first_column,
      first_row,
      lossless_block_count(static_cast<std::uint32_t>(last_x), geometry.block_size) - first_column,
      lossless_block_count(static_cast<std::uint32_t>(last_y), geometry.block_size) - first_row,
    };
  }

  lossless_band_schedule_t::lossless_band_schedule_t(std::uint32_t band_count, clock::duration delay, clock::duration frame_interval, int bitrate_kbps, clock::time_point now):
      delay {delay},
      frame_interval {frame_interval},
      bitrate_kbps {bitrate_kbps},
      bands(band_count),
      last_loss_time {now} {
    bands_changed(0, band_count, now);
  }

  std::uint32_t lossless_band_schedule_t::band_count() const {
    return static_cast<std::uint32_t>(bands.size());
  }

  void lossless_band_schedule_t::bands_changed(std::uint32_t first_band, std::uint32_t last_band, clock::time_point now) {
    // Never refine a band in the frame that carries its new content
    const auto not_before = now + std::max(delay, frame_interval);
    for (auto band = first_band; band < std::min<std::size_t>(last_band, bands.size()); ++band) {
      bands[band] = {std::nullopt, not_before};
    }
  }

  void lossless_band_schedule_t::frames_invalidated(std::int64_t first_frame, clock::time_point now) {
    for (auto &band : bands) {
      if (band.frame && *band.frame >= first_frame) {
        band.frame.reset();
      }
    }
    loss_backoff = now - last_loss_time > 2 * max_loss_backoff ? initial_loss_backoff : std::clamp<clock::duration>(2 * loss_backoff, initial_loss_backoff, max_loss_backoff);
    last_loss_time = now;
    backoff_until = now + loss_backoff;
  }

  bool lossless_band_schedule_t::refined(std::uint32_t band) const {
    return band < bands.size() && bands[band].frame.has_value();
  }

  lossless_band_schedule_t::clock::duration lossless_band_schedule_t::wait_time(clock::time_point now, clock::duration max_wait) const {
    auto wait = max_wait;
    // Encode at the stream framerate while pacing, so rate control sees as much time pass as the frame takes to send
    if (now < paced_until) {
      wait = std::min(wait, frame_interval);
    }
    for (const auto &band : bands) {
      if (!band.frame) {
        wait = std::min<clock::duration>(wait, std::max({band.not_before, paced_until, backoff_until}) - now);
      }
    }
    return std::max(wait, clock::duration::zero());
  }

  bool lossless_band_schedule_t::damage_allowed(clock::time_point now) const {
    return now + wake_up_slack >= std::max(damage_paced_until, backoff_until);
  }

  std::optional<std::uint32_t> lossless_band_schedule_t::next_band(clock::time_point now, std::int64_t frame_index) {
    for (std::uint32_t offset = 0; offset < band_count(); ++offset) {
      const auto index = (next_band_index + offset) % band_count();
      auto &band = bands[index];
      if (!band.frame && now + wake_up_slack >= std::max({band.not_before, paced_until, backoff_until})) {
        band.frame = frame_index;
        next_band_index = (index + 1) % band_count();
        paced_until = std::max(paced_until, now + frame_interval);
        return index;
      }
    }
    return std::nullopt;
  }

  void lossless_band_schedule_t::lossless_frame_encoded(clock::time_point frame_time, std::size_t frame_size, std::size_t damage_size) {
    if (bitrate_kbps <= 0) {
      return;
    }
    const auto transmit_time = [this](std::size_t size) {
      return std::chrono::nanoseconds {static_cast<std::int64_t>(size) * 8'000'000 / bitrate_kbps};
    };
    paced_until = std::max(paced_until, frame_time + transmit_time(frame_size));
    damage_paced_until = std::max(damage_paced_until, frame_time + transmit_time(damage_size));
  }

  lossless_refinement_t::lossless_refinement_t(const lossless_geometry_t &geometry, clock::duration delay, clock::duration frame_interval, int bitrate_kbps, clock::time_point now):
      geometry {geometry},
      block_columns {frame_blocks(geometry.frame_width, geometry.block_size)},
      block_rows {frame_blocks(geometry.frame_height, geometry.block_size)},
      schedule {
        geometry.block_size > 0 ? lossless_band_count(block_columns, block_rows, geometry.block_size, geometry.yuv444, geometry.max_frame_bytes) : 0,
        delay,
        frame_interval,
        bitrate_kbps,
        now,
      } {
  }

  std::uint32_t lossless_refinement_t::band_count() const {
    return schedule.band_count();
  }

  void lossless_refinement_t::image_captured(const platf::img_t &img, clock::time_point now) {
    if (band_count() == 0) {
      return;
    }

    if (img.capture_sequence != 0 && img.capture_sequence == last_capture_sequence) {
      return;
    }
    const bool follows_last_capture = last_capture_sequence != 0 && img.capture_sequence == last_capture_sequence + 1;
    last_capture_sequence = img.capture_sequence;
    if (!follows_last_capture || !img.damage) {
      schedule.bands_changed(0, band_count(), now);
      return;
    }

    std::vector<lossless_block_rect_t> damaged;
    std::uint64_t pixels = 0;
    for (const auto &rect : *img.damage) {
      if (const auto blocks = lossless_blocks_for_display_rect(geometry, rect)) {
        damaged.push_back(*blocks);
        // Overlapping rectangles count twice, which errs on the side of encoding lossy
        pixels += std::uint64_t {blocks->width} * blocks->height * geometry.block_size * geometry.block_size;
      }
    }
    if (pixels <= lossless_damage_max_pixels(geometry.block_size) && schedule.damage_allowed(now)) {
      damage_blocks = std::move(damaged);
      damage_pixels = pixels;
      return;
    }

    // Too large to encode losslessly in place now, so the damaged bands wait for the delay instead
    for (const auto &blocks : damaged) {
      const auto [first_band, last_band] = lossless_bands_for_blocks(blocks, block_rows, band_count());
      schedule.bands_changed(first_band, last_band, now);
    }
  }

  lossless_frame_t lossless_refinement_t::next_frame(clock::time_point now, std::int64_t frame_index) {
    lossless_frame_t frame;
    frame.refined_band = schedule.next_band(now, frame_index);

    // Refined bands, including the one refined now, in rectangles of consecutive bands
    std::optional<std::uint32_t> first_refined;
    for (std::uint32_t band = 0; band <= band_count(); ++band) {
      if (schedule.refined(band)) {
        first_refined = first_refined.value_or(band);
      } else if (first_refined) {
        frame.blocks.push_back(band_blocks(*first_refined, band));
        first_refined.reset();
      }
    }

    // Only the band refined now and the damage cost bits worth pacing, unchanged refined bands cost almost nothing
    if (frame.refined_band || !damage_blocks.empty()) {
      const auto band = frame.refined_band ? band_blocks(*frame.refined_band, *frame.refined_band + 1) : lossless_block_rect_t {};
      paced_frame = {now, std::uint64_t {band.width} * band.height * geometry.block_size * geometry.block_size, damage_pixels};
    }
    frame.blocks.insert(frame.blocks.end(), damage_blocks.begin(), damage_blocks.end());
    damage_blocks.clear();
    damage_pixels = 0;
    return frame;
  }

  void lossless_refinement_t::frame_encoded(std::size_t frame_size) {
    if (const auto frame = std::exchange(paced_frame, std::nullopt)) {
      // Damage shares the frame with a band by area, so a band doesn't hold back damage
      const auto lossless_pixels = frame->band_pixels + frame->damage_pixels;
      const auto damage_size = lossless_pixels > 0 ? static_cast<std::size_t>(frame_size * frame->damage_pixels / lossless_pixels) : 0;
      schedule.lossless_frame_encoded(frame->time, frame_size, damage_size);
    }
  }

  void lossless_refinement_t::frames_lost(clock::time_point now) {
    schedule.frames_invalidated(std::numeric_limits<std::int64_t>::min(), now);
  }

  void lossless_refinement_t::frames_invalidated(std::int64_t first_frame, clock::time_point now) {
    schedule.frames_invalidated(first_frame, now);
  }

  lossless_refinement_t::clock::duration lossless_refinement_t::wait_time(clock::time_point now, clock::duration max_wait) const {
    return schedule.wait_time(now, max_wait);
  }

  lossless_block_rect_t lossless_refinement_t::band_blocks(std::uint32_t first_band, std::uint32_t last_band) const {
    const auto first_row = lossless_band_rows(block_rows, first_band, band_count()).first;
    const auto last_row = lossless_band_rows(block_rows, last_band - 1, band_count()).second;
    return {0, first_row, block_columns, last_row - first_row};
  }

}  // namespace video
