/**
 * @file src/nvenc/nvenc_encoder.h
 * @brief Declarations for the SDK-neutral NVENC encoder interface.
 */
#pragma once

// standard includes
#include <cstdint>
#include <span>

// local includes
#include "nvenc_config.h"
#include "nvenc_encoded_frame.h"
#include "src/lossless_refinement.h"

namespace platf {
  enum class pix_fmt_e;
}

namespace video {
  struct config_t;
  enum class ref_frames_invalidation_e;
  struct sunshine_colorspace_t;
}  // namespace video

namespace nvenc {

  /**
   * @brief SDK-neutral standalone NVENC encoder interface.
   */
  class nvenc_encoder {
  public:
    /**
     * @brief Destroy the SDK-neutral encoder interface.
     */
    virtual ~nvenc_encoder() = default;

    /**
     * @brief Create the encoder.
     *
     * @param config NVENC encoder configuration.
     * @param client_config Stream configuration requested by the client.
     * @param colorspace Sunshine colorspace metadata.
     * @param buffer_format Platform-agnostic input surface format.
     * @return `true` on success, `false` on error.
     */
    virtual bool create_encoder(
      const nvenc_config &config,
      const video::config_t &client_config,
      const video::sunshine_colorspace_t &colorspace,
      platf::pix_fmt_e buffer_format
    ) = 0;

    /**
     * @brief Destroy the encoder.
     */
    virtual void destroy_encoder() = 0;

    /**
     * @brief Encode the next frame using the platform-specific input surface.
     *
     * @param frame_index Frame index that uniquely identifies the frame.
     * @param force_idr Whether to encode the frame as a forced IDR.
     * @param lossless_blocks Rectangles of blocks of `lossless_block_size()` pixels to encode losslessly,
     *        ignored when the block size is zero.
     * @return Encoded frame.
     */
    virtual nvenc_encoded_frame encode_frame(std::uint64_t frame_index, bool force_idr, std::span<const video::lossless_block_rect_t> lossless_blocks) = 0;

    /**
     * @brief Get the size of the blocks the encoder can encode losslessly.
     *
     * @return Block size in pixels, or zero when lossless refinement is disabled or unsupported.
     */
    virtual std::uint32_t lossless_block_size() const = 0;

    /**
     * @brief Invalidate reference frames in the requested range.
     *
     * @param first_frame First frame index of the invalidation range.
     * @param last_frame Last frame index of the invalidation range.
     * @return Whether the frames were invalidated, or the next frame has to be encoded with `force_idr = true`.
     */
    virtual video::ref_frames_invalidation_e invalidate_ref_frames(std::uint64_t first_frame, std::uint64_t last_frame) = 0;
  };

}  // namespace nvenc
