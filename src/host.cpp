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

class HostTextInputProxy;

struct HostTextSession {
  TextInputSessionId id{};
  std::weak_ptr<TextInputClient> client;
  TextInputConfiguration configuration;
  TextEditingValue value;
  std::optional<Rect> editable_rect;
  std::uint64_t value_revision{1};
  std::uint64_t rect_revision{};
};

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
  std::shared_ptr<AccessibilityAdapter> accessibility_adapter;
  AccessibilityBridge accessibility_bridge;
  std::optional<SemanticsTree> desired_semantics;
  std::uint64_t accessibility_generation{};
  std::uint64_t accessibility_publication{};
  std::uint64_t accessibility_publication_service{};
  bool accessibility_in_flight{};
  bool accessibility_retry_pending{};
  bool accessibility_retry_requested{};
  std::shared_ptr<TextInputBackend> text_input_backend;
  std::shared_ptr<TextInputBackend> text_input_proxy;
  std::shared_ptr<TextInputBackend> native_text_backend;
  std::shared_ptr<TextInputClient> native_text_client;
  std::optional<HostTextSession> text_session;
  TextInputSessionId native_text_session{};
  TextInputSessionId native_text_public_session{};
  std::uint64_t text_service_generation{};
  std::uint64_t native_text_service_generation{};
  std::uint64_t native_text_callback_generation{};
  std::uint64_t text_next_session{1};
  std::uint64_t text_next_callback_generation{1};
  std::uint64_t native_text_value_revision{};
  std::uint64_t native_text_rect_revision{};
  bool text_sync_pending{};
  HostErrorHandler error_handler;
};

class HostTextInputProxy final : public TextInputBackend {
public:
  explicit HostTextInputProxy(std::weak_ptr<HostWindowState> state) : state_(std::move(state)) {}

  [[nodiscard]] TextInputSessionId start_text_input(std::weak_ptr<TextInputClient> client,
                                                    TextInputConfiguration configuration,
                                                    TextEditingValue initial_value) override;
  void update_editing_state(TextInputSessionId session, TextEditingValue value) override;
  void stop_text_input(TextInputSessionId session) override;
  void set_editable_rect(TextInputSessionId session, Rect rect) override;

private:
  std::weak_ptr<HostWindowState> state_;
};

} // namespace detail

namespace {

using State = detail::HostWindowState;

class ForwardingTextInputClient final : public TextInputClient {
public:
  ForwardingTextInputClient(std::weak_ptr<State> state, TextInputSessionId session,
                            std::uint64_t service, std::uint64_t callback_generation)
    : state_(std::move(state)), session_(session), service_(service),
      callback_generation_(callback_generation) {}

