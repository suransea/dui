#include "dui/host.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dui {

namespace detail {

struct HostWindowState {
  mutable std::recursive_mutex dispatch_mutex;
  mutable std::mutex mutex;
  WindowId id;
  WindowMetrics metrics;
  bool visible{};
  bool focused{};
  HostSurfaceState surface_state{HostSurfaceState::available};
  bool running{true};
  bool deliver_queued_events{true};
  bool shutdown_ui_pending{};
  bool platform_shutdown_pending{};
  bool platform_shutdown_complete{};
  std::weak_ptr<HostWindowDelegate> delegate;
  std::uint64_t delegate_generation{};
  bool frame_demand{};
  bool frame_armed{};
  bool frame_delivering{};
  bool frame_generation_exhausted{};
  std::uint64_t frame_generation{};
  std::uint64_t next_frame_sequence{1};
  std::optional<std::chrono::nanoseconds> last_frame_timestamp;
  std::unordered_set<PointerId> active_pointers;
  std::weak_ptr<TaskRunner> platform_runner;
  std::weak_ptr<TaskRunner> ui_runner;
  std::shared_ptr<HostWindowControl> control;
  std::shared_ptr<RasterSurface> raster_surface;
  HostErrorHandler error_handler;
};

} // namespace detail

namespace {

using State = detail::HostWindowState;

void report_error(const std::shared_ptr<State>& state, HostErrorSource source,
                  std::exception_ptr failure) noexcept {
  if (!state->error_handler) {
    return;
  }
  try {
    state->error_handler(state->id, source, std::move(failure));
  } catch (...) {
  }
}

template <class Operation>
void invoke_delegate(const std::shared_ptr<State>& state, std::uint64_t generation,
                     Operation operation, bool require_running = true) noexcept {
  std::shared_ptr<HostWindowDelegate> delegate;
  {
    std::lock_guard lock{state->mutex};
    if ((require_running && !state->running && !state->deliver_queued_events) ||
        state->delegate_generation != generation) {
      return;
    }
    delegate = state->delegate.lock();
  }
  if (delegate == nullptr) {
    return;
  }
  try {
    operation(*delegate);
  } catch (...) {
    report_error(state, HostErrorSource::delegate_callback, std::current_exception());
  }
}

template <class Task>
std::exception_ptr try_post_task(const std::shared_ptr<TaskRunner>& runner, Task task) noexcept {
  if (runner == nullptr) {
    return std::make_exception_ptr(std::runtime_error("Host task runner expired"));
  }
  try {
    runner->post(std::move(task));
    return {};
  } catch (...) {
    return std::current_exception();
  }
}

template <class Task>
bool post_task(const std::shared_ptr<State>& state, const std::shared_ptr<TaskRunner>& runner,
               Task task) noexcept {
  std::exception_ptr failure = try_post_task(runner, std::move(task));
  if (failure == nullptr) {
    return true;
  }
  report_error(state, HostErrorSource::task_post, std::move(failure));
  return false;
}

template <class Operation>
bool post_delegate(const std::shared_ptr<State>& state, std::uint64_t generation,
                   Operation operation) noexcept {
  return post_task(state, state->ui_runner.lock(),
                   [state, generation, operation = std::move(operation)]() mutable {
                     invoke_delegate(state, generation, std::move(operation));
                   });
}

template <class Operation>
bool post_control(const std::shared_ptr<State>& state, Operation operation) noexcept {
  return post_task(
    state, state->platform_runner.lock(), [state, operation = std::move(operation)]() mutable {
      std::shared_ptr<HostWindowControl> control;
      {
        std::lock_guard lock{state->mutex};
        if (!state->running) {
          return;
        }
        control = state->control;
      }
      try {
        operation(*control);
      } catch (...) {
        report_error(state, HostErrorSource::platform_control, std::current_exception());
      }
    });
}

std::optional<std::uint64_t> arm_frame_locked(State& state) {
  if (!state.running || !state.visible || state.surface_state != HostSurfaceState::available ||
      state.frame_armed || state.frame_delivering || !state.frame_demand) {
    return std::nullopt;
  }
  if (state.frame_generation == std::numeric_limits<std::uint64_t>::max()) {
    state.frame_generation_exhausted = true;
    return std::nullopt;
  }
  ++state.frame_generation;
  state.frame_armed = true;
  return state.frame_generation;
}

bool post_frame_control(const std::shared_ptr<State>& state, bool cancel,
                        std::optional<std::uint64_t> generation) noexcept {
  if (!cancel && !generation.has_value()) {
    return true;
  }
  return post_control(state, [state, cancel, generation](HostWindowControl& control) {
    if (cancel) {
      try {
        control.cancel_frame();
      } catch (...) {
        report_error(state, HostErrorSource::platform_control, std::current_exception());
      }
    }
    if (generation.has_value()) {
      try {
        control.request_frame(*generation);
      } catch (...) {
        {
          std::lock_guard lock{state->mutex};
          if (state->frame_armed && state->frame_generation == *generation) {
            state->frame_armed = false;
            state->frame_demand = false;
          }
        }
        report_error(state, HostErrorSource::platform_control, std::current_exception());
      }
    }
  });
}

void invalidate_frame_locked(State& state) {
  if (state.frame_armed || state.frame_delivering) {
    state.frame_demand = true;
    state.frame_armed = false;
  }
  if (state.frame_generation != std::numeric_limits<std::uint64_t>::max()) {
    ++state.frame_generation;
  } else {
    state.frame_generation_exhausted = true;
  }
}

std::vector<PointerId> take_active_pointers_locked(State& state) {
  std::vector<PointerId> pointers{state.active_pointers.begin(), state.active_pointers.end()};
  state.active_pointers.clear();
  std::ranges::sort(pointers);
  return pointers;
}

bool frame_generation_exhausted(const std::shared_ptr<State>& state) {
  bool exhausted{};
  {
    std::lock_guard lock{state->mutex};
    exhausted = std::exchange(state->frame_generation_exhausted, false);
  }
  if (exhausted) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::overflow_error("Host frame generation exhausted")));
  }
  return exhausted;
}

