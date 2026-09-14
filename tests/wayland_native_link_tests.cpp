#include "dui/platform/wayland_native.hpp"

#include <iostream>

int main(int argc, char** argv) {
  if (argc > 1) {
    const auto connection = dui::platform::WaylandConnection::connect(argv[1]);
    return connection->globals().window_ready() ? 0 : 1;
  }
  const dui::platform::WaylandGlobals globals{4, 1, 5, 6, 1, 1};
  if (!globals.window_ready()) {
    std::cerr << "FAILED: native Wayland symbols or required globals are unavailable\n";
    return 1;
  }
  std::cout << "Native Wayland target linked\n";
  return 0;
}
