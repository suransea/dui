#include "dui/ui.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct TrackedResource {
  int* constructions;
  int* destructions;
  bool active{true};

  TrackedResource(int& created, int& destroyed)
    : constructions(&created), destructions(&destroyed) {
    ++*constructions;
  }

  ~TrackedResource() {
    if (active) {
      ++*destructions;
    }
  }

  TrackedResource(const TrackedResource&) = delete;
  TrackedResource& operator=(const TrackedResource&) = delete;

  TrackedResource(TrackedResource&& other) noexcept
    : constructions(other.constructions), destructions(other.destructions),
      active(std::exchange(other.active, false)) {}
};

struct ResourceComponent {
  int* constructions;
  int* destructions;
  const TrackedResource** output;
  std::optional<dui::CancellationToken>* token;

  auto build(dui::BuildContext& context) const {
    auto& resource = context.resource<"tracked">([&] {
      return TrackedResource{*constructions, *destructions};
    });
    *output = &resource;
    *token = context.cancellation<"work">();
    return dui::Text{"resource"};
  }
};

void named_resource_is_stable_and_destroyed_on_unmount() {
  dui::BuildOwner owner;
  int constructions = 0;
  int destructions = 0;
  const TrackedResource* first = nullptr;
  std::optional<dui::CancellationToken> token;

  owner.render(ResourceComponent{&constructions, &destructions, &first, &token});
  const TrackedResource* original = first;
  owner.render(ResourceComponent{&constructions, &destructions, &first, &token});
  require(first == original, "named resource changed identity across rebuild");
  require(constructions == 1 && destructions == 0, "resource factory ran more than once");
  require(!token->stop_requested(), "component cancellation was requested while mounted");

  owner.render(dui::Text{"replacement"});
  require(destructions == 1, "resource was not destroyed during Element unmount");
  require(token->stop_requested(), "Element unmount did not cancel scoped work");
}

struct CancellationObserver {
  dui::CancellationToken token;
  bool* saw_cancellation;

  ~CancellationObserver() {
    if (saw_cancellation != nullptr) {
      *saw_cancellation = token.stop_requested();
    }
  }

  CancellationObserver(dui::CancellationToken value, bool& observed)
    : token(std::move(value)), saw_cancellation(&observed) {}
  CancellationObserver(const CancellationObserver&) = delete;
  CancellationObserver& operator=(const CancellationObserver&) = delete;
  CancellationObserver(CancellationObserver&& other) noexcept
    : token(std::move(other.token)),
      saw_cancellation(std::exchange(other.saw_cancellation, nullptr)) {}
};

struct CancellationOrderComponent {
  bool* saw_cancellation;

  auto build(dui::BuildContext& context) const {
    auto token = context.cancellation<"cancel-first">();
    static_cast<void>(context.resource<"worker">([&] {
      return CancellationObserver{token, *saw_cancellation};
    }));
    return dui::Text{"work"};
  }
};

void cancellation_precedes_resource_destruction() {
  dui::BuildOwner owner;
  bool saw_cancellation = false;
  owner.render(CancellationOrderComponent{&saw_cancellation});
  owner.render(dui::Text{"replacement"});
  require(saw_cancellation, "resource was destroyed before scoped cancellation was requested");
}

void ticker_scheduler_handles_lifetime_and_stop() {
  dui::TickerScheduler scheduler;
  int ticks = 0;
  auto ticker = scheduler.create_ticker([&](dui::FrameDuration) { ++ticks; });
  ticker->start();
  scheduler.elapse(dui::FrameDuration{0.1});
  ticker->stop();
  scheduler.elapse(dui::FrameDuration{0.1});
  require(ticks == 1, "stopped Ticker continued ticking");

  ticker.reset();
  scheduler.elapse(dui::FrameDuration{0.1});
  require(scheduler.live_ticker_count() == 0, "destroyed Ticker remained registered");
}

void animation_controller_runs_forward_and_reverse() {
  dui::TickerScheduler scheduler;
  std::vector<double> values;
  dui::AnimationController animation{scheduler, dui::FrameDuration{1.0},
                                     [&](double value) { values.push_back(value); }};

  animation.forward();
  scheduler.elapse(dui::FrameDuration{0.25});
  require(animation.value() == 0.25, "forward animation progress is incorrect");
  scheduler.elapse(dui::FrameDuration{1.0});
  require(animation.value() == 1.0, "forward animation did not clamp at completion");
  require(animation.status() == dui::AnimationStatus::completed,
          "animation status is not completed");

  animation.reverse();
  scheduler.elapse(dui::FrameDuration{0.4});
  require(animation.value() == 0.6, "reverse animation progress is incorrect");
  scheduler.elapse(dui::FrameDuration{1.0});
  require(animation.value() == 0.0, "reverse animation did not clamp at dismissal");
  require(animation.status() == dui::AnimationStatus::dismissed,
          "animation status is not dismissed");
  require(values.size() == 4, "animation callback count is incorrect");
}

