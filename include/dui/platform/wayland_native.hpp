#pragma once

#include "dui/host.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace dui::platform {

class WaylandWindow;

struct WaylandGlobals {
  std::uint32_t compositor_version{};
  std::uint32_t shm_version{};
  std::uint32_t seat_version{};
  std::uint32_t xdg_wm_base_version{};
  std::uint32_t viewporter_version{};
  std::uint32_t fractional_scale_version{};

  [[nodiscard]] constexpr bool window_ready() const noexcept {
    return compositor_version != 0 && shm_version != 0 && xdg_wm_base_version != 0;
  }
  friend constexpr bool operator==(WaylandGlobals, WaylandGlobals) = default;
};

class WaylandConnection {
public:
  static std::unique_ptr<WaylandConnection> connect(std::string_view display_name = {});

  ~WaylandConnection();
  WaylandConnection(const WaylandConnection&) = delete;
  WaylandConnection& operator=(const WaylandConnection&) = delete;

  // The connection and all methods remain on the thread that called connect().
  // Destroy every child window first. Violating either lifetime or thread
  // affinity terminates rather than releasing live proxies unsafely.
  [[nodiscard]] std::shared_ptr<TaskRunner> task_runner() const;
  void run_pending();
  void dispatch();
  void roundtrip();
  [[nodiscard]] WaylandGlobals globals() const;

private:
  friend class WaylandWindow;
  struct Impl;
  explicit WaylandConnection(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

struct WaylandWindowStatus {
  bool configured{};
  std::uint64_t committed_buffers{};
  bool frame_callback_pending{};
};

class WaylandWindow {
public:
  static std::unique_ptr<WaylandWindow> create(WaylandConnection& connection, WindowId id,
                                               WindowConfiguration configuration,
                                               HostErrorHandler error_handler = {});

  // Like the connection, the window must be destroyed on its owner thread and
  // before its connection; violating this affinity terminates during cleanup.
  ~WaylandWindow();
  WaylandWindow(const WaylandWindow&) = delete;
  WaylandWindow& operator=(const WaylandWindow&) = delete;

  [[nodiscard]] HostWindow window() const;
  [[nodiscard]] WaylandWindowStatus status() const;

private:
  struct Impl;
  explicit WaylandWindow(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace dui::platform
