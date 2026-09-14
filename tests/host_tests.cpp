#include "dui/ui.hpp"

#include <cmath>
#include <cstddef>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <class Exception, class Operation> bool throws(Operation&& operation) {
  try {
    operation();
  } catch (const Exception&) {
    return true;
  }
  return false;
}

class QueuedRunner final : public dui::TaskRunner {
public:
  void post(Task task) override {
    if (fail_next_post) {
      fail_next_post = false;
      throw std::runtime_error("post failed");
    }
    tasks.push_back(std::move(task));
  }

  [[nodiscard]] bool runs_tasks_on_current_thread() const noexcept override { return running; }

  bool run_one() {
    if (tasks.empty()) {
      return false;
    }
    Task task = std::move(tasks.front());
    tasks.pop_front();
    running = true;
    try {
      task();
      running = false;
    } catch (...) {
      running = false;
      throw;
    }
    return true;
  }

  void run_all() {
    while (run_one()) {
    }
  }

  [[nodiscard]] std::size_t pending_count() const { return tasks.size(); }

  std::deque<Task> tasks;
  bool fail_next_post{};
  bool running{};
};

class RecordingControl final : public dui::HostWindowControl {
public:
  void request_frame(std::uint64_t generation) override {
    if (throw_on_frame) {
      throw std::runtime_error("frame control failed");
    }
    frame_generations.push_back(generation);
    if (reentrant_window != nullptr) {
      reentered_without_lock = reentrant_window->valid();
    }
  }
  void cancel_frame() override {
    calls.push_back("cancel-frame");
    if (throw_on_cancel) {
      throw std::runtime_error("cancel control failed");
    }
  }
  void set_title(std::string_view title) override {
    calls.push_back("title:" + std::string{title});
  }
  void request_close() override { calls.push_back("request-close"); }
  void shutdown() override { calls.push_back("shutdown"); }

  std::vector<std::uint64_t> frame_generations;
  std::vector<std::string> calls;
  dui::HostWindow* reentrant_window{};
  bool throw_on_frame{};
  bool throw_on_cancel{};
  bool reentered_without_lock{};
};

class UnavailableSurface final : public dui::RasterSurface {
protected:
  dui::SurfaceAcquisition do_acquire(const dui::SurfaceRequest&) override {
    return dui::SurfaceAcquisition::failed(dui::SurfaceAcquireStatus::unavailable);
  }
};

class RecordingAccessibilityAdapter final : public dui::AccessibilityAdapter {
public:
  void apply(std::span<const dui::SemanticsChange> changes) override {
    deliveries.emplace_back(changes.begin(), changes.end());
    if (fail_next) {
      fail_next = false;
      throw std::runtime_error("accessibility apply failed");
    }
  }

  std::vector<std::vector<dui::SemanticsChange>> deliveries;
  bool fail_next{};
};

struct TextBackendLog {
  struct Start {
    dui::TextInputSessionId native_session{};
    dui::TextEditingValue value;
    std::weak_ptr<dui::TextInputClient> client;
  };

  std::vector<Start> starts;
  std::vector<std::pair<dui::TextInputSessionId, dui::TextEditingValue>> updates;
  std::vector<std::pair<dui::TextInputSessionId, dui::Rect>> rects;
  std::vector<dui::TextInputSessionId> stops;
};

class RecordingTextBackend final : public dui::TextInputBackend {
public:
  explicit RecordingTextBackend(std::shared_ptr<TextBackendLog> log) : log_(std::move(log)) {}

  dui::TextInputSessionId start_text_input(std::weak_ptr<dui::TextInputClient> client,
                                           dui::TextInputConfiguration,
                                           dui::TextEditingValue value) override {
    if (fail_start) {
      fail_start = false;
      throw std::runtime_error("text start failed");
    }
    const auto session = ++next_session;
    log_->starts.push_back({session, std::move(value), std::move(client)});
    return session;
  }
  void update_editing_state(dui::TextInputSessionId session, dui::TextEditingValue value) override {
    if (fail_update) {
      fail_update = false;
      throw std::runtime_error("text update failed after session loss");
    }
    log_->updates.emplace_back(session, std::move(value));
  }
  void stop_text_input(dui::TextInputSessionId session) override { log_->stops.push_back(session); }
  void set_editable_rect(dui::TextInputSessionId session, dui::Rect rect) override {
    if (fail_rect) {
      fail_rect = false;
      throw std::runtime_error("text rectangle failed after session loss");
    }
    log_->rects.emplace_back(session, rect);
  }

  std::shared_ptr<TextBackendLog> log_;
  dui::TextInputSessionId next_session{};
  bool fail_start{};
  bool fail_update{};
  bool fail_rect{};
};

class RecordingTextClient final : public dui::TextInputClient {
public:
  void update_editing_value(dui::TextEditingValue value) override {
    values.push_back(std::move(value));
  }
  void perform_action(dui::TextInputAction action) override { actions.push_back(action); }

  std::vector<dui::TextEditingValue> values;
  std::vector<dui::TextInputAction> actions;
};

dui::SemanticsNode semantics_node(std::uint64_t id, std::string label) {
  return {id,
          dui::SemanticsRole::button,
          std::move(label),
          {},
          true,
          {},
          {dui::SemanticsAction::activate},
          {},
          false,
          false};
}

