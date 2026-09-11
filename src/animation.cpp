#include "dui/animation.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <stdexcept>

namespace dui {

std::shared_ptr<Ticker> TickerScheduler::create_ticker(Ticker::Callback callback) {
    auto ticker = std::make_shared<Ticker>(std::move(callback));
    tickers_.push_back(ticker);
    return ticker;
}

void TickerScheduler::elapse(FrameDuration elapsed) {
    if (elapsed.count() < 0.0 || !std::isfinite(elapsed.count())) {
        throw std::invalid_argument("Ticker elapsed duration must be finite and non-negative");
    }

    std::vector<std::shared_ptr<Ticker>> live;
    for (const auto& weak_ticker : tickers_) {
        if (auto ticker = weak_ticker.lock()) {
            live.push_back(std::move(ticker));
        }
    }
    std::erase_if(tickers_, [](const auto& ticker) { return ticker.expired(); });

    for (const auto& ticker : live) {
        ticker->tick(elapsed);
    }
}

std::size_t TickerScheduler::live_ticker_count() const {
    return static_cast<std::size_t>(std::ranges::count_if(tickers_, [](const auto& ticker) {
        return !ticker.expired();
    }));
}

AnimationController::AnimationController(
    TickerScheduler& scheduler,
    FrameDuration duration,
    ValueCallback callback
) :
    duration_(duration),
    callback_(std::move(callback)),
    ticker_(scheduler.create_ticker([this](FrameDuration elapsed) {
        on_tick(elapsed);
    })) {
    if (duration_.count() <= 0.0 || !std::isfinite(duration_.count())) {
        throw std::invalid_argument("Animation duration must be finite and positive");
    }
}

AnimationController::~AnimationController() {
    ticker_->stop();
    ticker_->clear_callback();
}

void AnimationController::forward() {
    if (value_ >= 1.0) {
        value_ = 1.0;
        status_ = AnimationStatus::completed;
        return;
    }
    elapsed_ = duration_ * value_;
    status_ = AnimationStatus::forward;
    ticker_->start();
}

void AnimationController::reverse() {
    if (value_ <= 0.0) {
        value_ = 0.0;
        status_ = AnimationStatus::dismissed;
        return;
    }
    elapsed_ = duration_ * value_;
    status_ = AnimationStatus::reverse;
    ticker_->start();
}

void AnimationController::stop() {
    ticker_->stop();
}

void AnimationController::set_value(double value) {
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        throw std::invalid_argument("Animation value must be in the range [0, 1]");
    }
    value_ = value;
    elapsed_ = duration_ * value_;
    status_ = value_ <= 0.0
        ? AnimationStatus::dismissed
        : value_ >= 1.0 ? AnimationStatus::completed : status_;
    publish();
}

void AnimationController::on_tick(FrameDuration elapsed) {
    if (status_ == AnimationStatus::forward) {
        elapsed_ += elapsed;
    } else if (status_ == AnimationStatus::reverse) {
        elapsed_ -= elapsed;
    } else {
        return;
    }

    const double progress = elapsed_.count() / duration_.count();
    value_ = std::clamp(progress, 0.0, 1.0);
    if (value_ >= 1.0) {
        status_ = AnimationStatus::completed;
        ticker_->stop();
    } else if (value_ <= 0.0) {
        status_ = AnimationStatus::dismissed;
        ticker_->stop();
    }
    publish();
}

void AnimationController::publish() {
    if (callback_) {
        ValueCallback callback = callback_;
        callback(value_);
    }
}

} // namespace dui
