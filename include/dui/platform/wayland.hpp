#pragma once

#include "dui/input.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>

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

class WaylandPointerState {
public:
  explicit WaylandPointerState(PointerId pointer = 1);

  void enter(double x, double y);
  [[nodiscard]] std::optional<PointerEvent> leave() noexcept;
  [[nodiscard]] std::optional<PointerEvent> motion(double x, double y);
  [[nodiscard]] std::optional<PointerEvent> primary_button(bool pressed);
  [[nodiscard]] std::optional<PointerEvent> capability_lost() noexcept;

  [[nodiscard]] bool focused() const noexcept { return focused_; }
  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] Offset position() const noexcept { return position_; }

private:
  void set_position(double x, double y);

  PointerId pointer_;
  Offset position_;
  bool focused_{};
  bool active_{};
};

struct WaylandKeyModifiers {
  bool shift{};
  bool control{};
  bool alt{};
  bool meta{};
};

class WaylandKeyboardState {
public:
  void focus_gained() noexcept { focused_ = true; }
  void focus_lost() noexcept;
  void clear_pressed() noexcept { pressed_.clear(); }

  [[nodiscard]] std::optional<KeyEvent> key_down(std::uint32_t key, std::string logical_key,
                                                 WaylandKeyModifiers modifiers);
  [[nodiscard]] std::optional<KeyEvent> key_repeat(std::uint32_t key,
                                                   WaylandKeyModifiers modifiers) const;
  [[nodiscard]] std::optional<KeyEvent> key_up(std::uint32_t key, WaylandKeyModifiers modifiers);

  [[nodiscard]] bool focused() const noexcept { return focused_; }
  [[nodiscard]] std::size_t pressed_count() const noexcept { return pressed_.size(); }

private:
  static KeyEvent event(std::string logical_key, KeyPhase phase, WaylandKeyModifiers modifiers);

  std::unordered_map<std::uint32_t, std::string> pressed_;
  bool focused_{};
};

struct WaylandRepeatSchedule {
  std::uint32_t key{};
  std::chrono::nanoseconds delay{};
  std::chrono::nanoseconds interval{};
  std::uint64_t generation{};

  friend constexpr bool operator==(WaylandRepeatSchedule, WaylandRepeatSchedule) = default;
};

class WaylandRepeatState {
public:
  static constexpr std::uint64_t maximum_events_per_dispatch = 16;

  void configure(std::int32_t rate, std::int32_t delay_milliseconds, std::chrono::nanoseconds now);
  void key_down(std::uint32_t key, bool repeatable, std::chrono::nanoseconds now);
  void key_up(std::uint32_t key);
  void cancel();

  [[nodiscard]] std::optional<WaylandRepeatSchedule> schedule(std::chrono::nanoseconds now) const;
  [[nodiscard]] std::uint64_t delivery_count(std::uint64_t expirations,
                                             std::uint64_t generation) const noexcept;
  [[nodiscard]] std::optional<std::uint32_t> candidate() const noexcept { return candidate_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
  void advance_generation();

  std::optional<std::uint32_t> candidate_;
  std::chrono::nanoseconds key_down_time_{};
  std::chrono::nanoseconds delay_{};
  std::chrono::nanoseconds interval_{};
  std::uint64_t generation_{1};
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