class RecordingDelegate final : public dui::HostWindowDelegate {
public:
  void window_created(dui::WindowId, dui::WindowMetrics metrics) override {
    events.push_back("created:" + std::to_string(metrics.generation));
  }
  void metrics_changed(dui::WindowId, dui::WindowMetrics metrics) override {
    events.push_back("metrics:" + std::to_string(metrics.generation));
    if (throw_on_metrics) {
      throw std::runtime_error("metrics callback failed");
    }
  }
  void visibility_changed(dui::WindowId, bool visible) override {
    events.push_back(visible ? "visible" : "hidden");
  }
  void focus_changed(dui::WindowId, bool focused) override {
    events.push_back(focused ? "focused" : "unfocused");
  }
  void frame_requested(dui::FrameRequest request) override {
    events.push_back("frame:" + std::to_string(request.metrics.generation));
    frames.push_back(request);
    if (request_followup && window != nullptr) {
      require(window->request_frame(), "frame callback could not request a follow-up");
    }
  }
  void pointer_event(dui::WindowId, dui::PointerEvent event) override {
    events.push_back(event.phase == dui::PointerPhase::cancel
                       ? "cancel:" + std::to_string(event.pointer)
                       : "pointer:" + std::to_string(event.pointer));
    pointers.push_back(event);
    if (throw_on_first_cancel && event.phase == dui::PointerPhase::cancel) {
      throw_on_first_cancel = false;
      throw std::runtime_error("pointer cancellation failed");
    }
  }
  void key_event(dui::WindowId, dui::KeyEvent event) override {
    events.push_back("key:" + event.logical_key);
    keys.push_back(std::move(event));
  }
  void surface_changed(dui::SurfaceEvent event) override {
    events.push_back("surface:" + std::to_string(static_cast<int>(event.state)) + ':' +
                     std::to_string(event.generation));
    surfaces.push_back(event);
  }
  void semantics_action(dui::WindowId, std::uint64_t node, dui::SemanticsAction action) override {
    semantics_actions.emplace_back(node, action);
    events.push_back("semantics:" + std::to_string(node));
  }
  void close_requested(dui::WindowId) override {
    events.push_back("close");
    if (throw_on_close) {
      throw std::runtime_error("delegate failed");
    }
  }
  void window_shutting_down(dui::WindowId) override { events.push_back("shutdown"); }

  std::vector<std::string> events;
  std::vector<dui::FrameRequest> frames;
  std::vector<dui::PointerEvent> pointers;
  std::vector<dui::KeyEvent> keys;
  std::vector<dui::SurfaceEvent> surfaces;
  std::vector<std::pair<std::uint64_t, dui::SemanticsAction>> semantics_actions;
  dui::HostWindow* window{};
  bool request_followup{};
  bool throw_on_close{};
  bool throw_on_first_cancel{};
  bool throw_on_metrics{};
};

struct Rig {
  std::shared_ptr<QueuedRunner> platform{std::make_shared<QueuedRunner>()};
  std::shared_ptr<QueuedRunner> ui{std::make_shared<QueuedRunner>()};
  std::shared_ptr<RecordingControl> control{std::make_shared<RecordingControl>()};
  std::shared_ptr<UnavailableSurface> surface{std::make_shared<UnavailableSurface>()};
  std::vector<dui::HostErrorSource> errors;
  bool shutdown_on_error{};
  bool retry_on_accessibility_error{};
  std::optional<bool> accessibility_retry_result;
  dui::HostWindowEndpoints endpoints;

  explicit Rig(bool visible = true)
    : endpoints(dui::make_host_window(
        {7}, {"test", {100.0, 50.0}, 200, 100, 2.0, visible}, platform, ui, control, surface,
        [this](dui::WindowId, dui::HostErrorSource source, std::exception_ptr) {
          errors.push_back(source);
          static_cast<void>(endpoints.window.valid());
          if (shutdown_on_error) {
            endpoints.window.shutdown();
          }
          if (retry_on_accessibility_error && source == dui::HostErrorSource::accessibility) {
            accessibility_retry_result = endpoints.window.retry_semantics();
          }
        })) {
    control->reentrant_window = &endpoints.window;
  }

  void drain() {
    ui->run_all();
    platform->run_all();
  }
};

void metrics_are_validated_and_generation_ordered() {
  require(!dui::WindowMetrics{}.valid() && !dui::WindowMetrics{{1.0, 1.0}, 1, 1, 0.0, 1}.valid() &&
            !dui::WindowMetrics{{1.0, 1.0}, 3, 1, 1.0, 1}.valid() &&
            !dui::WindowMetrics{{NAN, 1.0}, 1, 1, 1.0, 1}.valid(),
          "WindowMetrics accepted an invalid extent, DPR, generation, or rounding mismatch");
  require(throws<std::invalid_argument>([] {
            static_cast<void>(dui::make_host_window(
              {}, {"bad", {1.0, 1.0}, 1, 1, 1.0, true}, std::make_shared<QueuedRunner>(),
              std::make_shared<QueuedRunner>(), std::make_shared<RecordingControl>()));
          }),
          "host factory accepted a zero WindowId");

  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  require(delegate->events == std::vector<std::string>{"created:1", "visible"},
          "delegate bind did not publish creation before visibility");
  rig.endpoints.driver.set_metrics({120.0, 60.0}, 240, 120, 2.0);
  rig.endpoints.driver.set_metrics({120.0, 60.0}, 240, 120, 2.0);
  rig.ui->run_all();
  require(delegate->events.back() == "metrics:2" &&
            rig.endpoints.window.metrics()->surface_request().generation == 2,
          "metrics update was duplicated or did not advance its generation");
  rig.endpoints.driver.set_metrics({120.0, 60.0}, 1, 1, 2.0);
  require(rig.errors.back() == dui::HostErrorSource::invalid_platform_event &&
            rig.endpoints.window.metrics()->generation == 2,
          "invalid native metrics mutated host state or escaped containment");
}

