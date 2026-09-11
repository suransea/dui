#include "dui/ui.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct FrameState {
  int presents{};
  int abandons{};
  int mappings{};
  dui::SurfaceFrameDelegate::PresentStatus present_status{
    dui::SurfaceFrameDelegate::PresentStatus::presented};
};

class MemoryFrameDelegate final : public dui::SurfaceFrameDelegate {
public:
  MemoryFrameDelegate(std::shared_ptr<FrameState> state, std::size_t size)
    : state_(std::move(state)), pixels_(size) {}

  [[nodiscard]] std::span<std::byte> pixels() override {
    ++state_->mappings;
    return pixels_;
  }
  [[nodiscard]] PresentStatus present() noexcept override {
    ++state_->presents;
    return state_->present_status;
  }
  void abandon() noexcept override { ++state_->abandons; }

private:
  std::shared_ptr<FrameState> state_;
  std::vector<std::byte> pixels_;
};

void surface_frame_is_exactly_once_transaction() {
  auto presented = std::make_shared<FrameState>();
  {
    dui::SurfaceFrame frame{{2, 2, 8, dui::SurfacePixelFormat::rgba8888_srgb_premultiplied, 1},
                            std::make_unique<MemoryFrameDelegate>(presented, 16)};
    require(frame.pixels().size() == 16, "surface frame exposed wrong pixel extent");
    static_cast<void>(frame.present());
    bool rejected = false;
    try {
      static_cast<void>(frame.present());
    } catch (const std::logic_error&) {
      rejected = true;
    }
    require(rejected, "surface frame presented more than once");
  }
  require(presented->presents == 1 && presented->abandons == 0, "presented frame was abandoned");
  require(presented->mappings == 1, "surface pixel storage was mapped more than once");

  auto abandoned = std::make_shared<FrameState>();
  {
    dui::SurfaceFrame frame{{1, 1, 4, dui::SurfacePixelFormat::rgba8888_srgb_premultiplied, 2},
                            std::make_unique<MemoryFrameDelegate>(abandoned, 4)};
  }
  require(abandoned->presents == 0 && abandoned->abandons == 1,
          "dropped frame was not abandoned once");
}

void surface_contract_rejects_invalid_frames_and_acquisitions() {
  auto state = std::make_shared<FrameState>();
  bool invalid_frame = false;
  try {
    dui::SurfaceFrame frame{{2, 2, 7, dui::SurfacePixelFormat::rgba8888_srgb_premultiplied, 1},
                            std::make_unique<MemoryFrameDelegate>(state, 16)};
  } catch (const std::invalid_argument&) {
    invalid_frame = true;
  }
  require(invalid_frame && state->abandons == 1, "invalid acquired frame was not safely abandoned");

  bool invalid_status = false;
  try {
    static_cast<void>(dui::SurfaceAcquisition::failed(dui::SurfaceAcquireStatus::ready));
  } catch (const std::invalid_argument&) {
    invalid_status = true;
  }
  require(invalid_status, "ready acquisition without frame was accepted");
  bool invalid_enum = false;
  try {
    static_cast<void>(dui::SurfaceAcquisition::failed(static_cast<dui::SurfaceAcquireStatus>(99)));
  } catch (const std::invalid_argument&) {
    invalid_enum = true;
  }
  require(invalid_enum, "unknown surface acquisition status was accepted");
  auto unavailable = dui::SurfaceAcquisition::failed(dui::SurfaceAcquireStatus::unavailable);
  require(unavailable.frame() == nullptr, "failed acquisition exposed a frame");

  auto ready_state = std::make_shared<FrameState>();
  auto ready = dui::SurfaceAcquisition::ready(std::make_unique<dui::SurfaceFrame>(
    dui::SurfaceDescriptor{1, 1, 4, dui::SurfacePixelFormat::rgba8888_srgb_premultiplied, 3},
    std::make_unique<MemoryFrameDelegate>(ready_state, 4)));
  auto frame = ready.take_frame();
  require(ready.status() == dui::SurfaceAcquireStatus::consumed,
          "taken acquisition remained ready");
  bool taken_twice = false;
  try {
    static_cast<void>(ready.take_frame());
  } catch (const std::logic_error&) {
    taken_twice = true;
  }
  require(taken_twice, "surface acquisition returned its frame twice");
  frame->abandon();

  require(dui::SurfaceRequest{{100.0, 50.0}, 200, 100, 2.0, 9}.valid(),
          "valid surface request was rejected");
  require(!dui::SurfaceRequest{{100.0, 50.0}, 0, 100, 2.0, 9}.valid(),
          "non-empty logical frame accepted zero physical width");
}

