/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>

// platform includes
#include <Windows.h>
#include <winnt.h>

// local includes
#include "src/utility.h"

namespace platf {
  /**
   * @brief Write status details to the log.
   *
   * @param prefix Text prefix used when formatting the message.
   * @param status Native status code returned by the platform API.
   */
  void print_status(const std::string_view &prefix, HRESULT status);
  /**
   * @brief Owning handle of a desktop.
   */
  using desktop_t = util::safe_ptr_v2<std::remove_pointer_t<HDESK>, BOOL, &CloseDesktop>;

  /**
   * @brief Desktop the calling thread is attached to while the object lives.
   *
   * Windows can't close a desktop handle while a thread of the process uses it, so the attached desktop stays open
   * until the thread moves to another one. Destroying the object returns the thread to the desktop it used when
   * the object was created and closes the attached desktop. The object must be used and destroyed on one thread.
   */
  class thread_desktop_t {
  public:
    /**
     * @brief Remember the current desktop of the calling thread.
     */
    thread_desktop_t();

    /**
     * @brief Return the thread to its original desktop and close the attached one.
     *
     * If the thread can't be returned, for example because it has created windows on the attached desktop, it
     * stays there and the attached desktop is left open.
     */
    ~thread_desktop_t();

    thread_desktop_t(const thread_desktop_t &) = delete;
    thread_desktop_t &operator=(const thread_desktop_t &) = delete;

    /**
     * @brief Attach the calling thread to a desktop and close the previously attached one.
     *
     * @param desktop Desktop to attach. It is closed if the thread can't be attached to it.
     * @return `ERROR_SUCCESS`, or the `SetThreadDesktop()` error if the thread stays on its current desktop.
     */
    DWORD attach(desktop_t desktop);

    /**
     * @brief Get the attached desktop.
     *
     * @return The attached desktop, or `nullptr` if none is attached.
     */
    HDESK get();

  private:
    HDESK original_desktop;  ///< Desktop the thread used before. GetThreadDesktop() handles need no closing.
    desktop_t attached_desktop;  ///< Desktop the thread is attached to through this object.
  };

  /**
   * @brief Open the desktop that receives user input, which is the secure desktop while the session is locked.
   *
   * @return The input desktop, or `nullptr` with the error in `GetLastError()`.
   */
  desktop_t open_input_desktop();

  /**
   * @brief Attach the calling thread to the input desktop.
   *
   * The thread stays there until the next call, and returns to its original desktop when it exits.
   *
   * @return The attached input desktop, or `nullptr` on failure.
   */
  HDESK syncThreadDesktop();

  /**
   * @brief Build a command that delegates URL activation to the Windows shell.
   *
   * @param command_processor Absolute path to the system command processor.
   * @param url URL to pass unchanged to the shell's default protocol handler.
   * @return Command line suitable for CreateProcessW().
   */
  std::wstring make_url_launch_command(const std::wstring &command_processor, const std::wstring &url);

  /**
   * @brief Attach the calling thread to the input desktop until destroyed.
   *
   * The display configuration API only works on threads attached to the input desktop. The thread stays on its
   * current desktop if the input desktop can't be opened or attached.
   */
  class input_desktop_guard_t: public thread_desktop_t {
  public:
    /**
     * @brief Attach the calling thread to the input desktop if possible.
     */
    input_desktop_guard_t();
  };

  /**
   * @brief Read the current Windows high-resolution performance counter.
   *
   * @return Raw QPC tick value from `QueryPerformanceCounter`.
   */
  int64_t qpc_counter();

  /**
   * @brief Convert the difference between two QPC readings to nanoseconds.
   *
   * @param performance_counter1 Newer performance-counter reading.
   * @param performance_counter2 Older performance-counter reading.
   * @return Duration represented by the difference between two QPC values.
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);
}  // namespace platf