  void update_editing_value(TextEditingValue value) override;
  void perform_action(TextInputAction action) override;

private:
  std::weak_ptr<State> state_;
  TextInputSessionId session_{};
  std::uint64_t service_{};
  std::uint64_t callback_generation_{};
};

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
  std::shared_ptr<AccessibilityAdapter> accessibility_adapter;
  std::shared_ptr<TextInputBackend> text_backend;
  std::shared_ptr<TextInputBackend> native_text_backend;
  TextInputSessionId native_text_session{};
  {
    std::lock_guard lock{state->mutex};
    if (state->platform_shutdown_complete) {
      return;
    }
    state->platform_shutdown_pending = false;
    state->platform_shutdown_complete = true;
    control = state->control;
    accessibility_adapter = std::move(state->accessibility_adapter);
    text_backend = std::move(state->text_input_backend);
    native_text_backend = std::move(state->native_text_backend);
    native_text_session = std::exchange(state->native_text_session, 0);
    state->native_text_client.reset();
  }
  if (native_text_backend != nullptr && native_text_session != 0) {
    try {
      native_text_backend->stop_text_input(native_text_session);
    } catch (...) {
      report_error(state, HostErrorSource::shutdown, std::current_exception());
    }
  }
  native_text_backend.reset();
  text_backend.reset();
  accessibility_adapter.reset();
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

void dispatch_desired_semantics(const std::shared_ptr<State>& state) noexcept;
void retry_semantics_on_ui(const std::shared_ptr<State>& state,
                           std::uint64_t service_generation) noexcept;

bool post_accessibility_publication(const std::shared_ptr<State>& state,
                                    AccessibilityPublication publication,
                                    AccessibilityServiceId service,
                                    std::shared_ptr<AccessibilityAdapter> adapter) noexcept {
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || state->accessibility_adapter != adapter ||
        state->accessibility_generation != service.value || state->accessibility_in_flight) {
      static_cast<void>(state->accessibility_bridge.reject(publication.generation));
      return true;
    }
    state->accessibility_in_flight = true;
    state->accessibility_publication = publication.generation;
    state->accessibility_publication_service = service.value;
  }

  if (!post_task(state, state->platform_runner.lock(),
                 [state, publication = std::move(publication), service,
                  adapter = std::move(adapter)]() mutable {
                   {
                     std::lock_guard lock{state->mutex};
                     if (!state->running || state->accessibility_adapter != adapter ||
                         state->accessibility_generation != service.value ||
                         !state->accessibility_in_flight ||
                         state->accessibility_publication != publication.generation) {
                       return;
                     }
                   }
                   bool success{};
                   std::exception_ptr apply_failure;
                   try {
                     adapter->apply(publication.update.changes);
                     success = true;
                   } catch (...) {
                     apply_failure = std::current_exception();
                   }
                   if (!post_task(
                         state, state->ui_runner.lock(),
                         [state, service, generation = publication.generation, success] {
                           {
                             std::lock_guard lock{state->mutex};
                             if (!state->running || !state->accessibility_in_flight ||
                                 state->accessibility_generation != service.value ||
                                 state->accessibility_publication != generation ||
                                 state->accessibility_publication_service != service.value) {
                               return;
                             }
                           }
                           if (success) {
                             static_cast<void>(state->accessibility_bridge.acknowledge(generation));
                           } else {
                             static_cast<void>(state->accessibility_bridge.reject(generation));
                           }
                           bool retry_requested{};
                           {
                             std::lock_guard lock{state->mutex};
                             state->accessibility_in_flight = false;
                             state->accessibility_retry_pending = !success;
                             retry_requested =
                               std::exchange(state->accessibility_retry_requested, false);
                           }
                           if (success) {
                             dispatch_desired_semantics(state);
                           } else if (retry_requested) {
                             retry_semantics_on_ui(state, service.value);
                           }
                         })) {
                     detail::shutdown_host_window(state);
                   }
                   if (apply_failure != nullptr) {
                     report_error(state, HostErrorSource::accessibility, std::move(apply_failure));
                   }
                 })) {
    {
      std::lock_guard lock{state->mutex};
      if (state->accessibility_publication == publication.generation &&
          state->accessibility_publication_service == service.value) {
        state->accessibility_in_flight = false;
      }
    }
    static_cast<void>(state->accessibility_bridge.reject(publication.generation));
    return false;
  }
  return true;
}

void retry_semantics_on_ui(const std::shared_ptr<State>& state,
                           std::uint64_t service_generation) noexcept {
  try {
    std::shared_ptr<AccessibilityAdapter> adapter;
    AccessibilityServiceId service;
    {
      std::lock_guard lock{state->mutex};
      if (!state->running || state->accessibility_adapter == nullptr ||
          state->accessibility_generation != service_generation ||
          state->accessibility_publication_service != service_generation) {
        return;
      }
      if (state->accessibility_in_flight) {
        state->accessibility_retry_requested = true;
        return;
      }
      if (!state->accessibility_retry_pending) {
        return;
      }
      adapter = state->accessibility_adapter;
      service = {state->accessibility_generation};
    }
    std::optional<AccessibilityPublication> publication = state->accessibility_bridge.retry();
    if (!publication.has_value()) {
      return;
    }
    {
      std::lock_guard lock{state->mutex};
      state->accessibility_retry_pending = false;
    }
    if (!post_accessibility_publication(state, std::move(*publication), service,
                                        std::move(adapter))) {
      detail::shutdown_host_window(state);
    }
  } catch (...) {
    report_error(state, HostErrorSource::accessibility, std::current_exception());
    detail::shutdown_host_window(state);
  }
}

