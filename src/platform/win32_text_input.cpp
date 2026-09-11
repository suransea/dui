#include "dui/platform/win32_text_input.hpp"

#ifndef _WIN32
#error "win32_text_input.cpp is only supported on Windows"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imm.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dui::win32 {

class TextInputBackend::Impl {
public:
  Impl(NativeWindowHandle native_window, double scale) : state_(std::make_shared<State>()) {
    if (native_window == nullptr) {
      throw std::invalid_argument("Win32 text input requires a window handle");
    }
    if (!std::isfinite(scale) || scale <= 0.0) {
      throw std::invalid_argument("Win32 text input requires a positive finite DPR");
    }
    state_->window = static_cast<HWND>(native_window);
    state_->device_pixel_ratio = scale;
  }

  ~Impl() {
    const std::shared_ptr<State> state = state_;
    const bool was_composing =
      state->active.has_value() && state->active->value.composing.has_value();
    state->active.reset();
    ++state->generation;
    state_->alive = false;
    if (was_composing) {
      static_cast<void>(cancel_native_composition(*state));
    }
  }

  [[nodiscard]] TextInputSessionId start(std::weak_ptr<TextInputClient> client,
                                         TextInputConfiguration configuration,
                                         TextEditingValue initial_value) {
    validate(initial_value);
    const std::shared_ptr<State> state = state_;
    if (client.expired()) {
      throw std::invalid_argument("Win32 text input client has expired");
    }
    if (state->active.has_value()) {
      const bool was_composing = state->active->value.composing.has_value();
      state->active.reset();
      const std::uint64_t generation = ++state->generation;
      const bool cancelled = !was_composing || cancel_native_composition(*state);
      if (!state->alive || state->generation != generation) {
        throw std::logic_error("Win32 text input session changed during cancellation");
      }
      if (!cancelled) {
        throw std::runtime_error("Win32 IME refused to cancel the active composition");
      }
    }
    TextInputSessionId id = state->next_session++;
    if (id == 0) {
      id = state->next_session++;
    }
    state->active =
      Session{id,           std::move(client), configuration, std::move(initial_value),
              std::nullopt, std::nullopt,      std::nullopt};
    ++state->generation;
    return id;
  }

  void update(TextInputSessionId session, TextEditingValue value) {
    const std::shared_ptr<State> state = state_;
    if (Session* active = find(*state, session); active != nullptr) {
      validate(value);
      if (active->value.composing.has_value()) {
        active->value.composing.reset();
        active->before_composition.reset();
        const std::uint64_t generation = ++state->generation;
        const bool cancelled = cancel_native_composition(*state);
        if (!state->alive || state->generation != generation) {
          return;
        }
        if (!cancelled) {
          state->active.reset();
          ++state->generation;
          throw std::runtime_error("Win32 IME refused to cancel the active composition");
        }
        active = find(*state, session);
        if (active == nullptr) {
          return;
        }
      }
      active->value = std::move(value);
      active->pending_high_surrogate.reset();
      active->before_composition.reset();
      ++state->generation;
    }
  }

  void stop(TextInputSessionId session) {
    const std::shared_ptr<State> state = state_;
    if (find(*state, session) == nullptr) {
      return;
    }
    const bool was_composing = state->active->value.composing.has_value();
    state->active.reset();
    ++state->generation;
    if (was_composing && !cancel_native_composition(*state)) {
      throw std::runtime_error("Win32 IME refused to cancel the active composition");
    }
  }

  void set_rect(TextInputSessionId session, Rect rect) {
    if (Session* active = find(*state_, session); active != nullptr) {
      if (!valid_rect(rect)) {
        throw std::invalid_argument("Win32 editable rectangle must be finite and non-negative");
      }
      active->editable_rect = rect;
      apply_rect(*state_, rect);
    }
  }

