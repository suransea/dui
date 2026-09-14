#include "dui/platform/wayland_native.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
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
    auto native_window = dui::platform::WaylandWindow::create(
      *connection, {1}, {"DUI Wayland H2", {320.0, 200.0}, 320, 200, 1.0, true});
    auto host_window = native_window->window();
    auto delegate = std::make_shared<Delegate>();
    auto binding = host_window.bind_delegate(delegate);
    require(host_window.request_frame(), "host rejected the initial Wayland frame request");
    require(host_window.request_frame(), "host rejected coalesced Wayland frame demand");

    for (int iteration = 0; iteration < 20 && delegate->frames.empty(); ++iteration) {
      connection->roundtrip();
    }
    const auto status = native_window->status();
    require(delegate->created, "Wayland host did not create its framework delegate");
    require(status.configured && status.committed_buffers != 0,
            "xdg configure did not produce a diagnostic shared-memory buffer");
    require(delegate->frames.size() == 1 && delegate->frames.front().window == dui::WindowId{1} &&
              delegate->frames.front().timestamp >= std::chrono::nanoseconds::zero(),
            "Wayland frame callback did not reach HostWindowDriver exactly once");

    delegate->on_shutdown = [&native_window] { native_window.reset(); };
    host_window.shutdown();
    connection->run_pending();
    require(!host_window.valid() && delegate->shutdown && native_window == nullptr,
            "Wayland host shutdown did not survive reentrant native-window destruction");
    connection->roundtrip();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Native Wayland window test passed\n";
  return 0;
}