void dispatch_desired_semantics(const std::shared_ptr<State>& state) noexcept {
  try {
    std::shared_ptr<AccessibilityAdapter> adapter;
    std::optional<SemanticsTree> desired;
    AccessibilityServiceId service;
    {
      std::lock_guard lock{state->mutex};
      if (!state->running || state->accessibility_in_flight || state->accessibility_retry_pending ||
          state->accessibility_adapter == nullptr || !state->desired_semantics.has_value()) {
        return;
      }
      adapter = state->accessibility_adapter;
      desired = state->desired_semantics;
      service = {state->accessibility_generation};
    }
    std::optional<AccessibilityPublication> publication =
      state->accessibility_bridge.prepare(*desired);
    if (!publication.has_value()) {
      return;
    }
    if (!post_accessibility_publication(state, std::move(*publication), service,
                                        std::move(adapter))) {
      detail::shutdown_host_window(state);
    }
  } catch (...) {
    report_error(state, HostErrorSource::accessibility, std::current_exception());
    detail::shutdown_host_window(state);
  }
}

void synchronize_text_input(const std::shared_ptr<State>& state) noexcept;

bool schedule_text_sync(const std::shared_ptr<State>& state) noexcept {
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || state->text_sync_pending) {
      return state->running;
    }
    state->text_sync_pending = true;
  }
  if (post_task(state, state->platform_runner.lock(), [state] { synchronize_text_input(state); })) {
    return true;
  }
  {
    std::lock_guard lock{state->mutex};
    state->text_sync_pending = false;
  }
  detail::shutdown_host_window(state);
  return false;
}

