#include "dui/ui.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
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
  return {role, std::move(label), {}, enabled, hidden, false, false};
}

dui::SemanticsNode node(std::uint64_t id, std::string label,
                        std::vector<dui::SemanticsNode> children = {}) {
  return {id,
          dui::SemanticsRole::generic,
          std::move(label),
          {},
          true,
          {{static_cast<double>(id), 0.0}, {10.0, 10.0}},
          {},
          std::move(children)};
}

struct RecordingAccessibilityAdapter final : dui::AccessibilityAdapter {
  std::vector<dui::SemanticsUpdate> deliveries;
  bool fail_next{};
  dui::AccessibilityBridge* reentrant_bridge{};
  const dui::SemanticsTree* reentrant_tree{};
  bool publish_rejected{};
  bool clear_rejected{};
  std::function<void()> on_apply;

  void apply(std::span<const dui::SemanticsChange> changes) override {
    deliveries.push_back({std::vector<dui::SemanticsChange>{changes.begin(), changes.end()}});
    if (on_apply) {
      auto callback = on_apply;
      callback();
    }
    if (reentrant_bridge != nullptr && reentrant_tree != nullptr) {
      try {
        static_cast<void>(reentrant_bridge->publish(*reentrant_tree, *this));
      } catch (const std::logic_error&) {
        publish_rejected = true;
      }
      try {
        static_cast<void>(reentrant_bridge->clear(*this));
      } catch (const std::logic_error&) {
        clear_rejected = true;
      }
    }
    if (fail_next) {
      fail_next = false;
      throw std::runtime_error("accessibility delivery failed");
    }
  }
};

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
            button.children.size() == 1 && button.children.front().label == "tap" &&
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
  const auto replacement_tree = owner.semantics_tree();
  require(replacement_tree.roots.size() == 1 &&
            replacement_tree.roots.front().role == dui::SemanticsRole::text &&
            replacement_tree.roots.front().label == "replacement",
          "replacement Text did not produce automatic semantics");
}