  [[nodiscard]] std::optional<std::intptr_t> handle(std::uint32_t message, std::uintptr_t wparam,
                                                    std::intptr_t lparam) noexcept {
    const std::shared_ptr<State> state = state_;
    if (state->active == std::nullopt) {
      return std::nullopt;
    }
    if (state->active->client.expired()) {
      state->active.reset();
      ++state->generation;
      static_cast<void>(cancel_native_composition(*state));
      return std::nullopt;
    }
    try {
      switch (message) {
      case WM_CHAR:
        process_character(state, static_cast<wchar_t>(wparam));
        return 0;
      case WM_IME_CHAR:
        // Unicode IMEs commit through GCS_RESULTSTR. Consuming this
        // compatibility message avoids committing the result twice.
        return 0;
      case WM_IME_STARTCOMPOSITION:
        state->active->pending_high_surrogate.reset();
        state->active->before_composition = state->active->value;
        state->active->value.composing = state->active->value.selection;
        if (state->active->editable_rect.has_value()) {
          apply_rect(*state, *state->active->editable_rect);
        }
        return 0;
      case WM_IME_COMPOSITION:
        process_composition(state, static_cast<LPARAM>(lparam));
        return 0;
      case WM_IME_ENDCOMPOSITION:
        state->active->pending_high_surrogate.reset();
        if (state->active->before_composition.has_value()) {
          TextEditingValue value = std::move(*state->active->before_composition);
          state->active->before_composition.reset();
          static_cast<void>(emit_value(state, std::move(value)));
        }
        return 0;
      default:
        return std::nullopt;
      }
    } catch (...) {
      // No C++ exception may cross a native window procedure boundary.
      return 0;
    }
  }

private:
  struct Session {
    TextInputSessionId id{};
    std::weak_ptr<TextInputClient> client;
    TextInputConfiguration configuration;
    TextEditingValue value;
    std::optional<Rect> editable_rect;
    std::optional<wchar_t> pending_high_surrogate;
    std::optional<TextEditingValue> before_composition;
  };

  struct State {
    HWND window{};
    double device_pixel_ratio{1.0};
    TextInputSessionId next_session{1};
    std::optional<Session> active;
    bool alive{true};
    std::uint64_t generation{};
  };

  class InputContext {
  public:
    explicit InputContext(HWND window) : window_(window), context_(ImmGetContext(window)) {}
    ~InputContext() {
      if (context_ != nullptr) {
        static_cast<void>(ImmReleaseContext(window_, context_));
      }
    }
    [[nodiscard]] HIMC get() const { return context_; }

  private:
    HWND window_;
    HIMC context_;
  };

  [[nodiscard]] static Session* find(State& state, TextInputSessionId session) {
    return state.active.has_value() && state.active->id == session ? &*state.active : nullptr;
  }

  static void validate(const TextEditingValue& value) {
    if (!value.valid()) {
      throw std::invalid_argument("Invalid UTF-8 text editing value");
    }
  }

  [[nodiscard]] static bool valid_rect(Rect rect) {
    return std::isfinite(rect.origin.x) && std::isfinite(rect.origin.y) &&
           std::isfinite(rect.size.width) && std::isfinite(rect.size.height) &&
           rect.size.width >= 0.0 && rect.size.height >= 0.0;
  }

  [[nodiscard]] static LONG native_coordinate(double logical, double scale) {
    const double physical = std::round(logical * scale);
    if (physical < static_cast<double>(std::numeric_limits<LONG>::min()) ||
        physical > static_cast<double>(std::numeric_limits<LONG>::max())) {
      throw std::invalid_argument("Win32 editable rectangle is out of range");
    }
    return static_cast<LONG>(physical);
  }

  static void apply_rect(const State& state, Rect rect) {
    InputContext input{state.window};
    if (input.get() == nullptr) {
      return;
    }
    const LONG x = native_coordinate(rect.origin.x, state.device_pixel_ratio);
    const LONG y = native_coordinate(rect.origin.y + rect.size.height, state.device_pixel_ratio);
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = {x, y};
    static_cast<void>(ImmSetCandidateWindow(input.get(), &candidate));

    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = {x, y};
    static_cast<void>(ImmSetCompositionWindow(input.get(), &composition));
  }

