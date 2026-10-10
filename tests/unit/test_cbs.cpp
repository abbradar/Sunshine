/**
 * @file tests/unit/test_cbs.cpp
 * @brief Test src/cbs.*.
 */
// test includes
#include "../tests_common.h"

// standard includes
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include <src/cbs.h>
#include <src/utility.h>

namespace {

  // Parameter sets that NVENC (Tesla T4, driver 535) produced for a 1920x1088 HEVC stream, and its PPS after a round
  // trip through lossless tuning
  constexpr std::string_view hevc_vps = "0000000140010c01ffff01400000030090000003000003007b970240";
  constexpr std::string_view hevc_sps = "0000000142010101400000030090000003000003007ba003c0801105965d29084645d50c0404000003000400000300f020";
  constexpr std::string_view hevc_pps = "000000014401c0f7c0cc90";
  constexpr std::string_view hevc_pps_bypass = "000000014401c0f7c4cc90";

  // SPS that NVENC (Tesla T4, driver 535) produced for a 1920x1080 H.264 YUV 4:4:4 stream with and without lossless
  // refinement
  constexpr std::string_view h264_sps_bypass = "0000000167f4003291c56601e0089f89c05a808080a000007d00003a9811e3854d";
  constexpr std::string_view h264_sps = "0000000167f4003291856601e0089f89c05a808080a000007d00003a9811e3854d";

  /**
   * @brief Decode hexadecimal NAL units into one byte stream.
   *
   * @param nal_units NAL units in hexadecimal digits.
   * @return Byte stream.
   */
  std::vector<std::uint8_t> from_hex(std::initializer_list<std::string_view> nal_units) {
    std::string hex;
    for (const auto nal : nal_units) {
      hex += nal;
    }
    const auto bytes = util::from_hex_vec(hex, true);
    return {bytes.begin(), bytes.end()};
  }

}  // namespace

TEST(HevcTransquantBypassTests, ReadsFlagFromPps) {
  EXPECT_TRUE(cbs::hevc_transquant_bypass_enabled(from_hex({hevc_vps, hevc_sps, hevc_pps_bypass})));
  EXPECT_FALSE(cbs::hevc_transquant_bypass_enabled(from_hex({hevc_vps, hevc_sps, hevc_pps})));
}

TEST(HevcTransquantBypassTests, FalseWithoutReadablePps) {
  EXPECT_FALSE(cbs::hevc_transquant_bypass_enabled(from_hex({hevc_vps, hevc_sps})));
  // The PPS can't be read without its SPS
  EXPECT_FALSE(cbs::hevc_transquant_bypass_enabled(from_hex({hevc_pps_bypass})));
}

TEST(H264TransformBypassTests, ReadsFlagFromSps) {
  EXPECT_TRUE(cbs::h264_transform_bypass_enabled(from_hex({h264_sps_bypass})));
  EXPECT_FALSE(cbs::h264_transform_bypass_enabled(from_hex({h264_sps})));
}

TEST(H264TransformBypassTests, FalseForTruncatedSps) {
  EXPECT_FALSE(cbs::h264_transform_bypass_enabled(from_hex({h264_sps_bypass.substr(0, 16)})));
}