void synchronize_text_input(const std::shared_ptr<State>& state) noexcept {
  try {
    std::shared_ptr<TextInputBackend> stopped_backend;
    TextInputSessionId stopped_session{};
    {
      std::lock_guard lock{state->mutex};
      state->text_sync_pending = false;
      if (state->text_session.has_value() && state->text_session->client.expired()) {
        state->text_session.reset();
      }
      const bool native_is_current =
        state->native_text_session != 0 && state->text_session.has_value() &&
        state->native_text_backend == state->text_input_backend &&
        state->native_text_service_generation == state->text_service_generation &&
        state->native_text_public_session == state->text_session->id;
      if (state->native_text_session != 0 && !native_is_current) {
        stopped_backend = std::move(state->native_text_backend);
        stopped_session = std::exchange(state->native_text_session, 0);
        state->native_text_client.reset();
        state->native_text_public_session = 0;
        state->native_text_callback_generation = 0;
        state->native_text_service_generation = 0;
        state->native_text_callback_generation = 0;
        state->native_text_value_revision = 0;
        state->native_text_rect_revision = 0;
      }
    }
    if (stopped_backend != nullptr) {
      try {
        stopped_backend->stop_text_input(stopped_session);
      } catch (...) {
        report_error(state, HostErrorSource::text_input, std::current_exception());
      }
    }

    std::shared_ptr<TextInputBackend> backend;
    std::shared_ptr<TextInputClient> forwarding;
    std::optional<detail::HostTextSession> session;
    std::uint64_t service{};
    bool start{};
    bool update_value{};
    bool update_rect{};
    TextInputSessionId native_session{};
    std::uint64_t callback_generation{};
    {
      std::lock_guard lock{state->mutex};
      if (!state->running || !state->text_session.has_value() ||
          state->text_input_backend == nullptr) {
        return;
      }
      backend = state->text_input_backend;
      session = state->text_session;
      service = state->text_service_generation;
      native_session = state->native_text_session;
      start = native_session == 0;
      if (start) {
        if (state->text_next_callback_generation == std::numeric_limits<std::uint64_t>::max()) {
          throw std::overflow_error("Host native text callback generation exhausted");
        }
        callback_generation = state->text_next_callback_generation++;
        forwarding = std::make_shared<ForwardingTextInputClient>(state, session->id, service,
                                                                 callback_generation);
      } else {
        update_value = state->native_text_value_revision < session->value_revision;
        update_rect = session->editable_rect.has_value() &&
                      state->native_text_rect_revision < session->rect_revision;
      }
    }

    if (start) {
      TextInputSessionId started{};
      try {
        started = backend->start_text_input(forwarding, session->configuration, session->value);
        if (started == 0) {
          throw std::runtime_error("Text input backend returned a zero session");
        }
      } catch (...) {
        report_error(state, HostErrorSource::text_input, std::current_exception());
        return;
      }
      bool accepted{};
      {
        std::lock_guard lock{state->mutex};
        accepted = state->running && state->text_session.has_value() &&
                   state->text_session->id == session->id &&
                   state->text_service_generation == service &&
                   state->text_input_backend == backend && state->native_text_session == 0;
        if (accepted) {
          state->native_text_backend = backend;
          state->native_text_client = std::move(forwarding);
          state->native_text_session = started;
          state->native_text_public_session = session->id;
          state->native_text_service_generation = service;
          state->native_text_callback_generation = callback_generation;
          state->native_text_value_revision = session->value_revision;
          state->native_text_rect_revision = 0;
        }
      }
      if (!accepted) {
        try {
          backend->stop_text_input(started);
        } catch (...) {
          report_error(state, HostErrorSource::text_input, std::current_exception());
        }
        return;
      }
      static_cast<void>(schedule_text_sync(state));
      return;
    }

    if (update_value) {
      try {
        backend->update_editing_state(native_session, session->value);
        std::lock_guard lock{state->mutex};
        if (state->native_text_session == native_session &&
            state->native_text_service_generation == service) {
          state->native_text_value_revision = session->value_revision;
        }
      } catch (...) {
        report_error(state, HostErrorSource::text_input, std::current_exception());
        {
          std::lock_guard lock{state->mutex};
          if (state->native_text_session == native_session &&
              state->native_text_service_generation == service) {
            state->native_text_public_session = 0;
            state->native_text_callback_generation = 0;
          }
        }
        static_cast<void>(schedule_text_sync(state));
      }
    }
    if (update_rect) {
      try {
        backend->set_editable_rect(native_session, *session->editable_rect);
        std::lock_guard lock{state->mutex};
        if (state->native_text_session == native_session &&
            state->native_text_service_generation == service) {
          state->native_text_rect_revision = session->rect_revision;
        }
      } catch (...) {
        report_error(state, HostErrorSource::text_input, std::current_exception());
        {
          std::lock_guard lock{state->mutex};
          if (state->native_text_session == native_session &&
              state->native_text_service_generation == service) {
            state->native_text_public_session = 0;
            state->native_text_callback_generation = 0;
          }
        }
      }
    }
  } catch (...) {
    report_error(state, HostErrorSource::text_input, std::current_exception());
  }
}

void ForwardingTextInputClient::update_editing_value(TextEditingValue value) {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr || !value.valid()) {
    return;
  }
  if (!post_task(state, state->ui_runner.lock(),
                 [state, session = session_, service = service_,
                  callback_generation = callback_generation_, value = std::move(value)]() mutable {
                   std::shared_ptr<TextInputClient> client;
                   bool exhausted{};
                   {
                     std::lock_guard lock{state->mutex};
                     if (!state->running || !state->text_session.has_value() ||
                         state->text_session->id != session ||
                         state->text_service_generation != service ||
                         state->native_text_callback_generation != callback_generation) {
                       return;
                     }
                     client = state->text_session->client.lock();
                     if (client == nullptr) {
                       state->text_session.reset();
                     } else if (state->text_session->value_revision ==
                                std::numeric_limits<std::uint64_t>::max()) {
                       exhausted = true;
                     } else {
                       state->text_session->value = value;
                       ++state->text_session->value_revision;
                       state->native_text_value_revision = state->text_session->value_revision;
                     }
                   }
                   if (client == nullptr) {
                     static_cast<void>(schedule_text_sync(state));
                     return;
                   }
                   if (exhausted) {
                     report_error(state, HostErrorSource::text_input,
                                  std::make_exception_ptr(
                                    std::overflow_error("Host text editing revision exhausted")));
                     detail::shutdown_host_window(state);
                     return;
                   }
                   try {
                     client->update_editing_value(std::move(value));
                   } catch (...) {
                     report_error(state, HostErrorSource::text_input, std::current_exception());
                   }
                 })) {
    detail::shutdown_host_window(state);
  }
}

