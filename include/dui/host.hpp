#pragma once

#include "dui/backend.hpp"
#include "dui/input.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dui {

struct WindowId {
  std::uint64_t value{};

  [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
  friend constexpr auto operator<=>(WindowId, WindowId) = default;
};

struct WindowMetrics {
  Size logical_size;
  std::uint32_t physical_width{};
  std::uint32_t physical_height{};
  double device_pixel_ratio{1.0};
  std::uint64_t generation{};

  [[nodiscard]] bool valid() const;
  [[nodiscard]] SurfaceRequest surface_request() const;

  friend constexpr bool operator==(WindowMetrics, WindowMetrics) = default;
};

struct WindowConfiguration {
  std::string title;
  Size logical_size;
  std::uint32_t physical_width{};
  std::uint32_t physical_height{};
  double device_pixel_ratio{1.0};
  bool visible{};
};

class TaskRunner {
public:
  using Task = std::function<void()>;

  virtual ~TaskRunner() = default;
  // post() enqueues without invoking task inline. Throwing guarantees that task
  // was not enqueued. The embedding owns runners and keeps them alive until all
  // windows have completed shutdown.
  virtual void post(Task task) = 0;
  [[nodiscard]] virtual bool runs_tasks_on_current_thread() const noexcept = 0;
};

enum class HostErrorSource {
  invalid_platform_event,
  task_post,
  platform_control,
  delegate_callback,
  shutdown,
};

using HostErrorHandler = std::function<void(WindowId, HostErrorSource, std::exception_ptr)>;

enum class HostSurfaceState { available, unavailable, out_of_date, lost };

struct SurfaceEvent {
  WindowId window;
  HostSurfaceState state{HostSurfaceState::available};
  std::uint64_t generation{};

  friend constexpr bool operator==(SurfaceEvent, SurfaceEvent) = default;
};

struct FrameRequest {
  WindowId window;
  WindowMetrics metrics;
  std::chrono::nanoseconds timestamp{};
  std::uint64_t sequence{};

  friend constexpr bool operator==(FrameRequest, FrameRequest) = default;
};

class HostWindowDelegate {
public:
  virtual ~HostWindowDelegate() = default;

  virtual void window_created(WindowId, WindowMetrics) {}
  virtual void metrics_changed(WindowId, WindowMetrics) {}
  virtual void visibility_changed(WindowId, bool) {}
  virtual void focus_changed(WindowId, bool) {}
  virtual void frame_requested(FrameRequest) {}
  virtual void pointer_event(WindowId, PointerEvent) {}
  virtual void key_event(WindowId, KeyEvent) {}
  virtual void surface_changed(SurfaceEvent) {}
  virtual void close_requested(WindowId) {}
  virtual void window_shutting_down(WindowId) {}
};

class HostWindowControl {
public:
  virtual ~HostWindowControl() = default;

  virtual void request_frame(std::uint64_t generation) = 0;
  virtual void cancel_frame() = 0;
  virtual void set_title(std::string_view title) = 0;
  virtual void request_close() = 0;
  virtual void shutdown() = 0;
};

namespace detail {
struct HostWindowState;
}

class HostWindow;
class HostWindowDriver;
struct HostWindowEndpoints;

class DelegateBinding {
public:
  DelegateBinding() = default;
  ~DelegateBinding();

  DelegateBinding(const DelegateBinding&) = delete;
  DelegateBinding& operator=(const DelegateBinding&) = delete;
  DelegateBinding(DelegateBinding&& other) noexcept;
  DelegateBinding& operator=(DelegateBinding&& other) noexcept;

  void reset() noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  friend class HostWindow;
  DelegateBinding(std::weak_ptr<detail::HostWindowState> state, std::uint64_t generation)
    : state_(std::move(state)), generation_(generation) {}

  std::weak_ptr<detail::HostWindowState> state_;
  std::uint64_t generation_{};
};

class HostWindow {
public:
  HostWindow() = default;

  [[nodiscard]] WindowId id() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::optional<WindowMetrics> metrics() const;
  [[nodiscard]] std::shared_ptr<RasterSurface> raster_surface() const;

  [[nodiscard]] DelegateBinding bind_delegate(std::weak_ptr<HostWindowDelegate> delegate);
  [[nodiscard]] bool request_frame() noexcept;
  [[nodiscard]] bool set_title(std::string title) noexcept;
  [[nodiscard]] bool request_close() noexcept;
  void shutdown() noexcept;

private:
  friend class HostWindowDriver;
  friend struct HostWindowEndpoints;
  friend HostWindowEndpoints make_host_window(WindowId, WindowConfiguration,
                                              std::shared_ptr<TaskRunner>,
                                              std::shared_ptr<TaskRunner>,
                                              std::shared_ptr<HostWindowControl>,
                                              std::shared_ptr<RasterSurface>, HostErrorHandler);
  explicit HostWindow(std::shared_ptr<detail::HostWindowState> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::HostWindowState> state_;
};

class HostWindowDriver {
public:
  HostWindowDriver() = default;

  // Native adapters serialize these entries on the platform executor.
  void set_metrics(Size logical_size, std::uint32_t physical_width, std::uint32_t physical_height,
                   double device_pixel_ratio) noexcept;
  void set_visible(bool visible) noexcept;
  void set_focused(bool focused) noexcept;
  void frame_pulse(std::chrono::nanoseconds timestamp, std::uint64_t generation) noexcept;
  void send_pointer(PointerEvent event) noexcept;
  void send_key(KeyEvent event) noexcept;
  void set_surface_state(HostSurfaceState state) noexcept;
  void request_framework_close() noexcept;
  void shutdown() noexcept;
  [[nodiscard]] bool valid() const noexcept;

private:
  friend struct HostWindowEndpoints;
  friend HostWindowEndpoints make_host_window(WindowId, WindowConfiguration,
                                              std::shared_ptr<TaskRunner>,
                                              std::shared_ptr<TaskRunner>,
                                              std::shared_ptr<HostWindowControl>,
                                              std::shared_ptr<RasterSurface>, HostErrorHandler);
  explicit HostWindowDriver(std::weak_ptr<detail::HostWindowState> state)
    : state_(std::move(state)) {}

  std::weak_ptr<detail::HostWindowState> state_;
};

struct HostWindowEndpoints {
  HostWindow window;
  HostWindowDriver driver;
};

[[nodiscard]] HostWindowEndpoints make_host_window(
  WindowId id, WindowConfiguration configuration, std::shared_ptr<TaskRunner> platform_runner,
  std::shared_ptr<TaskRunner> ui_runner, std::shared_ptr<HostWindowControl> control,
  std::shared_ptr<RasterSurface> raster_surface = {}, HostErrorHandler error_handler = {});

class HostApplication {
public:
  virtual ~HostApplication() = default;

  [[nodiscard]] virtual HostWindow create_window(WindowConfiguration configuration) = 0;
  [[nodiscard]] virtual std::optional<HostWindow> window(WindowId id) const = 0;
  virtual void shutdown() noexcept = 0;
};

} // namespace dui
