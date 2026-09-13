#include "dui/ui.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <ranges>
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

class StepTimelineClock final : public dui::TimelineClock {
public:
  std::chrono::nanoseconds now() const noexcept override {
    ++call_count_;
    return std::chrono::nanoseconds{tick_++};
  }

  [[nodiscard]] std::size_t call_count() const { return call_count_; }

private:
  mutable std::int64_t tick_{};
  mutable std::size_t call_count_{};
};

class BlockingFinishTimelineClock final : public dui::TimelineClock {
public:
  std::chrono::nanoseconds now() const noexcept override {
    std::unique_lock lock{mutex_};
    if (call_count_++ == 0) {
      return std::chrono::nanoseconds::zero();
    }
    finish_waiting_ = true;
    ready_.notify_all();
    ready_.wait(lock, [&] { return finish_released_; });
    return std::chrono::nanoseconds{1};
  }

  [[nodiscard]] bool wait_for_finish() const {
    std::unique_lock lock{mutex_};
    return ready_.wait_for(lock, std::chrono::seconds{5}, [&] { return finish_waiting_; });
  }

  void release_finish() {
    {
      std::lock_guard lock{mutex_};
      finish_released_ = true;
    }
    ready_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable ready_;
  mutable std::size_t call_count_{};
  mutable bool finish_waiting_{};
  bool finish_released_{};
};

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
  auto clock = std::make_shared<StepTimelineClock>();
  auto timeline = std::make_shared<dui::TimelineRecorder>(32, clock);
  dui::RasterThread raster{
    surface, [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }, timeline};
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

  const auto trace = timeline->snapshot();
  require(trace.events.size() == 8 && trace.dropped_event_count == 0,
          "raster timeline omitted successful frame phases");
  const std::array expected_phases{
    dui::TimelinePhase::raster_frame, dui::TimelinePhase::surface_acquire,
    dui::TimelinePhase::rasterize, dui::TimelinePhase::surface_present};
  const std::array frame_ids{first_ticket.id(), second_ticket.id()};
  for (std::size_t frame_index = 0; frame_index < frame_ids.size(); ++frame_index) {
    const std::size_t first_event = frame_index * expected_phases.size();
    const auto root_span = trace.events[first_event].span_id;
    for (std::size_t phase_index = 0; phase_index < expected_phases.size(); ++phase_index) {
      const auto& event = trace.events[first_event + phase_index];
      require(event.sequence == first_event + phase_index + 1 &&
                event.phase == expected_phases[phase_index] &&
                event.lane == dui::TimelineLane::raster &&
                event.frame_id == frame_ids[frame_index] &&
                event.outcome == dui::TimelineOutcome::completed && event.pass == 0 &&
                event.work_count == 1,
              "successful raster timeline phase metadata was incorrect");
      if (phase_index != 0) {
        require(event.parent_span_id == root_span && event.duration == std::chrono::nanoseconds{1},
                "raster child span had incorrect parent or duration");
      }
    }
    require(trace.events[first_event].parent_span_id == 0 &&
              trace.events[first_event].duration == std::chrono::nanoseconds{7},
            "raster root span timing or parent was incorrect");
  }
  require(trace.to_json().starts_with("{\"version\":2") &&
            trace.to_json().contains("\"lane\":\"raster\"") &&
            trace.to_json().contains("\"phase\":\"surfacePresent\""),
          "raster timeline did not select canonical JSON version 2");
}

class ThrowingRenderer final : public dui::SurfaceRenderer {
public:
  void render(dui::SurfaceFrame&, const dui::LayerTree&, const dui::SurfaceRequest&) override {
    throw std::runtime_error("raster failure");
  }
};