void delegate_binding_is_weak_and_generation_checked() {
  Rig rig;
  auto first = std::make_shared<RecordingDelegate>();
  auto first_binding = rig.endpoints.window.bind_delegate(first);
  rig.endpoints.driver.request_framework_close();
  auto second = std::make_shared<RecordingDelegate>();
  auto second_binding = rig.endpoints.window.bind_delegate(second);
  first_binding.reset();
  rig.ui->run_all();
  require(first->events.empty() &&
            second->events == std::vector<std::string>{"created:1", "visible"} && second_binding,
          "rebinding delivered stale callbacks or an old binding detached the new delegate");
  second.reset();
  rig.endpoints.driver.request_framework_close();
  rig.ui->run_all();
  require(!second_binding, "HostWindow retained an expired delegate");
}

void frame_requests_coalesce_and_rearm_after_callbacks() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  delegate->window = &rig.endpoints.window;
  delegate->request_followup = true;
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  require(rig.endpoints.window.request_frame() && rig.endpoints.window.request_frame() &&
            rig.platform->pending_count() == 1,
          "frame demand did not coalesce before the native request");
  rig.platform->run_all();
  require(rig.control->frame_generations.size() == 1 && rig.control->reentered_without_lock,
          "frame control was duplicated or invoked while the host lock was held");
  const auto first_generation = rig.control->frame_generations.back();
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{10}, first_generation);
  rig.ui->run_all();
  require(delegate->frames.size() == 1 && delegate->frames[0].sequence == 1 &&
            rig.platform->pending_count() == 1,
          "frame callback did not produce one request and one requested follow-up");
  rig.platform->run_all();
  require(rig.control->frame_generations.size() == 2 &&
            rig.control->frame_generations[1] != first_generation,
          "follow-up frame reused a native frame generation");
}

void metrics_invalidate_stale_pulses_and_precede_frames() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  static_cast<void>(rig.endpoints.window.request_frame());
  rig.platform->run_all();
  const auto stale = rig.control->frame_generations.back();
  rig.endpoints.driver.set_metrics({80.0, 40.0}, 160, 80, 2.0);
  rig.platform->run_all();
  const auto current = rig.control->frame_generations.back();
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{1}, stale);
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{2}, current);
  rig.ui->run_all();
  require(delegate->frames.size() == 1 && delegate->frames[0].metrics.generation == 2 &&
            delegate->events[2] == "metrics:2" && delegate->events[3] == "frame:2",
          "stale pulse survived metrics invalidation or frame preceded metrics delivery");
}

void lifecycle_cancels_pointers_before_focus_and_visibility() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  rig.endpoints.driver.set_focused(true);
  rig.endpoints.driver.send_pointer({9, dui::PointerPhase::down, {1.0, 2.0}});
  rig.endpoints.driver.send_pointer({3, dui::PointerPhase::down, {2.0, 3.0}});
  rig.endpoints.driver.set_visible(false);
  rig.ui->run_all();
  require(delegate->events == std::vector<std::string>{"created:1", "visible", "focused",
                                                       "pointer:9", "pointer:3", "cancel:3",
                                                       "cancel:9", "unfocused", "hidden"},
          "visibility loss did not cancel pointers in sorted order before focus and visibility");
  rig.endpoints.driver.set_focused(true);
  require(rig.errors.back() == dui::HostErrorSource::invalid_platform_event,
          "hidden host window accepted focus");
}

void input_validation_and_fifo_forwarding_are_shared() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  rig.endpoints.driver.send_pointer({1, dui::PointerPhase::move, {}});
  rig.endpoints.driver.send_pointer({1, dui::PointerPhase::down, {1.0, 1.0}});
  rig.endpoints.driver.send_pointer({1, dui::PointerPhase::up, {2.0, 2.0}});
  rig.endpoints.driver.send_key({"Enter", dui::KeyPhase::down, false, true, false, false});
  rig.endpoints.driver.send_key({"", dui::KeyPhase::down});
  rig.ui->run_all();
  require(delegate->pointers.size() == 2 && delegate->keys.size() == 1 &&
            delegate->keys[0].control && rig.errors.size() == 2,
          "host input validation lost values or forwarded malformed events");
}

void surface_state_parks_resumes_and_invalidates_frames() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::unavailable);
  static_cast<void>(rig.endpoints.window.request_frame());
  require(rig.platform->pending_count() == 0, "unavailable surface armed a frame");
  rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::available);
  rig.platform->run_all();
  const auto available_generation = rig.control->frame_generations.back();
  rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::out_of_date);
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{1}, available_generation);
  rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::available);
  rig.platform->run_all();
  const auto recovered_generation = rig.control->frame_generations.back();
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{2}, recovered_generation);
  rig.ui->run_all();
  require(delegate->frames.size() == 1 && delegate->frames[0].metrics.generation == 4 &&
            delegate->surfaces.size() == 4,
          "surface recovery did not park demand, invalidate stale pulse, or advance generations");
  rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::lost);
  rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::available);
  rig.endpoints.driver.set_surface_state(static_cast<dui::HostSurfaceState>(99));
  rig.ui->run_all();
  require(delegate->surfaces.size() == 6 && delegate->surfaces.back().generation == 5 &&
            rig.errors.back() == dui::HostErrorSource::invalid_platform_event,
          "lost-surface recovery or malformed surface validation was not observable");
}

