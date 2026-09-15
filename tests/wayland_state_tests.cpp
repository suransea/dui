#include "dui/platform/wayland.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

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

void initial_commit_and_configure_are_ordered() {
  using dui::platform::WaylandExtent;
  using dui::platform::WaylandSurfaceState;

  require(throws<std::invalid_argument>([] {
            WaylandSurfaceState state{{0, 20}};
          }),
          "zero initial extent was accepted");
  WaylandSurfaceState state{{640, 480}};
  require(!state.prepare_commit().has_value(), "content was prepared before initial configure");
  require(throws<std::invalid_argument>([&] { state.receive_surface_configure(1); }),
          "surface configure was accepted before the initial bufferless commit");
  require(state.begin_initial_commit() && !state.begin_initial_commit(),
          "initial bufferless commit was not exactly once");
  state.receive_surface_configure(0);
  const auto wrapped_serial = state.prepare_commit();
  require(wrapped_serial.has_value() && wrapped_serial->valid() &&
            wrapped_serial->configure_serial == 0 && state.discard_commit(*wrapped_serial),
          "wrapped zero configure serial was not treated as an opaque valid value");
  require(throws<std::invalid_argument>([&] {
            state.receive_toplevel_configure(
              static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1U, 10);
          }),
          "toplevel extent outside the signed protocol range was accepted");

  state.receive_toplevel_configure(800, 0);
  state.receive_surface_configure(11);
  state.receive_toplevel_configure(1024, 0);
  state.receive_surface_configure(12);
  const auto commit = state.prepare_commit();
  require(commit.has_value() && commit->valid() && commit->configure_serial == 12 &&
            commit->logical_extent == WaylandExtent{1024, 480} &&
            commit->buffer_extent == WaylandExtent{1024, 480},
          "superseded configure was not coalesced to the latest serial and extent");
  require(state.submit_commit(*commit) && state.configured() &&
            state.logical_extent() == WaylandExtent{1024, 480} &&
            !state.prepare_commit().has_value(),
          "accepted initial content did not establish configured state");
}

void stale_plans_and_buffer_failures_remain_retryable() {
  using dui::platform::WaylandExtent;
  using dui::platform::WaylandSurfaceState;

  WaylandSurfaceState state{{320, 200}};
  static_cast<void>(state.begin_initial_commit());
  state.receive_surface_configure(20);
  const auto stale = state.prepare_commit();
  state.receive_toplevel_configure(400, 300);
  state.receive_surface_configure(21);
  require(!state.submit_commit(*stale) && !state.discard_commit(*stale),
          "a plan invalidated by newer configure remained consumable");
  const auto current = state.prepare_commit();
  require(current->configure_serial == 21 && current->logical_extent == WaylandExtent{400, 300} &&
            state.discard_commit(*current),
          "latest configure did not replace stale prepared content");
  const auto retry = state.prepare_commit();
  require(retry.has_value() && retry->generation != current->generation &&
            retry->configure_serial == current->configure_serial &&
            retry->buffer_extent == current->buffer_extent && state.submit_commit(*retry),
          "failed buffer preparation did not retry the same desired configure under a new token");

  state.buffer_lost();
  const auto recovery = state.prepare_commit();
  require(recovery.has_value() && !recovery->configure_serial.has_value() &&
            state.submit_commit(*recovery),
          "recoverable buffer loss did not request content recreation");
}