void complete_platform_shutdown(const std::shared_ptr<State>& state) noexcept {
  std::shared_ptr<HostWindowControl> control;
  {
    std::lock_guard lock{state->mutex};
    if (state->platform_shutdown_complete) {
      return;
    }
    state->platform_shutdown_pending = false;
    state->platform_shutdown_complete = true;
    control = state->control;
  }
  try {
    control->cancel_frame();
  } catch (...) {
    report_error(state, HostErrorSource::shutdown, std::current_exception());
  }
  try {
    control->shutdown();
  } catch (...) {
    report_error(state, HostErrorSource::shutdown, std::current_exception());
  }
}

bool schedule_platform_shutdown(const std::shared_ptr<State>& state) noexcept {
  std::shared_ptr<TaskRunner> runner = state->platform_runner.lock();
  {
    std::lock_guard lock{state->mutex};
    if (state->platform_shutdown_complete || state->platform_shutdown_pending) {
      return true;
    }
    state->platform_shutdown_pending = true;
  }
  if (runner != nullptr && runner->runs_tasks_on_current_thread()) {
    complete_platform_shutdown(state);
    return true;
  }
  if (post_task(state, runner, [state] { complete_platform_shutdown(state); })) {
    return true;
  }
  std::lock_guard lock{state->mutex};
  state->platform_shutdown_pending = false;
  return false;
}

bool same_metrics(const WindowMetrics& current, Size logical_size, std::uint32_t physical_width,
                  std::uint32_t physical_height, double device_pixel_ratio) {
  return current.logical_size == logical_size && current.physical_width == physical_width &&
         current.physical_height == physical_height &&
         current.device_pixel_ratio == device_pixel_ratio;
}

} // namespace

bool WindowMetrics::valid() const { return surface_request().valid(); }

SurfaceRequest WindowMetrics::surface_request() const {
  return {logical_size, physical_width, physical_height, device_pixel_ratio, generation};
}

DelegateBinding::~DelegateBinding() { reset(); }

DelegateBinding::DelegateBinding(DelegateBinding&& other) noexcept
  : state_(std::move(other.state_)), generation_(std::exchange(other.generation_, 0)) {}

DelegateBinding& DelegateBinding::operator=(DelegateBinding&& other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}