void renderer_failure_is_reported_to_waiter() {
  auto timeline = std::make_shared<dui::TimelineRecorder>(8, std::make_shared<StepTimelineClock>());
  dui::BuildOwner owner;
  owner.set_timeline_recorder(timeline);
  owner.render(dui::Text{"frame"});
  const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  const auto flow_id = tree.timeline_flow_id();
  timeline->clear();
  auto surface = std::make_shared<ContractSurface>();
  dui::RasterThread raster{surface, [] { return std::make_unique<ThrowingRenderer>(); }, timeline};
  const auto frame = raster.submit(tree, {{100.0, 100.0}, 100, 100, 1.0, 1});

  bool reported = false;
  try {
    static_cast<void>(frame.wait());
  } catch (const std::runtime_error&) {
    reported = true;
  }
  require(reported, "raster renderer failure was swallowed");
  require(surface->state->abandons == 1, "failed render did not abandon acquired frame");
  const auto trace = timeline->snapshot();
  require(trace.events.size() == 3 && trace.events[0].phase == dui::TimelinePhase::raster_frame &&
            trace.events[0].outcome == dui::TimelineOutcome::failed &&
            trace.events[1].phase == dui::TimelinePhase::surface_acquire &&
            trace.events[1].outcome == dui::TimelineOutcome::completed &&
            trace.events[2].phase == dui::TimelinePhase::rasterize &&
            trace.events[2].outcome == dui::TimelineOutcome::failed &&
            std::ranges::all_of(trace.events,
                                [&](const auto& event) { return event.flow_id == flow_id; }),
          "renderer failure timeline was incomplete or reported a present phase");
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

struct RenderGate {
  std::mutex mutex;
  std::condition_variable ready;
  bool entered{};
  bool released{};
};

class BlockingRenderer final : public dui::SurfaceRenderer {
public:
  explicit BlockingRenderer(std::shared_ptr<RenderGate> gate) : gate_(std::move(gate)) {}

  void render(dui::SurfaceFrame&, const dui::LayerTree&, const dui::SurfaceRequest&) override {
    std::unique_lock lock{gate_->mutex};
    gate_->entered = true;
    gate_->ready.notify_one();
    gate_->ready.wait(lock, [&] { return gate_->released; });
  }

private:
  std::shared_ptr<RenderGate> gate_;
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

void queued_cancellation_emits_no_raster_timeline_event() {
  dui::BuildOwner owner;
  owner.render(dui::Text{"queued cancellation"});
  const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  const dui::SurfaceRequest request{{100.0, 100.0}, 100, 100, 1.0, 1};
  auto gate = std::make_shared<RenderGate>();
  auto surface = std::make_shared<ContractSurface>();
  auto timeline =
    std::make_shared<dui::TimelineRecorder>(16, std::make_shared<StepTimelineClock>());
  dui::RasterThread raster{surface, [gate] { return std::make_unique<BlockingRenderer>(gate); },
                           timeline};
  const auto active = raster.submit(tree, request);
  {
    std::unique_lock lock{gate->mutex};
    gate->ready.wait(lock, [&] { return gate->entered; });
  }
  timeline->clear();
  std::weak_ptr<dui::TimelineRecorder> retained_timeline = timeline;
  timeline.reset();
  auto queued_request = request;
  queued_request.generation = 2;
  const auto queued = raster.submit(tree, queued_request);
  raster.request_stop();
  require(queued.wait() == dui::RasterThread::FrameOutcome::canceled,
          "queued raster frame was not canceled by stop");
  {
    std::lock_guard lock{gate->mutex};
    gate->released = true;
  }
  gate->ready.notify_one();
  require(active.wait() == dui::RasterThread::FrameOutcome::presented,
          "active raster frame did not finish after stop");
  raster.wait_idle();
  const auto recorder = retained_timeline.lock();
  require(recorder != nullptr, "RasterThread did not retain its timeline recorder");
  const auto trace = recorder->snapshot();
  require(trace.events.size() == 3 && trace.events[0].phase == dui::TimelinePhase::raster_frame &&
            trace.events[1].phase == dui::TimelinePhase::rasterize &&
            trace.events[2].phase == dui::TimelinePhase::surface_present,
          "timeline clear did not remove a completed acquire or retain in-progress spans");
  for (const auto& event : trace.events) {
    require(event.frame_id == active.id() && event.frame_id != queued.id(),
            "queued cancellation appeared in the raster timeline");
  }
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
    auto timeline =
      std::make_shared<dui::TimelineRecorder>(8, std::make_shared<StepTimelineClock>());
    dui::BuildOwner traced_owner;
    traced_owner.set_timeline_recorder(timeline);
    traced_owner.render(dui::Text{"traced acquisition"});
    const auto traced_tree = traced_owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    const auto flow_id = traced_tree.timeline_flow_id();
    timeline->clear();
    dui::RasterThread raster{
      surface, [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }, timeline};
    const auto frame = raster.submit(traced_tree, request);
    const auto expected = status == dui::SurfaceAcquireStatus::unavailable
                            ? dui::RasterThread::FrameOutcome::unavailable
                            : dui::RasterThread::FrameOutcome::out_of_date;
    require(frame.wait() == expected, "transient surface result was not reported");
    raster.wait_idle();
    require(log->frames.empty(), "non-ready surface reached renderer");
    const auto trace = timeline->snapshot();
    const auto timeline_outcome = status == dui::SurfaceAcquireStatus::unavailable
                                    ? dui::TimelineOutcome::unavailable
                                    : dui::TimelineOutcome::out_of_date;
    require(trace.events.size() == 2 && trace.events[0].phase == dui::TimelinePhase::raster_frame &&
              trace.events[0].outcome == timeline_outcome &&
              trace.events[1].phase == dui::TimelinePhase::surface_acquire &&
              trace.events[1].outcome == timeline_outcome && trace.events[0].flow_id == flow_id &&
              trace.events[1].flow_id == flow_id,
            "transient acquisition timeline reported later raster phases");
  }

  auto lost_surface = std::make_shared<ContractSurface>();
  lost_surface->next_status = dui::SurfaceAcquireStatus::lost;
  auto log = std::make_shared<RenderLog>();
  auto lost_timeline =
    std::make_shared<dui::TimelineRecorder>(8, std::make_shared<StepTimelineClock>());
  dui::BuildOwner lost_owner;
  lost_owner.set_timeline_recorder(lost_timeline);
  lost_owner.render(dui::Text{"traced loss"});
  const auto lost_tree = lost_owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  const auto lost_flow_id = lost_tree.timeline_flow_id();
  lost_timeline->clear();
  dui::RasterThread lost{
    lost_surface, [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }, lost_timeline};
  const auto lost_frame = lost.submit(lost_tree, request);
  bool failed = false;
  try {
    static_cast<void>(lost_frame.wait());
  } catch (const std::runtime_error&) {
    failed = true;
  }
  require(failed, "lost raster surface was not terminal");
  const auto lost_trace = lost_timeline->snapshot();
  require(
    lost_trace.events.size() == 2 && lost_trace.events[0].outcome == dui::TimelineOutcome::lost &&
      lost_trace.events[1].phase == dui::TimelinePhase::surface_acquire &&
      lost_trace.events[1].outcome == dui::TimelineOutcome::lost &&
      lost_trace.events[0].flow_id == lost_flow_id && lost_trace.events[1].flow_id == lost_flow_id,
    "surface acquisition loss was not preserved in the raster timeline");

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

void raster_timeline_classifies_present_results_and_disabled_path() {
  dui::BuildOwner owner;
  owner.render(dui::Text{"present timeline"});
  const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  const dui::SurfaceRequest request{{100.0, 100.0}, 100, 100, 1.0, 1};

  auto disabled_clock = std::make_shared<StepTimelineClock>();
  auto unused = std::make_shared<dui::TimelineRecorder>(8, disabled_clock);
  auto disabled_surface = std::make_shared<ContractSurface>();
  auto disabled_log = std::make_shared<RenderLog>();
  dui::RasterThread disabled{disabled_surface, [disabled_log] {
                               return std::make_unique<RecordingSurfaceRenderer>(disabled_log);
                             }};
  require(disabled.submit(tree, request).wait() == dui::RasterThread::FrameOutcome::presented &&
            disabled_clock->call_count() == 0 && unused->snapshot().events.empty(),
          "raster thread read an unattached timeline clock");

  for (const auto status : {dui::SurfaceFrameDelegate::PresentStatus::out_of_date,
                            dui::SurfaceFrameDelegate::PresentStatus::lost}) {
    auto surface = std::make_shared<ContractSurface>();
    surface->state->present_status = status;
    auto timeline =
      std::make_shared<dui::TimelineRecorder>(8, std::make_shared<StepTimelineClock>());
    dui::BuildOwner traced_owner;
    traced_owner.set_timeline_recorder(timeline);
    traced_owner.render(dui::Text{"traced present"});
    const auto traced_tree = traced_owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    const auto flow_id = traced_tree.timeline_flow_id();
    timeline->clear();
    auto log = std::make_shared<RenderLog>();
    dui::RasterThread raster{
      surface, [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }, timeline};
    const auto ticket = raster.submit(traced_tree, request);
    bool lost = false;
    dui::RasterThread::FrameOutcome outcome = dui::RasterThread::FrameOutcome::failed;
    try {
      outcome = ticket.wait();
    } catch (const std::runtime_error&) {
      lost = true;
    }
    const auto trace = timeline->snapshot();
    const auto timeline_outcome = status == dui::SurfaceFrameDelegate::PresentStatus::out_of_date
                                    ? dui::TimelineOutcome::out_of_date
                                    : dui::TimelineOutcome::lost;
    require(trace.events.size() == 4 && trace.events[0].outcome == timeline_outcome &&
              trace.events[1].outcome == dui::TimelineOutcome::completed &&
              trace.events[2].outcome == dui::TimelineOutcome::completed &&
              trace.events[3].phase == dui::TimelinePhase::surface_present &&
              trace.events[3].outcome == timeline_outcome &&
              std::ranges::all_of(trace.events,
                                  [&](const auto& event) { return event.flow_id == flow_id; }),
            "surface present timeline outcome or phase sequence was incorrect");
    require(status == dui::SurfaceFrameDelegate::PresentStatus::out_of_date
              ? outcome == dui::RasterThread::FrameOutcome::out_of_date && !lost
              : lost,
            "timeline instrumentation changed the surface present result");
  }
}

void incremental_timeline_clear_linearizes_at_completion() {
  auto clock = std::make_shared<BlockingFinishTimelineClock>();
  auto timeline = std::make_shared<dui::TimelineRecorder>(4, clock);
  const auto initial = timeline->read_completed(1);
  dui::BuildOwner owner;
  owner.set_timeline_recorder(timeline);
  std::thread producer{[&] { owner.render(dui::Text{"finishes after clear"}); }};
  const bool finish_waiting = clock->wait_for_finish();
  timeline->clear();
  clock->release_finish();
  producer.join();
  require(finish_waiting, "timeline producer did not reach its blocked completion point");

  const auto completed = timeline->read_completed(1, initial.next_cursor);
  require(completed.events.size() == 1 && completed.events[0].sequence == 1 &&
            completed.events[0].phase == dui::TimelinePhase::reconcile &&
            completed.missed_event_count == 0,
          "timeline clear discarded or counted a span completed after its linearization point");
}

void timeline_recorder_supports_concurrent_ui_raster_observation_and_clear() {
  auto clock = std::make_shared<StepTimelineClock>();
  auto timeline = std::make_shared<dui::TimelineRecorder>(64, clock);
  auto surface = std::make_shared<ContractSurface>();
  auto log = std::make_shared<RenderLog>();
  dui::RasterThread raster{
    surface, [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }, timeline};
  dui::BuildOwner owner;
  owner.set_timeline_recorder(timeline);

  std::atomic<bool> done{};
  std::atomic<bool> invalid{};
  std::thread observer{[&] {
    dui::TimelineCursor cursor;
    while (!done.load()) {
      const auto snapshot = timeline->snapshot();
      const auto batch = timeline->read_completed(16, cursor);
      cursor = batch.next_cursor;
      try {
        static_cast<void>(snapshot.to_json());
      } catch (...) {
        invalid.store(true);
      }
      for (const auto& event : snapshot.events) {
        if (event.sequence == 0 || event.span_id == 0 ||
            event.duration < std::chrono::nanoseconds::zero() ||
            (event.lane == dui::TimelineLane::raster &&
             (event.flow_id == 0 ||
              (event.phase != dui::TimelinePhase::raster_frame && event.parent_span_id == 0)))) {
          invalid.store(true);
        }
      }
      for (const auto& event : batch.events) {
        if (event.sequence == 0 || event.span_id == 0 ||
            event.duration < std::chrono::nanoseconds::zero()) {
          invalid.store(true);
        }
      }
    }
  }};
  std::thread clearer{[&] {
    while (!done.load()) {
      timeline->clear();
      std::this_thread::yield();
    }
  }};

  std::vector<dui::RasterThread::FrameTicket> tickets;
  for (std::size_t frame = 0; frame < 32; ++frame) {
    owner.render(dui::Text{"concurrent " + std::to_string(frame)});
    tickets.push_back(
      raster.submit(owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0})),
                    {{100.0, 100.0}, 100, 100, 1.0, static_cast<std::uint64_t>(frame + 1)}));
  }
  for (const auto& ticket : tickets) {
    require(ticket.wait() == dui::RasterThread::FrameOutcome::presented,
            "concurrent timeline collection changed a raster result");
  }
  raster.wait_idle();
  done.store(true);
  observer.join();
  clearer.join();
  require(!invalid.load() && clock->call_count() != 0,
          "concurrent timeline snapshot, clear, or serialization was inconsistent");
}

