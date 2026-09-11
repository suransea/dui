#pragma once

#include <atomic>
#include <memory>
#include <utility>

namespace dui {

class CancellationToken {
public:
  [[nodiscard]] bool stop_requested() const {
    const auto state = state_.lock();
    return state == nullptr || state->load(std::memory_order_acquire);
  }

private:
  friend class CancellationSource;
  explicit CancellationToken(std::weak_ptr<std::atomic_bool> state) : state_(std::move(state)) {}

  std::weak_ptr<std::atomic_bool> state_;
};

class CancellationSource {
public:
  CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}
  ~CancellationSource() { request_stop(); }

  CancellationSource(const CancellationSource&) = delete;
  CancellationSource& operator=(const CancellationSource&) = delete;
  CancellationSource(CancellationSource&&) noexcept = default;
  CancellationSource& operator=(CancellationSource&&) noexcept = default;

  [[nodiscard]] CancellationToken token() const { return CancellationToken{state_}; }

  void request_stop() noexcept {
    if (state_ != nullptr) {
      state_->store(true, std::memory_order_release);
    }
  }

private:
  std::shared_ptr<std::atomic_bool> state_;
};

} // namespace dui