void integer_and_fractional_scaling_are_explicit() {
  using dui::platform::WaylandExtent;
  using dui::platform::WaylandSurfaceState;

  WaylandSurfaceState integer{{101, 51}};
  static_cast<void>(integer.begin_initial_commit());
  integer.set_preferred_scale(150);
  integer.receive_surface_configure(30);
  const auto integer_commit = integer.prepare_commit();
  require(integer_commit->scale_numerator == 240 && integer_commit->buffer_scale == 2 &&
            !integer_commit->use_viewporter &&
            integer_commit->buffer_extent == WaylandExtent{202, 102},
          "integer fallback did not round scale up consistently");

  WaylandSurfaceState fractional{{101, 51}, true};
  static_cast<void>(fractional.begin_initial_commit());
  fractional.set_preferred_scale(150);
  fractional.receive_surface_configure(31);
  const auto fractional_commit = fractional.prepare_commit();
  require(fractional_commit->scale_numerator == 150 && fractional_commit->buffer_scale == 1 &&
            fractional_commit->use_viewporter &&
            fractional_commit->buffer_extent == WaylandExtent{127, 64},
          "fractional scaling did not preserve logical-to-buffer mapping");
  auto malformed = *fractional_commit;
  malformed.buffer_scale = 2;
  require(!malformed.valid(), "malformed viewporter plan reported itself as valid");
  fractional.set_preferred_scale(180);
  require(!fractional.submit_commit(*fractional_commit) &&
            !fractional.discard_commit(*fractional_commit),
          "scale change did not invalidate a prepared plan");
  const auto rescaled = fractional.prepare_commit();
  require(rescaled.has_value() && rescaled->scale_numerator == 180 &&
            fractional.discard_commit(*rescaled),
          "scale invalidation did not preserve a fresh pre-submission plan");
  require(throws<std::invalid_argument>([&] { fractional.set_preferred_scale(0); }) &&
            throws<std::overflow_error>([&] {
              integer.set_preferred_scale(std::numeric_limits<std::uint32_t>::max());
              static_cast<void>(integer.prepare_commit());
            }),
          "invalid or overflowing scales were accepted");
}

void frame_callbacks_gate_content_without_blocking_configure() {
  using dui::platform::WaylandExtent;
  using dui::platform::WaylandSurfaceState;

  WaylandSurfaceState state{{200, 100}};
  static_cast<void>(state.begin_initial_commit());
  state.request_frame();
  state.receive_surface_configure(40);
  const auto first = state.prepare_commit();
  require(first->request_frame_callback && state.submit_commit(*first) &&
            state.frame_callback_pending(),
          "first configured content did not arm one requested frame callback");

  state.buffer_lost();
  const auto recovery = state.prepare_commit();
  require(recovery.has_value() && !recovery->request_frame_callback &&
            state.submit_commit(*recovery) && state.frame_callback_pending(),
          "buffer recovery duplicated or discarded an outstanding frame callback");

  state.request_frame();
  require(!state.prepare_commit().has_value(),
          "content bypassed an outstanding frame callback without a configure change");
  state.receive_toplevel_configure(240, 120);
  state.receive_surface_configure(41);
  const auto configured = state.prepare_commit();
  require(configured.has_value() && !configured->request_frame_callback &&
            configured->logical_extent == WaylandExtent{240, 120} &&
            state.submit_commit(*configured),
          "outstanding frame callback incorrectly blocked a required configure commit");
  require(!state.frame_done(first->generation + 100) && state.frame_done(first->generation) &&
            !state.frame_callback_pending(),
          "stale frame callback was accepted or current callback was rejected");
  const auto followup = state.prepare_commit();
  require(followup.has_value() && followup->request_frame_callback &&
            state.submit_commit(*followup),
          "frame demand did not resume once the matching callback completed");
}

void scale_changes_replace_frames_without_accepting_stale_generations() {
  using dui::platform::WaylandExtent;
  using dui::platform::WaylandSurfaceState;

  WaylandSurfaceState state{{160, 100}};
  static_cast<void>(state.begin_initial_commit());
  state.request_frame();
  state.receive_surface_configure(50);
  const auto first = state.prepare_commit();
  require(first.has_value() && first->request_frame_callback && state.submit_commit(*first),
          "initial scale test frame did not commit");

  state.set_preferred_scale(240);
  const auto scaled = state.prepare_commit();
  require(scaled.has_value() && !scaled->request_frame_callback &&
            scaled->buffer_extent == WaylandExtent{320, 200} && state.submit_commit(*scaled),
          "scale transition did not repaint behind the outstanding callback");
  state.request_frame();
  require(!state.prepare_commit().has_value() && state.frame_done(first->generation),
          "outstanding pre-scale callback did not retain its exact gate");
  const auto replacement = state.prepare_commit();
  require(replacement.has_value() && replacement->request_frame_callback &&
            replacement->buffer_extent == WaylandExtent{320, 200} &&
            state.submit_commit(*replacement),
          "scaled frame demand did not resume after the old callback");
}

