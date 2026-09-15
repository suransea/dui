#include "dui/platform/wayland_native.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

class Delegate final : public dui::HostWindowDelegate {
public:
  void window_created(dui::WindowId, dui::WindowMetrics) override { created = true; }
  void frame_requested(dui::FrameRequest request) override { frames.push_back(request); }
  void close_requested(dui::WindowId) override { close_received = true; }
  void window_shutting_down(dui::WindowId) override {
    shutdown = true;
    if (on_shutdown) {
      on_shutdown();
    }
  }

  std::vector<dui::FrameRequest> frames;
  std::function<void()> on_shutdown;
  bool created{};
  bool close_received{};
  bool shutdown{};
};

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

} // namespace

int main() {
  try {
    auto connection = dui::platform::WaylandConnection::connect();
    const auto globals = connection->globals();
    require(globals.output_count != 0, "Wayland compositor advertised no output");
    const bool fractional_path =
      globals.viewporter_version != 0 && globals.fractional_scale_version != 0;
    auto native_window = dui::platform::WaylandWindow::create(
      *connection, {1}, {"DUI Wayland H2", {320.0, 200.0}, 320, 200, 1.0, true});
    auto host_window = native_window->window();
    auto delegate = std::make_shared<Delegate>();
    auto binding = host_window.bind_delegate(delegate);
    require(host_window.request_frame(), "host rejected the initial Wayland frame request");
    require(host_window.request_frame(), "host rejected coalesced Wayland frame demand");

    for (int iteration = 0; iteration < 100; ++iteration) {
      connection->roundtrip();
      const auto current = native_window->status();
      if (!delegate->frames.empty() && current.scale_numerator == 240) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    const auto status = native_window->status();
    const auto final_globals = connection->globals();
    require(delegate->created, "Wayland host did not create its framework delegate");
    require(status.configured && status.committed_buffers != 0,
            "xdg configure did not produce a diagnostic shared-memory buffer");
    require(status.uses_viewporter == fractional_path && status.scale_numerator == 240 &&
              status.physical_width == 640 && status.physical_height == 400,
            "native scaling path did not apply the scale-two Weston output");
    const auto metrics = host_window.metrics();
    require(metrics.has_value() && metrics->physical_width == 640 &&
              metrics->physical_height == 400 && metrics->device_pixel_ratio == 2.0,
            "scaled Wayland commit did not publish matching host metrics");
    require(status.pointer_available == final_globals.pointer_available(),
            "Wayland pointer object did not follow the advertised seat capability");
    require(status.keyboard_available == final_globals.keyboard_available(),
            "Wayland keyboard object did not follow the advertised seat capability");
    if (delegate->frames.size() != 1 || delegate->frames.front().window != dui::WindowId{1} ||
        delegate->frames.front().timestamp < std::chrono::nanoseconds::zero() ||
        delegate->frames.front().metrics.physical_width != 640 ||
        delegate->frames.front().metrics.physical_height != 400 ||
        delegate->frames.front().metrics.device_pixel_ratio != 2.0) {
      throw std::runtime_error("Wayland frame callback count was " +
                               std::to_string(delegate->frames.size()) +
                               ", pending=" + (status.frame_callback_pending ? "true" : "false"));
    }

    std::atomic<bool> cross_thread_task_ran{};
    std::atomic<bool> task_ran_on_owner{};
    const std::thread::id owner_thread = std::this_thread::get_id();
    auto runner = connection->task_runner();
    std::jthread worker{[runner, &cross_thread_task_ran, &task_ran_on_owner, owner_thread] {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
      runner->post([&cross_thread_task_ran, &task_ran_on_owner, owner_thread] {
        task_ran_on_owner = std::this_thread::get_id() == owner_thread;
        cross_thread_task_ran = true;
      });
    }};
    while (!cross_thread_task_ran) {
      connection->dispatch();
    }
    require(task_ran_on_owner, "cross-thread Wayland task did not run on the owner thread");

    delegate->on_shutdown = [&native_window] { native_window.reset(); };
    host_window.shutdown();
    connection->run_pending();
    require(!host_window.valid() && delegate->shutdown && native_window == nullptr,
            "Wayland host shutdown did not survive reentrant native-window destruction");
    connection->roundtrip();
    connection.reset();
    bool retired_runner_rejected_post{};
    try {
      runner->post([] {});
    } catch (const std::logic_error&) {
      retired_runner_rejected_post = true;
    }
    require(retired_runner_rejected_post,
            "retained Wayland task runner accepted work after connection shutdown");
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Native Wayland window test passed\n";
  return 0;
}