void errors_are_contained_at_runner_control_and_delegate_boundaries() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  delegate->throw_on_close = true;
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  rig.endpoints.driver.request_framework_close();
  rig.ui->run_all();
  rig.control->throw_on_frame = true;
  static_cast<void>(rig.endpoints.window.request_frame());
  rig.platform->run_all();
  require(rig.errors == std::vector<dui::HostErrorSource>{dui::HostErrorSource::delegate_callback,
                                                          dui::HostErrorSource::platform_control},
          "delegate or platform-control exception escaped its host boundary");
  rig.control->throw_on_frame = false;
  require(rig.endpoints.window.request_frame(), "frame control failure wedged future demand");
  rig.platform->run_all();
  require(rig.control->frame_generations.size() == 1,
          "frame scheduling did not retry after a control failure");

  Rig failed;
  failed.platform->fail_next_post = true;
  require(!failed.endpoints.window.set_title("new") && !failed.endpoints.window.valid() &&
            failed.errors[0] == dui::HostErrorSource::task_post,
          "task-post failure did not report and stop the host window");
  failed.drain();

  Rig cancel_failure;
  static_cast<void>(cancel_failure.endpoints.window.request_frame());
  cancel_failure.platform->run_all();
  cancel_failure.control->throw_on_cancel = true;
  cancel_failure.endpoints.driver.set_metrics({80.0, 40.0}, 160, 80, 2.0);
  cancel_failure.platform->run_all();
  require(cancel_failure.control->frame_generations.size() == 2 &&
            cancel_failure.errors.back() == dui::HostErrorSource::platform_control,
          "failed native frame cancellation suppressed the replacement request");
}

void native_event_post_failures_stop_consistently() {
  {
    Rig rig;
    rig.ui->fail_next_post = true;
    rig.endpoints.driver.set_metrics({80.0, 40.0}, 160, 80, 2.0);
    require(!rig.endpoints.window.valid() && rig.errors[0] == dui::HostErrorSource::task_post,
            "metrics post failure left the host running");
    rig.drain();
  }
  {
    Rig rig;
    auto delegate = std::make_shared<RecordingDelegate>();
    auto binding = rig.endpoints.window.bind_delegate(delegate);
    rig.ui->run_all();
    rig.endpoints.driver.send_pointer({1, dui::PointerPhase::down, {1.0, 1.0}});
    rig.ui->run_all();
    rig.shutdown_on_error = true;
    rig.ui->fail_next_post = true;
    rig.endpoints.driver.set_visible(false);
    require(!rig.endpoints.window.valid(), "visibility post failure left the host running");
    rig.drain();
    require(delegate->events[delegate->events.size() - 2] == "cancel:1" &&
              delegate->events.back() == "shutdown",
            "visibility post failure lost active-pointer shutdown cancellation");
  }
  {
    Rig rig;
    auto delegate = std::make_shared<RecordingDelegate>();
    auto binding = rig.endpoints.window.bind_delegate(delegate);
    rig.ui->run_all();
    rig.endpoints.driver.set_focused(true);
    rig.endpoints.driver.send_pointer({1, dui::PointerPhase::down, {1.0, 1.0}});
    rig.ui->run_all();
    rig.shutdown_on_error = true;
    rig.ui->fail_next_post = true;
    rig.endpoints.driver.set_focused(false);
    require(!rig.endpoints.window.valid(), "focus post failure left the host running");
    rig.drain();
    require(delegate->events[delegate->events.size() - 2] == "cancel:1" &&
              delegate->events.back() == "shutdown",
            "focus post failure lost active-pointer shutdown cancellation");
  }
  {
    Rig rig;
    auto delegate = std::make_shared<RecordingDelegate>();
    auto binding = rig.endpoints.window.bind_delegate(delegate);
    rig.ui->run_all();
    rig.shutdown_on_error = true;
    rig.ui->fail_next_post = true;
    rig.endpoints.driver.send_pointer({1, dui::PointerPhase::down, {1.0, 1.0}});
    require(!rig.endpoints.window.valid(), "pointer post failure left mutated input state running");
    rig.drain();
    require(delegate->pointers.empty(),
            "failed pointer-down post produced an orphan shutdown cancellation");
  }
  {
    Rig rig;
    rig.ui->fail_next_post = true;
    rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::lost);
    require(!rig.endpoints.window.valid(), "surface post failure left the host running");
    rig.drain();
  }
  {
    Rig rig;
    rig.ui->fail_next_post = true;
    rig.endpoints.driver.request_framework_close();
    require(!rig.endpoints.window.valid(), "close post failure left the host running");
    rig.drain();
  }
}

