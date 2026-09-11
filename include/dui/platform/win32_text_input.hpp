#pragma once

#include "dui/input.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace dui::win32 {

class TextInputBackend final : public dui::TextInputBackend {
public:
    using NativeWindowHandle = void*;

    // Editable rectangles use logical client coordinates and are scaled by DPR.
    explicit TextInputBackend(NativeWindowHandle window, double device_pixel_ratio = 1.0);
    ~TextInputBackend() override;

    TextInputBackend(const TextInputBackend&) = delete;
    TextInputBackend& operator=(const TextInputBackend&) = delete;
    TextInputBackend(TextInputBackend&&) noexcept;
    TextInputBackend& operator=(TextInputBackend&&) noexcept;

    [[nodiscard]] TextInputSessionId start_text_input(
        std::weak_ptr<TextInputClient> client,
        TextInputConfiguration configuration,
        TextEditingValue initial_value
    ) override;
    void update_editing_state(TextInputSessionId session, TextEditingValue value) override;
    void stop_text_input(TextInputSessionId session) override;
    void set_editable_rect(TextInputSessionId session, Rect rect) override;

    // Call from the host window procedure before DefWindowProcW. A value means
    // the message was consumed and should be returned by the window procedure.
    [[nodiscard]] std::optional<std::intptr_t> handle_message(
        std::uint32_t message,
        std::uintptr_t wparam,
        std::intptr_t lparam
    ) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dui::win32
