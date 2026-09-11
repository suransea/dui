#include "dui/task.hpp"

namespace dui {

bool ManualExecutor::run_one() { return run_one(state_); }

void ManualExecutor::run_all() {
  const auto state = state_;
  while (run_one(state)) {
  }
}

bool ManualExecutor::run_one(const std::shared_ptr<detail::ExecutorState>& state) {
  if (state->pending.empty()) {
    return false;
  }
  const auto control = state->pending.front().lock();
  state->pending.pop_front();
  if (control != nullptr && !control->coroutine.done()) {
    control->coroutine.resume();
  }
  return true;
}

} // namespace dui