void primary_pointer_state_preserves_stream_invariants() {
  using dui::PointerEvent;
  using dui::PointerPhase;
  using dui::platform::WaylandPointerState;

  require(throws<std::invalid_argument>([] { WaylandPointerState state{0}; }),
          "zero Wayland pointer ID was accepted");
  WaylandPointerState state{7};
  require(!state.motion(1.0, 2.0).has_value() && !state.primary_button(false).has_value(),
          "unfocused motion or unmatched release invented a stream");
  state.enter(3.0, 4.0);
  const auto down = state.primary_button(true);
  const auto move = state.motion(5.0, 6.0);
  require(down == PointerEvent{7, PointerPhase::down, {3.0, 4.0}} &&
            move == PointerEvent{7, PointerPhase::move, {5.0, 6.0}} && state.active(),
          "focused primary pointer did not preserve position or phase");
  require(throws<std::logic_error>([&] { static_cast<void>(state.primary_button(true)); }),
          "duplicate primary press was accepted");
  const auto cancel = state.leave();
  require(cancel == PointerEvent{7, PointerPhase::cancel, {5.0, 6.0}} && !state.focused() &&
            !state.active(),
          "surface leave did not cancel the active pointer");

  state.enter(7.0, 8.0);
  require(!state.primary_button(false).has_value(),
          "focused unmatched release invented a pointer stream");
  static_cast<void>(state.primary_button(true));
  const auto second_move = state.motion(9.0, 10.0);
  const auto up = state.primary_button(false);
  require(second_move == PointerEvent{7, PointerPhase::move, {9.0, 10.0}} &&
            up == PointerEvent{7, PointerPhase::up, {9.0, 10.0}} && !state.active(),
          "primary pointer did not complete a normal down/move/up stream");
  static_cast<void>(state.primary_button(true));
  const auto lost = state.capability_lost();
  require(lost == PointerEvent{7, PointerPhase::cancel, {9.0, 10.0}} &&
            !state.capability_lost().has_value(),
          "capability loss did not emit exactly one cancellation");
  require(throws<std::invalid_argument>(
            [&] { state.enter(std::numeric_limits<double>::infinity(), 0.0); }),
          "non-finite pointer coordinates were accepted");
}

void keyboard_state_preserves_logical_identity_and_focus() {
  using dui::KeyPhase;
  using dui::platform::WaylandKeyboardState;
  using dui::platform::WaylandKeyModifiers;

  WaylandKeyboardState state;
  require(!state.key_down(30, "a", {}).has_value() && !state.key_up(30, {}).has_value(),
          "unfocused Wayland keyboard invented events");
  state.focus_gained();
  require(throws<std::invalid_argument>([&] { static_cast<void>(state.key_down(30, "", {})); }),
          "empty logical key was accepted");
  const auto down = state.key_down(30, "a", {.shift = true});
  const auto repeat = state.key_repeat(30, {.control = true});
  require(down.has_value() && down->logical_key == "a" && down->phase == KeyPhase::down &&
            down->shift && repeat.has_value() && repeat->logical_key == "a" &&
            repeat->phase == KeyPhase::repeat && repeat->control,
          "keyboard down/repeat lost logical identity or modifiers");
  require(throws<std::logic_error>([&] { static_cast<void>(state.key_down(30, "A", {})); }),
          "duplicate key down was accepted");
  const auto up = state.key_up(30, {.alt = true, .meta = true});
  require(up.has_value() && up->logical_key == "a" && up->phase == KeyPhase::up && up->alt &&
            up->meta && state.pressed_count() == 0 && !state.key_up(30, {}).has_value(),
          "keyboard up renamed the held key or unmatched release produced an event");

  static_cast<void>(state.key_down(31, "s", {}));
  state.clear_pressed();
  require(state.focused() && state.pressed_count() == 0,
          "keymap replacement did not preserve focus while clearing held keys");
  static_cast<void>(state.key_down(32, "d", {}));
  state.focus_lost();
  require(!state.focused() && state.pressed_count() == 0 &&
            !state.key_repeat(32, WaylandKeyModifiers{}).has_value(),
          "keyboard focus loss retained held-key state");
}

