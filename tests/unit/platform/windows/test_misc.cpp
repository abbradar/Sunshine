/**
 * @file tests/unit/platform/windows/test_misc.cpp
 * @brief Tests for Windows thread priority mapping and thread desktops.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <array>
  #include <atomic>
  #include <format>
  #include <stdexcept>
  #include <string>
  #include <thread>
  #include <utility>

  // platform includes
  #include <Windows.h>

  // local includes
  #include <src/platform/common.h>
  #include <src/platform/windows/misc.h>
  #include <src/utility.h>

namespace {
  /**
   * @brief Create a desktop in the current window station for a test.
   *
   * @return The new desktop, or `nullptr` if the window station doesn't allow creating one.
   */
  platf::desktop_t create_test_desktop() {
    static std::atomic_int counter;
    const auto name = std::format(L"sunshine-test-{}-{}", GetCurrentProcessId(), counter++);
    return platf::desktop_t {CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr)};
  }

  /**
   * @brief Check whether a handle is still open in this process.
   *
   * @param handle Handle to check.
   * @return `true` if the handle is open.
   */
  bool is_open(HANDLE handle) {
    DWORD flags;
    return GetHandleInformation(handle, &flags);
  }

  /**
   * @brief Get the desktop of the calling thread.
   *
   * @return The thread desktop.
   */
  HDESK current_desktop() {
    return GetThreadDesktop(GetCurrentThreadId());
  }

  /**
   * @brief Create a window, which keeps the calling thread on its current desktop until it is destroyed.
   *
   * @return The new message-only window.
   */
  HWND create_window() {
    return CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
  }

  /**
   * @brief Run a function on a new thread, which has no windows that would keep it on its desktop.
   *
   * @param function Function to run.
   */
  template<class F>
  void run_on_new_thread(F &&function) {
    std::thread {std::forward<F>(function)}.join();
  }
}  // namespace

TEST(WindowsThreadPriorityTest, MapsEveryPriorityAndIgnoresUnknownValues) {
  using enum platf::thread_priority_e;
  const auto thread = GetCurrentThread();
  const auto original_priority = GetThreadPriority(thread);
  ASSERT_NE(original_priority, THREAD_PRIORITY_ERROR_RETURN);
  const auto restore_priority = util::fail_guard([thread, original_priority]() {
    EXPECT_TRUE(SetThreadPriority(thread, original_priority));
  });
  const std::array cases {
    std::pair {low, THREAD_PRIORITY_BELOW_NORMAL},
    std::pair {normal, THREAD_PRIORITY_NORMAL},
    std::pair {high, THREAD_PRIORITY_ABOVE_NORMAL},
    std::pair {critical, THREAD_PRIORITY_HIGHEST},
  };
  for (const auto &[priority, expected] : cases) {
    platf::adjust_thread_priority(priority);
    EXPECT_EQ(GetThreadPriority(thread), expected);
  }
  platf::adjust_thread_priority(static_cast<platf::thread_priority_e>(-1));
  EXPECT_EQ(GetThreadPriority(thread), THREAD_PRIORITY_HIGHEST);
}

TEST(WindowsThreadDesktopTest, RestoresOriginalDesktopBeforeClosingAttachedOne) {
  auto desktop = create_test_desktop();
  if (!desktop) {
    GTEST_SKIP() << "Can't create a desktop [0x" << util::hex(GetLastError()).to_string_view() << ']';
  }
  const auto handle = desktop.get();

  run_on_new_thread([&desktop, handle]() {
    const auto original = current_desktop();
    {
      platf::thread_desktop_t thread_desktop;
      EXPECT_EQ(thread_desktop.get(), nullptr);
      ASSERT_EQ(thread_desktop.attach(std::move(desktop)), ERROR_SUCCESS);
      EXPECT_EQ(thread_desktop.get(), handle);
      EXPECT_EQ(current_desktop(), handle);
    }
    EXPECT_EQ(current_desktop(), original);
  });

  // Windows refuses to close a desktop a thread still uses
  EXPECT_FALSE(is_open(handle));
}

TEST(WindowsThreadDesktopTest, RestoresOriginalDesktopWhenUnwinding) {
  auto desktop = create_test_desktop();
  if (!desktop) {
    GTEST_SKIP() << "Can't create a desktop [0x" << util::hex(GetLastError()).to_string_view() << ']';
  }
  const auto handle = desktop.get();

  run_on_new_thread([&desktop, handle]() {
    const auto original = current_desktop();
    EXPECT_THROW(
      {
        platf::thread_desktop_t thread_desktop;
        ASSERT_EQ(thread_desktop.attach(std::move(desktop)), ERROR_SUCCESS);
        throw std::runtime_error("unwind");
      },
      std::runtime_error
    );
    EXPECT_EQ(current_desktop(), original);
  });

  EXPECT_FALSE(is_open(handle));
}

