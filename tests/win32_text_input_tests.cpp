#include "dui/platform/win32_text_input.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class InputClient final : public dui::TextInputClient {
public:
  void update_editing_value(dui::TextEditingValue value) override {
    last_value = std::move(value);
    ++updates;
  }

  void perform_action(dui::TextInputAction action) override { last_action = action; }

  std::optional<dui::TextEditingValue> last_value;
  std::optional<dui::TextInputAction> last_action;
  int updates{};
};

class TestWindow {
public:
  TestWindow()
    : handle_(CreateWindowExW(0, L"STATIC", L"DUI text input test", 0, 0, 0, 100, 100, HWND_MESSAGE,
                              nullptr, GetModuleHandleW(nullptr), nullptr)) {
    if (handle_ == nullptr) {
      throw std::runtime_error("failed to create Win32 test window");
    }
  }

  ~TestWindow() { static_cast<void>(DestroyWindow(handle_)); }
  [[nodiscard]] HWND get() const { return handle_; }

private:
  HWND handle_;
};

void character_editing_and_actions() {
  TestWindow window;
  dui::win32::TextInputBackend backend{window.get()};
  auto client = std::make_shared<InputClient>();
  const auto session = backend.start_text_input(client, {}, {"A", {1, 1}, std::nullopt});

  require(backend.handle_message(WM_CHAR, L'B', 0).has_value(), "WM_CHAR was not consumed");
  require(client->last_value.has_value() && client->last_value->text == "AB",
          "character was not inserted");
  static_cast<void>(backend.handle_message(WM_CHAR, L'\b', 0));
  require(client->last_value->text == "A", "backspace did not remove the previous scalar");
  static_cast<void>(backend.handle_message(WM_CHAR, L'\r', 0));
  require(client->last_action == dui::TextInputAction::done,
          "Enter did not perform the configured action");

  backend.stop_text_input(session);
  require(!backend.handle_message(WM_CHAR, L'C', 0).has_value(), "stopped session consumed text");
}

void surrogate_pair_and_stale_sessions() {
  TestWindow window;
  dui::win32::TextInputBackend backend{window.get()};
  auto client = std::make_shared<InputClient>();
  const auto stale = backend.start_text_input(client, {}, {"", {}, std::nullopt});
  const auto active = backend.start_text_input(client, {}, {"", {}, std::nullopt});
  backend.update_editing_state(stale, {"stale", {5, 5}, std::nullopt});
  backend.update_editing_state(stale, {"\x80", {}, std::nullopt});

  static_cast<void>(backend.handle_message(WM_CHAR, 0xd83d, 0));
  require(client->updates == 0, "high surrogate emitted an incomplete scalar");
  static_cast<void>(backend.handle_message(WM_CHAR, 0xde00, 0));
  require(client->last_value.has_value() && client->last_value->text == "\xf0\x9f\x98\x80",
          "UTF-16 surrogate pair was not converted to UTF-8");
  backend.stop_text_input(stale);
  static_cast<void>(backend.handle_message(WM_CHAR, L'X', 0));
  require(client->last_value->text.ends_with("X"), "stale stop ended the active session");
  backend.stop_text_input(active);
}

} // namespace

int main() {
  try {
    character_editing_and_actions();
    surrogate_pair_and_stale_sessions();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All DUI Win32 text input tests passed\n";
  return EXIT_SUCCESS;
}