void DelegateBinding::reset() noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state != nullptr && generation_ != 0) {
    std::lock_guard lock{state->mutex};
    if (state->delegate_generation == generation_) {
      state->delegate.reset();
      state->active_pointers.clear();
      if (state->delegate_generation != std::numeric_limits<std::uint64_t>::max()) {
        ++state->delegate_generation;
      }
    }
  }
  state_.reset();
  generation_ = 0;
}

DelegateBinding::operator bool() const noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr || generation_ == 0) {
    return false;
  }
  std::lock_guard lock{state->mutex};
  return state->running && state->delegate_generation == generation_ && !state->delegate.expired();
}

WindowId HostWindow::id() const noexcept {
  if (state_ == nullptr) {
    return {};
  }
  std::lock_guard lock{state_->mutex};
  return state_->id;
}

bool HostWindow::valid() const noexcept {
  if (state_ == nullptr) {
    return false;
  }
  std::lock_guard lock{state_->mutex};
  return state_->running;
}

std::optional<WindowMetrics> HostWindow::metrics() const {
  if (state_ == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock{state_->mutex};
  return state_->running ? std::optional{state_->metrics} : std::nullopt;
}

std::shared_ptr<RasterSurface> HostWindow::raster_surface() const {
  if (state_ == nullptr) {
    return {};
  }
  std::lock_guard lock{state_->mutex};
  return state_->running ? state_->raster_surface : nullptr;
}

DelegateBinding HostWindow::bind_delegate(std::weak_ptr<HostWindowDelegate> delegate) {
  if (state_ == nullptr) {
    throw std::logic_error("Cannot bind a delegate to an empty HostWindow");
  }
  std::lock_guard dispatch_lock{state_->dispatch_mutex};
  std::uint64_t generation{};
  WindowId id;
  WindowMetrics metrics;
  bool visible{};
  {
    std::lock_guard lock{state_->mutex};
    if (!state_->running) {
      throw std::logic_error("Cannot bind a delegate to a stopped HostWindow");
    }
    if (state_->delegate_generation == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Host delegate generation exhausted");
    }
    generation = ++state_->delegate_generation;
    state_->delegate = std::move(delegate);
    state_->active_pointers.clear();
    id = state_->id;
    metrics = state_->metrics;
    visible = state_->visible;
  }
  if (!post_task(state_, state_->ui_runner.lock(),
                 [state = state_, generation, id, metrics, visible] {
                   invoke_delegate(state, generation, [id, metrics](HostWindowDelegate& target) {
                     target.window_created(id, metrics);
                   });
                   if (visible) {
                     invoke_delegate(state, generation, [id](HostWindowDelegate& target) {
                       target.visibility_changed(id, true);
                     });
                   }
                 })) {
    shutdown();
    return {};
  }
  return DelegateBinding{state_, generation};
}

bool HostWindow::request_frame() noexcept {
  if (state_ == nullptr) {
    return false;
  }
  std::optional<std::uint64_t> generation;
  {
    std::lock_guard lock{state_->mutex};
    if (!state_->running) {
      return false;
    }
    state_->frame_demand = true;
    generation = arm_frame_locked(*state_);
  }
  if (frame_generation_exhausted(state_)) {
    shutdown();
    return false;
  }
  if (generation.has_value() && !post_frame_control(state_, false, generation)) {
    shutdown();
    return false;
  }
  return true;
}

bool HostWindow::set_title(std::string title) noexcept {
  if (state_ == nullptr || !valid()) {
    return false;
  }
  if (!post_control(state_, [title = std::move(title)](HostWindowControl& control) {
        control.set_title(title);
      })) {
    shutdown();
    return false;
  }
  return true;
}

bool HostWindow::request_close() noexcept {
  if (state_ == nullptr || !valid()) {
    return false;
  }
  if (!post_control(state_, [](HostWindowControl& control) { control.request_close(); })) {
    shutdown();
    return false;
  }
  return true;
}

void HostWindow::shutdown() noexcept {
  if (state_ == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state_->dispatch_mutex};
  std::vector<PointerId> pointers;
  WindowId id;
  std::uint64_t delegate_generation{};
  bool retry_platform{};
  {
    std::lock_guard lock{state_->mutex};
    if (!state_->running) {
      retry_platform = !state_->shutdown_ui_pending && !state_->platform_shutdown_complete;
    } else {
      state_->running = false;
      state_->shutdown_ui_pending = true;
      state_->frame_demand = false;
      state_->frame_armed = false;
      if (state_->frame_generation != std::numeric_limits<std::uint64_t>::max()) {
        ++state_->frame_generation;
      }
      pointers = take_active_pointers_locked(*state_);
      delegate_generation = state_->delegate_generation;
      id = state_->id;
    }
  }
  if (retry_platform) {
    static_cast<void>(schedule_platform_shutdown(state_));
    return;
  }
  if (id.value == 0) {
    return;
  }
  const bool posted =
    post_task(state_, state_->ui_runner.lock(),
              [state = state_, delegate_generation, pointers = std::move(pointers), id]() mutable {
                for (PointerId pointer : pointers) {
                  invoke_delegate(
                    state, delegate_generation,
                    [id, pointer](HostWindowDelegate& delegate) {
                      delegate.pointer_event(id, PointerEvent{pointer, PointerPhase::cancel, {}});
                    },
                    false);
                }
                invoke_delegate(
                  state, delegate_generation,
                  [id](HostWindowDelegate& delegate) { delegate.window_shutting_down(id); }, false);
                {
                  std::lock_guard lock{state->mutex};
                  state->deliver_queued_events = false;
                  state->shutdown_ui_pending = false;
                  if (state->delegate_generation == delegate_generation) {
                    state->delegate.reset();
                    if (state->delegate_generation != std::numeric_limits<std::uint64_t>::max()) {
                      ++state->delegate_generation;
                    }
                  }
                }
                static_cast<void>(schedule_platform_shutdown(state));
              });
  if (!posted) {
    {
      std::lock_guard lock{state_->mutex};
      state_->deliver_queued_events = false;
      state_->shutdown_ui_pending = false;
      if (state_->delegate_generation == delegate_generation) {
        state_->delegate.reset();
        if (state_->delegate_generation != std::numeric_limits<std::uint64_t>::max()) {
          ++state_->delegate_generation;
        }
      }
    }
    static_cast<void>(schedule_platform_shutdown(state_));
  }
}

bool HostWindowDriver::valid() const noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return false;
  }
  std::lock_guard lock{state->mutex};
  return state->running;
}

