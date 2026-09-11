#include "dui/backend.hpp"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace dui {

namespace {

bool multiplication_overflows(std::size_t left, std::size_t right) {
  return right != 0 && left > std::numeric_limits<std::size_t>::max() / right;
}

} // namespace

bool SurfaceRequest::valid() const {
  return std::isfinite(logical_size.width) && std::isfinite(logical_size.height) &&
         logical_size.width >= 0.0 && logical_size.height >= 0.0 &&
         std::isfinite(device_pixel_ratio) && device_pixel_ratio > 0.0 && generation != 0 &&
         ((logical_size.width == 0.0 || logical_size.height == 0.0)
            ? physical_width == 0 || physical_height == 0
            : physical_width != 0 && physical_height != 0) &&
         std::abs(static_cast<double>(physical_width) - logical_size.width * device_pixel_ratio) <=
           1.0 &&
         std::abs(static_cast<double>(physical_height) -
                  logical_size.height * device_pixel_ratio) <= 1.0;
}

SurfaceFrame::SurfaceFrame(SurfaceDescriptor descriptor,
                           std::unique_ptr<SurfaceFrameDelegate> delegate)
  : descriptor_(descriptor), delegate_(std::move(delegate)) {
  if (delegate_ == nullptr) {
    throw std::invalid_argument(
      "Ready surface frame requires dimensions, generation, and delegate");
  }
  const auto reject = [&](const char* message) -> void {
    delegate_->abandon();
    active_ = false;
    throw std::invalid_argument(message);
  };
  if (descriptor_.width == 0 || descriptor_.height == 0 || descriptor_.generation == 0) {
    reject("Ready surface frame requires dimensions, generation, and delegate");
  }
  if (descriptor_.format != SurfacePixelFormat::rgba8888_srgb_premultiplied ||
      descriptor_.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      descriptor_.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    reject("Surface format or dimensions are unsupported");
  }
  constexpr std::size_t bytes_per_pixel = 4;
  const std::size_t width = descriptor_.width;
  if (multiplication_overflows(width, bytes_per_pixel)) {
    reject("Surface width overflows row size");
  }
  const std::size_t minimum_row = width * bytes_per_pixel;
  if (descriptor_.row_bytes < minimum_row ||
      descriptor_.height > std::numeric_limits<std::size_t>::max() / descriptor_.row_bytes) {
    reject("Surface stride or extent is invalid");
  }
  const std::size_t required = descriptor_.row_bytes * descriptor_.height;
  try {
    const std::span<std::byte> mapped = delegate_->pixels();
    if (mapped.size() < required) {
      reject("Surface pixel storage is smaller than its descriptor");
    }
    pixels_ = mapped.first(required);
  } catch (...) {
    if (active_) {
      delegate_->abandon();
      active_ = false;
    }
    throw;
  }
}

SurfaceFrame::~SurfaceFrame() { abandon(); }

std::span<std::byte> SurfaceFrame::pixels() {
  if (!active_) {
    throw std::logic_error("Surface frame transaction is already complete");
  }
  return pixels_;
}

SurfaceFrameDelegate::PresentStatus SurfaceFrame::present() {
  if (!active_) {
    throw std::logic_error("Surface frame transaction is already complete");
  }
  active_ = false;
  pixels_ = {};
  return delegate_->present();
}

void SurfaceFrame::abandon() noexcept {
  if (!active_) {
    return;
  }
  active_ = false;
  pixels_ = {};
  delegate_->abandon();
}

SurfaceAcquisition::SurfaceAcquisition(SurfaceAcquisition&& other) noexcept
  : status_(other.status_), frame_(std::move(other.frame_)) {
  other.status_ = SurfaceAcquireStatus::consumed;
}

SurfaceAcquisition& SurfaceAcquisition::operator=(SurfaceAcquisition&& other) noexcept {
  if (this != &other) {
    status_ = other.status_;
    frame_ = std::move(other.frame_);
    other.status_ = SurfaceAcquireStatus::consumed;
  }
  return *this;
}

SurfaceAcquisition SurfaceAcquisition::ready(std::unique_ptr<SurfaceFrame> frame) {
  if (frame == nullptr || !frame->active()) {
    throw std::invalid_argument("Ready surface acquisition requires a frame");
  }
  return SurfaceAcquisition{SurfaceAcquireStatus::ready, std::move(frame)};
}