void primitive_views_produce_semantics_and_actions() {
  dui::BuildOwner owner;
  int gesture_activations = 0;
  int text_activations = 0;
  const auto make_view = [&](std::string image_label, bool gesture_enabled, bool can_focus) {
    std::function<void()> gesture_callback;
    if (gesture_enabled) {
      gesture_callback = [&] { ++gesture_activations; };
    }
    return dui::VStack{dui::Text{"hello"},
                       dui::Image{"decorative.png", {20.0, 10.0}},
                       dui::Image{"portrait.png", {20.0, 10.0}, std::move(image_label)},
                       dui::on_tap(dui::Text{"tap"}, std::move(gesture_callback)),
                       dui::FocusView<dui::Text>{dui::Text{"focus"}, {}, can_focus, false},
                       dui::Text{"direct", [&] { ++text_activations; }},
                       dui::Text{"outside", [] {}}};
  };

  owner.render(make_view("portrait", true, false));
  [[maybe_unused]] const auto frame = owner.frame(dui::BoxConstraints::tight({200.0, 78.0}));
  const auto tree = owner.semantics_tree();
  require(tree.roots.size() == 5 && tree.roots[0].role == dui::SemanticsRole::text &&
            tree.roots[0].label == "hello" && tree.roots[1].role == dui::SemanticsRole::image &&
            tree.roots[1].label == "portrait",
          "Text, labeled Image, or unlabeled Image primitive semantics were incorrect");

  const auto& gesture = tree.roots[2];
  const auto& focus = tree.roots[3];
  const auto& direct = tree.roots[4];
  require(gesture.role == dui::SemanticsRole::button && gesture.label.empty() &&
            gesture.children.size() == 1 && gesture.children.front().label == "tap" &&
            gesture.supports(dui::SemanticsAction::activate),
          "GestureDetector did not produce an actionable semantic container");
  require(focus.role == dui::SemanticsRole::button && !focus.enabled && !focus.focusable &&
            !focus.focused && focus.children.size() == 1 &&
            focus.children.front().label == "focus" &&
            !focus.supports(dui::SemanticsAction::activate) &&
            !focus.supports(dui::SemanticsAction::focus),
          "disabled FocusView exposed an enabled semantic action");
  owner.dispatch_pointer({30, dui::BuildOwner::PointerPhase::down, {1.0, 53.0}});
  owner.dispatch_pointer({30, dui::BuildOwner::PointerPhase::up, {1.0, 53.0}});
  require(owner.focused_node() == nullptr, "disabled FocusView participated in pointer activation");
  require(direct.role == dui::SemanticsRole::text && direct.label == "direct" &&
            direct.bounds == dui::Rect{{0.0, 68.0}, {48.0, 10.0}} &&
            direct.supports(dui::SemanticsAction::activate) &&
            !owner.perform_semantics_action(owner.root()->children()[6]->render_object()->id(),
                                            dui::SemanticsAction::activate),
          "activatable Text clipping or off-frame exclusion was incorrect");
  require(owner.perform_semantics_action(gesture.id, dui::SemanticsAction::activate) &&
            gesture_activations == 1,
          "GestureDetector semantic action did not report callback dispatch");
  require(owner.perform_semantics_action(direct.id, dui::SemanticsAction::activate) &&
            text_activations == 1,
          "Text semantic action reused pointer propagation as its success result");

  auto* image_render = owner.root()->children()[2]->render_object();
  const auto image_id = tree.roots[1].id;
  const auto layouts = image_render->layout_count();
  const auto paints = image_render->paint_count();
  owner.render(make_view("updated portrait", true, false));
  require(owner.pending_layout_count() == 0 && owner.pending_paint_count() == 0,
          "image semantic-label update dirtied layout or paint");
  const auto updated = owner.semantics_tree();
  const auto* updated_image = updated.find(image_id);
  require(updated_image != nullptr && updated_image->label == "updated portrait" &&
            image_render->layout_count() == layouts && image_render->paint_count() == paints,
          "image semantic-label update lost identity or changed rendering");

  const auto gesture_id = gesture.id;
  const auto focus_id = focus.id;
  owner.render(make_view("updated portrait", false, true));
  require(owner.pending_layout_count() == 0 && owner.pending_paint_count() == 0,
          "primitive callback or enabled-state update dirtied rendering");
  const auto action_update = owner.semantics_tree();
  const auto* disabled_gesture = action_update.find(gesture_id);
  const auto* enabled_focus = action_update.find(focus_id);
  require(disabled_gesture != nullptr && !disabled_gesture->enabled &&
            !disabled_gesture->supports(dui::SemanticsAction::activate) &&
            enabled_focus != nullptr && enabled_focus->enabled && enabled_focus->focusable &&
            !enabled_focus->focused && enabled_focus->supports(dui::SemanticsAction::activate) &&
            enabled_focus->supports(dui::SemanticsAction::focus) &&
            owner.perform_semantics_action(focus_id, dui::SemanticsAction::focus),
          "primitive callback or focus eligibility update retained stale actions");
  const auto focused_tree = owner.semantics_tree();
  const auto* focused = focused_tree.find(focus_id);
  require(focused != nullptr && focused->focusable && focused->focused &&
            owner.focused_node() != nullptr,
          "semantic focus action did not update neutral focus state");

  owner.render(make_view("updated portrait", false, false));
  const auto cleared_focus_tree = owner.semantics_tree();
  const auto* cleared_focus = cleared_focus_tree.find(focus_id);
  require(cleared_focus != nullptr && !cleared_focus->focusable && !cleared_focus->focused &&
            owner.focused_node() == nullptr &&
            !owner.perform_semantics_action(focus_id, dui::SemanticsAction::focus),
          "disabled FocusView retained semantic or framework focus");
}