void raster_timeline_correlates_only_matching_recorder_provenance() {
  auto shared_timeline =
    std::make_shared<dui::TimelineRecorder>(32, std::make_shared<StepTimelineClock>());
  dui::BuildOwner owner;
  owner.set_timeline_recorder(shared_timeline);
  owner.render(dui::Text{"matching flow"});
  const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  const auto flow_id = tree.timeline_flow_id();
  auto surface = std::make_shared<ContractSurface>();
  auto log = std::make_shared<RenderLog>();
  dui::RasterThread raster{
    surface, [log] { return std::make_unique<RecordingSurfaceRenderer>(log); }, shared_timeline};
  const auto ticket = raster.submit(tree, {{100.0, 100.0}, 100, 100, 1.0, 1});
  require(ticket.wait() == dui::RasterThread::FrameOutcome::presented,
          "matching flow frame was not presented");
  raster.wait_idle();

  const auto matching = shared_timeline->snapshot();
  require(flow_id != 0 && matching.to_json().starts_with("{\"version\":3"),
          "matching UI/raster timeline did not select a nonzero version-3 flow");
  bool saw_ui_frame = false;
  bool saw_raster_frame = false;
  for (const auto& event : matching.events) {
    if (event.phase == dui::TimelinePhase::reconcile) {
      require(event.flow_id == 0, "standalone reconciliation unexpectedly joined a frame flow");
      continue;
    }
    require(event.flow_id == flow_id, "matching recorder did not propagate one cross-lane flow");
    if (event.phase == dui::TimelinePhase::frame) {
      saw_ui_frame = true;
    }
    if (event.phase == dui::TimelinePhase::raster_frame) {
      saw_raster_frame = true;
      require(event.frame_id == ticket.id(),
              "cross-lane flow replaced the raster ticket frame identity");
    }
  }
  require(saw_ui_frame && saw_raster_frame, "cross-lane flow omitted its UI or raster root");

  auto source_timeline =
    std::make_shared<dui::TimelineRecorder>(16, std::make_shared<StepTimelineClock>());
  std::weak_ptr<dui::TimelineRecorder> source_lifetime = source_timeline;
  dui::LayerTree foreign_tree;
  {
    dui::BuildOwner source_owner;
    source_owner.set_timeline_recorder(source_timeline);
    source_owner.render(dui::Text{"foreign flow"});
    foreign_tree = source_owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  }
  auto target_timeline =
    std::make_shared<dui::TimelineRecorder>(16, std::make_shared<StepTimelineClock>());
  auto target_surface = std::make_shared<ContractSurface>();
  auto target_log = std::make_shared<RenderLog>();
  dui::RasterThread target{
    target_surface, [target_log] { return std::make_unique<RecordingSurfaceRenderer>(target_log); },
    target_timeline};
  const auto target_ticket = target.submit(foreign_tree, {{100.0, 100.0}, 100, 100, 1.0, 2});
  require(target_ticket.wait() == dui::RasterThread::FrameOutcome::presented,
          "mismatched recorder changed raster execution");
  target.wait_idle();
  const auto mismatched = target_timeline->snapshot();
  require(foreign_tree.timeline_flow_id() != 0 && target_ticket.id() == 1 &&
            mismatched.to_json().starts_with("{\"version\":2"),
          "mismatched recorder falsely selected a correlated schema");
  for (const auto& event : mismatched.events) {
    require(event.flow_id == 0, "mismatched recorder accepted foreign weak provenance");
  }
  source_timeline.reset();
  require(source_lifetime.expired(), "LayerTree retained its source timeline recorder");
  target_timeline->clear();
  const auto expired_ticket = target.submit(foreign_tree, {{100.0, 100.0}, 100, 100, 1.0, 3});
  require(expired_ticket.wait() == dui::RasterThread::FrameOutcome::presented,
          "expired provenance changed raster execution");
  target.wait_idle();
  const auto expired = target_timeline->snapshot();
  require(
    expired.events.size() == 4 &&
      std::ranges::all_of(expired.events, [](const auto& event) { return event.flow_id == 0; }),
    "expired weak provenance correlated with a different recorder");
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
    queued_cancellation_emits_no_raster_timeline_event();
    raster_thread_classifies_surface_acquisition_results();
    raster_timeline_classifies_present_results_and_disabled_path();
    incremental_timeline_clear_linearizes_at_completion();
    timeline_recorder_supports_concurrent_ui_raster_observation_and_clear();
    raster_timeline_correlates_only_matching_recorder_provenance();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All DUI backend tests passed\n";
  return EXIT_SUCCESS;
}