void keyboard_repeat_policy_is_bounded_and_generation_checked() {
  using namespace std::chrono_literals;
  using dui::platform::WaylandRepeatSchedule;
  using dui::platform::WaylandRepeatState;

  WaylandRepeatState state;
  require(throws<std::invalid_argument>([&] { state.configure(-1, 10, 0ns); }) &&
            throws<std::invalid_argument>([&] { state.configure(10, -1, 0ns); }) &&
            throws<std::invalid_argument>([&] { state.configure(1'000'000'001, 10, 0ns); }),
          "invalid keyboard repeat timing was accepted");
  state.configure(1'000'000'000, 0, 0ns);
  state.key_down(29, true, 0ns);
  const auto precision_boundary = state.schedule(0ns);
  require(precision_boundary.has_value() && precision_boundary->delay == 1ns &&
            precision_boundary->interval == 1ns &&
            state.delivery_count(0, precision_boundary->generation) == 0,
          "one-nanosecond repeat boundary was not represented exactly");
  state.cancel();
  state.configure(25, 400, 0ns);
  state.key_down(30, true, 1s);
  const auto initial = state.schedule(1s);
  require(initial == WaylandRepeatSchedule{30, 400ms, 40ms, state.generation()},
          "repeat key did not retain compositor timing");

  state.key_down(31, false, 1100ms);
  require(state.candidate() == 30 && state.schedule(1200ms)->delay == 200ms,
          "non-repeatable key replaced the repeat candidate");
  state.key_up(31);
  require(state.candidate() == 30, "non-candidate key release canceled repeat");

  state.configure(50, 100, 1250ms);
  const auto replacement = state.schedule(1250ms);
  require(replacement.has_value() && replacement->key == 30 && replacement->delay == 1ns &&
            replacement->interval == 20ms,
          "repeat-info replacement restarted delay from update time");
  require(state.delivery_count(100, replacement->generation) ==
            WaylandRepeatState::maximum_events_per_dispatch,
          "coalesced repeat delivery was not bounded");

  const std::uint64_t stale_generation = replacement->generation;
  state.key_down(32, true, 1300ms);
  require(state.delivery_count(1, stale_generation) == 0 && state.candidate() == 32,
          "replaced repeat generation remained deliverable");
  state.configure(0, 100, 1300ms);
  require(!state.schedule(1300ms).has_value() && state.candidate() == 32,
          "zero rate did not disable repeat while retaining its candidate");
  state.configure(10, 100, 1350ms);
  require(state.schedule(1350ms)->delay == 50ms,
          "positive repeat-info did not rearm from original key-down time");
  state.key_up(32);
  require(!state.candidate().has_value() && !state.schedule(1400ms).has_value(),
          "active key release did not cancel repeat");

  state.key_down(33, true, 1500ms);
  const std::uint64_t canceled_generation = state.generation();
  state.cancel();
  require(!state.candidate().has_value() && state.delivery_count(1, canceled_generation) == 0,
          "focus or keymap cancellation retained repeat state");
  require(throws<std::invalid_argument>([&] { state.key_down(34, true, -1ns); }),
          "negative monotonic key time was accepted");
}

} // namespace

int main() {
  try {
    initial_commit_and_configure_are_ordered();
    stale_plans_and_buffer_failures_remain_retryable();
    integer_and_fractional_scaling_are_explicit();
    frame_callbacks_gate_content_without_blocking_configure();
    scale_changes_replace_frames_without_accepting_stale_generations();
    primary_pointer_state_preserves_stream_invariants();
    keyboard_state_preserves_logical_identity_and_focus();
    keyboard_repeat_policy_is_bounded_and_generation_checked();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Wayland state tests passed\n";
  return 0;
}
