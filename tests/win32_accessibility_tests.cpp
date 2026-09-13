#include "dui/platform/win32_accessibility.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class TestWindow {
public:
  TestWindow()
    : handle_(CreateWindowExW(0, L"STATIC", L"DUI accessibility test", 0, 0, 0, 100, 100,
                              HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr)) {
    if (handle_ == nullptr) {
      throw std::runtime_error("failed to create Win32 test window");
    }
  }

  ~TestWindow() { static_cast<void>(DestroyWindow(handle_)); }
  [[nodiscard]] HWND get() const { return handle_; }

private:
  HWND handle_;
};

template <typename Function>
void require_invalid_argument(Function&& function, const std::string& message) {
  try {
    function();
  } catch (const std::invalid_argument&) {
    return;
  }
  throw std::runtime_error(message);
}

void validates_incremental_model_transactionally() {
  TestWindow window;
  dui::win32::AccessibilityAdapter adapter{window.get(), {}};
  dui::SemanticsEntry root;
  root.id = 1;
  root.label = "root";
  root.bounds = {{0, 0}, {50, 50}};
  dui::SemanticsEntry child;
  child.id = 2;
  child.parent_id = 1;
  child.role = dui::SemanticsRole::button;
  child.label = "activate";
  child.bounds = {{5, 5}, {20, 20}};
  child.actions = {dui::SemanticsAction::activate, dui::SemanticsAction::focus};
  child.focusable = true;
  child.focused = true;
  const std::array initial{dui::SemanticsChange{dui::SemanticsChangeKind::added, root},
                           dui::SemanticsChange{dui::SemanticsChangeKind::added, child}};
  adapter.apply(initial);
  adapter.set_device_pixel_ratio(2.0);
  require_invalid_argument([&] { adapter.set_device_pixel_ratio(0.0); },
                           "invalid DPR did not preserve the current coordinate scale");

  dui::SemanticsEntry invalid = child;
  invalid.parent_id = 99;
  const std::array malformed{dui::SemanticsChange{dui::SemanticsChangeKind::updated, invalid}};
  require_invalid_argument([&] { adapter.apply(malformed); },
                           "missing parent did not reject the UIA update");

  dui::SemanticsEntry duplicate_focus = root;
  duplicate_focus.focusable = true;
  duplicate_focus.focused = true;
  duplicate_focus.actions.push_back(dui::SemanticsAction::focus);
  const std::array malformed_focus{
    dui::SemanticsChange{dui::SemanticsChangeKind::updated, duplicate_focus}};
  require_invalid_argument([&] { adapter.apply(malformed_focus); },
                           "multiple focused nodes did not reject the UIA update");

  const std::array removed{dui::SemanticsChange{dui::SemanticsChangeKind::removed, child},
                           dui::SemanticsChange{dui::SemanticsChangeKind::removed, root}};
  adapter.apply(removed);
  require_invalid_argument([&] { adapter.apply(removed); },
                           "repeated removals did not preserve the acknowledged model");
}

void filters_window_messages() {
  TestWindow window;
  dui::win32::AccessibilityAdapter adapter{window.get(), {}};
  require(!adapter.handle_message(WM_PAINT, 0, 0).has_value(),
          "non-accessibility message was consumed");
  require(!adapter.handle_message(WM_GETOBJECT, 0, 0).has_value(),
          "unrelated object request was consumed");
}

} // namespace

int main() {
  try {
    validates_incremental_model_transactionally();
    filters_window_messages();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All DUI Win32 accessibility tests passed\n";
  return EXIT_SUCCESS;
}
