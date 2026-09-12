#include "dui/ui.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

dui::SemanticsProperties properties(std::string label, dui::SemanticsRole role, bool enabled = true,
                                    bool hidden = false) {
  return {role, std::move(label), {}, enabled, hidden};
}

void semantics_tree_preserves_hierarchy_identity_and_actions() {
  dui::BuildOwner owner;
  int activations = 0;
  int hidden_activations = 0;
  const auto make_view = [&](std::string button_label, dui::SemanticsRole button_role,
                             std::string value, bool enabled, auto callback) {
    auto button_properties = properties(std::move(button_label), button_role, enabled);
    button_properties.value = std::move(value);
    return dui::VStack{
      dui::semantics(
        dui::VStack{dui::semantics(dui::Text{"A"}, properties("first", dui::SemanticsRole::text)),
                    dui::semantics(dui::Text{"B"}, properties("second", dui::SemanticsRole::text))},
        properties("group", dui::SemanticsRole::generic)),
      dui::semantics(dui::semantics(dui::Text{"hidden"},
                                    properties("hidden descendant", dui::SemanticsRole::button),
                                    [&] { ++hidden_activations; }),
                     properties("hidden ancestor", dui::SemanticsRole::generic, true, true)),
      dui::semantics(dui::Text{"tap"}, std::move(button_properties), std::move(callback))};
  };

  owner.render(
    make_view("activate", dui::SemanticsRole::button, "ready", true, [&] { ++activations; }));
  bool pre_frame_rejected = false;
  try {
    static_cast<void>(owner.semantics_tree());
  } catch (const std::logic_error&) {
    pre_frame_rejected = true;
  }
  require(pre_frame_rejected, "semantics query before the first frame was accepted");

  [[maybe_unused]] const auto frame = owner.frame(dui::BoxConstraints::tight({100.0, 64.0}));
  const auto tree = owner.semantics_tree();
  require(tree.roots.size() == 2, "transparent or hidden semantics produced incorrect roots");
  const auto& group = tree.roots[0];
  const auto& button = tree.roots[1];
  require(group.label == "group" && group.children.size() == 2 &&
            group.children[0].label == "first" && group.children[1].label == "second",
          "nested semantics hierarchy or ordering was incorrect");
  require(button.label == "activate" && button.role == dui::SemanticsRole::button &&
            button.supports(dui::SemanticsAction::activate) &&
            button.bounds == dui::Rect{{0.0, 48.0}, {24.0, 16.0}},
          "semantic role, action, or global bounds were incorrect");
  const auto button_id = button.id;
  const auto hidden_id = owner.root()->children()[1]->children().front()->render_object()->id();
  require(tree.find(group.children[1].id) == &group.children[1],
          "SemanticsTree::find did not locate a nested node");
  require(owner.perform_semantics_action(button_id, dui::SemanticsAction::activate) &&
            activations == 1,
          "enabled semantic activation was not dispatched");
  require(!owner.perform_semantics_action(hidden_id, dui::SemanticsAction::activate) &&
            hidden_activations == 0,
          "hidden semantic subtree remained actionable by retained ID");

  auto* button_render = owner.root()->children()[2]->render_object();
  const auto layouts = button_render->layout_count();
  const auto paints = button_render->paint_count();
  owner.render(
    make_view("disabled", dui::SemanticsRole::image, "blocked", false, [&] { ++activations; }));
  require(owner.pending_layout_count() == 0 && owner.pending_paint_count() == 0,
          "property-only semantics update dirtied layout or paint");
  const auto disabled_tree = owner.semantics_tree();
  const auto* disabled = disabled_tree.find(button_id);
  require(disabled != nullptr && disabled->label == "disabled" && disabled->value == "blocked" &&
            disabled->role == dui::SemanticsRole::image && !disabled->enabled &&
            !disabled->supports(dui::SemanticsAction::activate) &&
            button_render->layout_count() == layouts && button_render->paint_count() == paints,
          "property-only semantics update lost identity or changed rendering");
  require(!owner.perform_semantics_action(button_id, dui::SemanticsAction::activate) &&
            activations == 1,
          "disabled semantic action was dispatched");

  owner.render(
    make_view("no callback", dui::SemanticsRole::button, "idle", true, std::function<void()>{}));
  const auto no_callback_tree = owner.semantics_tree();
  const auto* no_callback = no_callback_tree.find(button_id);
  require(no_callback != nullptr && no_callback->value == "idle" &&
            !no_callback->supports(dui::SemanticsAction::activate),
          "removing a semantic callback retained a stale action");

  owner.render(make_view("replace", dui::SemanticsRole::button, "ready", true, [&] {
    ++activations;
    owner.render(dui::Text{"replacement"});
  }));
  const auto replace_tree = owner.semantics_tree();
  require(replace_tree.find(button_id) != nullptr &&
            owner.perform_semantics_action(button_id, dui::SemanticsAction::activate) &&
            activations == 2,
          "semantic callback could not safely replace its own retained tree");
  require(!owner.perform_semantics_action(button_id, dui::SemanticsAction::activate),
          "stale semantic node ID remained actionable");
  require(owner.semantics_tree().roots.empty(),
          "unannotated replacement did not produce an empty explicit semantics tree");
}