void invalid_animation_inputs_are_rejected() {
  dui::TickerScheduler scheduler;
  bool duration_rejected = false;
  try {
    dui::AnimationController invalid{scheduler, dui::FrameDuration{0.0}};
  } catch (const std::invalid_argument&) {
    duration_rejected = true;
  }
  require(duration_rejected, "zero animation duration was accepted");

  dui::AnimationController animation{scheduler, dui::FrameDuration{1.0}};
  bool value_rejected = false;
  try {
    animation.set_value(2.0);
  } catch (const std::invalid_argument&) {
    value_rejected = true;
  }
  require(value_rejected, "out-of-range animation value was accepted");
}

dui::Task<int> delayed_value(dui::ManualExecutor& executor, dui::CancellationToken token,
                             int& side_effect) {
  co_await executor.schedule();
  if (token.stop_requested()) {
    co_return -1;
  }
  ++side_effect;
  co_return 42;
}

void manual_executor_resumes_live_task() {
  dui::ManualExecutor executor;
  dui::CancellationSource cancellation;
  int side_effect = 0;
  auto task = delayed_value(executor, cancellation.token(), side_effect);

  task.start();
  require(executor.pending_count() == 1 && !task.done(), "Task did not suspend on ManualExecutor");
  executor.run_all();
  require(task.done() && task.result() == 42, "ManualExecutor did not complete Task");
  require(side_effect == 1, "live Task did not perform its work exactly once");
}

void destroyed_task_is_not_resumed() {
  dui::ManualExecutor executor;
  dui::CancellationSource cancellation;
  int side_effect = 0;
  {
    auto task = delayed_value(executor, cancellation.token(), side_effect);
    task.start();
    require(executor.pending_count() == 1, "Task was not queued before destruction");
  }

  executor.run_all();
  require(side_effect == 0, "executor resumed a destroyed coroutine");
}

struct TaskResourceComponent {
  dui::ManualExecutor* executor;
  int* side_effect;

  auto build(dui::BuildContext& context) const {
    auto token = context.cancellation<"task-cancel">();
    auto& task =
      context.resource<"task">([&] { return delayed_value(*executor, token, *side_effect); });
    task.start();
    return dui::Text{"task"};
  }
};

void element_unmount_invalidates_queued_coroutine() {
  dui::ManualExecutor executor;
  dui::BuildOwner owner;
  int side_effect = 0;
  owner.render(TaskResourceComponent{&executor, &side_effect});
  require(executor.pending_count() == 1, "Element Task was not scheduled");

  owner.render(dui::Text{"replacement"});
  executor.run_all();
  require(side_effect == 0, "unmounted Element coroutine was resumed");
}

dui::Task<void> replace_owning_element(dui::ManualExecutor& executor, dui::BuildOwner& owner,
                                       int& side_effect) {
  co_await executor.schedule();
  owner.render(dui::Text{"replacement"});
  ++side_effect;
}

struct SelfReplacingTaskComponent {
  dui::ManualExecutor* executor;
  dui::BuildOwner* owner;
  int* side_effect;

  auto build(dui::BuildContext& context) const {
    auto& task = context.resource<"self-replacing-task">(
      [&] { return replace_owning_element(*executor, *owner, *side_effect); });
    task.start();
    return dui::Text{"task"};
  }
};

void running_coroutine_survives_destruction_of_its_task_owner() {
  dui::ManualExecutor executor;
  dui::BuildOwner owner;
  int side_effect = 0;
  owner.render(SelfReplacingTaskComponent{&executor, &owner, &side_effect});

  executor.run_all();
  require(side_effect == 1, "running coroutine frame was not retained through owner destruction");
  require(owner.dump_tree().find("replacement") != std::string::npos,
          "coroutine did not replace its owner");
}

dui::Task<void> destroy_executor_while_running(dui::ManualExecutor& executor,
                                               std::unique_ptr<dui::ManualExecutor>& owner,
                                               int& side_effect) {
  co_await executor.schedule();
  owner.reset();
  ++side_effect;
}

void run_all_survives_executor_destruction_from_coroutine() {
  auto executor = std::make_unique<dui::ManualExecutor>();
  int side_effect = 0;
  auto task = destroy_executor_while_running(*executor, executor, side_effect);
  task.start();

  executor->run_all();
  require(task.done() && side_effect == 1,
          "run_all accessed an executor destroyed by resumed work");
}

} // namespace

int main() {
  try {
    named_resource_is_stable_and_destroyed_on_unmount();
    cancellation_precedes_resource_destruction();
    ticker_scheduler_handles_lifetime_and_stop();
    animation_controller_runs_forward_and_reverse();
    invalid_animation_inputs_are_rejected();
    manual_executor_resumes_live_task();
    destroyed_task_is_not_resumed();
    element_unmount_invalidates_queued_coroutine();
    running_coroutine_survives_destruction_of_its_task_owner();
    run_all_survives_executor_destruction_from_coroutine();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "All DUI lifecycle tests passed\n";
  return EXIT_SUCCESS;
}