TEST(WindowsThreadDesktopTest, ClosesDesktopThatCantBeAttached) {
  auto desktop = create_test_desktop();
  if (!desktop) {
    GTEST_SKIP() << "Can't create a desktop [0x" << util::hex(GetLastError()).to_string_view() << ']';
  }
  const auto handle = desktop.get();

  run_on_new_thread([&desktop]() {
    const auto original = current_desktop();
    const auto window = create_window();
    ASSERT_NE(window, nullptr);
    {
      platf::thread_desktop_t thread_desktop;
      EXPECT_NE(thread_desktop.attach(std::move(desktop)), ERROR_SUCCESS);
      EXPECT_EQ(thread_desktop.get(), nullptr);
      EXPECT_EQ(current_desktop(), original);
    }
    EXPECT_EQ(current_desktop(), original);
    DestroyWindow(window);
  });

  EXPECT_FALSE(is_open(handle));
}

TEST(WindowsThreadDesktopTest, ClosesReplacedDesktopAfterSwitching) {
  auto first = create_test_desktop();
  auto second = create_test_desktop();
  if (!first || !second) {
    GTEST_SKIP() << "Can't create a desktop [0x" << util::hex(GetLastError()).to_string_view() << ']';
  }
  const auto first_handle = first.get();
  const auto second_handle = second.get();

  run_on_new_thread([&first, &second, first_handle, second_handle]() {
    const auto original = current_desktop();
    {
      platf::thread_desktop_t thread_desktop;
      ASSERT_EQ(thread_desktop.attach(std::move(first)), ERROR_SUCCESS);
      ASSERT_EQ(thread_desktop.attach(std::move(second)), ERROR_SUCCESS);
      EXPECT_EQ(thread_desktop.get(), second_handle);
      EXPECT_EQ(current_desktop(), second_handle);
      EXPECT_FALSE(is_open(first_handle));
    }
    EXPECT_EQ(current_desktop(), original);
  });

  EXPECT_FALSE(is_open(second_handle));
}

TEST(WindowsThreadDesktopTest, KeepsAttachedDesktopOpenWhileThreadCantLeaveIt) {
  auto first = create_test_desktop();
  auto second = create_test_desktop();
  if (!first || !second) {
    GTEST_SKIP() << "Can't create a desktop [0x" << util::hex(GetLastError()).to_string_view() << ']';
  }
  const auto first_handle = first.get();
  const auto second_handle = second.get();

  run_on_new_thread([&first, &second, first_handle]() {
    const auto original = current_desktop();
    HWND window = nullptr;
    {
      platf::thread_desktop_t thread_desktop;
      ASSERT_EQ(thread_desktop.attach(std::move(first)), ERROR_SUCCESS);
      window = create_window();
      ASSERT_NE(window, nullptr);

      EXPECT_NE(thread_desktop.attach(std::move(second)), ERROR_SUCCESS);
      EXPECT_EQ(thread_desktop.get(), first_handle);
      EXPECT_EQ(current_desktop(), first_handle);
      EXPECT_TRUE(is_open(first_handle));
    }
    // The window keeps the thread on the attached desktop, so its handle must stay valid
    EXPECT_EQ(current_desktop(), first_handle);
    EXPECT_TRUE(is_open(first_handle));

    DestroyWindow(window);
    EXPECT_TRUE(SetThreadDesktop(original));
    EXPECT_TRUE(CloseDesktop(first_handle));
  });

  EXPECT_FALSE(is_open(second_handle));
}

TEST(WindowsThreadDesktopTest, InputDesktopGuardRestoresThreadDesktop) {
  HDESK input = nullptr;

  run_on_new_thread([&input]() {
    const auto original = current_desktop();
    {
      platf::input_desktop_guard_t guard;
      input = guard.get();
      EXPECT_EQ(current_desktop(), input ? input : original);
    }
    EXPECT_EQ(current_desktop(), original);
  });

  if (input) {
    EXPECT_FALSE(is_open(input));
  }
}

TEST(WindowsThreadDesktopTest, SyncThreadDesktopReplacesInputDesktopAndClosesItAtThreadExit) {
  HDESK first = nullptr;
  HDESK second = nullptr;

  run_on_new_thread([&first, &second]() {
    first = platf::syncThreadDesktop();
    if (!first) {
      return;
    }
    EXPECT_EQ(current_desktop(), first);

    second = platf::syncThreadDesktop();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(current_desktop(), second);
    EXPECT_FALSE(is_open(first));
  });

  if (!first) {
    GTEST_SKIP() << "Can't attach to the input desktop";
  }
  EXPECT_FALSE(is_open(second));
}
#endif