void HostWindowDriver::set_metrics(Size logical_size, std::uint32_t physical_width,
                                   std::uint32_t physical_height,
                                   double device_pixel_ratio) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  WindowMetrics metrics;
  std::uint64_t delegate_generation{};
  bool cancel{};
  std::optional<std::uint64_t> frame_generation;
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || same_metrics(state->metrics, logical_size, physical_width,
                                        physical_height, device_pixel_ratio)) {
      return;
    }
    if (state->metrics.generation == std::numeric_limits<std::uint64_t>::max()) {
      metrics = {};
    } else {
      metrics = {logical_size, physical_width, physical_height, device_pixel_ratio,
                 state->metrics.generation + 1};
    }
    if (!metrics.valid()) {
      metrics = {};
    } else {
      cancel = state->frame_armed;
      invalidate_frame_locked(*state);
      state->metrics = metrics;
      delegate_generation = state->delegate_generation;
      frame_generation = arm_frame_locked(*state);
    }
  }
  if (!metrics.valid()) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::invalid_argument("Invalid host window metrics")));
    return;
  }
  if (!post_delegate(state, delegate_generation,
                     [id = state->id, metrics](HostWindowDelegate& delegate) {
                       delegate.metrics_changed(id, metrics);
                     }) ||
      !post_frame_control(state, cancel, frame_generation) || frame_generation_exhausted(state)) {
    HostWindow{state}.shutdown();
  }
}