class ContractSurface final : public dui::RasterSurface {
public:
  bool mismatch{};
  dui::SurfaceAcquireStatus next_status{dui::SurfaceAcquireStatus::ready};
  std::shared_ptr<FrameState> state{std::make_shared<FrameState>()};

protected:
  [[nodiscard]] dui::SurfaceAcquisition do_acquire(const dui::SurfaceRequest& request) override {
    if (next_status != dui::SurfaceAcquireStatus::ready) {
      return dui::SurfaceAcquisition::failed(next_status);
    }
    const std::uint32_t width = mismatch ? request.physical_width + 1 : request.physical_width;
    return dui::SurfaceAcquisition::ready(std::make_unique<dui::SurfaceFrame>(
      dui::SurfaceDescriptor{width, request.physical_height, static_cast<std::size_t>(width) * 4,
                             dui::SurfacePixelFormat::rgba8888_srgb_premultiplied,
                             request.generation},
      std::make_unique<MemoryFrameDelegate>(state, static_cast<std::size_t>(width) *
                                                     request.physical_height * 4)));
  }
};

void raster_surface_validates_request_and_acquired_frame() {
  ContractSurface surface;
  const dui::SurfaceRequest request{{2.0, 2.0}, 4, 4, 2.0, 7};
  auto acquisition = surface.acquire(request);
  require(acquisition.status() == dui::SurfaceAcquireStatus::ready,
          "valid surface was not acquired");
  acquisition.take_frame()->abandon();

  surface.mismatch = true;
  bool mismatch_rejected = false;
  try {
    static_cast<void>(surface.acquire(request));
  } catch (const std::logic_error&) {
    mismatch_rejected = true;
  }
  require(mismatch_rejected, "surface returned a frame for the wrong request generation or extent");
}

struct RenderLog {
  std::mutex mutex;
  std::vector<std::string> frames;
  std::vector<std::thread::id> threads;
  std::vector<std::uint64_t> generations;
};

class RecordingSurfaceRenderer final : public dui::SurfaceRenderer {
public:
  explicit RecordingSurfaceRenderer(std::shared_ptr<RenderLog> log) : log_(std::move(log)) {}

  void render(dui::SurfaceFrame& frame, const dui::LayerTree& layer_tree,
              const dui::SurfaceRequest& request) override {
    frame.pixels().front() = std::byte{0xff};
    std::lock_guard lock{log_->mutex};
    log_->frames.push_back(layer_tree.flatten().dump());
    log_->threads.push_back(std::this_thread::get_id());
    log_->generations.push_back(request.generation);
  }

private:
  std::shared_ptr<RenderLog> log_;
};

void raster_thread_submits_immutable_frames_in_order() {
  dui::BuildOwner owner;
  owner.render(dui::Text{"first"});
  const auto first = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  owner.render(dui::Text{"second"});
  const auto second = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));

  auto surface = std::make_shared<ContractSurface>();
  auto log = std::make_shared<RenderLog>();
  dui::RasterThread raster{surface,
                           [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }};
  const auto first_ticket = raster.submit(first, {{100.0, 100.0}, 100, 100, 1.0, 1});
  const auto second_ticket = raster.submit(second, {{100.0, 100.0}, 100, 100, 1.0, 2});
  require(first_ticket.id() != 0 && second_ticket.id() > first_ticket.id(),
          "raster frame IDs are not monotonic");
  require(first_ticket.wait() == dui::RasterThread::FrameOutcome::presented,
          "first frame was not presented");
  require(first_ticket.wait() == dui::RasterThread::FrameOutcome::presented,
          "frame ticket was consumptive");
  require(second_ticket.wait() == dui::RasterThread::FrameOutcome::presented,
          "second frame was not presented");
  raster.wait_idle();

  std::lock_guard lock{log->mutex};
  require(log->frames.size() == 2, "raster thread did not render both acquired frames");
  require(log->frames[0].contains("first"), "first immutable snapshot was changed");
  require(log->frames[1].contains("second"), "second frame was rendered out of order");
  require(log->threads[0] != std::this_thread::get_id(), "renderer ran on submitting thread");
  require(log->threads[0] == log->threads[1], "frames used different raster threads");
  require(log->generations == std::vector<std::uint64_t>{1, 2},
          "captured surface generations changed");
  require(surface->state->presents == 2 && surface->state->abandons == 0,
          "rendered frames were not presented exactly once");
  require(raster.pending_count() == 0, "raster queue did not drain");
}

