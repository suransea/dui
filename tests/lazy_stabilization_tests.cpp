#include "dui/ui.hpp"

#include <array>
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
  owner.render(NestedLazy<16>{});
  const auto frame = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(frame.dump().contains("nested lazy leaf"),
          "sixteen realization rounds were rejected before the convergence probe");
}

void rejects_a_seventeenth_realization_round() {
  dui::BuildOwner owner;
  owner.render(NestedLazy<17>{});
  bool rejected = false;
  try {
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "seventeenth lazy realization round exceeded the deterministic limit");
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
