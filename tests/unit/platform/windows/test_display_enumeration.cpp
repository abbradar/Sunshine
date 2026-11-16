/**
 * @file tests/unit/platform/windows/test_display_enumeration.cpp
 * @brief Tests for selecting the displays listed for capture on Windows.
 */

#ifdef _WIN32
  // test includes
  #include "../../../tests_common.h"
  #include "fake_dxgi.h"

  // standard includes
  #include <string>
  #include <vector>

  // local includes
  #include <src/config.h>
  #include <src/platform/windows/display.h>
  #include <src/utility.h>

using namespace std::literals;

namespace {
  /**
   * @brief Set the configured capture adapter for the lifetime of the object.
   */
  class adapter_name_override_t {
  public:
    /**
     * @brief Replace the configured capture adapter.
     *
     * @param name Adapter name to configure.
     */
    explicit adapter_name_override_t(const std::string &name):
        previous {config::video.adapter_name} {
      config::video.adapter_name = name;
    }

    /**
     * @brief Restore the previous capture adapter.
     */
    ~adapter_name_override_t() {
      config::video.adapter_name = previous;
    }

    adapter_name_override_t(const adapter_name_override_t &) = delete;
    adapter_name_override_t &operator=(const adapter_name_override_t &) = delete;

  private:
    std::string previous;  ///< Capture adapter configured before the override.
  };

  /**
   * @brief Capture probe that accepts every output except the rejected one and records what it checked.
   */
  struct probe_t {
    std::wstring rejected;  ///< Device name of the output that can't be captured.
    std::vector<std::wstring> probed;  ///< Device names of the checked outputs, in order.

    /**
     * @brief Get the probe as the function enumeration takes.
     *
     * @return Function recording each checked output.
     */
    platf::dxgi::output_probe_t function() {
      return [this](platf::dxgi::adapter_t &, platf::dxgi::output_t &output) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);
        probed.emplace_back(desc.DeviceName);
        return probed.back() != rejected;
      };
    }
  };
}  // namespace

TEST(DisplayEnumerationTest, ListsOnlyDisplaysOfConfiguredAdapter) {
  fake_dxgi::output_t output_a {LR"(\\.\DISPLAY1)"};
  fake_dxgi::output_t output_b {LR"(\\.\DISPLAY2)"};
  fake_dxgi::adapter_t adapter_a {L"Adapter A", {&output_a}};
  fake_dxgi::adapter_t adapter_b {L"Adapter B", {&output_b}};
  fake_dxgi::factory_t factory {{&adapter_a, &adapter_b}};
  const adapter_name_override_t adapter_name {"Adapter B"};
  probe_t probe;

  const auto names = platf::dxgi::enum_display_names(&factory, probe.function());

  EXPECT_EQ(names, std::vector {R"(\\.\DISPLAY2)"s});
  EXPECT_EQ(adapter_a.enum_outputs_calls, 0u);
  EXPECT_EQ(probe.probed, std::vector {LR"(\\.\DISPLAY2)"s});
  for (const auto *object : {&adapter_a.references, &adapter_b.references, &output_a.references, &output_b.references}) {
    EXPECT_EQ(*object, 1u);
  }
}

TEST(DisplayEnumerationTest, ListsDisplaysOfAllAdaptersWithoutConfiguredAdapter) {
  fake_dxgi::output_t output_a {LR"(\\.\DISPLAY1)"};
  fake_dxgi::output_t output_b {LR"(\\.\DISPLAY2)"};
  fake_dxgi::adapter_t adapter_a {L"Adapter A", {&output_a}};
  fake_dxgi::adapter_t adapter_b {L"Adapter B", {&output_b}};
  fake_dxgi::factory_t factory {{&adapter_a, &adapter_b}};
  const adapter_name_override_t adapter_name {""};
  probe_t probe;

  const auto names = platf::dxgi::enum_display_names(&factory, probe.function());

  EXPECT_EQ(names, (std::vector {R"(\\.\DISPLAY1)"s, R"(\\.\DISPLAY2)"s}));
}

TEST(DisplayEnumerationTest, ListsNothingForUnknownAdapter) {
  fake_dxgi::output_t output_a {LR"(\\.\DISPLAY1)"};
  fake_dxgi::adapter_t adapter_a {L"Adapter A", {&output_a}};
  fake_dxgi::factory_t factory {{&adapter_a}};
  const adapter_name_override_t adapter_name {"Adapter C"};
  probe_t probe;

  const auto names = platf::dxgi::enum_display_names(&factory, probe.function());

  EXPECT_TRUE(names.empty());
  EXPECT_EQ(adapter_a.enum_outputs_calls, 0u);
  EXPECT_TRUE(probe.probed.empty());
}

TEST(DisplayEnumerationTest, ListsOnlyCapturableDisplaysOfConfiguredAdapter) {
  fake_dxgi::output_t detached {LR"(\\.\DISPLAY1)", false};
  fake_dxgi::output_t rejected {LR"(\\.\DISPLAY2)"};
  fake_dxgi::output_t capturable {LR"(\\.\DISPLAY3)"};
  fake_dxgi::adapter_t adapter {L"Adapter B", {&detached, &rejected, &capturable}};
  fake_dxgi::factory_t factory {{&adapter}};
  const adapter_name_override_t adapter_name {"Adapter B"};
  probe_t probe {LR"(\\.\DISPLAY2)"};

  const auto names = platf::dxgi::enum_display_names(&factory, probe.function());

  EXPECT_EQ(names, std::vector {R"(\\.\DISPLAY3)"s});
  EXPECT_EQ(probe.probed, (std::vector {LR"(\\.\DISPLAY2)"s, LR"(\\.\DISPLAY3)"s}));
}
#endif