void ForwardingTextInputClient::perform_action(TextInputAction action) {
  const std::shared_ptr<State> state = state_.lock();
  const bool valid_action = action == TextInputAction::none || action == TextInputAction::done ||
                            action == TextInputAction::next || action == TextInputAction::search ||
                            action == TextInputAction::send;
  if (state == nullptr || !valid_action) {
    return;
  }
  if (!post_task(state, state->ui_runner.lock(),
                 [state, session = session_, service = service_,
                  callback_generation = callback_generation_, action] {
                   std::shared_ptr<TextInputClient> client;
                   {
                     std::lock_guard lock{state->mutex};
                     if (!state->running || !state->text_session.has_value() ||
                         state->text_session->id != session ||
                         state->text_service_generation != service ||
                         state->native_text_callback_generation != callback_generation) {
                       return;
                     }
                     client = state->text_session->client.lock();
                     if (client == nullptr) {
                       state->text_session.reset();
                     }
                   }
                   if (client == nullptr) {
                     static_cast<void>(schedule_text_sync(state));
                     return;
                   }
                   try {
                     client->perform_action(action);
                   } catch (...) {
                     report_error(state, HostErrorSource::text_input, std::current_exception());
                   }
                 })) {
    detail::shutdown_host_window(state);
  }
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

TextInputSessionId
detail::HostTextInputProxy::start_text_input(std::weak_ptr<TextInputClient> client,
                                             TextInputConfiguration configuration,
                                             TextEditingValue initial_value) {
  const std::shared_ptr<State> state = state_.lock();
  const bool valid_action = configuration.action == TextInputAction::none ||
                            configuration.action == TextInputAction::done ||
                            configuration.action == TextInputAction::next ||
                            configuration.action == TextInputAction::search ||
                            configuration.action == TextInputAction::send;
  if (state == nullptr || !initial_value.valid() || client.expired() || !valid_action) {
    throw std::invalid_argument("Invalid host text input session");
  }
  TextInputSessionId session{};
  {
    std::lock_guard lock{state->mutex};
    if (!state->running) {
      throw std::logic_error("Cannot start text input on a stopped host window");
    }
    if (state->text_next_session == std::numeric_limits<TextInputSessionId>::max()) {
      throw std::overflow_error("Host text input session generation exhausted");
    }
    session = state->text_next_session++;
    state->text_session = detail::HostTextSession{
      session, std::move(client), configuration, std::move(initial_value), std::nullopt, 1, 0};
  }
  if (!schedule_text_sync(state)) {
    throw std::runtime_error("Could not schedule host text input start");
  }
  return session;
}

void detail::HostTextInputProxy::update_editing_state(TextInputSessionId session,
                                                      TextEditingValue value) {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->text_session.has_value() || state->text_session->id != session) {
      return;
    }
  }
  if (!value.valid()) {
    throw std::invalid_argument("Invalid host text editing value");
  }
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->text_session.has_value() || state->text_session->id != session) {
      return;
    }
    if (state->text_session->value_revision == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Host text editing revision exhausted");
    }
    state->text_session->value = std::move(value);
    ++state->text_session->value_revision;
  }
  static_cast<void>(schedule_text_sync(state));
}

