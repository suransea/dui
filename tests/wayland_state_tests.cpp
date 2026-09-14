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

} // namespace

int main() {
  try {
    initial_commit_and_configure_are_ordered();
    stale_plans_and_buffer_failures_remain_retryable();
    integer_and_fractional_scaling_are_explicit();
    frame_callbacks_gate_content_without_blocking_configure();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "All Wayland state tests passed\n";
  return 0;
}