SurfaceAcquisition SurfaceAcquisition::failed(SurfaceAcquireStatus status) {
  if (status != SurfaceAcquireStatus::unavailable && status != SurfaceAcquireStatus::out_of_date &&
      status != SurfaceAcquireStatus::lost) {
    throw std::invalid_argument("Failed surface acquisition cannot have ready status");
  }
  return SurfaceAcquisition{status, nullptr};
}

std::unique_ptr<SurfaceFrame> SurfaceAcquisition::take_frame() {
  if (status_ != SurfaceAcquireStatus::ready || frame_ == nullptr) {
    throw std::logic_error("Surface acquisition has no ready frame");
  }
  status_ = SurfaceAcquireStatus::consumed;
  return std::move(frame_);
}

SurfaceAcquisition RasterSurface::acquire(const SurfaceRequest& request) {
  if (!request.valid()) {
    throw std::invalid_argument("RasterSurface received an invalid request");
  }
  SurfaceAcquisition acquisition = do_acquire(request);
  if (acquisition.status() != SurfaceAcquireStatus::ready) {
    if (acquisition.frame() != nullptr) {
      throw std::logic_error("Failed surface acquisition returned a frame");
    }
    return acquisition;
  }
  const SurfaceFrame* frame = acquisition.frame();
  if (frame == nullptr || !frame->active()) {
    throw std::logic_error("Ready surface acquisition returned no active frame");
  }
  const SurfaceDescriptor& descriptor = frame->descriptor();
  if (descriptor.width != request.physical_width || descriptor.height != request.physical_height ||
      descriptor.generation != request.generation) {
    throw std::logic_error("Acquired surface frame does not match its request");
  }
  return acquisition;
}

class RasterThread::Impl {
public:
  struct Submission {
    FrameId id{};
    LayerTree layer_tree;
    SurfaceRequest request;
    std::promise<FrameOutcome> completion;
  };

  struct State {
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable idle;
    std::deque<Submission> pending;
    std::shared_ptr<RasterSurface> surface;
    RendererFactory renderer_factory;
    std::exception_ptr failure;
    FrameId next_frame{1};
    bool rendering{};
    bool stopping{};
  };

  Impl(std::shared_ptr<RasterSurface> surface, RendererFactory renderer_factory)
    : state_(std::make_shared<State>()) {
    if (surface == nullptr || !renderer_factory) {
      throw std::invalid_argument("RasterThread requires a surface and renderer");
    }
    state_->surface = std::move(surface);
    state_->renderer_factory = std::move(renderer_factory);
    worker_ = std::thread{[state = state_] { run(std::move(state)); }};
  }

  ~Impl() {
    request_stop();
    if (!worker_.joinable()) {
      return;
    }
    if (worker_.get_id() == std::this_thread::get_id()) {
      std::terminate();
    }
    worker_.join();
  }

  void request_stop() {
    {
      std::lock_guard lock{state_->mutex};
      state_->stopping = true;
      cancel_pending(*state_);
      if (!state_->rendering) {
        state_->idle.notify_all();
      }
    }
    state_->ready.notify_one();
  }

  [[nodiscard]] FrameTicket submit(LayerTree layer_tree, SurfaceRequest request) {
    if (layer_tree.root() == nullptr) {
      throw std::invalid_argument("RasterThread cannot submit an empty LayerTree");
    }
    if (!request.valid() || request.logical_size != layer_tree.frame_size()) {
      throw std::invalid_argument("Raster submission has invalid or mismatched surface metrics");
    }
    std::lock_guard lock{state_->mutex};
    rethrow_failure();
    if (state_->stopping) {
      throw std::logic_error("RasterThread is stopping");
    }
    FrameId id = state_->next_frame++;
    if (id == 0) {
      id = state_->next_frame++;
    }
    std::promise<FrameOutcome> completion;
    std::shared_future<FrameOutcome> future = completion.get_future().share();
    state_->pending.push_back({id, std::move(layer_tree), request, std::move(completion)});
    state_->ready.notify_one();
    return FrameTicket{id, std::move(future)};
  }

  void wait_idle() {
    if (worker_.get_id() == std::this_thread::get_id()) {
      throw std::logic_error("RasterThread cannot wait for itself");
    }
    std::unique_lock lock{state_->mutex};
    state_->idle.wait(lock, [&] { return state_->pending.empty() && !state_->rendering; });
    rethrow_failure();
  }

