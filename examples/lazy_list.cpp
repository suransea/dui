#include "dui/ui.hpp"

#include <iostream>
#include <string>
#include <vector>

struct Item {
  int id;
  std::string label;
};

struct Catalog {
  std::vector<Item> items;
  double offset;
  int* builds;

  auto build(dui::BuildContext&) const {
    auto children =
      dui::lazy_for_each(items, dui::key<&Item::id>, [counter = builds](const Item& item) {
        ++*counter;
        return dui::Text{item.label};
      });
    return dui::Viewport{
      offset, dui::SliverFixedExtentList{20.0, dui::cache_extent(20.0), std::move(children)}};
  }
};

int main() {
  std::vector<Item> items;
  for (int index = 0; index < 20; ++index) {
    items.push_back({index, "Item " + std::to_string(index)});
  }

  int builds{};
  dui::BuildOwner owner;
  owner.render(Catalog{items, 0.0, &builds});
  [[maybe_unused]] const auto first_frame = owner.frame(dui::BoxConstraints::tight({120.0, 60.0}));
  const int initial_builds = builds;

  owner.render(Catalog{items, 100.0, &builds});
  [[maybe_unused]] const auto scrolled_frame =
    owner.frame(dui::BoxConstraints::tight({120.0, 60.0}));
  const std::string tree = owner.dump_tree();

  if (initial_builds <= 0 || initial_builds >= static_cast<int>(items.size()) ||
      builds <= initial_builds || builds >= initial_builds + static_cast<int>(items.size()) ||
      tree.find("Item 5") == std::string::npos || tree.find("Item 0") != std::string::npos) {
    std::cerr << "Lazy list did not limit or replace its realized range\n";
    return 1;
  }

  std::cout << "Items in model: " << items.size() << '\n';
  std::cout << "Initially realized: " << initial_builds << '\n';
  std::cout << "New builds after scroll: " << builds - initial_builds << '\n';
  std::cout << tree;
}
