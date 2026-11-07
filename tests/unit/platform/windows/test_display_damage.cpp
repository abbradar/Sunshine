/**
 * @file tests/unit/platform/windows/test_display_damage.cpp
 * @brief Test src/platform/windows/display_vram.cpp damage mapping.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <tuple>

  // local includes
  #include <src/platform/windows/display.h>

using DisplayRectFromSurfaceParam = std::tuple<DXGI_MODE_ROTATION, int, int, platf::damage_rect_t>;

/**
 * @brief Test mapping Desktop Duplication surface rectangles to rotated displays.
 */
struct DisplayRectFromSurfaceTest: testing::TestWithParam<DisplayRectFromSurfaceParam> {};

TEST_P(DisplayRectFromSurfaceTest, RotatesLikeTheDisplay) {
  const auto &[rotation, display_width, display_height, expected] = GetParam();
  EXPECT_EQ(expected, platf::dxgi::display_rect_from_surface({10, 20, 50, 100}, rotation, display_width, display_height));
}

INSTANTIATE_TEST_SUITE_P(
  DisplayRectFromSurface,
  DisplayRectFromSurfaceTest,
  testing::Values(
    DisplayRectFromSurfaceParam {DXGI_MODE_ROTATION_IDENTITY, 1920, 1080, {10, 20, 40, 80}},
    DisplayRectFromSurfaceParam {DXGI_MODE_ROTATION_ROTATE90, 1080, 1920, {980, 10, 80, 40}},
    DisplayRectFromSurfaceParam {DXGI_MODE_ROTATION_ROTATE180, 1920, 1080, {1870, 980, 40, 80}},
    DisplayRectFromSurfaceParam {DXGI_MODE_ROTATION_ROTATE270, 1080, 1920, {20, 1870, 80, 40}}
  )
);
#endif