void HostWindowDriver::set_visible(bool visible) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  std::uint64_t generation{};
  WindowId id;
  bool focus_lost{};
  bool cancel_frame{};
  std::vector<PointerId> pointers;
  std::optional<std::uint64_t> frame_generation;
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || state->visible == visible) {
      return;
    }
    state->visible = visible;
    generation = state->delegate_generation;
    id = state->id;
    if (!visible) {
      focus_lost = std::exchange(state->focused, false);
      pointers = take_active_pointers_locked(*state);
      cancel_frame = state->frame_armed;
      invalidate_frame_locked(*state);
    } else {
      frame_generation = arm_frame_locked(*state);
    }
  }
  const std::vector<PointerId> rollback_pointers = pointers;
  std::exception_ptr post_failure =
    try_post_task(state->ui_runner.lock(), [state, generation, id, visible, focus_lost,
                                            pointers = std::move(pointers)] {
      for (PointerId pointer : pointers) {
        invoke_delegate(state, generation, [id, pointer](HostWindowDelegate& delegate) {
          delegate.pointer_event(id, PointerEvent{pointer, PointerPhase::cancel, {}});
        });
      }
      if (focus_lost) {
        invoke_delegate(state, generation,
                        [id](HostWindowDelegate& delegate) { delegate.focus_changed(id, false); });
      }
      invoke_delegate(state, generation, [id, visible](HostWindowDelegate& delegate) {
        delegate.visibility_changed(id, visible);
      });
    });
  if (post_failure != nullptr) {
    {
      std::lock_guard lock{state->mutex};
      if (state->running && state->delegate_generation == generation) {
        state->active_pointers.insert(rollback_pointers.begin(), rollback_pointers.end());
      }
    }
    report_error(state, HostErrorSource::task_post, std::move(post_failure));
    HostWindow{state}.shutdown();
    return;
  }
  if (!post_frame_control(state, cancel_frame, frame_generation) ||
      frame_generation_exhausted(state)) {
    HostWindow{state}.shutdown();
  }
}

void HostWindowDriver::set_focused(bool focused) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  std::uint64_t generation{};
  WindowId id;
  std::vector<PointerId> pointers;
  bool invalid{};
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || state->focused == focused) {
      return;
    }
    if (focused && !state->visible) {
      invalid = true;
    } else {
      state->focused = focused;
      generation = state->delegate_generation;
      id = state->id;
      if (!focused) {
        pointers = take_active_pointers_locked(*state);
      }
    }
  }
  if (invalid) {
    report_error(
      state, HostErrorSource::invalid_platform_event,
      std::make_exception_ptr(std::logic_error("Hidden host window cannot receive focus")));
    return;
  }
  const std::vector<PointerId> rollback_pointers = pointers;
  std::exception_ptr post_failure = try_post_task(
    state->ui_runner.lock(), [state, generation, id, focused, pointers = std::move(pointers)] {
      for (PointerId pointer : pointers) {
        invoke_delegate(state, generation, [id, pointer](HostWindowDelegate& delegate) {
          delegate.pointer_event(id, PointerEvent{pointer, PointerPhase::cancel, {}});
        });
      }
      invoke_delegate(state, generation, [id, focused](HostWindowDelegate& delegate) {
        delegate.focus_changed(id, focused);
      });
    });
  if (post_failure != nullptr) {
    {
      std::lock_guard lock{state->mutex};
      if (state->running && state->delegate_generation == generation) {
        state->active_pointers.insert(rollback_pointers.begin(), rollback_pointers.end());
      }
    }
    report_error(state, HostErrorSource::task_post, std::move(post_failure));
    HostWindow{state}.shutdown();
  }
}