void semantic_focus_transfers_between_stable_nodes() {
  dui::BuildOwner owner;
  owner.render(dui::VStack{
    dui::FocusView<dui::Text>{dui::Text{"first"}, {}, true, true},
    dui::FocusView<dui::Text>{dui::Text{"second"}, {}, true, false},
  });
  [[maybe_unused]] const auto frame = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  const auto initial = owner.semantics_tree();
  require(initial.roots.size() == 2 && initial.roots[0].focused && initial.roots[0].focusable &&
            !initial.roots[1].focused && initial.roots[1].focusable,
          "autofocus was not represented by exactly one semantic node");
  const auto first_id = initial.roots[0].id;
  const auto second_id = initial.roots[1].id;

  require(owner.perform_semantics_action(second_id, dui::SemanticsAction::focus),
          "second semantic focus action was not dispatched");
  const auto transferred = owner.semantics_tree();
  require(transferred.roots.size() == 2 && transferred.roots[0].id == first_id &&
            !transferred.roots[0].focused && transferred.roots[1].id == second_id &&
            transferred.roots[1].focused,
          "semantic focus transfer changed identity or retained multiple focused nodes");
}

void semantics_tree_clips_lazy_visible_children() {
  struct Item {
    int id;
  };
  const std::vector<Item> items{{0}, {1}, {2}, {3}, {4}, {5}};
  int activations = 0;
  auto builder = [&](const Item& item) {
    return dui::Text{"item " + std::to_string(item.id), [&] { ++activations; }};
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

void semantics_differ_emits_deterministic_transactional_updates() {
  dui::SemanticsDiffer differ;
  auto first_child = node(2, "child");
  first_child.role = dui::SemanticsRole::text;
  auto removed_root = node(3, "removed");
  removed_root.role = dui::SemanticsRole::image;
  const dui::SemanticsTree first{{node(1, "root", {first_child}), removed_root}};

  const auto initial = differ.update(first);
  require(initial.changes.size() == 3 &&
            initial.changes[0].kind == dui::SemanticsChangeKind::added &&
            initial.changes[0].entry.id == 1 && !initial.changes[0].entry.parent_id.has_value() &&
            initial.changes[1].entry.id == 2 && initial.changes[1].entry.parent_id == 1 &&
            initial.changes[1].entry.child_index == 0 && initial.changes[2].entry.id == 3,
          "initial semantics diff was not emitted in parent-first preorder");
  const auto* stable_entries = differ.entries().data();
  require(differ.update(first).empty() && differ.entries().data() == stable_entries,
          "equivalent semantics snapshot emitted changes or replaced retained storage");

  auto moved_child = node(2, "child");
  moved_child.role = dui::SemanticsRole::text;
  moved_child.value = "changed value";
  moved_child.enabled = false;
  moved_child.bounds = {{20.0, 30.0}, {40.0, 50.0}};
  moved_child.actions.push_back(dui::SemanticsAction::activate);
  auto added_child = node(5, "added child");
  auto new_parent = node(4, "new parent", {moved_child, added_child});
  auto updated_root = node(1, "updated root");
  updated_root.actions.push_back(dui::SemanticsAction::activate);
  updated_root.actions.push_back(dui::SemanticsAction::focus);
  updated_root.focusable = true;
  updated_root.focused = true;
  const dui::SemanticsTree second{{new_parent, updated_root}};
  const auto delta = differ.update(second);
  const std::array expected_ids{std::uint64_t{4}, std::uint64_t{5}, std::uint64_t{2},
                                std::uint64_t{1}, std::uint64_t{3}};
  const std::array expected_kinds{dui::SemanticsChangeKind::added, dui::SemanticsChangeKind::added,
                                  dui::SemanticsChangeKind::updated,
                                  dui::SemanticsChangeKind::updated,
                                  dui::SemanticsChangeKind::removed};
  require(delta.changes.size() == expected_ids.size(),
          "semantic add/update/remove delta had the wrong size");
  for (std::size_t index = 0; index < expected_ids.size(); ++index) {
    require(delta.changes[index].entry.id == expected_ids[index] &&
              delta.changes[index].kind == expected_kinds[index],
            "semantic delta ordering was not deterministic");
  }
  require(delta.changes[2].entry.parent_id == 4 && delta.changes[2].entry.child_index == 0 &&
            delta.changes[2].entry.role == dui::SemanticsRole::text &&
            delta.changes[2].entry.value == "changed value" && !delta.changes[2].entry.enabled &&
            delta.changes[2].entry.bounds == dui::Rect{{20.0, 30.0}, {40.0, 50.0}} &&
            delta.changes[2].entry.supports(dui::SemanticsAction::activate) &&
            delta.changes[3].entry.child_index == 1 &&
            delta.changes[3].entry.supports(dui::SemanticsAction::activate) &&
            delta.changes[3].entry.supports(dui::SemanticsAction::focus) &&
            delta.changes[3].entry.focusable && delta.changes[3].entry.focused &&
            delta.changes[4].entry.label == "removed",
          "semantic move, focus, reorder, action update, or removal metadata was incorrect");

  const dui::SemanticsTree duplicate{{node(6, "first"), node(6, "duplicate")}};
  bool duplicate_rejected = false;
  try {
    static_cast<void>(differ.update(duplicate));
  } catch (const std::logic_error&) {
    duplicate_rejected = true;
  }
  const dui::SemanticsTree zero{{node(0, "zero")}};
  bool zero_rejected = false;
  try {
    static_cast<void>(differ.update(zero));
  } catch (const std::logic_error&) {
    zero_rejected = true;
  }
  auto invalid_focus = node(7, "invalid focus");
  invalid_focus.focused = true;
  bool invalid_focus_rejected = false;
  try {
    static_cast<void>(differ.update({{invalid_focus}}));
  } catch (const std::logic_error&) {
    invalid_focus_rejected = true;
  }
  auto first_focus = node(8, "first focus");
  first_focus.focusable = true;
  first_focus.focused = true;
  first_focus.actions.push_back(dui::SemanticsAction::focus);
  auto second_focus = node(9, "second focus");
  second_focus.focusable = true;
  second_focus.focused = true;
  second_focus.actions.push_back(dui::SemanticsAction::focus);
  bool duplicate_focus_rejected = false;
  try {
    static_cast<void>(differ.update({{first_focus, second_focus}}));
  } catch (const std::logic_error&) {
    duplicate_focus_rejected = true;
  }
  require(duplicate_rejected && zero_rejected && invalid_focus_rejected &&
            duplicate_focus_rejected && differ.entries().size() == 4 &&
            differ.entries()[0].id == 4 && differ.entries()[3].id == 1,
          "malformed semantics update replaced the retained snapshot");

  const auto cleared = differ.clear();
  const std::array clear_ids{std::uint64_t{1}, std::uint64_t{5}, std::uint64_t{2},
                             std::uint64_t{4}};
  require(cleared.changes.size() == clear_ids.size(),
          "semantics clear emitted the wrong removal count");
  for (std::size_t index = 0; index < clear_ids.size(); ++index) {
    require(cleared.changes[index].kind == dui::SemanticsChangeKind::removed &&
              cleared.changes[index].entry.id == clear_ids[index],
            "semantics clear did not remove children before parents");
  }
  require(differ.clear().empty() && differ.entries().empty(),
          "clearing an empty semantics differ emitted changes");

  dui::SemanticsDiffer removal_differ;
  const dui::SemanticsTree subtree{{node(10, "parent", {node(11, "middle", {node(12, "leaf")})})}};
  static_cast<void>(removal_differ.update(subtree));
  const auto subtree_removed = removal_differ.update({});
  require(subtree_removed.changes.size() == 3 &&
            subtree_removed.changes[0].kind == dui::SemanticsChangeKind::removed &&
            subtree_removed.changes[0].entry.id == 12 &&
            subtree_removed.changes[0].entry.label == "leaf" &&
            subtree_removed.changes[1].kind == dui::SemanticsChangeKind::removed &&
            subtree_removed.changes[1].entry.id == 11 &&
            subtree_removed.changes[2].kind == dui::SemanticsChangeKind::removed &&
            subtree_removed.changes[2].entry.id == 10,
          "snapshot update did not remove a nested subtree child-first");

  dui::BuildOwner owner;
  owner.render(dui::Text{"generated", [] {}});
  [[maybe_unused]] const auto generated_frame =
    owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto generated = differ.update(owner.semantics_tree());
  require(generated.changes.size() == 1 &&
            generated.changes.front().kind == dui::SemanticsChangeKind::added &&
            generated.changes.front().entry.label == "generated",
          "SemanticsDiffer rejected a BuildOwner snapshot");
  owner.render(dui::Text{"updated", [] {}});
  const auto generated_update = differ.update(owner.semantics_tree());
  require(generated_update.changes.size() == 1 &&
            generated_update.changes.front().kind == dui::SemanticsChangeKind::updated &&
            generated_update.changes.front().entry.label == "updated",
          "SemanticsDiffer did not update a stable BuildOwner node");
  owner.render(dui::Text{"updated", [] {}});
  require(differ.update(owner.semantics_tree()).empty(),
          "callback replacement with unchanged actions emitted a semantics update");
}

void accessibility_bridge_acknowledges_successful_delivery() {
  dui::AccessibilityBridge bridge;
  RecordingAccessibilityAdapter adapter;
  const dui::SemanticsTree first{{node(20, "first", {node(21, "child")})}};
  require(bridge.publish(first, adapter) && adapter.deliveries.size() == 1 &&
            adapter.deliveries.front().changes.size() == 2 && bridge.entries().size() == 2,
          "AccessibilityBridge did not publish and acknowledge its first snapshot");
  require(!bridge.publish(first, adapter) && adapter.deliveries.size() == 1,
          "AccessibilityBridge delivered an equivalent snapshot");

  const std::size_t deliveries_before_invalid = adapter.deliveries.size();
  bool invalid_rejected = false;
  try {
    static_cast<void>(bridge.publish({{node(22, "first"), node(22, "duplicate")}}, adapter));
  } catch (const std::logic_error&) {
    invalid_rejected = true;
  }
  require(invalid_rejected && adapter.deliveries.size() == deliveries_before_invalid &&
            bridge.entries().front().label == "first",
          "malformed bridge publication called the adapter or changed acknowledgment");

  const dui::SemanticsTree second{{node(20, "updated", {node(21, "child")})}};
  bool adapter_saw_acknowledged_snapshot = false;
  adapter.on_apply = [&] {
    adapter_saw_acknowledged_snapshot =
      bridge.entries().size() == 2 && bridge.entries().front().label == "first";
  };
  adapter.fail_next = true;
  bool publish_failed = false;
  try {
    static_cast<void>(bridge.publish(second, adapter));
  } catch (const std::runtime_error&) {
    publish_failed = true;
  }
  adapter.on_apply = {};
  require(publish_failed && adapter_saw_acknowledged_snapshot && bridge.entries().size() == 2 &&
            bridge.entries().front().label == "first",
          "failed accessibility delivery advanced the acknowledged snapshot");
  const auto failed_publish = adapter.deliveries.back();
  require(bridge.publish(second, adapter) && adapter.deliveries.back() == failed_publish &&
            bridge.entries().front().label == "updated",
          "accessibility delivery retry did not reproduce and acknowledge the same delta");

  adapter.fail_next = true;
  bool clear_failed = false;
  try {
    static_cast<void>(bridge.clear(adapter));
  } catch (const std::runtime_error&) {
    clear_failed = true;
  }
  require(clear_failed && bridge.entries().size() == 2,
          "failed accessibility clear discarded the acknowledged snapshot");
  const auto failed_clear = adapter.deliveries.back();
  require(bridge.clear(adapter) && adapter.deliveries.back() == failed_clear &&
            bridge.entries().empty() && !bridge.clear(adapter),
          "accessibility clear retry was not deterministic or did not acknowledge removal");

  adapter.reentrant_bridge = &bridge;
  adapter.reentrant_tree = &first;
  require(bridge.publish(first, adapter) && adapter.publish_rejected && adapter.clear_rejected &&
            bridge.entries().size() == 2,
          "AccessibilityBridge accepted reentrant publish or clear during delivery");
  adapter.reentrant_bridge = nullptr;
  adapter.reentrant_tree = nullptr;

  dui::BuildOwner owner;
  owner.render(dui::Text{"generated bridge"});
  [[maybe_unused]] const auto frame = owner.frame(dui::BoxConstraints::tight({200.0, 16.0}));
  dui::AccessibilityBridge generated_bridge;
  RecordingAccessibilityAdapter generated_adapter;
  require(generated_bridge.publish(owner.semantics_tree(), generated_adapter) &&
            generated_bridge.entries().size() == 1 &&
            generated_bridge.entries().front().label == "generated bridge",
          "AccessibilityBridge rejected a BuildOwner snapshot");

  auto destroyed_publish_bridge = std::make_unique<dui::AccessibilityBridge>();
  RecordingAccessibilityAdapter destroyed_publish_adapter;
  destroyed_publish_adapter.on_apply = [&] { destroyed_publish_bridge.reset(); };
  const bool destroyed_publish_delivered =
    destroyed_publish_bridge->publish(first, destroyed_publish_adapter);
  require(destroyed_publish_delivered && destroyed_publish_bridge == nullptr,
          "adapter could not destroy its bridge during successful publish");

  auto destroyed_clear_bridge = std::make_unique<dui::AccessibilityBridge>();
  RecordingAccessibilityAdapter destroyed_clear_adapter;
  require(destroyed_clear_bridge->publish(first, destroyed_clear_adapter),
          "bridge destruction clear test could not publish its initial tree");
  destroyed_clear_adapter.on_apply = [&] { destroyed_clear_bridge.reset(); };
  const bool destroyed_clear_delivered = destroyed_clear_bridge->clear(destroyed_clear_adapter);
  require(destroyed_clear_delivered && destroyed_clear_bridge == nullptr,
          "adapter could not destroy its bridge during successful clear");

  auto destroyed_failure_bridge = std::make_unique<dui::AccessibilityBridge>();
  RecordingAccessibilityAdapter destroyed_failure_adapter;
  destroyed_failure_adapter.on_apply = [&] { destroyed_failure_bridge.reset(); };
  destroyed_failure_adapter.fail_next = true;
  bool destroyed_failure_propagated = false;
  try {
    static_cast<void>(destroyed_failure_bridge->publish(first, destroyed_failure_adapter));
  } catch (const std::runtime_error&) {
    destroyed_failure_propagated = true;
  }
  require(destroyed_failure_propagated && destroyed_failure_bridge == nullptr,
          "bridge destruction during failed delivery caused unsafe exception handling");

  auto destroyed_clear_failure_bridge = std::make_unique<dui::AccessibilityBridge>();
  RecordingAccessibilityAdapter destroyed_clear_failure_adapter;
  require(destroyed_clear_failure_bridge->publish(first, destroyed_clear_failure_adapter),
          "failed-clear destruction test could not publish its initial tree");
  destroyed_clear_failure_adapter.on_apply = [&] { destroyed_clear_failure_bridge.reset(); };
  destroyed_clear_failure_adapter.fail_next = true;
  bool destroyed_clear_failure_propagated = false;
  try {
    static_cast<void>(destroyed_clear_failure_bridge->clear(destroyed_clear_failure_adapter));
  } catch (const std::runtime_error&) {
    destroyed_clear_failure_propagated = true;
  }
  require(destroyed_clear_failure_propagated && destroyed_clear_failure_bridge == nullptr,
          "bridge destruction during failed clear caused unsafe exception handling");
}

void accessibility_bridge_supports_owned_async_transactions() {
  dui::AccessibilityBridge bridge;
  dui::SemanticsTree first{{node(30, "first", {node(31, "child")})}};
  const auto initial = bridge.prepare(first);
  require(initial.has_value() && initial->valid() && initial->update.changes.size() == 2 &&
            bridge.entries().empty(),
          "async prepare did not retain acknowledgment or produce an owned initial delta");
  first.roots.front().label = "mutated source";
  require(initial->update.changes.front().entry.label == "first",
          "prepared accessibility publication retained the source tree");

  RecordingAccessibilityAdapter adapter;
  bool second_prepare_rejected = false;
  bool synchronous_publish_rejected = false;
  try {
    static_cast<void>(bridge.prepare({{node(32, "second")}}));
  } catch (const std::logic_error&) {
    second_prepare_rejected = true;
  }
  try {
    static_cast<void>(bridge.publish({{node(32, "second")}}, adapter));
  } catch (const std::logic_error&) {
    synchronous_publish_rejected = true;
  }
  require(second_prepare_rejected && synchronous_publish_rejected &&
            !bridge.acknowledge(initial->generation + 1),
          "bridge mixed a pending async transaction or accepted a stale acknowledgment");

  require(bridge.reject(initial->generation) && !bridge.reject(initial->generation) &&
            bridge.entries().empty(),
          "async rejection advanced acknowledgment or accepted a duplicate result");
  const auto retry = bridge.retry();
  require(retry.has_value() && retry->generation > initial->generation &&
            retry->update == initial->update && !bridge.acknowledge(initial->generation) &&
            bridge.acknowledge(retry->generation) && bridge.entries().size() == 2 &&
            bridge.entries().front().label == "first",
          "async retry changed its delta or accepted the wrong publication generation");
  require(!bridge.prepare({{node(30, "first", {node(31, "child")})}}).has_value(),
          "async prepare emitted an unchanged acknowledged tree");

  const auto clear = bridge.prepare_clear();
  require(clear.has_value() && clear->update.changes.size() == 2 &&
            clear->update.changes.front().entry.id == 31 && bridge.reject(clear->generation),
          "async clear was not child-first or could not be rejected");
  const auto clear_retry = bridge.retry();
  require(clear_retry.has_value() && clear_retry->generation > clear->generation &&
            clear_retry->update == clear->update && bridge.acknowledge(clear_retry->generation) &&
            bridge.entries().empty(),
          "async clear retry did not commit the retained removal batch");

  const auto reset_candidate = bridge.prepare({{node(40, "reset")}});
  require(reset_candidate.has_value(), "reset test could not prepare a publication");
  bridge.reset_acknowledged();
  require(!bridge.acknowledge(reset_candidate->generation) && bridge.entries().empty(),
          "reset did not invalidate an in-flight publication");

  require(bridge.acknowledge(0) == false && bridge.reject(0) == false &&
            !bridge.retry().has_value(),
          "empty async bridge accepted a result or exposed a retry");
  bool malformed_rejected = false;
  try {
    static_cast<void>(bridge.prepare({{node(41, "duplicate"), node(41, "duplicate")}}));
  } catch (const std::logic_error&) {
    malformed_rejected = true;
  }
  require(malformed_rejected && bridge.entries().empty() && !bridge.retry().has_value(),
          "malformed async publication changed acknowledged or retry state");
}

} // namespace

int main() {
  try {
    semantics_tree_preserves_hierarchy_identity_and_actions();
    primitive_views_produce_semantics_and_actions();
    semantic_focus_transfers_between_stable_nodes();
    semantics_tree_clips_lazy_visible_children();
    render_owner_rejects_semantics_without_current_layout();
    semantics_differ_emits_deterministic_transactional_updates();
    accessibility_bridge_acknowledges_successful_delivery();
    accessibility_bridge_supports_owned_async_transactions();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "All DUI semantics tests passed\n";
  return EXIT_SUCCESS;
}