class ThrowingRenderer final : public dui::SurfaceRenderer {
public:
  void render(dui::SurfaceFrame&, const dui::LayerTree&, const dui::SurfaceRequest&) override {
    throw std::runtime_error("raster failure");
  }
};

void renderer_failure_is_reported_to_waiter() {
  dui::BuildOwner owner;
  owner.render(dui::Text{"frame"});
  auto surface = std::make_shared<ContractSurface>();
  dui::RasterThread raster{surface, [] { return std::make_unique<ThrowingRenderer>(); }};
  const auto frame = raster.submit(owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0})),
                                   {{100.0, 100.0}, 100, 100, 1.0, 1});

  bool reported = false;
  try {
    static_cast<void>(frame.wait());
  } catch (const std::runtime_error&) {
    reported = true;
  }
  require(reported, "raster renderer failure was swallowed");
  require(surface->state->abandons == 1, "failed render did not abandon acquired frame");
}

class StoppingRenderer final : public dui::SurfaceRenderer {
public:
  explicit StoppingRenderer(dui::RasterThread*& owner) : owner_(&owner) {}

  void render(dui::SurfaceFrame&, const dui::LayerTree&, const dui::SurfaceRequest&) override {
    (*owner_)->request_stop();
  }

private:
  dui::RasterThread** owner_;
};

void renderer_callback_can_request_quiescent_stop() {
  dui::BuildOwner owner;
  owner.render(dui::Text{"frame"});
  auto surface = std::make_shared<ContractSurface>();
  dui::RasterThread* raster_pointer = nullptr;
  dui::RasterThread raster{surface,
                           [&] { return std::make_unique<StoppingRenderer>(raster_pointer); }};
  raster_pointer = &raster;
  static_cast<void>(raster.submit(owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0})),
                                  {{100.0, 100.0}, 100, 100, 1.0, 1}));
  raster.wait_idle();
  bool rejected = false;
  try {
    static_cast<void>(raster.submit(owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0})),
                                    {{100.0, 100.0}, 100, 100, 1.0, 2}));
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "RasterThread accepted work after requested stop");
}

void raster_thread_classifies_surface_acquisition_results() {
  dui::BuildOwner owner;
  owner.render(dui::Text{"frame"});
  const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  const dui::SurfaceRequest request{{100.0, 100.0}, 100, 100, 1.0, 1};

  for (const auto status :
       {dui::SurfaceAcquireStatus::unavailable, dui::SurfaceAcquireStatus::out_of_date}) {
    auto surface = std::make_shared<ContractSurface>();
    surface->next_status = status;
    auto log = std::make_shared<RenderLog>();
    dui::RasterThread raster{surface,
                             [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }};
    const auto frame = raster.submit(tree, request);
    const auto expected = status == dui::SurfaceAcquireStatus::unavailable
                            ? dui::RasterThread::FrameOutcome::unavailable
                            : dui::RasterThread::FrameOutcome::out_of_date;
    require(frame.wait() == expected, "transient surface result was not reported");
    raster.wait_idle();
    require(log->frames.empty(), "non-ready surface reached renderer");
  }

  auto lost_surface = std::make_shared<ContractSurface>();
  lost_surface->next_status = dui::SurfaceAcquireStatus::lost;
  auto log = std::make_shared<RenderLog>();
  dui::RasterThread lost{lost_surface,
                         [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }};
  const auto lost_frame = lost.submit(tree, request);
  bool failed = false;
  try {
    static_cast<void>(lost_frame.wait());
  } catch (const std::runtime_error&) {
    failed = true;
  }
  require(failed, "lost raster surface was not terminal");

  auto valid_surface = std::make_shared<ContractSurface>();
  dui::RasterThread validating{
    valid_surface,
    [] { return std::make_unique<RecordingSurfaceRenderer>(std::make_shared<RenderLog>()); }};
  bool mismatch_rejected = false;
  try {
    static_cast<void>(validating.submit(tree, {{50.0, 50.0}, 50, 50, 1.0, 2}));
  } catch (const std::invalid_argument&) {
    mismatch_rejected = true;
  }
  require(mismatch_rejected, "RasterThread accepted surface metrics for another LayerTree");
}

} // namespace

int main() {
  try {
    surface_frame_is_exactly_once_transaction();
    surface_contract_rejects_invalid_frames_and_acquisitions();
    raster_surface_validates_request_and_acquired_frame();
    raster_thread_submits_immutable_frames_in_order();
    renderer_failure_is_reported_to_waiter();
    renderer_callback_can_request_quiescent_stop();
    raster_thread_classifies_surface_acquisition_results();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All DUI backend tests passed\n";
  return EXIT_SUCCESS;
}
