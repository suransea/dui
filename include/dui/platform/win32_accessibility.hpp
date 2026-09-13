#pragma once

#include "dui/rendering.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace dui::win32 {

class AccessibilityAdapter final : public dui::AccessibilityAdapter {
public:
  using NativeWindowHandle = void*;
  using ActionHandler = std::function<bool(std::uint64_t, SemanticsAction)>;

  // Semantic bounds use logical client coordinates and are scaled by DPR.
  AccessibilityAdapter(NativeWindowHandle window, ActionHandler action_handler,
                       double device_pixel_ratio = 1.0);
  ~AccessibilityAdapter() override;

  AccessibilityAdapter(const AccessibilityAdapter&) = delete;
  AccessibilityAdapter& operator=(const AccessibilityAdapter&) = delete;
  AccessibilityAdapter(AccessibilityAdapter&&) noexcept;
  AccessibilityAdapter& operator=(AccessibilityAdapter&&) noexcept;

  void apply(std::span<const SemanticsChange> changes) override;
  void set_device_pixel_ratio(double device_pixel_ratio);

  // Call from the host window procedure before DefWindowProcW. A value means
  // the message was consumed and should be returned by the window procedure.
  [[nodiscard]] std::optional<std::intptr_t>
  handle_message(std::uint32_t message, std::uintptr_t wparam, std::intptr_t lparam) noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace dui::win32