void semantics_tree_clips_lazy_visible_children() {
  struct Item {
    int id;
  };
  const std::vector<Item> items{{0}, {1}, {2}, {3}, {4}, {5}};
  int activations = 0;
  auto builder = [&](const Item& item) {
    return dui::semantics(dui::Text{"item"},
                          properties("item " + std::to_string(item.id), dui::SemanticsRole::text),
                          [&] { ++activations; });
  };
  const auto make_view = [&](double offset) {
    auto source =
      dui::lazy_for_each(items, dui::key<&Item::id>, builder).keep_alive_when([](const Item& item) {
        return item.id == 1;
      });
    return dui::Viewport{
      offset, dui::SliverFixedExtentList{16.0, dui::cache_extent(16.0), std::move(source)}};
  };

  dui::BuildOwner owner;
  owner.render(make_view(24.0));
  [[maybe_unused]] const auto first = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto clipped = owner.semantics_tree();
  require(clipped.roots.size() == 2 && clipped.roots[0].label == "item 1" &&
            clipped.roots[1].label == "item 2" &&
            clipped.roots[0].bounds == dui::Rect{{0.0, 0.0}, {100.0, 8.0}} &&
            clipped.roots[1].bounds == dui::Rect{{0.0, 8.0}, {100.0, 8.0}},
          "partially visible lazy semantics were not ordered and clipped correctly");
  auto* list_element = owner.root()->children().front().get();
  const auto cache_only_id = list_element->children().front()->render_object()->id();
  require(!owner.perform_semantics_action(cache_only_id, dui::SemanticsAction::activate) &&
            activations == 0,
          "cache-only semantic node remained actionable by retained ID");
  const auto formerly_visible_id = clipped.roots.front().id;

  owner.render(make_view(48.0));
  [[maybe_unused]] const auto second = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto scrolled = owner.semantics_tree();
  require(scrolled.roots.size() == 1 && scrolled.roots.front().label == "item 3" &&
            owner.root()->children().front()->kept_alive_child_count() == 1,
          "lazy cache or dormant keep-alive item leaked into semantics");
  require(!owner.perform_semantics_action(formerly_visible_id, dui::SemanticsAction::activate) &&
            activations == 0,
          "dormant semantic node remained actionable by retained ID");
}

void render_owner_rejects_semantics_without_current_layout() {
  dui::RenderOwner owner;
  dui::RenderSemanticsBox semantics{owner, properties("raw", dui::SemanticsRole::button), [] {}};
  dui::RenderText text{owner, "raw"};
  std::array<dui::RenderObject*, 1> child{&text};
  semantics.set_children(child);
  std::array<dui::RenderObject*, 1> roots{&semantics};
  owner.set_roots(roots);

  bool tree_rejected = false;
  bool action_rejected = false;
  try {
    static_cast<void>(owner.semantics_tree());
  } catch (const std::logic_error&) {
    tree_rejected = true;
  }
  try {
    static_cast<void>(
      owner.perform_semantics_action(semantics.id(), dui::SemanticsAction::activate));
  } catch (const std::logic_error&) {
    action_rejected = true;
  }
  require(tree_rejected && action_rejected,
          "RenderOwner exposed semantics before a completed frame");

  [[maybe_unused]] const auto frame = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(owner.semantics_tree().roots.size() == 1,
          "RenderOwner did not expose semantics after a completed frame");
  semantics.mark_needs_layout();
  bool dirty_rejected = false;
  try {
    static_cast<void>(owner.semantics_tree());
  } catch (const std::logic_error&) {
    dirty_rejected = true;
  }
  require(dirty_rejected, "RenderOwner exposed semantics with dirty layout geometry");
}

} // namespace

int main() {
  try {
    semantics_tree_preserves_hierarchy_identity_and_actions();
    semantics_tree_clips_lazy_visible_children();
    render_owner_rejects_semantics_without_current_layout();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All DUI semantics tests passed\n";
  return EXIT_SUCCESS;
}