void delegate_failures_do_not_skip_compound_callbacks() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  rig.endpoints.driver.set_focused(true);
  rig.endpoints.driver.send_pointer({9, dui::PointerPhase::down, {1.0, 1.0}});
  rig.endpoints.driver.send_pointer({3, dui::PointerPhase::down, {1.0, 1.0}});
  rig.ui->run_all();
  delegate->throw_on_first_cancel = true;
  rig.endpoints.driver.set_visible(false);
  rig.ui->run_all();
  require(delegate->events[delegate->events.size() - 4] == "cancel:3" &&
            delegate->events[delegate->events.size() - 3] == "cancel:9" &&
            delegate->events[delegate->events.size() - 2] == "unfocused" &&
            delegate->events.back() == "hidden",
          "one failed pointer cancellation suppressed later lifecycle callbacks");

  rig.endpoints.driver.set_visible(true);
  rig.ui->run_all();
  delegate->throw_on_metrics = true;
  rig.endpoints.driver.set_surface_state(dui::HostSurfaceState::out_of_date);
  rig.ui->run_all();
  require(delegate->events[delegate->events.size() - 2] == "metrics:2" &&
            delegate->events.back().starts_with("surface:"),
          "failed metrics callback suppressed its surface transition");
}

void binding_replacement_ends_old_pointer_streams() {
  Rig rig;
  auto first = std::make_shared<RecordingDelegate>();
  auto first_binding = rig.endpoints.window.bind_delegate(first);
  rig.ui->run_all();
  rig.endpoints.driver.send_pointer({2, dui::PointerPhase::down, {1.0, 1.0}});
  rig.ui->run_all();
  auto second = std::make_shared<RecordingDelegate>();
  auto second_binding = rig.endpoints.window.bind_delegate(second);
  first_binding.reset();
  rig.ui->run_all();
  rig.endpoints.driver.send_pointer({2, dui::PointerPhase::up, {1.0, 1.0}});
  rig.ui->run_all();
  require(second->pointers.empty() &&
            rig.errors.back() == dui::HostErrorSource::invalid_platform_event && second_binding,
          "replacement delegate inherited a pointer stream whose down went to its predecessor");
}

void frame_timestamps_must_be_monotonic() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  static_cast<void>(rig.endpoints.window.request_frame());
  rig.platform->run_all();
  auto generation = rig.control->frame_generations.back();
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{10}, generation);
  rig.ui->run_all();
  static_cast<void>(rig.endpoints.window.request_frame());
  rig.platform->run_all();
  generation = rig.control->frame_generations.back();
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{9}, generation);
  rig.endpoints.driver.frame_pulse(std::chrono::nanoseconds{11}, generation);
  rig.ui->run_all();
  require(delegate->frames.size() == 2 && delegate->frames.back().timestamp.count() == 11 &&
            rig.errors.back() == dui::HostErrorSource::invalid_platform_event,
          "regressing frame timestamp was accepted or consumed the valid native token");
}

void shutdown_delivery_stays_weak_and_platform_affine() {
  {
    Rig rig;
    auto delegate = std::make_shared<RecordingDelegate>();
    std::weak_ptr<RecordingDelegate> weak = delegate;
    auto binding = rig.endpoints.window.bind_delegate(delegate);
    rig.ui->run_all();
    rig.endpoints.window.shutdown();
    binding.reset();
    delegate.reset();
    require(weak.expired(), "queued shutdown retained framework delegate ownership");
    rig.drain();
  }
  {
    Rig rig;
    auto delegate = std::make_shared<RecordingDelegate>();
    auto binding = rig.endpoints.window.bind_delegate(delegate);
    rig.ui->run_all();
    rig.endpoints.window.shutdown();
    rig.platform->fail_next_post = true;
    rig.ui->run_all();
    require(rig.control->calls.empty() && rig.errors.back() == dui::HostErrorSource::task_post,
            "failed platform post invoked native shutdown control from the UI executor");
    rig.endpoints.window.shutdown();
    rig.platform->run_all();
    require(rig.control->calls == std::vector<std::string>{"cancel-frame", "shutdown"},
            "native shutdown could not retry after a transient platform-post failure");
  }
}

void shutdown_is_ordered_idempotent_and_invalidates_endpoints() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();
  rig.endpoints.driver.send_pointer({4, dui::PointerPhase::down, {1.0, 1.0}});
  rig.endpoints.window.shutdown();
  rig.endpoints.window.shutdown();
  rig.ui->run_all();
  require(delegate->events == std::vector<std::string>{"created:1", "visible", "pointer:4",
                                                       "cancel:4", "shutdown"} &&
            rig.platform->pending_count() == 1,
          "shutdown did not invalidate queued input and notify the delegate first");
  rig.platform->run_all();
  require(rig.control->calls == std::vector<std::string>{"cancel-frame", "shutdown"} &&
            !rig.endpoints.window.valid() && !rig.endpoints.driver.valid() && !binding &&
            !rig.endpoints.window.request_frame() && rig.endpoints.window.metrics() == std::nullopt,
          "shutdown was not idempotent or left old endpoints active");
  const auto error_count = rig.errors.size();
  rig.endpoints.driver.set_surface_state(static_cast<dui::HostSurfaceState>(99));
  require(rig.errors.size() == error_count, "stopped driver emitted an invalid-event callback");
}