void HostWindowDriver::frame_pulse(std::chrono::nanoseconds timestamp,
                                   std::uint64_t generation) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  FrameRequest request;
  std::uint64_t delegate_generation{};
  bool exhausted{};
  bool invalid_timestamp{};
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->frame_armed || state->frame_generation != generation ||
        !state->visible || state->surface_state != HostSurfaceState::available) {
      return;
    }
    if (timestamp.count() < 0 ||
        (state->last_frame_timestamp.has_value() && timestamp < *state->last_frame_timestamp)) {
      invalid_timestamp = true;
    } else if (state->next_frame_sequence == std::numeric_limits<std::uint64_t>::max()) {
      state->frame_armed = false;
      state->frame_delivering = false;
      exhausted = true;
    } else {
      state->frame_armed = false;
      state->frame_delivering = true;
      state->frame_demand = false;
      state->last_frame_timestamp = timestamp;
      request = {state->id, state->metrics, timestamp, state->next_frame_sequence++};
      delegate_generation = state->delegate_generation;
    }
  }
  if (invalid_timestamp) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::invalid_argument("Non-monotonic frame timestamp")));
    return;
  }
  if (exhausted) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::overflow_error("Host frame sequence exhausted")));
    HostWindow{state}.shutdown();
    return;
  }
  if (!post_task(state, state->ui_runner.lock(), [state, delegate_generation, request]() {
        bool current{};
        {
          std::lock_guard lock{state->mutex};
          current = state->running && state->frame_delivering && state->visible &&
                    state->surface_state == HostSurfaceState::available &&
                    state->metrics.generation == request.metrics.generation;
        }
        if (current) {
          invoke_delegate(state, delegate_generation, [request](HostWindowDelegate& delegate) {
            delegate.frame_requested(request);
          });
        }
        std::optional<std::uint64_t> next;
        {
          std::lock_guard lock{state->mutex};
          state->frame_delivering = false;
          next = arm_frame_locked(*state);
        }
        if (!post_frame_control(state, false, next) || frame_generation_exhausted(state)) {
          HostWindow{state}.shutdown();
        }
      })) {
    HostWindow{state}.shutdown();
  }
}

void HostWindowDriver::send_pointer(PointerEvent event) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::unique_lock dispatch_lock{state->dispatch_mutex};
  bool deliver{};
  bool invalid{};
  std::uint64_t generation{};
  WindowId id;
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->visible || !std::isfinite(event.position.x) ||
        !std::isfinite(event.position.y)) {
      invalid = state->running;
    } else {
      const bool active = state->active_pointers.contains(event.pointer);
      switch (event.phase) {
      case PointerPhase::down:
        invalid = active;
        if (!active) {
          state->active_pointers.insert(event.pointer);
          deliver = true;
        }
        break;
      case PointerPhase::move:
        invalid = !active;
        deliver = active;
        break;
      case PointerPhase::up:
        invalid = !active;
        if (active) {
          state->active_pointers.erase(event.pointer);
          deliver = true;
        }
        break;
      case PointerPhase::cancel:
        if (active) {
          state->active_pointers.erase(event.pointer);
          deliver = true;
        }
        break;
      default:
        invalid = true;
        break;
      }
      generation = state->delegate_generation;
      id = state->id;
    }
  }
  if (invalid) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::logic_error("Invalid host pointer sequence")));
    return;
  }
  if (deliver) {
    std::exception_ptr failure =
      try_post_task(state->ui_runner.lock(), [state, generation, id, event] {
        invoke_delegate(state, generation, [id, event](HostWindowDelegate& delegate) {
          delegate.pointer_event(id, event);
        });
      });
    if (failure != nullptr) {
      {
        std::lock_guard lock{state->mutex};
        if (state->running && state->delegate_generation == generation) {
          if (event.phase == PointerPhase::down) {
            state->active_pointers.erase(event.pointer);
          } else if (event.phase == PointerPhase::up || event.phase == PointerPhase::cancel) {
            state->active_pointers.insert(event.pointer);
          }
        }
      }
      dispatch_lock.unlock();
      report_error(state, HostErrorSource::task_post, std::move(failure));
      HostWindow{state}.shutdown();
    }
  }
}

void HostWindowDriver::send_key(KeyEvent event) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  std::uint64_t generation{};
  WindowId id;
  bool invalid{};
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->visible) {
      return;
    }
    invalid = event.logical_key.empty() || !is_valid_utf8(event.logical_key) ||
              (event.phase != KeyPhase::down && event.phase != KeyPhase::repeat &&
               event.phase != KeyPhase::up);
    generation = state->delegate_generation;
    id = state->id;
  }
  if (invalid) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::invalid_argument("Invalid host key event")));
    return;
  }
  if (!post_delegate(state, generation,
                     [id, event = std::move(event)](HostWindowDelegate& delegate) mutable {
                       delegate.key_event(id, std::move(event));
                     })) {
    HostWindow{state}.shutdown();
  }
}

