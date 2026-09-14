#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>

namespace dui::platform {

struct WaylandExtent {
  std::uint32_t width{};
  std::uint32_t height{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    return width != 0 && height != 0 && width <= maximum && height <= maximum;
  }
  friend constexpr auto operator<=>(WaylandExtent, WaylandExtent) = default;
};

struct WaylandCommit {
  std::uint64_t generation{};
  std::uint64_t state_revision{};
  std::optional<std::uint32_t> configure_serial;
  WaylandExtent logical_extent;
  WaylandExtent buffer_extent;
  std::uint32_t buffer_scale{1};
  std::uint32_t scale_numerator{120};
  bool use_viewporter{};
  bool request_frame_callback{};

  [[nodiscard]] bool valid() const noexcept;
  friend constexpr bool operator==(const WaylandCommit&, const WaylandCommit&) = default;
};

// Platform-neutral protocol state used by the native Wayland adapter. All
// methods are called serially on that adapter's platform executor.
class WaylandSurfaceState {
public:
  static constexpr std::uint32_t scale_denominator = 120;

  explicit WaylandSurfaceState(WaylandExtent initial_extent,
                               bool fractional_scale_supported = false);

  [[nodiscard]] bool begin_initial_commit() noexcept;
  void receive_toplevel_configure(std::uint32_t width, std::uint32_t height);
  void receive_surface_configure(std::uint32_t serial);
  void set_preferred_scale(std::uint32_t numerator);
  void request_frame() noexcept;
  void buffer_lost();

  // No protocol request occurs during preparation. Allocate fallible resources
  // first, then discard or submit immediately before ack/frame/attach/commit.
  // Failure after submission is terminal for that native host connection.
  [[nodiscard]] std::optional<WaylandCommit> prepare_commit();
  [[nodiscard]] bool submit_commit(const WaylandCommit& commit) noexcept;
  [[nodiscard]] bool discard_commit(const WaylandCommit& commit) noexcept;
  [[nodiscard]] bool frame_done(std::uint64_t generation) noexcept;

  [[nodiscard]] bool configured() const noexcept { return configured_; }
  [[nodiscard]] bool frame_callback_pending() const noexcept {
    return frame_callback_generation_ != 0;
  }
  [[nodiscard]] WaylandExtent logical_extent() const noexcept { return applied_extent_; }
  [[nodiscard]] std::uint32_t scale_numerator() const noexcept { return applied_scale_; }

private:
  void invalidate_prepared() noexcept;
  void advance_revision();

  WaylandExtent desired_extent_;
  WaylandExtent applied_extent_;
  std::uint32_t staged_width_{};
  std::uint32_t staged_height_{};
  std::uint32_t desired_scale_{scale_denominator};
  std::uint32_t applied_scale_{scale_denominator};
  std::optional<std::uint32_t> pending_configure_;
  std::optional<WaylandCommit> prepared_;
  std::uint64_t state_revision_{1};
  std::uint64_t next_commit_generation_{1};
  std::uint64_t frame_callback_generation_{};
  bool fractional_scale_supported_{};
  bool initial_commit_started_{};
  bool configured_{};
  bool dirty_{};
  bool frame_demand_{};
};

} // namespace dui::platform
