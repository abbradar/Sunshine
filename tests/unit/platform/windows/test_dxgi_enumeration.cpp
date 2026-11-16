/**
 * @file tests/unit/platform/windows/test_dxgi_enumeration.cpp
 * @brief Tests for DXGI adapter and output enumeration failures.
 */

#ifdef _WIN32
  // test includes
  #include "../../../tests_common.h"
  #include "fake_dxgi.h"

  // standard includes
  #include <initializer_list>
  #include <string>
  #include <vector>

  // local includes
  #include <src/platform/windows/display.h>
  #include <src/platform/windows/dxgi_enumeration.h>

using namespace std::literals;

namespace {
  /**
   * @brief Capture probe that accepts every output.
   *
   * @return Whether the output can be captured.
   */
  bool can_capture(platf::dxgi::adapter_t &, platf::dxgi::output_t &) {
    return true;
  }

  /**
   * @brief Check that enumeration released every reference it took.
   *
   * @param objects Reference counts of the substitutes.
   */
  void expect_released(std::initializer_list<const ULONG *> objects) {
    for (const auto *references : objects) {
      EXPECT_EQ(*references, 1u);
    }
  }
}  // namespace

TEST(DxgiEnumerationTest, VisitsEveryAdapterUntilNotFound) {
  fake_dxgi::adapter_t first {L"First"};
  fake_dxgi::adapter_t second {L"Second"};
  fake_dxgi::factory_t factory {{&first, &second}};
  std::vector<IDXGIAdapter1 *> visited;

  const auto status = platf::dxgi::for_each_adapter<platf::dxgi::adapter_t>(&factory, [&visited](platf::dxgi::adapter_t &adapter) {
    visited.push_back(adapter.get());
    return true;
  });

  EXPECT_EQ(status, S_OK);
  EXPECT_EQ(visited, (std::vector<IDXGIAdapter1 *> {&first, &second}));
  expect_released({&first.references, &second.references});
}

TEST(DxgiEnumerationTest, StopsAdapterEnumerationWhenCallbackDeclines) {
  fake_dxgi::adapter_t first {L"First"};
  fake_dxgi::adapter_t second {L"Second"};
  fake_dxgi::factory_t factory {{&first, &second}};
  int calls = 0;

  const auto status = platf::dxgi::for_each_adapter<platf::dxgi::adapter_t>(&factory, [&calls](platf::dxgi::adapter_t &) {
    ++calls;
    return false;
  });

  EXPECT_EQ(status, S_OK);
  EXPECT_EQ(calls, 1);
  expect_released({&first.references, &second.references});
}

TEST(DxgiEnumerationTest, StopsAdapterEnumerationOnFailure) {
  fake_dxgi::adapter_t first {L"First"};
  fake_dxgi::factory_t factory {{&first}};
  factory.adapters_end = E_FAIL;
  int calls = 0;

  const auto status = platf::dxgi::for_each_adapter<platf::dxgi::adapter_t>(&factory, [&calls](platf::dxgi::adapter_t &) {
    ++calls;
    return true;
  });

  EXPECT_EQ(status, E_FAIL);
  EXPECT_EQ(calls, 1);
  expect_released({&first.references});
}

TEST(DxgiEnumerationTest, StopsAdapterEnumerationOnFirstFailure) {
  fake_dxgi::factory_t factory;
  factory.adapters_end = E_FAIL;
  int calls = 0;

  const auto status = platf::dxgi::for_each_adapter<platf::dxgi::adapter_t>(&factory, [&calls](platf::dxgi::adapter_t &) {
    ++calls;
    return true;
  });

  EXPECT_EQ(status, E_FAIL);
  EXPECT_EQ(calls, 0);
}

TEST(DxgiEnumerationTest, VisitsEveryOutputUntilNotFound) {
  fake_dxgi::output_t first {LR"(\\.\DISPLAY1)"};
  fake_dxgi::output_t second {LR"(\\.\DISPLAY2)"};
  fake_dxgi::adapter_t adapter {L"Adapter", {&first, &second}};
  std::vector<IDXGIOutput *> visited;

  const auto status = platf::dxgi::for_each_output<platf::dxgi::output_t>(&adapter, [&visited](platf::dxgi::output_t &output) {
    visited.push_back(output.get());
    return true;
  });

  EXPECT_EQ(status, S_OK);
  EXPECT_EQ(visited, (std::vector<IDXGIOutput *> {&first, &second}));
  expect_released({&first.references, &second.references});
}

