/**
 * @file src/lossless_refinement.h
 * @brief Declarations for encoder-independent lossless refinement of static content.
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace platf {
  struct damage_rect_t;
  struct img_t;
}  // namespace platf

namespace video {

  /**
   * @brief Worst size of losslessly coded 8-bit content in percent of its uncompressed size.
   *
   * Measured with x264 and x265 on full-range random noise: 147% for H.264 with CABAC, 156% with CAVLC, 132% for
   * HEVC 4:4:4 and 121% for HEVC 4:2:0.
   */
  constexpr std::uint64_t lossless_max_expansion_percent = 160;

  /**
   * @brief Largest mouse pointer in frame pixels whose old and new area are encoded losslessly in place.
   */
  constexpr std::uint32_t lossless_pointer_max_size = 128;

  /**
   * @brief Rectangle of whole blocks of a frame, in units of the encoder's lossless block size.
   */
  struct lossless_block_rect_t {
    std::uint32_t x;  ///< First block column.
    std::uint32_t y;  ///< First block row.
    std::uint32_t width;  ///< Number of block columns.
    std::uint32_t height;  ///< Number of block rows.

    /**
     * @brief Compare two rectangles.
     *
     * @return `true` when both rectangles cover the same blocks.
     */
    bool operator==(const lossless_block_rect_t &) const = default;
  };

  /**
   * @brief Compute the number of blocks needed to cover a frame dimension.
   *
   * @param pixels Frame dimension in pixels.
   * @param block_size Block size in pixels.
   * @return Number of blocks.
   */
  constexpr std::uint32_t lossless_block_count(std::uint32_t pixels, std::uint32_t block_size) {
    return (pixels + block_size - 1) / block_size;
  }

  /**
   * @brief Compute the largest area of blocks that damage of one frame may cover to be encoded losslessly in place.
   *
   * Covers the old and new area of a mouse pointer up to `lossless_pointer_max_size`, with the scaling margin and at
   * any block alignment.
   *
   * @param block_size Block size in pixels.
   * @return Area in pixels.
   */
  constexpr std::uint64_t lossless_damage_max_pixels(std::uint32_t block_size) {
    // One pixel of margin on both sides, and partial blocks at both ends
    const std::uint64_t side = std::uint64_t {lossless_block_count(lossless_pointer_max_size + 2 + block_size - 1, block_size)} * block_size;
    return 2 * side * side;
  }

  /**
   * @brief Compute how many horizontal bands a lossless refinement pass of an 8-bit YUV frame is split into.
   *
   * Each band fits into one frame next to the largest lossless damage, even at `lossless_max_expansion_percent`.
   *
   * @param block_columns Number of block columns in the frame.
   * @param block_rows Number of block rows in the frame.
   * @param block_size Block size in pixels.
   * @param yuv444 Whether the frame is YUV 4:4:4, otherwise YUV 4:2:0.
   * @param max_frame_bytes Largest encoded frame in bytes.
   * @return Number of bands, at least one and at most the number of block rows.
   */
  constexpr std::uint32_t lossless_band_count(std::uint32_t block_columns, std::uint32_t block_rows, std::uint32_t block_size, bool yuv444, std::uint64_t max_frame_bytes) {
    // Bytes per two pixels: one luma and one chroma pair for 4:4:4, half of that for 4:2:0
    const std::uint64_t bytes_per_two_pixels = yuv444 ? 6 : 3;
    const std::uint64_t block_row_bytes = std::uint64_t {block_columns} * block_size * block_size * bytes_per_two_pixels / 2;
    const std::uint64_t damage_max_bytes = lossless_damage_max_pixels(block_size) * bytes_per_two_pixels / 2;
    const std::uint64_t frame_max_bytes = max_frame_bytes * 100 / lossless_max_expansion_percent;
    const std::uint64_t band_max_bytes = frame_max_bytes > damage_max_bytes ? frame_max_bytes - damage_max_bytes : 0;
    const std::uint64_t rows = std::max<std::uint32_t>(block_rows, 1);
    const std::uint64_t rows_per_band = block_row_bytes == 0 ? rows : std::max<std::uint64_t>(band_max_bytes / block_row_bytes, 1);
    return static_cast<std::uint32_t>((rows + rows_per_band - 1) / rows_per_band);
  }

  /**
   * @brief Compute the block rows covered by one lossless refinement band.
   *
   * Bands cover all rows without gaps or overlaps and differ in height by at most one row.
   *
   * @param block_rows Number of block rows in the frame.
   * @param band Zero-based band index.
   * @param band_count Number of bands in the refinement pass.
   * @return Half-open range of block rows.
   */
  constexpr std::pair<std::uint32_t, std::uint32_t> lossless_band_rows(std::uint32_t block_rows, std::uint32_t band, std::uint32_t band_count) {
    const auto row = [block_rows, band_count](std::uint64_t index) {
      return static_cast<std::uint32_t>(index * block_rows / band_count);
    };
    return {row(band), row(band + 1)};
  }

  /**
   * @brief Find the lossless refinement bands that contain the rows of a block rectangle.
   *
   * @param blocks Block rectangle inside the frame.
   * @param block_rows Number of block rows in the frame.
   * @param band_count Number of bands in the refinement pass.
   * @return Half-open range of bands, empty when the rectangle has no rows or there are no bands.
   */
  constexpr std::pair<std::uint32_t, std::uint32_t> lossless_bands_for_blocks(const lossless_block_rect_t &blocks, std::uint32_t block_rows, std::uint32_t band_count) {
    if (blocks.height == 0 || band_count == 0 || blocks.y >= block_rows) {
      return {0, 0};
    }
    // The last band that starts at or before the row, see lossless_band_rows()
    const auto band_of_row = [block_rows, band_count](std::uint64_t row) {
      return static_cast<std::uint32_t>(((row + 1) * band_count + block_rows - 1) / block_rows - 1);
    };
    const auto last_row = std::min(blocks.y + blocks.height, block_rows) - 1;
    return {band_of_row(blocks.y), band_of_row(last_row) + 1};
  }

  /**
   * @brief Sizes lossless refinement works with.
   */
  struct lossless_geometry_t {
    std::int32_t display_width;  ///< Captured display width in pixels.
    std::int32_t display_height;  ///< Captured display height in pixels.
    std::int32_t frame_width;  ///< Encoded frame width in pixels.
    std::int32_t frame_height;  ///< Encoded frame height in pixels.
    std::uint32_t block_size;  ///< Size of the blocks the encoder can encode losslessly, zero when it can't.
    bool yuv444;  ///< Whether frames are YUV 4:4:4, otherwise YUV 4:2:0.
    std::uint64_t max_frame_bytes;  ///< Largest encoded frame in bytes the stream protects with FEC.
  };

  /**
   * @brief Find the blocks of the encoded frame that cover a rectangle of the captured display.
   *
   * Maps coordinates the way encode devices scale the display into the frame: preserving the aspect ratio and
   * centered. A margin of one display pixel covers the spread of linear filtering when scaling.
   *
   * @param geometry Display, frame and block sizes.
   * @param rect Rectangle in display pixels.
   * @return Covered blocks, or `std::nullopt` when the rectangle is empty or doesn't map to any frame pixel.
   */
  std::optional<lossless_block_rect_t> lossless_blocks_for_display_rect(const lossless_geometry_t &geometry, const platf::damage_rect_t &rect);

  /**
   * @brief Fill a per-block map of an encoder, e.g. a QP delta map, from lossless block rectangles.
   *
   * @tparam T Type of the map values.
   * @param map Map in raster scan order, one value per block.
   * @param map_width Number of block columns.
   * @param blocks Block rectangles to encode losslessly, clipped to the map.
   * @param lossless_value Value for blocks inside the rectangles, all other blocks get `T {}`.
   */
  template<class T>
  void fill_lossless_block_map(std::span<T> map, std::uint32_t map_width, std::span<const lossless_block_rect_t> blocks, T lossless_value) {
    std::ranges::fill(map, T {});
    if (map_width == 0) {
      return;
    }

    const auto map_height = static_cast<std::uint32_t>(map.size() / map_width);
    for (const auto &rect : blocks) {
      const auto last_column = std::min<std::uint64_t>(std::uint64_t {rect.x} + rect.width, map_width);
      const auto last_row = std::min<std::uint64_t>(std::uint64_t {rect.y} + rect.height, map_height);
      for (std::uint64_t row = rect.y; row < last_row; ++row) {
        for (std::uint64_t column = rect.x; column < last_column; ++column) {
          map[row * map_width + column] = lossless_value;
        }
      }
    }
  }

  /**
   * @brief Schedules lossless refinement of static content in horizontal bands.
   *
   * A band is refined once it hasn't changed for the configured delay, one band per frame and round-robin, so changes
   * that recur faster than a whole pass still let every band be refined in turn. Frames with lossless bands or damage
   * are paced by the stream bitrate, and bands lost with frames the client couldn't decode are refined again after an
   * exponential backoff.
   */
  class lossless_band_schedule_t {
  public:
    using clock = std::chrono::steady_clock;  ///< Clock used for scheduling.

    /**
     * @brief Create a refinement schedule that treats `now` as the time of the last content change.
     *
     * @param band_count Number of bands per refinement pass, zero disables refinement.
     * @param delay Time without new content before refinement starts.
     * @param frame_interval Stream frame interval, the minimum time between two bands.
     * @param bitrate_kbps Stream bitrate in kilobits per second that refinement may use on average.
     * @param now Current time.
     */
    lossless_band_schedule_t(std::uint32_t band_count, clock::duration delay, clock::duration frame_interval, int bitrate_kbps, clock::time_point now);

    /**
     * @brief Get the number of bands per refinement pass.
     *
     * @return Number of bands, zero when refinement is disabled.
     */
    std::uint32_t band_count() const;

    /**
     * @brief Record that new content has been captured in some bands, so they have to be encoded again after the delay.
     *
     * @param first_band First changed band.
     * @param last_band Band after the last changed band.
     * @param now Current time.
     */
    void bands_changed(std::uint32_t first_band, std::uint32_t last_band, clock::time_point now);

    /**
     * @brief Record that the client invalidated frames, so bands encoded in them have to be encoded again after a backoff.
     *
     * @param first_frame First invalidated frame, it and all later frames are lost.
     * @param now Current time.
     */
    void frames_invalidated(std::int64_t first_frame, clock::time_point now);

    /**
     * @brief Check whether a band was encoded losslessly since its last change.
     *
     * @param band Zero-based band index.
     * @return `true` when the band is refined, `false` when it's pending or out of range.
     */
    bool refined(std::uint32_t band) const;

    /**
     * @brief Compute how long the encoder may wait for new content before encoding the next frame.
     *
     * @param now Current time.
     * @param max_wait Longest wait allowed by the minimum framerate.
     * @return Time until the next refinement band is due, capped by `max_wait`, and by the frame interval while
     *         the last band or lossless frame is being paced.
     */
    clock::duration wait_time(clock::time_point now, clock::duration max_wait) const;

    /**
     * @brief Check whether the bitrate allows encoding damage losslessly in the next frame.
     *
     * @param now Current time.
     * @return `true` when the damage of earlier frames has been paced, bands don't count, and no loss backoff is
     *         pending.
     */
    bool damage_allowed(clock::time_point now) const;

    /**
     * @brief Get the band to encode losslessly in the next frame and mark it as encoded.
     *
     * @param now Current time.
     * @param frame_index Index of the frame the band will be encoded in.
     * @return Zero-based band index, or `std::nullopt` when no band is due.
     */
    std::optional<std::uint32_t> next_band(clock::time_point now, std::int64_t frame_index);

    /**
     * @brief Pace further bands and damage after the size of a frame with lossless areas is known.
     *
     * @param frame_time Time the frame was encoded.
     * @param frame_size Size of the encoded frame in bytes, which paces further bands.
     * @param damage_size Part of the frame size spent on lossless damage, which paces further damage.
     */
    void lossless_frame_encoded(clock::time_point frame_time, std::size_t frame_size, std::size_t damage_size);

  private:
    /**
     * @brief Refinement state of one band: the frame that refined it since its last change, if any, and when it's due.
     */
    struct band_t {
      std::optional<std::int64_t> frame;
      clock::time_point not_before;
    };

    clock::duration delay;
    clock::duration frame_interval;
    int bitrate_kbps;
    std::vector<band_t> bands;
    std::uint32_t next_band_index = 0;
    clock::time_point paced_until;
    clock::time_point damage_paced_until;
    clock::time_point backoff_until;
    clock::time_point last_loss_time;
    clock::duration loss_backoff = clock::duration::zero();
  };

  /**
   * @brief Areas of one frame to encode losslessly.
   */
  struct lossless_frame_t {
    std::vector<lossless_block_rect_t> blocks;  ///< Block rectangles to encode losslessly.
    std::optional<std::uint32_t> refined_band;  ///< Band refined in this frame for the first time since its last change.
  };

  /**
   * @brief Encoder-independent lossless refinement of static content for one encode session.
   *
   * Decides which blocks of every frame the encoder codes losslessly, so the encoder only has to force lossless coding
   * of block rectangles:
   *
   * - Unchanged bands are refined one per frame, see `lossless_band_schedule_t`.
   * - Refined bands stay lossless in every later frame. Unchanged blocks then cost almost nothing, and in-loop
   *   filters like HEVC's SAO and deblocking, which skip lossless blocks, can't alter them next to new content.
   * - Small damage, like the old and new area of a moving mouse pointer, is encoded losslessly in the frame that
   *   carries it, so its bands stay refined. Larger damage, or damage while the bitrate is used up or after a loss,
   *   makes its bands wait for the refinement delay.
   * - Losses make the bands refined in lost frames refine again. Bands refined before stay lossless, so the next frame
   *   recodes their lost damage losslessly.
   *
   * Forced IDR frames follow `frames_lost()`, whose backoff keeps them from refining a band.
   */
  class lossless_refinement_t {
  public:
    using clock = lossless_band_schedule_t::clock;  ///< Clock used for scheduling.

    /**
     * @brief Create the refinement of a session that treats `now` as the time of the last content change.
     *
     * @param geometry Display, frame and block sizes. A block size of zero disables refinement.
     * @param delay Time without new content before refinement starts.
     * @param frame_interval Stream frame interval, the minimum time between two bands.
     * @param bitrate_kbps Stream bitrate in kilobits per second that refinement may use on average.
     * @param now Current time.
     */
    lossless_refinement_t(const lossless_geometry_t &geometry, clock::duration delay, clock::duration frame_interval, int bitrate_kbps, clock::time_point now);

    /**
     * @brief Get the number of bands per refinement pass.
     *
     * @return Number of bands, zero when refinement is disabled.
     */
    std::uint32_t band_count() const;

    /**
     * @brief Record what a newly captured image changed.
     *
     * Damage is relative to the previous capture, so it's only used when the session converted that one. An image
     * delivered again unchanged changes nothing. Call it at most once before each `next_frame()`.
     *
     * @param img Newly captured image.
     * @param now Current time.
     */
    void image_captured(const platf::img_t &img, clock::time_point now);

    /**
     * @brief Get the areas to encode losslessly in the next frame.
     *
     * Call `frame_encoded()` once the frame is encoded.
     *
     * @param now Current time.
     * @param frame_index Index of the frame.
     * @return Lossless areas, empty when refinement is disabled.
     */
    lossless_frame_t next_frame(clock::time_point now, std::int64_t frame_index);

    /**
     * @brief Record that the frame from the last `next_frame()` call was encoded, which paces the next band.
     *
     * @param frame_size Size of the encoded frame in bytes.
     */
    void frame_encoded(std::size_t frame_size);

    /**
     * @brief Record that the client lost all reference frames, so every band has to be encoded again after a backoff.
     *
     * @param now Current time.
     */
    void frames_lost(clock::time_point now);

    /**
     * @brief Record that the encoder invalidated frames, so bands encoded in them have to be encoded again after a backoff.
     *
     * @param first_frame First invalidated frame, it and all later frames are lost.
     * @param now Current time.
     */
    void frames_invalidated(std::int64_t first_frame, clock::time_point now);

    /**
     * @brief Compute how long the encoder may wait for new content before encoding the next frame.
     *
     * @param now Current time.
     * @param max_wait Longest wait allowed by the minimum framerate.
     * @return Time to wait, at most `max_wait`.
     */
    clock::duration wait_time(clock::time_point now, clock::duration max_wait) const;

  private:
    /**
     * @brief Get the blocks of a range of bands.
     *
     * @param first_band First band.
     * @param last_band Band after the last band.
     * @return Full-width block rectangle.
     */
    lossless_block_rect_t band_blocks(std::uint32_t first_band, std::uint32_t last_band) const;

    /**
     * @brief Frame being encoded with lossless areas to pace.
     */
    struct paced_frame_t {
      clock::time_point time;
      std::uint64_t band_pixels;
      std::uint64_t damage_pixels;
    };

    lossless_geometry_t geometry;
    std::uint32_t block_columns;
    std::uint32_t block_rows;
    lossless_band_schedule_t schedule;
    std::uint64_t last_capture_sequence = 0;
    std::vector<lossless_block_rect_t> damage_blocks;
    std::uint64_t damage_pixels = 0;
    std::optional<paced_frame_t> paced_frame;
  };

}  // namespace video