void detail::HostTextInputProxy::stop_text_input(TextInputSessionId session) {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->text_session.has_value() || state->text_session->id != session) {
      return;
    }
    state->text_session.reset();
  }
  static_cast<void>(schedule_text_sync(state));
}

void detail::HostTextInputProxy::set_editable_rect(TextInputSessionId session, Rect rect) {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->text_session.has_value() || state->text_session->id != session) {
      return;
    }
  }
  if (!std::isfinite(rect.origin.x) || !std::isfinite(rect.origin.y) ||
      !std::isfinite(rect.size.width) || !std::isfinite(rect.size.height) ||
      rect.size.width < 0.0 || rect.size.height < 0.0) {
    throw std::invalid_argument("Invalid host editable rectangle");
  }
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || !state->text_session.has_value() || state->text_session->id != session) {
      return;
    }
    if (state->text_session->rect_revision == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Host editable rectangle revision exhausted");
    }
    state->text_session->editable_rect = rect;
    ++state->text_session->rect_revision;
  }
  static_cast<void>(schedule_text_sync(state));
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

std::shared_ptr<TextInputBackend> HostWindow::text_input_backend() const {
  if (state_ == nullptr) {
    return {};
  }
  std::lock_guard lock{state_->mutex};
  return state_->running ? state_->text_input_proxy : nullptr;
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

bool HostWindow::publish_semantics(SemanticsTree tree) {
  if (state_ == nullptr) {
    return false;
  }
  SemanticsDiffer validator;
  static_cast<void>(validator.update(tree));
  std::uint64_t service_generation{};
  {
    std::lock_guard lock{state_->mutex};
    if (!state_->running) {
      return false;
    }
    service_generation = state_->accessibility_generation;
  }
  if (!post_task(state_, state_->ui_runner.lock(),
                 [state = state_, service_generation, tree = std::move(tree)]() mutable {
                   bool dispatch{};
                   {
                     std::lock_guard lock{state->mutex};
                     if (!state->running) {
                       return;
                     }
                     state->desired_semantics = std::move(tree);
                     dispatch = state->accessibility_generation == service_generation;
                   }
                   if (dispatch) {
                     dispatch_desired_semantics(state);
                   }
                 })) {
    shutdown();
    return false;
  }
  return true;
}

bool HostWindow::clear_semantics() { return publish_semantics({}); }

bool HostWindow::retry_semantics() noexcept {
  if (state_ == nullptr || !valid()) {
    return false;
  }
  std::uint64_t service_generation{};
  {
    std::lock_guard lock{state_->mutex};
    if (state_->accessibility_adapter == nullptr ||
        (!state_->accessibility_in_flight && !state_->accessibility_retry_pending) ||
        state_->accessibility_publication_service != state_->accessibility_generation) {
      return false;
    }
    service_generation = state_->accessibility_generation;
  }
  if (!post_task(state_, state_->ui_runner.lock(), [state = state_, service_generation] {
        retry_semantics_on_ui(state, service_generation);
      })) {
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
      state_->text_session.reset();
      state_->text_sync_pending = false;
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
                state->accessibility_bridge.reset_acknowledged();
                {
                  std::lock_guard lock{state->mutex};
                  state->desired_semantics.reset();
                  state->accessibility_in_flight = false;
                  state->accessibility_retry_pending = false;
                  state->accessibility_retry_requested = false;
                  state->accessibility_publication = 0;
                  state->accessibility_publication_service = 0;
                }
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

namespace detail {

void shutdown_host_window(const std::shared_ptr<HostWindowState>& state) noexcept {
  HostWindow{state}.shutdown();
}

} // namespace detail

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

std::optional<AccessibilityServiceId> HostWindowDriver::set_accessibility_adapter(
  std::shared_ptr<AccessibilityAdapter> adapter) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return std::nullopt;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  AccessibilityServiceId service;
  bool exhausted{};
  std::shared_ptr<AccessibilityAdapter> previous;
  {
    std::lock_guard lock{state->mutex};
    if (!state->running) {
      return std::nullopt;
    }
    if (state->accessibility_generation == std::numeric_limits<std::uint64_t>::max()) {
      exhausted = true;
    } else {
      service = {++state->accessibility_generation};
      previous = std::move(state->accessibility_adapter);
      state->accessibility_adapter = std::move(adapter);
      state->accessibility_in_flight = false;
      state->accessibility_retry_pending = false;
      state->accessibility_retry_requested = false;
      state->accessibility_publication = 0;
      state->accessibility_publication_service = 0;
    }
  }
  previous.reset();
  if (exhausted) {
    report_error(
      state, HostErrorSource::accessibility,
      std::make_exception_ptr(std::overflow_error("Accessibility service generation exhausted")));
    detail::shutdown_host_window(state);
    return std::nullopt;
  }
  if (!post_task(state, state->ui_runner.lock(), [state, service] {
        {
          std::lock_guard lock{state->mutex};
          if (!state->running || state->accessibility_generation != service.value) {
            return;
          }
          state->accessibility_in_flight = false;
          state->accessibility_retry_pending = false;
          state->accessibility_retry_requested = false;
          state->accessibility_publication = 0;
          state->accessibility_publication_service = 0;
        }
        state->accessibility_bridge.reset_acknowledged();
        dispatch_desired_semantics(state);
      })) {
    detail::shutdown_host_window(state);
    return std::nullopt;
  }
  return service;
}

std::optional<TextInputServiceId>
HostWindowDriver::set_text_input_backend(std::shared_ptr<TextInputBackend> backend) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return std::nullopt;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  TextInputServiceId service;
  std::shared_ptr<TextInputBackend> previous;
  bool exhausted{};
  {
    std::lock_guard lock{state->mutex};
    if (!state->running) {
      return std::nullopt;
    }
    if (state->text_service_generation == std::numeric_limits<std::uint64_t>::max()) {
      exhausted = true;
    } else {
      service = {++state->text_service_generation};
      previous = std::move(state->text_input_backend);
      state->text_input_backend = std::move(backend);
    }
  }
  previous.reset();
  if (exhausted) {
    report_error(
      state, HostErrorSource::text_input,
      std::make_exception_ptr(std::overflow_error("Text input service generation exhausted")));
    detail::shutdown_host_window(state);
    return std::nullopt;
  }
  if (!schedule_text_sync(state)) {
    return std::nullopt;
  }
  return service;
}

void HostWindowDriver::send_semantics_action(AccessibilityServiceId service, std::uint64_t node,
                                             SemanticsAction action) noexcept {
  const std::shared_ptr<State> state = state_.lock();
  if (state == nullptr) {
    return;
  }
  std::lock_guard dispatch_lock{state->dispatch_mutex};
  std::uint64_t delegate_generation{};
  WindowId id;
  bool invalid{};
  {
    std::lock_guard lock{state->mutex};
    if (!state->running || state->accessibility_adapter == nullptr ||
        state->accessibility_generation != service.value) {
      return;
    }
    invalid = !service.valid() || node == 0 ||
              (action != SemanticsAction::activate && action != SemanticsAction::focus);
    delegate_generation = state->delegate_generation;
    id = state->id;
  }
  if (invalid) {
    report_error(state, HostErrorSource::invalid_platform_event,
                 std::make_exception_ptr(std::invalid_argument("Invalid semantics action")));
    return;
  }
  if (!post_task(state, state->ui_runner.lock(),
                 [state, service, delegate_generation, id, node, action] {
                   {
                     std::lock_guard lock{state->mutex};
                     if (!state->running || state->accessibility_adapter == nullptr ||
                         state->accessibility_generation != service.value) {
                       return;
                     }
                   }
                   invoke_delegate(state, delegate_generation,
                                   [id, node, action](HostWindowDelegate& delegate) {
                                     delegate.semantics_action(id, node, action);
                                   });
                 })) {
    detail::shutdown_host_window(state);
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
  state->text_input_proxy = std::make_shared<detail::HostTextInputProxy>(state);
  return {HostWindow{state}, HostWindowDriver{state}};
}

} // namespace dui