  [[nodiscard]] static bool cancel_native_composition(const State& state) {
    InputContext input{state.window};
    if (input.get() == nullptr) {
      return true;
    }
    return ImmNotifyIME(input.get(), NI_COMPOSITIONSTR, CPS_CANCEL, 0) != FALSE;
  }

  [[nodiscard]] static std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
      return {};
    }
    const int source_size = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), source_size,
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
      throw std::runtime_error("Invalid UTF-16 text from Win32");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), source_size, result.data(),
                            size, nullptr, nullptr) != size) {
      throw std::runtime_error("Failed to convert Win32 text to UTF-8");
    }
    return result;
  }

  [[nodiscard]] static std::wstring read_composition(HIMC context, DWORD index) {
    const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    if (bytes < 0 || bytes % static_cast<LONG>(sizeof(wchar_t)) != 0) {
      throw std::runtime_error("Failed to read Win32 IME composition");
    }
    std::wstring result(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
    if (bytes != 0 && ImmGetCompositionStringW(context, index, result.data(),
                                               static_cast<DWORD>(bytes)) != bytes) {
      throw std::runtime_error("Win32 IME composition changed while reading");
    }
    return result;
  }

  [[nodiscard]] static TextEditingValue replace_range(TextEditingValue value, TextRange range,
                                                      std::string replacement,
                                                      std::size_t cursor_in_replacement,
                                                      bool composing) {
    value.text.replace(range.start, range.end - range.start, replacement);
    const std::size_t end = range.start + replacement.size();
    value.selection = {range.start + cursor_in_replacement, range.start + cursor_in_replacement};
    value.composing = composing ? std::optional<TextRange>{{range.start, end}} : std::nullopt;
    return value;
  }

  [[nodiscard]] static bool emit_value(const std::shared_ptr<State>& state,
                                       TextEditingValue value) {
    validate(value);
    if (!state->alive || !state->active.has_value()) {
      return false;
    }
    const TextInputSessionId session = state->active->id;
    state->active->value = value;
    const std::shared_ptr<TextInputClient> client = state->active->client.lock();
    if (client == nullptr) {
      state->active.reset();
      ++state->generation;
      static_cast<void>(cancel_native_composition(*state));
      return false;
    }
    const std::uint64_t generation = ++state->generation;
    client->update_editing_value(std::move(value));
    return state->alive && state->active.has_value() && state->active->id == session &&
           state->generation == generation;
  }

  static void perform_action(const std::shared_ptr<State>& state) {
    if (!state->alive || !state->active.has_value()) {
      return;
    }
    const TextInputAction action = state->active->configuration.action;
    const std::shared_ptr<TextInputClient> client = state->active->client.lock();
    if (client == nullptr) {
      state->active.reset();
      ++state->generation;
      static_cast<void>(cancel_native_composition(*state));
      return;
    }
    ++state->generation;
    client->perform_action(action);
  }

  static void process_character(const std::shared_ptr<State>& state, wchar_t character) {
    if (!state->active.has_value()) {
      return;
    }
    Session& session = *state->active;
    if (character == L'\r') {
      if (!session.configuration.multiline ||
          session.configuration.action != TextInputAction::none) {
        perform_action(state);
        return;
      }
      character = L'\n';
    }
    if (character == L'\b') {
      TextEditingValue value = session.value;
      TextRange range = value.selection;
      if (range.collapsed() && range.start != 0) {
        std::size_t previous = range.start - 1;
        while (previous != 0 &&
               (static_cast<unsigned char>(value.text[previous]) & 0xc0u) == 0x80u) {
          --previous;
        }
        range.start = previous;
      }
      static_cast<void>(emit_value(state, replace_range(std::move(value), range, {}, 0, false)));
      return;
    }

    if (character >= 0xd800 && character <= 0xdbff) {
      session.pending_high_surrogate = character;
      return;
    }
    std::wstring characters;
    if (character >= 0xdc00 && character <= 0xdfff) {
      if (!session.pending_high_surrogate.has_value()) {
        return;
      }
      characters.push_back(*session.pending_high_surrogate);
      characters.push_back(character);
      session.pending_high_surrogate.reset();
    } else {
      session.pending_high_surrogate.reset();
      characters.push_back(character);
    }
    const std::string inserted = wide_to_utf8(characters);
    static_cast<void>(emit_value(state, replace_range(session.value, session.value.selection,
                                                      inserted, inserted.size(), false)));
  }

  static void process_composition(const std::shared_ptr<State>& state, LPARAM flags) {
    if (!state->active.has_value()) {
      return;
    }
    const TextInputSessionId session = state->active->id;
    const std::uint64_t generation = state->generation;
    std::optional<std::string> result;
    std::optional<std::pair<std::string, std::size_t>> composition;
    {
      InputContext input{state->window};
      if (input.get() == nullptr) {
        return;
      }
      if ((flags & GCS_RESULTSTR) != 0) {
        result = wide_to_utf8(read_composition(input.get(), GCS_RESULTSTR));
      }
      if ((flags & GCS_COMPSTR) != 0) {
        const std::wstring wide = read_composition(input.get(), GCS_COMPSTR);
        LONG cursor = ImmGetCompositionStringW(input.get(), GCS_CURSORPOS, nullptr, 0);
        if (cursor < 0) {
          cursor = 0;
        }
        cursor = std::min<LONG>(cursor, static_cast<LONG>(wide.size()));
        if (cursor != 0 && static_cast<std::size_t>(cursor) < wide.size() &&
            wide[static_cast<std::size_t>(cursor) - 1] >= 0xd800 &&
            wide[static_cast<std::size_t>(cursor) - 1] <= 0xdbff &&
            wide[static_cast<std::size_t>(cursor)] >= 0xdc00 &&
            wide[static_cast<std::size_t>(cursor)] <= 0xdfff) {
          --cursor;
        }
        composition = std::pair{
          wide_to_utf8(wide),
          wide_to_utf8(std::wstring_view{wide}.substr(0, static_cast<std::size_t>(cursor))).size()};
      }
    }
    if (!state->alive || !state->active.has_value() || state->active->id != session ||
        state->generation != generation) {
      return;
    }
    if (result.has_value()) {
      const TextRange range =
        state->active->value.composing.value_or(state->active->value.selection);
      state->active->before_composition.reset();
      if (!emit_value(state,
                      replace_range(state->active->value, range, *result, result->size(), false))) {
        return;
      }
    }
    if (composition.has_value() && state->active.has_value() && state->active->id == session) {
      if (!state->active->before_composition.has_value()) {
        state->active->before_composition = state->active->value;
      }
      const TextRange range =
        state->active->value.composing.value_or(state->active->value.selection);
      static_cast<void>(
        emit_value(state, replace_range(state->active->value, range, composition->first,
                                        composition->second, true)));
    }
  }

  std::shared_ptr<State> state_;
};

