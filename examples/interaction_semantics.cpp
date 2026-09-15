#include "dui/ui.hpp"

#include <iostream>

struct InteractiveCard {
  int* activations;
  int* key_presses;

  auto build(dui::BuildContext&) const {
    return dui::VStack{
      dui::on_tap(
        dui::background(dui::padding(dui::Text{"Activate"}, dui::Insets::all(4.0)), 0xff336699U),
        [counter = activations] { ++*counter; }),
      dui::focusable(
        dui::Text{"Keyboard target"},
        [counter = key_presses](const dui::KeyEvent& event) {
          if (event.phase == dui::KeyPhase::down && event.logical_key == "Enter") {
            ++*counter;
            return dui::KeyEventResult::handled;
          }
          return dui::KeyEventResult::ignored;
        },
        true)};
  }
};

const dui::SemanticsNode* supporting(const dui::SemanticsNode& node, dui::SemanticsAction action) {
  if (node.supports(action)) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const auto* match = supporting(child, action); match != nullptr) {
      return match;
    }
  }
  return nullptr;
}

const dui::SemanticsNode* supporting(const dui::SemanticsTree& tree, dui::SemanticsAction action) {
  for (const auto& root : tree.roots) {
    if (const auto* match = supporting(root, action); match != nullptr) {
      return match;
    }
  }
  return nullptr;
}

int main() {
  int activations{};
  int key_presses{};
  dui::BuildOwner owner;
  owner.render(InteractiveCard{&activations, &key_presses});
  [[maybe_unused]] const auto frame = owner.frame(dui::BoxConstraints::tight({180.0, 60.0}));

  const auto semantics = owner.semantics_tree();
  const auto* activation = supporting(semantics, dui::SemanticsAction::activate);
  const auto* focus = supporting(semantics, dui::SemanticsAction::focus);
  if (activation == nullptr || focus == nullptr) {
    std::cerr << "Interactive semantics were not produced\n";
    return 1;
  }
  if (!owner.perform_semantics_action(activation->id, dui::SemanticsAction::activate) ||
      !owner.perform_semantics_action(focus->id, dui::SemanticsAction::focus)) {
    std::cerr << "Interactive semantics actions were rejected\n";
    return 1;
  }
  const auto key_result = owner.dispatch_key({"Enter", dui::KeyPhase::down});

  if (activations != 1 || key_presses != 1 || key_result != dui::KeyEventResult::handled) {
    std::cerr << "Interactive callbacks did not observe semantic and keyboard actions\n";
    return 1;
  }

  std::cout << "Activations: " << activations << '\n';
  std::cout << "Enter presses: " << key_presses << '\n';
  std::cout << "Key handled: " << (key_result == dui::KeyEventResult::handled ? "yes" : "no")
            << '\n';
  std::cout << "Semantic roots: " << semantics.roots.size() << '\n';
}
