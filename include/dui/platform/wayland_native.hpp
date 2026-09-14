#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace dui::platform {

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
  // Destroying it on another thread terminates rather than releasing proxies
  // outside their event-loop affinity.
  void dispatch();
  void roundtrip();
  [[nodiscard]] WaylandGlobals globals() const;

private:
  struct Impl;
  explicit WaylandConnection(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

} // namespace dui::platform