TextInputBackend::TextInputBackend(NativeWindowHandle window, double device_pixel_ratio)
  : impl_(std::make_unique<Impl>(window, device_pixel_ratio)) {}

TextInputBackend::~TextInputBackend() = default;
TextInputBackend::TextInputBackend(TextInputBackend&&) noexcept = default;
TextInputBackend& TextInputBackend::operator=(TextInputBackend&&) noexcept = default;

TextInputSessionId TextInputBackend::start_text_input(std::weak_ptr<TextInputClient> client,
                                                      TextInputConfiguration configuration,
                                                      TextEditingValue initial_value) {
  return impl_->start(std::move(client), configuration, std::move(initial_value));
}

void TextInputBackend::update_editing_state(TextInputSessionId session, TextEditingValue value) {
  impl_->update(session, std::move(value));
}

void TextInputBackend::stop_text_input(TextInputSessionId session) { impl_->stop(session); }

void TextInputBackend::set_editable_rect(TextInputSessionId session, Rect rect) {
  impl_->set_rect(session, rect);
}

std::optional<std::intptr_t> TextInputBackend::handle_message(std::uint32_t message,
                                                              std::uintptr_t wparam,
                                                              std::intptr_t lparam) noexcept {
  return impl_->handle(message, wparam, lparam);
}

} // namespace dui::win32