void accessibility_service_publishes_retries_and_replaces() {
  Rig rig;
  auto delegate = std::make_shared<RecordingDelegate>();
  auto binding = rig.endpoints.window.bind_delegate(delegate);
  rig.ui->run_all();

  require(rig.endpoints.window.publish_semantics({{semantics_node(10, "initial")}}) &&
            rig.platform->pending_count() == 0,
          "semantics published without retaining an unavailable adapter target");
  auto first_adapter = std::make_shared<RecordingAccessibilityAdapter>();
  const auto first_service = rig.endpoints.driver.set_accessibility_adapter(first_adapter);
  require(first_service.has_value() && first_service->valid(),
          "accessibility adapter installation did not return a service generation");
  rig.ui->run_all();
  require(rig.platform->pending_count() == 1, "adapter installation did not replay desired tree");
  rig.platform->run_all();
  rig.ui->run_all();
  require(first_adapter->deliveries.size() == 1 &&
            first_adapter->deliveries.front().front().kind == dui::SemanticsChangeKind::added,
          "initial accessibility publication was not a complete add batch");
  require(!rig.endpoints.window.retry_semantics(),
          "semantics retry was accepted without an in-flight or rejected transaction");

  require(rig.endpoints.window.publish_semantics({{semantics_node(10, "second")}}) &&
            rig.endpoints.window.publish_semantics({{semantics_node(10, "latest")}}),
          "multiple desired semantics trees escaped one-publication-in-flight coalescing");
  rig.ui->run_all();
  require(rig.platform->pending_count() == 1,
          "multiple desired semantics trees escaped one-publication-in-flight coalescing");
  rig.platform->run_all();
  rig.ui->run_all();
  require(rig.platform->pending_count() == 1,
          "acknowledgment did not schedule the latest desired semantics tree");
  rig.platform->run_all();
  rig.ui->run_all();
  require(first_adapter->deliveries.size() == 3 &&
            first_adapter->deliveries.back().front().entry.label == "latest",
          "semantics follow-up did not converge to the latest desired tree");

  first_adapter->fail_next = true;
  static_cast<void>(rig.endpoints.window.publish_semantics({{semantics_node(10, "failed")}}));
  rig.ui->run_all();
  rig.platform->run_all();
  rig.ui->run_all();
  const auto failed_batch = first_adapter->deliveries.back();
  static_cast<void>(
    rig.endpoints.window.publish_semantics({{semantics_node(10, "after failure")}}));
  rig.ui->run_all();
  require(rig.errors.back() == dui::HostErrorSource::accessibility &&
            rig.platform->pending_count() == 0 && rig.endpoints.window.valid() &&
            rig.endpoints.window.retry_semantics(),
          "failed accessibility apply was not reported and made explicitly retryable");
  rig.ui->run_all();
  rig.platform->run_all();
  rig.ui->run_all();
  require(first_adapter->deliveries.back() == failed_batch,
          "accessibility retry changed the rejected owned batch");
  require(rig.platform->pending_count() == 1,
          "successful retry did not converge to the desired tree published during failure");
  rig.platform->run_all();
  rig.ui->run_all();
  require(first_adapter->deliveries.back().front().entry.label == "after failure",
          "accessibility retry follow-up lost the latest desired tree");

  first_adapter->fail_next = true;
  rig.retry_on_accessibility_error = true;
  static_cast<void>(
    rig.endpoints.window.publish_semantics({{semantics_node(10, "handler retry")}}));
  rig.ui->run_all();
  rig.platform->run_all();
  require(rig.accessibility_retry_result == true,
          "accessibility error handler could not retain an in-flight retry request");
  rig.ui->run_all();
  require(rig.platform->pending_count() == 1,
          "error-handler retry was lost before the failure acknowledgment");
  rig.platform->run_all();
  rig.ui->run_all();
  rig.retry_on_accessibility_error = false;

  first_adapter->fail_next = true;
  static_cast<void>(rig.endpoints.window.publish_semantics({{semantics_node(10, "replacement")}}));
  rig.ui->run_all();
  rig.platform->run_all();
  rig.ui->run_all();
  require(rig.endpoints.window.retry_semantics(),
          "rejected old-service publication could not queue replacement race retry");
  auto replacement = std::make_shared<RecordingAccessibilityAdapter>();
  const auto replacement_service = rig.endpoints.driver.set_accessibility_adapter(replacement);
  require(!rig.endpoints.window.retry_semantics(),
          "replacement exposed the previous service's rejected transaction");
  require(rig.ui->run_one() && rig.platform->pending_count() == 0,
          "old-service retry was applied to the replacement adapter before reset");
  rig.ui->run_all();
  rig.platform->run_all();
  rig.ui->run_all();
  require(replacement_service.has_value() && replacement_service->value > first_service->value &&
            replacement->deliveries.size() == 1 &&
            replacement->deliveries.front().front().kind == dui::SemanticsChangeKind::added &&
            replacement->deliveries.front().front().entry.label == "replacement",
          "replacement adapter did not receive a fresh complete snapshot");

  rig.endpoints.driver.send_semantics_action(*first_service, 10, dui::SemanticsAction::activate);
  rig.endpoints.driver.send_semantics_action(*replacement_service, 10,
                                             dui::SemanticsAction::activate);
  auto final_adapter = std::make_shared<RecordingAccessibilityAdapter>();
  const auto final_service = rig.endpoints.driver.set_accessibility_adapter(final_adapter);
  require(final_service.has_value(), "final accessibility replacement failed");
  rig.ui->run_all();
  rig.platform->run_all();
  rig.ui->run_all();
  rig.endpoints.driver.send_semantics_action(*replacement_service, 10,
                                             dui::SemanticsAction::activate);
  rig.endpoints.driver.send_semantics_action(*final_service, 10, dui::SemanticsAction::activate);
  rig.ui->run_all();
  require(delegate->semantics_actions ==
            std::vector<std::pair<std::uint64_t, dui::SemanticsAction>>{
              {10, dui::SemanticsAction::activate}},
          "queued or stale accessibility service action survived replacement");
  rig.endpoints.driver.send_semantics_action(*final_service, 0, dui::SemanticsAction::activate);
  require(rig.errors.back() == dui::HostErrorSource::invalid_platform_event,
          "malformed semantics action was not contained");

  static_cast<void>(rig.endpoints.window.clear_semantics());
  rig.ui->run_all();
  rig.platform->run_all();
  rig.ui->run_all();
  require(final_adapter->deliveries.back().front().kind == dui::SemanticsChangeKind::removed,
          "host semantics clear did not publish removals");

  std::weak_ptr<RecordingAccessibilityAdapter> weak_replacement = final_adapter;
  first_adapter.reset();
  replacement.reset();
  final_adapter.reset();
  rig.endpoints.window.shutdown();
  rig.ui->run_all();
  require(!weak_replacement.expired(),
          "accessibility adapter was released before platform shutdown execution");
  rig.platform->run_all();
  require(weak_replacement.expired(),
          "accessibility adapter was not released on platform shutdown execution");
}

