#include "dui/ui.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <utility>

struct Profile {
  std::optional<dui::StateHandle<std::string>>* name;

  auto build(dui::BuildContext& context) const {
    auto value = context.restorable_state<"name">(std::string{"Guest"}, "profile.name");
    *name = value;
    return dui::Text{"Profile: " + value.get()};
  }
};

int main() {
  std::optional<dui::StateHandle<std::string>> original_name;
  dui::BuildOwner original;
  original.render(Profile{&original_name});
  original_name->set("Ada");
  original.flush();
  auto snapshot = original.save_restoration_state();

  std::optional<dui::StateHandle<std::string>> restored_name;
  dui::BuildOwner replacement;
  replacement.restore_state(std::move(snapshot));
  replacement.render(Profile{&restored_name});

  if (!original_name.has_value() || !restored_name.has_value() || original_name->get() != "Ada" ||
      restored_name->get() != "Ada" || replacement.pending_restoration_count() != 0) {
    std::cerr << "Profile state was not restored into the replacement owner\n";
    return 1;
  }

  std::cout << "Original: " << original_name->get() << '\n';
  std::cout << "Restored: " << restored_name->get() << '\n';
  std::cout << "Pending records: " << replacement.pending_restoration_count() << '\n';
  std::cout << replacement.dump_tree();
}
