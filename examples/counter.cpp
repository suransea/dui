#include "dui/ui.hpp"

#include <iostream>
#include <optional>
#include <string>

struct Counter {
  std::optional<dui::StateHandle<int>>* handle;

  auto build(dui::BuildContext& context) const {
    auto count = context.state<"count">(0);
    *handle = count;
    return dui::VStack{dui::Text{"Counter"}, dui::Text{"Count: " + std::to_string(count.get())}};
  }
};

int main() {
  dui::BuildOwner owner;
  std::optional<dui::StateHandle<int>> count;

  owner.render(Counter{&count});
  std::cout << owner.dump_tree() << '\n';

  count->update([](int value) { return value + 1; });
  owner.flush();
  std::cout << owner.dump_tree();
}
