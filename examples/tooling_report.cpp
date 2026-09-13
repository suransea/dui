#include "dui/ui.hpp"

#include <iostream>
#include <memory>
#include <string>

struct ReportCounter {
  auto build(dui::BuildContext& context) const {
    const auto count = context.restorable_state<"count">(
      3, "example.counter", [](const int& value) { return std::to_string(value); });
    return dui::VStack{dui::Text{"Counter <preview>"},
                       dui::Text{"Count: " + std::to_string(count.get())}};
  }
};

int main() {
  auto timeline = std::make_shared<dui::TimelineRecorder>(64);
  dui::BuildOwner owner;
  owner.set_timeline_recorder(timeline);
  owner.render(ReportCounter{});

  std::cout << dui::ToolingReport{owner.inspect(), timeline->snapshot()}.to_html();
}