void text_input_service_marshals_sessions_and_replacement() {
  Rig rig;
  auto proxy = rig.endpoints.window.text_input_backend();
  auto first_client = std::make_shared<RecordingTextClient>();
  const auto stale_session =
    proxy->start_text_input(first_client, {}, {"old", {3, 3}, std::nullopt});
  const auto session = proxy->start_text_input(first_client, {}, {"initial", {7, 7}, std::nullopt});
  proxy->update_editing_state(session, {"latest", {6, 6}, std::nullopt});
  proxy->set_editable_rect(session, {{1.0, 2.0}, {30.0, 10.0}});
  proxy->set_editable_rect(session, {{3.0, 4.0}, {40.0, 12.0}});
  require(rig.platform->pending_count() == 1,
          "pre-service text input commands did not coalesce to one platform sync");

  auto first_log = std::make_shared<TextBackendLog>();
  auto first_backend = std::make_shared<RecordingTextBackend>(first_log);
  const auto first_service = rig.endpoints.driver.set_text_input_backend(first_backend);
  require(first_service.has_value() && first_service->valid(),
          "text backend installation did not return a service generation");
  rig.platform->run_all();
  require(first_log->starts.size() == 1 && first_log->starts.front().value.text == "latest" &&
            first_log->rects.size() == 1 &&
            first_log->rects.front().second == dui::Rect{{3.0, 4.0}, {40.0, 12.0}} &&
            first_log->updates.empty(),
          "pending text start did not use the latest value and editable rectangle");

  require(!throws<std::invalid_argument>([&] {
    proxy->update_editing_state(stale_session, {"\x80", {0, 0}, std::nullopt});
  }) && throws<std::invalid_argument>([&] {
    proxy->update_editing_state(session, {"\x80", {0, 0}, std::nullopt});
  }),
          "stale text session was validated or active malformed UTF-8 was accepted");
  proxy->update_editing_state(session, {"framework", {9, 9}, std::nullopt});
  rig.platform->run_all();
  require(first_log->updates.size() == 1 && first_log->updates.front().second.text == "framework",
          "active framework editing state was not marshaled to the native session");

  auto old_forwarder = first_log->starts.front().client.lock();
  require(old_forwarder != nullptr, "native backend did not retain a live forwarding client");
  old_forwarder->update_editing_value({"native", {6, 6}, std::nullopt});
  old_forwarder->perform_action(dui::TextInputAction::done);
  proxy->stop_text_input(session);
  rig.ui->run_all();
  rig.platform->run_all();
  require(first_client->values.empty() && first_client->actions.empty() &&
            first_log->stops.size() == 1,
          "stopped public session accepted queued native callbacks or did not stop native input");

  auto second_client = std::make_shared<RecordingTextClient>();
  const auto replacement_session =
    proxy->start_text_input(second_client, {}, {"replacement", {11, 11}, std::nullopt});
  rig.platform->run_all();
  auto current_forwarder = first_log->starts.back().client.lock();
  current_forwarder->update_editing_value({"native retained", {15, 15}, std::nullopt});
  rig.ui->run_all();
  auto second_log = std::make_shared<TextBackendLog>();
  auto second_backend = std::make_shared<RecordingTextBackend>(second_log);
  const auto second_service = rig.endpoints.driver.set_text_input_backend(second_backend);
  rig.platform->run_all();
  require(second_service.has_value() && second_service->value > first_service->value &&
            first_log->stops.size() == 2 && second_log->starts.size() == 1 &&
            second_log->starts.front().value.text == "native retained",
          "text backend replacement did not stop old native input before restarting current state");
  second_client->values.clear();
  current_forwarder->update_editing_value({"stale", {5, 5}, std::nullopt});
  auto replacement_forwarder = second_log->starts.front().client.lock();
  replacement_forwarder->update_editing_value({"current", {7, 7}, std::nullopt});
  replacement_forwarder->perform_action(dui::TextInputAction::search);
  rig.ui->run_all();
  require(second_client->values.size() == 1 && second_client->values.front().text == "current" &&
            second_client->actions ==
              std::vector<dui::TextInputAction>{dui::TextInputAction::search},
          "text forwarding accepted an old service callback or lost current values/actions");

  second_backend->fail_update = true;
  proxy->update_editing_state(replacement_session, {"recreated", {9, 9}, std::nullopt});
  rig.platform->run_all();
  require(rig.errors.back() == dui::HostErrorSource::text_input && second_log->stops.size() == 1 &&
            second_log->starts.size() == 2 && second_log->starts.back().value.text == "recreated",
          "native update failure did not invalidate and recreate the backend session");

  second_client->values.clear();
  second_client->actions.clear();
  replacement_forwarder->update_editing_value(
    {"discarded native instance", {25, 25}, std::nullopt});
  auto recreated_forwarder = second_log->starts.back().client.lock();
  recreated_forwarder->update_editing_value({"recreated native", {16, 16}, std::nullopt});
  recreated_forwarder->perform_action(dui::TextInputAction::send);
  rig.ui->run_all();
  require(second_client->values.size() == 1 &&
            second_client->values.front().text == "recreated native" &&
            second_client->actions == std::vector<dui::TextInputAction>{dui::TextInputAction::send},
          "old native-instance forwarding callback survived session recreation");

  second_backend->fail_rect = true;
  proxy->set_editable_rect(replacement_session, {{5.0, 6.0}, {20.0, 8.0}});
  rig.platform->run_all();
  require(rig.platform->pending_count() == 0 && second_log->starts.size() == 2,
          "persistent editable-rectangle failure entered an automatic restart loop");
  proxy->set_editable_rect(replacement_session, {{7.0, 8.0}, {24.0, 9.0}});
  rig.platform->run_all();
  require(second_log->stops.size() == 2 && second_log->starts.size() == 3 &&
            second_log->rects.back().second == dui::Rect{{7.0, 8.0}, {24.0, 9.0}},
          "framework rectangle retry did not recreate and synchronize native text input");

  auto expiry_forwarder = second_log->starts.back().client.lock();
  second_client.reset();
  expiry_forwarder->perform_action(dui::TextInputAction::done);
  rig.ui->run_all();
  rig.platform->run_all();
  require(second_log->stops.size() == 3,
          "expired framework text client did not retire its native session");
  proxy->stop_text_input(replacement_session);
  auto retry_client = std::make_shared<RecordingTextClient>();
  second_backend->fail_start = true;
  const auto retry_session =
    proxy->start_text_input(retry_client, {}, {"retry", {5, 5}, std::nullopt});
  rig.platform->run_all();
  require(rig.errors.back() == dui::HostErrorSource::text_input && second_log->starts.size() == 3,
          "text start failure was not contained without installing a native session");
  proxy->update_editing_state(retry_session, {"retry latest", {12, 12}, std::nullopt});
  rig.platform->run_all();
  require(second_log->starts.size() == 4 && second_log->starts.back().value.text == "retry latest",
          "text desired update did not retry a failed native start");

  std::weak_ptr<RecordingTextBackend> weak_backend = second_backend;
  first_backend.reset();
  second_backend.reset();
  rig.endpoints.window.shutdown();
  rig.ui->run_all();
  require(!weak_backend.expired(), "native text backend was released before platform shutdown");
  rig.platform->run_all();
  require(weak_backend.expired() && second_log->stops.size() == 4 &&
            !rig.endpoints.window.text_input_backend() && throws<std::logic_error>([&] {
              static_cast<void>(proxy->start_text_input(retry_client, {}, {"", {}, std::nullopt}));
            }),
          "text shutdown did not stop/release the backend or make the old proxy inert");

  Rig post_failure;
  auto failed_proxy = post_failure.endpoints.window.text_input_backend();
  auto failed_client = std::make_shared<RecordingTextClient>();
  post_failure.platform->fail_next_post = true;
  require(throws<std::runtime_error>([&] {
            static_cast<void>(
              failed_proxy->start_text_input(failed_client, {}, {"post", {4, 4}, std::nullopt}));
          }) &&
            !post_failure.endpoints.window.valid() &&
            post_failure.errors.front() == dui::HostErrorSource::task_post,
          "text platform-post failure did not stop the host and fail session creation");
  post_failure.drain();
}