  [[nodiscard]] std::size_t pending_count() const {
    std::lock_guard lock{state_->mutex};
    return state_->pending.size() + (state_->rendering ? 1u : 0u);
  }

private:
  static void run(std::shared_ptr<State> state) {
    std::unique_ptr<SurfaceRenderer> renderer;
    try {
      renderer = state->renderer_factory();
      if (renderer == nullptr) {
        throw std::logic_error("Raster renderer factory returned null");
      }
      state->renderer_factory = {};
    } catch (...) {
      std::lock_guard lock{state->mutex};
      state->failure = std::current_exception();
      state->stopping = true;
      fail_pending(*state, state->failure);
      state->idle.notify_all();
      return;
    }
    while (true) {
      Submission submission;
      {
        std::unique_lock lock{state->mutex};
        state->ready.wait(lock, [&] { return state->stopping || !state->pending.empty(); });
        if (state->pending.empty()) {
          if (state->stopping) {
            return;
          }
          continue;
        }
        submission = std::move(state->pending.front());
        state->pending.pop_front();
        state->rendering = true;
      }

      FrameOutcome outcome = FrameOutcome::failed;
      try {
        SurfaceAcquisition acquisition = state->surface->acquire(submission.request);
        switch (acquisition.status()) {
        case SurfaceAcquireStatus::ready: {
          std::unique_ptr<SurfaceFrame> frame = acquisition.take_frame();
          renderer->render(*frame, submission.layer_tree, submission.request);
          switch (frame->present()) {
          case SurfaceFrameDelegate::PresentStatus::presented:
            outcome = FrameOutcome::presented;
            break;
          case SurfaceFrameDelegate::PresentStatus::out_of_date:
            outcome = FrameOutcome::out_of_date;
            break;
          case SurfaceFrameDelegate::PresentStatus::lost:
            throw std::runtime_error("Raster surface was lost during present");
          }
          break;
        }
        case SurfaceAcquireStatus::unavailable:
          outcome = FrameOutcome::unavailable;
          break;
        case SurfaceAcquireStatus::out_of_date:
          outcome = FrameOutcome::out_of_date;
          break;
        case SurfaceAcquireStatus::lost:
          throw std::runtime_error("Raster surface was lost");
        case SurfaceAcquireStatus::consumed:
          throw std::logic_error("Raster surface returned consumed acquisition");
        }
      } catch (...) {
        std::lock_guard lock{state->mutex};
        if (state->failure == nullptr) {
          state->failure = std::current_exception();
        }
        submission.completion.set_exception(state->failure);
        fail_pending(*state, state->failure);
        state->stopping = true;
        state->rendering = false;
        state->idle.notify_all();
        state->ready.notify_one();
        return;
      }

      {
        std::lock_guard lock{state->mutex};
        submission.completion.set_value(outcome);
        state->rendering = false;
        if (state->pending.empty()) {
          state->idle.notify_all();
        }
      }
    }
  }

  static void cancel_pending(State& state) {
    for (Submission& submission : state.pending) {
      submission.completion.set_value(FrameOutcome::canceled);
    }
    state.pending.clear();
    state.idle.notify_all();
  }

  static void fail_pending(State& state, const std::exception_ptr& failure) {
    for (Submission& submission : state.pending) {
      submission.completion.set_exception(failure);
    }
    state.pending.clear();
    state.idle.notify_all();
  }

  void rethrow_failure() const {
    if (state_->failure != nullptr) {
      std::rethrow_exception(state_->failure);
    }
  }

  std::shared_ptr<State> state_;
  std::thread worker_;
};

RasterThread::RasterThread(std::shared_ptr<RasterSurface> surface, RendererFactory renderer_factory)
  : impl_(std::make_unique<Impl>(std::move(surface), std::move(renderer_factory))) {}

RasterThread::~RasterThread() = default;

RasterThread::FrameTicket RasterThread::submit(LayerTree layer_tree, SurfaceRequest request) {
  return impl_->submit(std::move(layer_tree), request);
}

void RasterThread::request_stop() { impl_->request_stop(); }

void RasterThread::wait_idle() { impl_->wait_idle(); }

std::size_t RasterThread::pending_count() const { return impl_->pending_count(); }

} // namespace dui
