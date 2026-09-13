#include "dui/ui.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class StepTimelineClock final : public dui::TimelineClock {
public:
  std::chrono::nanoseconds now() const noexcept override {
    return std::chrono::nanoseconds{tick_++};
  }

private:
  mutable std::int64_t tick_{};
};

template <std::size_t Depth> struct NestedLazy {
  auto build(dui::BuildContext&) const {
    if constexpr (Depth == 0) {
      return dui::Text{"nested lazy leaf"};
    } else {
      const std::array items{0};
      return dui::Viewport{
        0.0, dui::SliverFixedExtentList{16.0, dui::cache_extent(16.0),
                                        dui::lazy_for_each(
                                          items, [](int value) { return value; },
                                          [](const int&) { return NestedLazy<Depth - 1>{}; })}};
    }
  }
};

void accepts_final_probe_after_sixteen_realization_rounds() {
  dui::BuildOwner owner;
  auto recorder =
    std::make_shared<dui::TimelineRecorder>(128, std::make_shared<StepTimelineClock>());
  owner.set_timeline_recorder(recorder);
  owner.render(NestedLazy<16>{});
  recorder->clear();
  const auto frame = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(frame.dump().contains("nested lazy leaf"),
          "sixteen realization rounds were rejected before the convergence probe");

  const auto timeline = recorder->snapshot();
  require(timeline.events.size() == 69 && timeline.dropped_event_count == 0 &&
            timeline.events.front().phase == dui::TimelinePhase::frame &&
            timeline.events.front().outcome == dui::TimelineOutcome::completed &&
            timeline.events.front().work_count == 16 &&
            timeline.events.back().phase == dui::TimelinePhase::composite,
          "timeline omitted successful lazy stabilization phases");
  const auto frame_id = timeline.events.front().frame_id;
  const auto frame_span_id = timeline.events.front().span_id;
  require(timeline.events[1].phase == dui::TimelinePhase::build,
          "timeline did not flush builds before lazy layout");
  for (std::size_t pass = 0; pass < 16; ++pass) {
    const std::size_t first = 2 + pass * 4;
    require(timeline.events[first].phase == dui::TimelinePhase::synchronize_render_tree &&
              timeline.events[first].pass == pass &&
              timeline.events[first + 1].phase == dui::TimelinePhase::layout &&
              timeline.events[first + 1].pass == pass &&
              timeline.events[first + 2].phase == dui::TimelinePhase::lazy_realization &&
              timeline.events[first + 2].pass == pass &&
              timeline.events[first + 3].phase == dui::TimelinePhase::build,
            "timeline lazy stabilization phase order or pass index was incorrect");
  }
  require(timeline.events[66].phase == dui::TimelinePhase::synchronize_render_tree &&
            timeline.events[67].phase == dui::TimelinePhase::layout &&
            timeline.events[67].pass == 16,
          "timeline omitted the final lazy convergence probe");
  for (std::size_t index = 1; index < timeline.events.size(); ++index) {
    require(timeline.events[index].frame_id == frame_id &&
              timeline.events[index].parent_span_id == frame_span_id,
            "lazy timeline event was not correlated with its frame");
  }
}

void rejects_a_seventeenth_realization_round() {
  dui::BuildOwner owner;
  auto recorder =
    std::make_shared<dui::TimelineRecorder>(128, std::make_shared<StepTimelineClock>());
  owner.set_timeline_recorder(recorder);
  owner.render(NestedLazy<17>{});
  recorder->clear();
  bool rejected = false;
  try {
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "seventeenth lazy realization round exceeded the deterministic limit");
  const auto timeline = recorder->snapshot();
  require(timeline.events.size() == 68 &&
            timeline.events.front().phase == dui::TimelinePhase::frame &&
            timeline.events.front().outcome == dui::TimelineOutcome::failed &&
            timeline.events.front().work_count == 16,
          "timeline did not close the non-converging frame as failed");
  for (const auto& event : timeline.events) {
    require(event.phase != dui::TimelinePhase::composite,
            "timeline reported composition for a non-converged frame");
  }

  recorder->clear();
  owner.render(dui::Text{"recovered frame"});
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  const auto recovered = recorder->snapshot();
  require(recovered.events.size() == 6 && recovered.events[1].phase == dui::TimelinePhase::frame &&
            recovered.events[1].outcome == dui::TimelineOutcome::completed &&
            recovered.events.back().phase == dui::TimelinePhase::composite,
          "timeline did not recover after a failed lazy frame");
}

} // namespace

int main() {
  try {
    accepts_final_probe_after_sixteen_realization_rounds();
    rejects_a_seventeenth_realization_round();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All DUI lazy stabilization tests passed\n";
  return EXIT_SUCCESS;
}
