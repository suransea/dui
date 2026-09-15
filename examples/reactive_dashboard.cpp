#include "dui/ui.hpp"

#include <iostream>
#include <optional>
#include <string>

struct DashboardTheme {
  std::string title;
};

struct Dashboard {
  dui::Signal<int>* temperature;
  std::optional<dui::StateHandle<bool>>* details;

  auto build(dui::BuildContext& context) const {
    auto show_details = context.state<"show-details">(true);
    *details = show_details;
    const int current = context.watch(*temperature);
    const auto& theme = context.environment<DashboardTheme>();
    return dui::VStack{dui::Text{theme.title},
                       dui::Text{"Temperature: " + std::to_string(current) + " C"},
                       dui::optional(show_details.get(), [current] {
                         return dui::Text{"Status: " + std::string{current > 25 ? "warm" : "cool"}};
                       })};
  }
};

int main() {
  dui::Signal<int> temperature{21};
  std::optional<dui::StateHandle<bool>> details;
  dui::BuildOwner owner;

  owner.render(
    dui::with_environment(Dashboard{&temperature, &details}, DashboardTheme{"Studio dashboard"}));
  const std::string initial = owner.dump_tree();
  std::cout << "Initial\n" << initial;

  temperature.set(28);
  owner.flush();
  details->set(false);
  owner.flush();
  const std::string updated = owner.dump_tree();
  std::cout << "Updated\n" << updated;
  if (!details.has_value() || initial.find("Temperature: 21 C") == std::string::npos ||
      initial.find("Status: cool") == std::string::npos ||
      updated.find("Temperature: 28 C") == std::string::npos ||
      updated.find("Status:") != std::string::npos) {
    std::cerr << "Reactive dashboard did not rebuild its dependencies\n";
    return 1;
  }
}