void HostWindowDriver::set_surface_state(HostSurfaceState surface) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  {
    std::lock_guard lock{state->mutex};
    if (!state->running) {
      return;
    }
  }
  bool cancel{};
  bool metrics_changed{};
  WindowMetrics metrics;
  SurfaceEvent event;
  std::uint64_t delegate_generation{};
  std::optional<std::uint64_t> frame_generation;
  bool exhausted{};
  const bool known_surface =
    surface == HostSurfaceState::available || surface == HostSurfaceState::unavailable ||
    surface == HostSurfaceState::out_of_date || surface == HostSurfaceState::lost;
  if (!known_surface) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::invalid_argument("Invalid host surface state")));
    return;
  }
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || state->surface_state == surface) {
      return;
    }
    const HostSurfaceState previous = state->surface_state;
    if (surface == HostSurfaceState::out_of_date ||
        (surface == HostSurfaceState::available && previous != HostSurfaceState::available)) {
      if (state->metrics.generation == std::numeric_limits<std::uint64_t>::max()) {
        exhausted = true;
      } else {
        ++state->metrics.generation;
        metrics_changed = true;
      }
    }
    if (exhausted) {
      event = {};
    } else if (surface != HostSurfaceState::available) {
      state->surface_state = surface;
      cancel = state->frame_armed;
      invalidate_frame_locked(*state);
    } else {
      state->surface_state = surface;
      frame_generation = arm_frame_locked(*state);
    }
    if (!exhausted) {
      metrics = state->metrics;
      event = {state->id, surface, metrics.generation};
      delegate_generation = state->delegate_generation;
    }
  }
  if (exhausted) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::overflow_error("Host metrics exhausted")));
    HostWindow{state}.shutdown();
    return;
  }
  const bool posted = post_task(
    state, state->ui_runner.lock(), [state, delegate_generation, event, metrics, metrics_changed] {
      if (metrics_changed) {
        invoke_delegate(state, delegate_generation, [event, metrics](HostWindowDelegate& delegate) {
          delegate.metrics_changed(event.window, metrics);
        });
      }
      invoke_delegate(state, delegate_generation,
                      [event](HostWindowDelegate& delegate) { delegate.surface_changed(event); });
    });
  if (!posted || !post_frame_control(state, cancel, frame_generation) ||
      frame_generation_exhausted(state)) {
    HostWindow{state}.shutdown();
  }
}

void HostWindowDriver::request_framework_close() noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  std::uint64_t generation{};
  WindowId id;
  {
    std::lock_guard lock{state->mutex};
    if (!state->running) {
      return;
    }
    generation = state->delegate_generation;
    id = state->id;
  }
  if (!post_delegate(state, generation,
                     [id](HostWindowDelegate& delegate) { delegate.close_requested(id); })) {
    HostWindow{state}.shutdown();
  }
}

void HostWindowDriver::shutdown() noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state != nullptr) {
    HostWindow{state}.shutdown();
  }
}

HostWindowEndpoints make_host_window(WindowId id, WindowConfiguration configuration,
                                     std::shared_ptr<TaskRunner> platform_runner,
                                     std::shared_ptr<TaskRunner> ui_runner,
                                     std::shared_ptr<HostWindowControl> control,
                                     std::shared_ptr<RasterSurface> raster_surface,
                                     HostErrorHandler error_handler) {
  if (!id.valid()) {
    throw std::invalid_argument("Host window ID must be nonzero");
  }
  if (platform_runner == nullptr || ui_runner == nullptr || control == nullptr) {
    throw std::invalid_argument("Host window requires runners and platform control");
  }
  WindowMetrics metrics{configuration.logical_size, configuration.physical_width,
                        configuration.physical_height, configuration.device_pixel_ratio, 1};
  if (!metrics.valid()) {
    throw std::invalid_argument("Host window configuration has invalid metrics");
  }
  auto state = std::make_shared<State>();
  state->id = id;
  state->metrics = metrics;
  state->visible = configuration.visible;
  state->platform_runner = std::move(platform_runner);
  state->ui_runner = std::move(ui_runner);
  state->control = std::move(control);
  state->raster_surface = std::move(raster_surface);
  state->error_handler = std::move(error_handler);
  return {HostWindow{state}, HostWindowDriver{state}};
}

} // namespace dui