void raster_surface_identity_remains_the_existing_boundary() {
  Rig rig;
  require(rig.endpoints.window.raster_surface() == rig.surface,
          "host coordinator replaced or wrapped the existing RasterSurface boundary");
  rig.endpoints.window.shutdown();
  require(rig.endpoints.window.raster_surface() == nullptr,
          "stopped HostWindow retained an externally usable RasterSurface");
  rig.drain();
}

} // namespace

int main() {
  try {
    metrics_are_validated_and_generation_ordered();
    delegate_binding_is_weak_and_generation_checked();
    frame_requests_coalesce_and_rearm_after_callbacks();
    metrics_invalidate_stale_pulses_and_precede_frames();
    lifecycle_cancels_pointers_before_focus_and_visibility();
    input_validation_and_fifo_forwarding_are_shared();
    surface_state_parks_resumes_and_invalidates_frames();
    errors_are_contained_at_runner_control_and_delegate_boundaries();
    native_event_post_failures_stop_consistently();
    delegate_failures_do_not_skip_compound_callbacks();
    binding_replacement_ends_old_pointer_streams();
    frame_timestamps_must_be_monotonic();
    shutdown_delivery_stays_weak_and_platform_affine();
    shutdown_is_ordered_idempotent_and_invalidates_endpoints();
    accessibility_service_publishes_retries_and_replaces();
    text_input_service_marshals_sessions_and_replacement();
    raster_surface_identity_remains_the_existing_boundary();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All host tests passed\n";
  return 0;
}