TEST(DxgiEnumerationTest, StopsOutputEnumerationWhenCallbackDeclines) {
  fake_dxgi::output_t first {LR"(\\.\DISPLAY1)"};
  fake_dxgi::output_t second {LR"(\\.\DISPLAY2)"};
  fake_dxgi::adapter_t adapter {L"Adapter", {&first, &second}};

  const auto status = platf::dxgi::for_each_output<platf::dxgi::output_t>(&adapter, [](platf::dxgi::output_t &) {
    return false;
  });

  EXPECT_EQ(status, S_OK);
  EXPECT_EQ(adapter.enum_outputs_calls, 1u);
  expect_released({&first.references, &second.references});
}

TEST(DxgiEnumerationTest, StopsOutputEnumerationOnFirstFailure) {
  fake_dxgi::adapter_t adapter {L"Adapter"};
  adapter.outputs_end = DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  int calls = 0;

  const auto status = platf::dxgi::for_each_output<platf::dxgi::output_t>(&adapter, [&calls](platf::dxgi::output_t &) {
    ++calls;
    return true;
  });

  EXPECT_EQ(status, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(adapter.enum_outputs_calls, 1u);
}

TEST(DxgiEnumerationTest, ListsDisplaysOfEveryAdapter) {
  fake_dxgi::output_t primary {LR"(\\.\DISPLAY1)"};
  fake_dxgi::output_t secondary {LR"(\\.\DISPLAY2)"};
  fake_dxgi::adapter_t first {L"First", {&primary}};
  fake_dxgi::adapter_t second {L"Second", {&secondary}};
  fake_dxgi::factory_t factory {{&first, &second}};

  const auto names = platf::dxgi::enum_display_names(&factory, can_capture);

  EXPECT_EQ(names, (std::vector {R"(\\.\DISPLAY1)"s, R"(\\.\DISPLAY2)"s}));
  expect_released({&first.references, &second.references, &primary.references, &secondary.references});
}

TEST(DxgiEnumerationTest, ListsNothingWhenAdapterEnumerationFailsAfterDisplay) {
  fake_dxgi::output_t primary {LR"(\\.\DISPLAY1)"};
  fake_dxgi::adapter_t first {L"First", {&primary}};
  fake_dxgi::factory_t factory {{&first}};
  factory.adapters_end = E_FAIL;

  const auto names = platf::dxgi::enum_display_names(&factory, can_capture);

  EXPECT_TRUE(names.empty());
  expect_released({&first.references, &primary.references});
}

TEST(DxgiEnumerationTest, ListsNothingWhenOutputEnumerationFailsAfterDisplay) {
  fake_dxgi::output_t primary {LR"(\\.\DISPLAY1)"};
  fake_dxgi::output_t secondary {LR"(\\.\DISPLAY2)"};
  fake_dxgi::adapter_t first {L"First", {&primary}};
  fake_dxgi::adapter_t second {L"Second", {&secondary}};
  first.outputs_end = E_FAIL;
  fake_dxgi::factory_t factory {{&first, &second}};

  const auto names = platf::dxgi::enum_display_names(&factory, can_capture);

  EXPECT_TRUE(names.empty());
  EXPECT_EQ(second.enum_outputs_calls, 0u);
  expect_released({&first.references, &second.references, &primary.references, &secondary.references});
}

TEST(DxgiEnumerationTest, ListsNothingWhenLaterAdapterOutputsFail) {
  fake_dxgi::output_t primary {LR"(\\.\DISPLAY1)"};
  fake_dxgi::adapter_t first {L"First", {&primary}};
  fake_dxgi::adapter_t second {L"Second"};
  second.outputs_end = DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
  fake_dxgi::factory_t factory {{&first, &second}};

  const auto names = platf::dxgi::enum_display_names(&factory, can_capture);

  EXPECT_TRUE(names.empty());
  expect_released({&first.references, &second.references, &primary.references});
}

TEST(DxgiEnumerationTest, ListsNothingWhenFirstAdapterEnumerationFails) {
  fake_dxgi::factory_t factory;
  factory.adapters_end = E_FAIL;

  EXPECT_TRUE(platf::dxgi::enum_display_names(&factory, can_capture).empty());
}
#endif
