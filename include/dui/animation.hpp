#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace dui {

using FrameDuration = std::chrono::duration<double>;

class Ticker {
public:
  using Callback = std::function<void(FrameDuration)>;

  explicit Ticker(Callback callback) : callback_(std::move(callback)) {}

  void start() { active_ = true; }
  void stop() { active_ = false; }
  void clear_callback() { callback_ = {}; }
  [[nodiscard]] bool active() const { return active_; }

private:
  friend class TickerScheduler;

  void tick(FrameDuration elapsed) {
    if (active_ && callback_) {
      Callback callback = callback_;
      callback(elapsed);
    }
  }

  Callback callback_;
  bool active_{};
};

class TickerScheduler {
public:
  [[nodiscard]] std::shared_ptr<Ticker> create_ticker(Ticker::Callback callback);
  void elapse(FrameDuration elapsed);
  [[nodiscard]] std::size_t live_ticker_count() const;

private:
  std::vector<std::weak_ptr<Ticker>> tickers_;
};

enum class AnimationStatus { dismissed, forward, reverse, completed };

class AnimationController {
public:
  using ValueCallback = std::function<void(double)>;

  AnimationController(TickerScheduler& scheduler, FrameDuration duration,
                      ValueCallback callback = {});
  ~AnimationController();

  AnimationController(const AnimationController&) = delete;
  AnimationController& operator=(const AnimationController&) = delete;

  void forward();
  void reverse();
  void stop();
  void set_value(double value);
  void set_callback(ValueCallback callback) { callback_ = std::move(callback); }

  [[nodiscard]] double value() const { return value_; }
  [[nodiscard]] AnimationStatus status() const { return status_; }

private:
  void on_tick(FrameDuration elapsed);
  void publish();

  FrameDuration duration_;
  FrameDuration elapsed_{};
  ValueCallback callback_;
  std::shared_ptr<Ticker> ticker_;
  double value_{};
  AnimationStatus status_{AnimationStatus::dismissed};
};

} // namespace dui
