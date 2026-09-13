#include "dui/ui.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

const dui::Element& only_child(const dui::Element& element) {
  require(element.children().size() == 1, "expected exactly one child");
  require(element.children().front() != nullptr, "expected a mounted child");
  return *element.children().front();
}

struct Counter {
  std::optional<dui::StateHandle<int>>* output;

  auto build(dui::BuildContext& context) const {
    auto count = context.state<"count">(0);
    *output = count;
    return dui::Text{"count=" + std::to_string(count.get())};
  }
};

void state_survives_and_batches() {
  dui::BuildOwner owner;
  std::optional<dui::StateHandle<int>> state;

  owner.render(Counter{&state});
  const auto root_id = owner.root()->id();
  require(only_child(*owner.root()).debug_value() == "count=0", "initial state was not built");

  state->set(1);
  state->set(2);
  require(owner.pending_build_count() == 1, "state writes were not coalesced");
  owner.flush();

  require(owner.root()->id() == root_id, "component identity changed during state rebuild");
  require(only_child(*owner.root()).debug_value() == "count=2",
          "state rebuild produced stale output");

  owner.render(Counter{&state});
  require(owner.root()->id() == root_id, "same component type was not reused");
  require(state->get() == 2, "named state did not survive parent-driven rebuild");
}

void replacing_type_unmounts_element() {
  dui::BuildOwner owner;
  owner.render(dui::Text{"leaf"});
  const auto old_id = owner.root()->id();

  owner.render(dui::VStack{dui::Text{"child"}});
  require(owner.root()->id() != old_id, "different view type reused an Element");
  require(owner.unmount_count() == 1, "replaced Element was not unmounted");
}

struct ConditionalComponent {
  bool show;
  bool left;

  auto build(dui::BuildContext&) const {
    return dui::VStack{
      dui::optional(show, [] { return dui::Text{"optional"}; }),
      dui::choose(
        left, [] { return dui::Text{"left"}; }, [] { return dui::VStack{dui::Text{"right"}}; })};
  }
};

void optional_and_choice_reconcile() {
  dui::BuildOwner owner;
  owner.render(ConditionalComponent{true, true});

  const dui::Element& group = only_child(*owner.root());
  const auto optional_child_id = only_child(*group.children()[0]).id();
  const auto left_id = only_child(*group.children()[1]).id();

  owner.render(ConditionalComponent{false, false});
  const dui::Element& updated_group = only_child(*owner.root());
  require(updated_group.children()[0]->children().empty(), "optional child remained mounted");
  require(only_child(*updated_group.children()[1]).id() != left_id,
          "choice branch type was not replaced");
  require(owner.unmount_count() >= 2, "conditional removals were not unmounted");

  owner.render(ConditionalComponent{true, false});
  const dui::Element& restored_group = only_child(*owner.root());
  require(only_child(*restored_group.children()[0]).id() != optional_child_id,
          "removed optional child retained identity");
}

struct SameTypeChoice {
  bool first;

  auto build(dui::BuildContext&) const {
    return dui::VStack{
      dui::choose(
        first, [] { return dui::Text{"first"}; }, [] { return dui::Text{"second"}; }),
      dui::Text{"stable sibling"}};
  }
};

void choice_branch_has_distinct_identity() {
  dui::BuildOwner owner;
  owner.render(SameTypeChoice{true});

  const dui::Element& initial_group = only_child(*owner.root());
  const auto choice_id = initial_group.children()[0]->id();
  const auto branch_id = only_child(*initial_group.children()[0]).id();
  const auto sibling_id = initial_group.children()[1]->id();
  const auto unmounts = owner.unmount_count();

  owner.render(SameTypeChoice{false});
  const dui::Element& updated_group = only_child(*owner.root());
  require(updated_group.children()[0]->id() == choice_id, "Choice wrapper lost identity");
  require(only_child(*updated_group.children()[0]).id() != branch_id,
          "same-type Choice branch retained identity");
  require(updated_group.children()[1]->id() == sibling_id,
          "Choice switch replaced an unaffected sibling");
  require(owner.unmount_count() == unmounts + 1,
          "Choice switch unmounted more than the active branch");
}

struct Item {
  int id;
  std::string label;
};

struct ItemList {
  std::vector<Item> items;

  auto build(dui::BuildContext&) const {
    return dui::ForEach{items, dui::key<&Item::id>,
                        [](const Item& item) { return dui::Text{item.label}; }};
  }
};

std::unordered_map<int, dui::Element::Id> item_ids(const dui::Element& for_each) {
  std::unordered_map<int, dui::Element::Id> result;
  for (const auto& child : for_each.children()) {
    result.emplace(std::stoi(child->key().to_string()), child->id());
  }
  return result;
}

void keyed_reorder_preserves_identity() {
  dui::BuildOwner owner;
  owner.render(ItemList{{{1, "one"}, {2, "two"}, {3, "three"}}});
  const auto before = item_ids(only_child(*owner.root()));

  owner.render(ItemList{{{3, "THREE"}, {1, "ONE"}, {2, "TWO"}}});
  const dui::Element& list = only_child(*owner.root());
  const auto after = item_ids(list);

  require(before == after, "keyed reorder changed item Element identity");
  require(list.children()[0]->debug_value() == "THREE", "reordered item was not updated");
  require(list.children()[1]->key().to_string() == "1", "new keyed order was not applied");
}

struct StatefulItem {
  Item item;
  std::unordered_map<int, dui::StateHandle<int>>* states;

  auto build(dui::BuildContext& context) const {
    auto value = context.state<"value">(item.id * 10);
    states->insert_or_assign(item.id, value);
    return dui::Text{item.label + "=" + std::to_string(value.get())};
  }
};

struct StatefulItemList {
  std::vector<Item> items;
  std::unordered_map<int, dui::StateHandle<int>>* states;

  auto build(dui::BuildContext&) const {
    return dui::ForEach{items, dui::key<&Item::id>, [states = states](const Item& item) {
                          return StatefulItem{item, states};
                        }};
  }
};

void keyed_component_state_follows_key() {
  dui::BuildOwner owner;
  std::unordered_map<int, dui::StateHandle<int>> states;
  owner.render(StatefulItemList{{{1, "one"}, {2, "two"}}, &states});

  const dui::Element& initial_list = only_child(*owner.root());
  const auto ids = item_ids(initial_list);
  states.at(2).set(99);
  owner.flush();

  owner.render(StatefulItemList{{{2, "TWO"}, {1, "ONE"}}, &states});
  const dui::Element& reordered = only_child(*owner.root());
  require(item_ids(reordered) == ids, "keyed stateful component identity changed during reorder");
  require(only_child(*reordered.children()[0]).debug_value() == "TWO=99",
          "state did not follow its item key");
  require(only_child(*reordered.children()[1]).debug_value() == "ONE=10",
          "another item's state was corrupted");
}

struct ObservedLabel {
  dui::Signal<std::string>* value;

  auto build(dui::BuildContext&) const { return dui::Text{value->get()}; }
};

struct StaticLabel {
  auto build(dui::BuildContext&) const { return dui::Text{"static"}; }
};

void observable_invalidates_only_readers() {
  dui::Signal<std::string> value{"before"};
  dui::BuildOwner owner;
  owner.render(dui::VStack{ObservedLabel{&value}, StaticLabel{}});

  const auto& root = *owner.root();
  const auto observed_updates = root.children()[0]->children()[0]->update_count();
  const auto static_updates = root.children()[1]->children()[0]->update_count();

  value.set("after");
  require(owner.pending_build_count() == 1, "observable did not target exactly one component");
  owner.flush();

  require(root.children()[0]->children()[0]->debug_value() == "after",
          "observed component did not rebuild");
  require(root.children()[0]->children()[0]->update_count() == observed_updates + 1,
          "reader update count is incorrect");
  require(root.children()[1]->children()[0]->update_count() == static_updates,
          "unrelated component rebuilt");
}

struct TrackedCounter {
  std::optional<dui::StateHandle<int>>* state;
  int* builds;

  auto build(dui::BuildContext& context) const {
    ++*builds;
    auto count = context.state<"count">(0);
    *state = count;
    return dui::Text{std::to_string(count.get())};
  }
};

struct ObservedParent {
  dui::Signal<int>* signal;
  std::optional<dui::StateHandle<int>>* child_state;
  int* child_builds;

  auto build(dui::BuildContext&) const {
    static_cast<void>(signal->get());
    return TrackedCounter{child_state, child_builds};
  }
};

void parent_rebuild_coalesces_dirty_child() {
  dui::Signal<int> signal{0};
  dui::BuildOwner owner;
  std::optional<dui::StateHandle<int>> child_state;
  int child_builds = 0;

  owner.render(ObservedParent{&signal, &child_state, &child_builds});
  child_state->set(1);
  signal.set(1);
  require(owner.pending_build_count() == 2, "parent and child were not both scheduled");

  owner.flush();
  require(child_builds == 2, "dirty child rebuilt again after its parent reconciled it");
}

void observable_can_die_before_owner() {
  dui::BuildOwner owner;
  {
    dui::Signal<std::string> value{"temporary"};
    owner.render(ObservedLabel{&value});
  }

  owner.render(dui::Text{"safe replacement"});
  require(owner.root()->debug_value() == "safe replacement",
          "destroyed observable left a dangling dependency");
}

struct ConditionalObserver {
  bool use_first;
  dui::Signal<int>* first;
  dui::Signal<int>* second;

  auto build(dui::BuildContext&) const {
    const int value = use_first ? first->get() : second->get();
    return dui::Text{std::to_string(value)};
  }
};

void conditional_observable_dependencies_are_replaced() {
  dui::Signal<int> first{1};
  dui::Signal<int> second{2};
  dui::BuildOwner owner;

  owner.render(ConditionalObserver{true, &first, &second});
  owner.render(ConditionalObserver{false, &first, &second});

  first.set(10);
  require(owner.pending_build_count() == 0,
          "component remained subscribed to an unread observable");
  second.set(20);
  require(owner.pending_build_count() == 1, "component did not subscribe to its new observable");
  owner.flush();
  require(only_child(*owner.root()).debug_value() == "20",
          "conditional observable rebuild was stale");
}

struct Theme {
  std::string name;

  friend bool operator==(const Theme&, const Theme&) = default;
};

struct ThemeReader {
  int* builds;

  auto build(dui::BuildContext& context) const {
    ++*builds;
    return dui::Text{context.environment<Theme>().name};
  }

  friend bool operator==(const ThemeReader&, const ThemeReader&) = default;
};

struct EnvironmentIndependent {
  int* builds;

  auto build(dui::BuildContext&) const {
    ++*builds;
    return dui::Text{"independent"};
  }

  friend bool operator==(const EnvironmentIndependent&, const EnvironmentIndependent&) = default;
};

struct ThemeProvider {
  std::optional<dui::StateHandle<std::string>>* theme_state;
  int* reader_builds;
  int* independent_builds;

  auto build(dui::BuildContext& context) const {
    auto theme = context.state<"theme">(std::string{"light"});
    *theme_state = theme;
    return dui::with_environment(
      dui::VStack{ThemeReader{reader_builds}, EnvironmentIndependent{independent_builds}},
      Theme{theme.get()});
  }
};

void environment_invalidates_only_readers() {
  dui::BuildOwner owner;
  std::optional<dui::StateHandle<std::string>> theme;
  int reader_builds = 0;
  int independent_builds = 0;
  owner.render(ThemeProvider{&theme, &reader_builds, &independent_builds});

  require(reader_builds == 1 && independent_builds == 1,
          "environment subtree did not initially build once");
  theme->set("dark");
  owner.flush();

  require(reader_builds == 2, "Environment reader was not invalidated");
  require(independent_builds == 1, "Environment update rebuilt an equal non-reader component");
  const dui::Element& environment = only_child(*owner.root());
  const dui::Element& stack = only_child(environment);
  require(only_child(*stack.children()[0]).debug_value() == "dark",
          "Environment reader saw a stale value");
}

void nearest_environment_scope_wins() {
  dui::BuildOwner owner;
  int builds = 0;
  owner.render(dui::with_environment(dui::with_environment(ThemeReader{&builds}, Theme{"inner"}),
                                     Theme{"outer"}));

  const dui::Element& outer = *owner.root();
  const dui::Element& inner = only_child(outer);
  const dui::Element& reader = only_child(inner);
  require(only_child(reader).debug_value() == "inner",
          "Environment lookup ignored nearest provider");
}

void missing_environment_is_rejected() {
  dui::BuildOwner owner;
  int builds = 0;
  bool rejected = false;
  try {
    owner.render(ThemeReader{&builds});
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "missing Environment value was silently accepted");
}

struct EqualComponentWithIgnoredConfig {
  dui::Signal<int>* invalidation;
  int config;
  int* observed_config;

  auto build(dui::BuildContext&) const {
    static_cast<void>(invalidation->get());
    *observed_config = config;
    return dui::Text{std::to_string(config)};
  }

  friend bool operator==(const EqualComponentWithIgnoredConfig& left,
                         const EqualComponentWithIgnoredConfig& right) {
    return left.invalidation == right.invalidation && left.observed_config == right.observed_config;
  }
};

void skipped_equal_component_keeps_latest_descriptor() {
  dui::Signal<int> invalidation{0};
  dui::BuildOwner owner;
  int observed = 0;
  owner.render(EqualComponentWithIgnoredConfig{&invalidation, 1, &observed});
  owner.render(EqualComponentWithIgnoredConfig{&invalidation, 2, &observed});
  require(observed == 1, "equal component unexpectedly rebuilt during parent update");

  invalidation.set(1);
  owner.flush();
  require(observed == 2, "dirty rebuild used a stale equal component descriptor");
}

struct WriteDuringBuild {
  bool write;
  std::optional<dui::StateHandle<int>>* state;
  int* builds;

  auto build(dui::BuildContext& context) const {
    ++*builds;
    auto value = context.state<"value">(0);
    *state = value;
    if (write) {
      value.set(1);
    }
    return dui::Text{std::to_string(value.get())};
  }
};

void state_write_during_build_remains_scheduled() {
  dui::BuildOwner owner;
  std::optional<dui::StateHandle<int>> state;
  int builds = 0;
  owner.render(WriteDuringBuild{false, &state, &builds});
  owner.render(WriteDuringBuild{true, &state, &builds});

  require(owner.pending_build_count() == 1, "state write during build was discarded");
  owner.flush();
  require(builds == 3, "state write during build did not schedule exactly one follow-up build");
  require(state->get() == 1, "state write during build did not commit");
}

struct ReentrantBuild {
  dui::BuildOwner* owner;
  bool* rejected;

  auto build(dui::BuildContext&) const {
    try {
      owner->render(dui::Text{"nested"});
    } catch (const std::logic_error&) {
      *rejected = true;
    }
    return dui::Text{"outer"};
  }
};

void reentrant_root_render_is_rejected() {
  dui::BuildOwner owner;
  bool rejected = false;
  owner.render(ReentrantBuild{&owner, &rejected});
  require(rejected, "reentrant BuildOwner::render was accepted");
  require(only_child(*owner.root()).debug_value() == "outer",
          "reentrant render corrupted the outer build");
}

void stale_state_handle_is_rejected() {
  dui::BuildOwner owner;
  std::optional<dui::StateHandle<int>> state;
  owner.render(Counter{&state});
  owner.render(dui::Text{"replacement"});

  bool rejected = false;
  try {
    static_cast<void>(state->get());
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "state handle remained usable after unmount");
}

void state_handle_rejects_destroyed_owner() {
  std::optional<dui::StateHandle<int>> state;
  {
    dui::BuildOwner owner;
    owner.render(Counter{&state});
  }

  bool rejected = false;
  try {
    static_cast<void>(state->get());
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "state handle dereferenced a destroyed BuildOwner");
}

void tree_dump_is_deterministic() {
  dui::BuildOwner owner;
  owner.render(dui::VStack{dui::Text{"alpha"}, dui::Optional<dui::Text>{dui::Text{"beta"}}});

  const std::string expected = "VStack#1 updates=1\n"
                               "  Text#2 value=\"alpha\" updates=1\n"
                               "  Optional#3 updates=1\n"
                               "    Text#4 value=\"beta\" updates=1\n";
  require(owner.dump_tree() == expected, "tree inspection output is not deterministic");
}

struct InspectorComponent {
  dui::Signal<int>* signal;

  auto build(dui::BuildContext& context) const {
    auto state = context.state<"inspector-state">(2);
    auto& resource = context.resource<"inspector-resource">([] { return 3; });
    const int total = state.get() + signal->get() + resource;
    return dui::FocusView<dui::Text>{
      dui::Text{context.environment<std::string>() + "=\"" + std::to_string(total) + "\"\\\n"},
      {},
      true,
      true};
  }
};

struct InspectDuringBuild {
  dui::BuildOwner* owner;
  bool* rejected;

  auto build(dui::BuildContext&) const {
    try {
      static_cast<void>(owner->inspect());
    } catch (const std::logic_error&) {
      *rejected = true;
    }
    return dui::Text{"safe"};
  }
};

struct InspectorStateValuesComponent {
  std::optional<dui::StateHandle<int>>* handle;
  bool expose;
  std::string prefix;
  dui::BuildOwner* owner;
  bool* reentrant_rejected;

  auto build(dui::BuildContext& context) const {
    auto count =
      expose
        ? context.state<"z-count">(2,
                                   [prefix = prefix, owner = owner, handle = handle,
                                    reentrant_rejected = reentrant_rejected](const int& value) {
                                     if (value == 13) {
                                       throw std::runtime_error("inspection failed");
                                     }
                                     if (value == 99) {
                                       try {
                                         owner->render(dui::Text{"replacement"});
                                       } catch (const std::logic_error&) {
                                         *reentrant_rejected = true;
                                         throw;
                                       }
                                     }
                                     if (prefix == "mutate:" && value == 14) {
                                       try {
                                         handle->value().set(15);
                                       } catch (const std::logic_error&) {
                                         *reentrant_rejected = true;
                                         throw;
                                       }
                                     }
                                     return prefix + std::to_string(value);
                                   })
        : context.state<"z-count">(2);
    auto label = expose ? context.state<"a-label">(
                            std::string{"safe"},
                            [](const std::string& value) { return "label=\"" + value + "\"\n"; })
                        : context.state<"a-label">(std::string{"safe"});
    static_cast<void>(context.state<"private">(99));
    *handle = count;
    return dui::Text{label.get() + std::to_string(count.get())};
  }
};

struct InspectorFormatterLifecycleControl {
  std::optional<dui::StateHandle<int>> handle;
  dui::BuildOwner* owner{};
  bool attack_copy{};
  bool attack_destruction{};
  bool attack_render{};
  bool copy_rejected{};
  bool destruction_rejected{};
};

struct ReentrantInspectorFormatter {
  explicit ReentrantInspectorFormatter(std::shared_ptr<InspectorFormatterLifecycleControl> control)
    : control(std::move(control)) {}

  ReentrantInspectorFormatter(const ReentrantInspectorFormatter& other) : control(other.control) {
    attack(control->attack_copy, control->copy_rejected);
  }
  ReentrantInspectorFormatter(ReentrantInspectorFormatter&&) noexcept = default;
  ~ReentrantInspectorFormatter() {
    if (control != nullptr) {
      attack(control->attack_destruction, control->destruction_rejected);
    }
  }

  std::string operator()(const int& value) const { return std::to_string(value); }

  std::shared_ptr<InspectorFormatterLifecycleControl> control;

private:
  void attack(bool enabled, bool& rejected) const noexcept {
    if (!enabled) {
      return;
    }
    try {
      if (control->attack_render) {
        control->owner->render(dui::Text{"destructor replacement"});
      } else if (control->handle.has_value()) {
        control->handle->set(77);
      }
    } catch (const std::logic_error&) {
      rejected = true;
    } catch (...) {
    }
  }
};

struct InspectorFormatterLifecycleComponent {
  std::shared_ptr<InspectorFormatterLifecycleControl> control;
  bool expose;

  auto build(dui::BuildContext& context) const {
    auto state = [&] {
      if (!expose) {
        return context.state<"lifecycle">(5);
      }
      const ReentrantInspectorFormatter formatter{control};
      return context.state<"lifecycle">(5, formatter);
    }();
    control->handle = state;
    return dui::Text{std::to_string(state.get())};
  }
};

struct InspectorContextReentrantComponent {
  bool* rejected;

  auto build(dui::BuildContext& context) const {
    auto state = context.state<"value">(1, [context = &context, rejected = rejected](const int&) {
      try {
        static_cast<void>(context->state<"nested">(2));
      } catch (const std::logic_error&) {
        *rejected = true;
        throw;
      }
      return std::string{"unreachable"};
    });
    return dui::Text{std::to_string(state.get())};
  }
};

void structured_inspector_is_detached_and_deterministic() {
  dui::BuildOwner owner;
  const auto empty = owner.inspect();
  require(!empty.root.has_value() && empty.mount_count == 0 && empty.find(1) == nullptr &&
            empty.to_json().contains("\"root\":null"),
          "empty BuildOwner inspection was not canonical");

  dui::Signal<int> signal{4};
  owner.render(dui::with_environment(InspectorComponent{&signal}, std::string{"value"}));
  const auto dirty = owner.inspect();
  require(dirty.root.has_value() && dirty.root->environment_count == 1 &&
            dirty.root->children.size() == 1 &&
            dirty.root->children.front().state_slot_count == 1 &&
            dirty.root->children.front().dependency_count == 2 &&
            dirty.root->children.front().resource_count == 1 &&
            dirty.root->children.front().children.size() == 1 &&
            dirty.root->children.front().children.front().has_focus_node &&
            dirty.root->children.front().children.front().focused &&
            dirty.root->children.front().children.front().render_object.has_value() &&
            dirty.root->children.front().children.front().render_object->protocol ==
              dui::InspectorRenderProtocol::box &&
            dirty.root->children.front().children.front().render_object->needs_layout &&
            dirty.pending_layout_count != 0,
          "dirty inspector snapshot omitted owner, focus, or RenderObject metadata");
  const auto text_id = dirty.root->children.front().children.front().children.front().id;
  require(dirty.find(text_id) != nullptr && dirty.find(text_id)->value == "value=\"9\"\\\n" &&
            dirty.find(999999) == nullptr,
          "inspector stable-ID lookup did not find the nested debug value");
  const std::string dirty_json = dirty.to_json();
  require(dirty_json == owner.inspect().to_json() && dirty_json.contains("value=\\\"9\\\"\\\\\\n"),
          "inspector JSON was nondeterministic or failed to escape debug text");
  require(!dirty_json.contains("\"stateValues\""),
          "ordinary state unexpectedly appeared in inspector JSON");

  [[maybe_unused]] const auto frame = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto framed = owner.inspect();
  const auto* framed_text = framed.find(text_id);
  require(framed_text != nullptr && framed_text->render_object.has_value() &&
            !framed_text->render_object->needs_layout && !framed_text->render_object->needs_paint &&
            framed_text->render_object->layout_count == 1 &&
            framed_text->render_object->paint_count == 1,
          "framed inspector snapshot did not report drained render state");

  const auto detached = framed;
  owner.render(dui::ForEach{std::vector<Item>{{7, "replacement"}}, dui::key<&Item::id>,
                            [](const Item& item) { return dui::Text{item.label}; }});
  const auto keyed = owner.inspect();
  require(keyed.root.has_value() && keyed.root->children.size() == 1 &&
            keyed.root->children.front().key == "7" && detached.find(text_id) != nullptr &&
            detached.find(text_id)->value == "value=\"9\"\\\n",
          "inspector key metadata or detached snapshot lifetime was incorrect");

  bool reentrant_rejected = false;
  owner.render(InspectDuringBuild{&owner, &reentrant_rejected});
  require(reentrant_rejected, "inspector exposed a partially reconciled tree during build");

  dui::InspectorSnapshot surviving_snapshot;
  dui::Element::Id surviving_id{};
  {
    dui::BuildOwner temporary_owner;
    temporary_owner.render(dui::Text{"survives owner"});
    surviving_id = temporary_owner.root()->id();
    surviving_snapshot = temporary_owner.inspect();
  }
  require(surviving_snapshot.find(surviving_id) != nullptr &&
            surviving_snapshot.find(surviving_id)->value == "survives owner",
          "inspector snapshot retained owner-dependent storage");

  dui::InspectorSnapshot encoded;
  encoded.root.emplace();
  encoded.root->id = 1;
  encoded.root->value =
    std::string{"\xe4\xb8\xad"} + static_cast<char>(0x80) + std::string{"\x01\b\f\r\t"};
  const std::string encoded_json = encoded.to_json();
  require(encoded_json.contains("\xe4\xb8\xad") && encoded_json.contains("\\ufffd") &&
            encoded_json.contains("\\u0001\\b\\f\\r\\t"),
          "inspector JSON did not preserve UTF-8 or normalize malformed/control bytes");

  dui::InspectorSnapshot deep;
  deep.root.emplace();
  deep.root->id = 1;
  dui::InspectorNode* cursor = &*deep.root;
  for (std::size_t depth = 1; depth <= dui::InspectorSnapshot::maximum_depth; ++depth) {
    cursor->children.emplace_back();
    cursor = &cursor->children.back();
    cursor->id = depth + 1;
  }
  require(deep.find(dui::InspectorSnapshot::maximum_depth + 1) != nullptr,
          "iterative inspector lookup failed on a deep snapshot");
  bool deep_json_rejected = false;
  try {
    static_cast<void>(deep.to_json());
  } catch (const std::length_error&) {
    deep_json_rejected = true;
  }
  require(deep_json_rejected, "inspector serialized beyond its safe maximum depth");

  std::optional<dui::StateHandle<int>> inspected_state;
  dui::BuildOwner state_owner;
  bool formatter_reentrant_rejected = false;
  state_owner.render(InspectorStateValuesComponent{&inspected_state, true, "first:", &state_owner,
                                                   &formatter_reentrant_rejected});
  const auto exposed = state_owner.inspect();
  require(exposed.root.has_value() && exposed.root->state_slot_count == 3 &&
            exposed.root->state_values.size() == 2 &&
            exposed.root->state_values[0] ==
              dui::InspectorStateSnapshot{"a-label", std::string{"label=\"safe\"\n"}} &&
            exposed.root->state_values[1] ==
              dui::InspectorStateSnapshot{"z-count", std::string{"first:2"}},
          "inspector did not expose only opted-in state in deterministic name order");
  const std::string exposed_json = exposed.to_json();
  require(exposed_json.contains("\"stateSlotCount\":3,\"stateValues\":[{\"name\":\"a-label\","
                                "\"value\":\"label=\\\"safe\\\"\\n\"},{\"name\":\"z-count\","
                                "\"value\":\"first:2\"}],\"dependencyCount\":0"),
          "inspector state values were not canonically encoded or escaped");

  inspected_state->set(7);
  const auto refreshed = state_owner.inspect();
  require(refreshed.root->state_values[1].value == std::optional<std::string>{"first:7"} &&
            exposed.root->state_values[1].value == std::optional<std::string>{"first:2"},
          "state inspection did not refresh immediately or mutated a detached snapshot");
  inspected_state->set(13);
  const auto failed = state_owner.inspect();
  require(inspected_state->get() == 13 && state_owner.pending_build_count() == 1 &&
            !failed.root->state_values[1].value.has_value() &&
            failed.to_json().contains("{\"name\":\"z-count\",\"value\":null}"),
          "state inspection failure changed state behavior or was not encoded as null");
  inspected_state->update([](int value) { return value + 1; });
  require(state_owner.inspect().root->state_values[1].value ==
            std::optional<std::string>{"first:14"},
          "state inspection did not refresh through StateHandle::update");

  const auto element_id = state_owner.root()->id();
  state_owner.render(InspectorStateValuesComponent{&inspected_state, true, "mutate:", &state_owner,
                                                   &formatter_reentrant_rejected});
  require(formatter_reentrant_rejected && state_owner.root()->id() == element_id &&
            inspected_state->get() == 14 &&
            !state_owner.inspect().root->state_values[1].value.has_value(),
          "state formatter installation allowed nested state mutation");
  formatter_reentrant_rejected = false;
  state_owner.render(InspectorStateValuesComponent{&inspected_state, true, "second:", &state_owner,
                                                   &formatter_reentrant_rejected});
  require(state_owner.root()->id() == element_id &&
            state_owner.inspect().root->state_values[1].value ==
              std::optional<std::string>{"second:14"},
          "state inspector replacement changed identity or retained its old formatter");
  inspected_state->set(99);
  require(formatter_reentrant_rejected && state_owner.root()->id() == element_id &&
            inspected_state->get() == 99 &&
            !state_owner.inspect().root->state_values[1].value.has_value(),
          "state formatter reentrancy unmounted its slot or changed assignment behavior");
  state_owner.render(InspectorStateValuesComponent{&inspected_state, false, "unused:", &state_owner,
                                                   &formatter_reentrant_rejected});
  const auto revoked = state_owner.inspect();
  require(revoked.root->state_values.empty() && !revoked.to_json().contains("\"stateValues\""),
          "ordinary state declaration did not revoke inspector exposure or restore JSON privacy");

  auto lifecycle = std::make_shared<InspectorFormatterLifecycleControl>();
  dui::BuildOwner lifecycle_owner;
  lifecycle->owner = &lifecycle_owner;
  lifecycle_owner.render(InspectorFormatterLifecycleComponent{lifecycle, true});
  lifecycle->handle->set(6);
  lifecycle->attack_copy = true;
  lifecycle_owner.render(InspectorFormatterLifecycleComponent{lifecycle, true});
  require(lifecycle->copy_rejected && lifecycle->handle->get() == 6 &&
            lifecycle_owner.inspect().root->state_values.front().value ==
              std::optional<std::string>{"6"},
          "formatter copy reentered state mutation or corrupted its cached value");
  lifecycle->attack_copy = false;
  lifecycle->attack_destruction = true;
  lifecycle_owner.render(InspectorFormatterLifecycleComponent{lifecycle, false});
  require(lifecycle->destruction_rejected && lifecycle->handle->get() == 6 &&
            lifecycle_owner.inspect().root->state_values.empty(),
          "formatter destruction reentered state mutation while revoking exposure");
  lifecycle->attack_destruction = false;

  auto unmount_lifecycle = std::make_shared<InspectorFormatterLifecycleControl>();
  auto unmount_owner = std::make_unique<dui::BuildOwner>();
  unmount_lifecycle->owner = unmount_owner.get();
  unmount_owner->render(InspectorFormatterLifecycleComponent{unmount_lifecycle, true});
  unmount_lifecycle->attack_destruction = true;
  unmount_lifecycle->attack_render = true;
  unmount_owner.reset();
  require(unmount_lifecycle->destruction_rejected,
          "formatter destruction reentered rendering while its Element was unmounting");
  unmount_lifecycle->attack_destruction = false;

  bool context_reentrant_rejected = false;
  dui::BuildOwner context_owner;
  context_owner.render(InspectorContextReentrantComponent{&context_reentrant_rejected});
  const auto context_snapshot = context_owner.inspect();
  require(context_reentrant_rejected && context_snapshot.root->state_slot_count == 1 &&
            context_snapshot.root->state_values.size() == 1 &&
            !context_snapshot.root->state_values.front().value.has_value(),
          "formatter declared nested state through a captured BuildContext");
}

class StepTimelineClock final : public dui::TimelineClock {
public:
  std::chrono::nanoseconds now() const noexcept override {
    ++call_count_;
    return std::chrono::nanoseconds{tick_++};
  }

  [[nodiscard]] std::size_t call_count() const { return call_count_; }

private:
  mutable std::int64_t tick_{};
  mutable std::size_t call_count_{};
};

struct ThrowingTimelineComponent {
  auto build(dui::BuildContext&) const -> dui::Text {
    throw std::runtime_error("timeline build failure");
  }
};

struct ReplaceTimelineDuringBuild {
  dui::BuildOwner* owner;
  std::shared_ptr<dui::TimelineRecorder> replacement;
  bool* rejected;

  auto build(dui::BuildContext&) const {
    try {
      owner->set_timeline_recorder(replacement);
    } catch (const std::logic_error&) {
      *rejected = true;
    }
    return dui::Text{"stable recorder"};
  }
};

void timeline_recorder_is_deterministic_bounded_and_failure_safe() {
  bool invalid_capacity_rejected = false;
  try {
    static_cast<void>(std::make_shared<dui::TimelineRecorder>(0));
  } catch (const std::invalid_argument&) {
    invalid_capacity_rejected = true;
  }
  require(invalid_capacity_rejected, "timeline recorder accepted zero capacity");
  bool missing_clock_rejected = false;
  try {
    static_cast<void>(
      std::make_shared<dui::TimelineRecorder>(1, std::shared_ptr<dui::TimelineClock>{}));
  } catch (const std::invalid_argument&) {
    missing_clock_rejected = true;
  }
  require(missing_clock_rejected, "timeline recorder accepted a null clock");

  auto disabled_clock = std::make_shared<StepTimelineClock>();
  {
    dui::BuildOwner owner;
    owner.set_timeline_recorder(std::make_shared<dui::TimelineRecorder>(4, disabled_clock));
    owner.set_timeline_recorder(nullptr);
    owner.render(dui::Text{"unrecorded"});
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({80.0, 16.0})));
  }
  require(disabled_clock->call_count() == 0,
          "an unattached timeline clock was read by the disabled path");

  auto clock = std::make_shared<StepTimelineClock>();
  auto recorder = std::make_shared<dui::TimelineRecorder>(32, clock);
  dui::TimelineSnapshot detached;
  {
    dui::BuildOwner owner;
    owner.set_timeline_recorder(recorder);
    owner.render(dui::Text{"timeline"});
    const auto first_tree = owner.layer_frame(dui::BoxConstraints::tight({80.0, 16.0}));
    dui::BuildOwner unrecorded_owner;
    unrecorded_owner.render(dui::Text{"timeline"});
    const auto unrecorded_tree =
      unrecorded_owner.layer_frame(dui::BoxConstraints::tight({80.0, 16.0}));
    require(first_tree.dump() == unrecorded_tree.dump() && unrecorded_tree.timeline_flow_id() == 0,
            "timeline recording changed retained frame output or unrecorded metadata");

    const auto snapshot = recorder->snapshot();
    require(snapshot.events.size() == 6 && snapshot.dropped_event_count == 0,
            "basic timeline did not record the exact UI operation set");
    const std::array expected_phases{
      dui::TimelinePhase::reconcile, dui::TimelinePhase::frame,
      dui::TimelinePhase::build,     dui::TimelinePhase::synchronize_render_tree,
      dui::TimelinePhase::layout,    dui::TimelinePhase::composite};
    const auto frame_id = snapshot.events[1].frame_id;
    const auto frame_span_id = snapshot.events[1].span_id;
    const auto first_flow_id = snapshot.events[1].flow_id;
    for (std::size_t index = 0; index < snapshot.events.size(); ++index) {
      const auto& event = snapshot.events[index];
      require(event.sequence == index + 1 && event.phase == expected_phases[index] &&
                event.outcome == dui::TimelineOutcome::completed &&
                event.duration > std::chrono::nanoseconds::zero(),
              "basic timeline order, outcome, or fake-clock duration was incorrect");
      if (index >= 2) {
        require(event.parent_span_id == frame_span_id && event.frame_id == frame_id,
                "frame phase was not correlated with its parent span");
      }
      require(index == 0 ? event.flow_id == 0 : event.flow_id == first_flow_id,
              "UI timeline event had an incorrect cross-lane flow ID");
    }
    require(snapshot.events[0].parent_span_id == 0 && snapshot.events[0].frame_id == 0 &&
              snapshot.events[1].parent_span_id == 0 && frame_id != 0 &&
              snapshot.events[1].duration == std::chrono::nanoseconds{9} && first_flow_id != 0 &&
              first_tree.timeline_flow_id() == first_flow_id,
            "root timeline correlation or deterministic frame timing was incorrect");

    recorder->clear();
    const auto clean_tree = owner.layer_frame(dui::BoxConstraints::tight({80.0, 16.0}));
    const auto clean = recorder->snapshot();
    require(
      clean.events.size() == 5 && first_tree.root() == clean_tree.root() &&
        clean.events[0].phase == dui::TimelinePhase::frame && clean.events[0].work_count == 0 &&
        clean.events[0].flow_id == clean_tree.timeline_flow_id() &&
        clean_tree.timeline_flow_id() != first_flow_id &&
        first_tree.timeline_flow_id() == first_flow_id &&
        clean.events[3].phase == dui::TimelinePhase::layout && clean.events[3].work_count == 0 &&
        clean.events[4].phase == dui::TimelinePhase::composite && clean.events[4].work_count == 0,
      "timeline changed or misreported a retained clean frame");
    const std::uint64_t last_clean_sequence = clean.events.back().sequence;

    recorder->clear();
    bool original_failure_preserved = false;
    try {
      owner.render(ThrowingTimelineComponent{});
    } catch (const std::runtime_error& error) {
      original_failure_preserved = std::string{error.what()} == "timeline build failure";
    }
    require(original_failure_preserved, "timeline replaced a reconciliation exception");
    owner.render(dui::Text{"recovered"});
    auto recovery = recorder->snapshot();
    require(recovery.events.size() == 2 && recovery.events[0].sequence == last_clean_sequence + 1 &&
              recovery.events[0].outcome == dui::TimelineOutcome::failed &&
              recovery.events[1].sequence == last_clean_sequence + 2 &&
              recovery.events[1].outcome == dui::TimelineOutcome::completed,
            "timeline did not close a failed span or recover without reusing IDs");

    std::optional<dui::StateHandle<int>> state;
    owner.render(Counter{&state});
    recorder->clear();
    state->set(4);
    owner.flush();
    const auto dirty = recorder->snapshot();
    require(dirty.events.size() == 1 && dirty.events.front().phase == dui::TimelinePhase::build &&
              dirty.events.front().work_count == 1 && dirty.events.front().flow_id == 0,
            "timeline omitted the pending dirty-build work count");

    auto replacement_clock = std::make_shared<StepTimelineClock>();
    auto replacement = std::make_shared<dui::TimelineRecorder>(4, replacement_clock);
    bool replacement_rejected = false;
    owner.render(ReplaceTimelineDuringBuild{&owner, replacement, &replacement_rejected});
    require(replacement_rejected && replacement->snapshot().events.empty(),
            "timeline recorder changed during reconciliation");
    detached = recorder->snapshot();
  }
  recorder.reset();
  require(!detached.events.empty() && detached.events.back().phase == dui::TimelinePhase::reconcile,
          "timeline snapshot retained recorder- or owner-dependent storage");

  auto bounded_clock = std::make_shared<StepTimelineClock>();
  auto bounded = std::make_shared<dui::TimelineRecorder>(2, bounded_clock);
  dui::BuildOwner bounded_owner;
  bounded_owner.set_timeline_recorder(bounded);
  bounded_owner.render(dui::Text{"one"});
  bounded_owner.render(dui::Text{"two"});
  bounded_owner.render(dui::Text{"three"});
  auto overflow = bounded->snapshot();
  require(overflow.events.size() == 2 && overflow.events[0].sequence == 2 &&
            overflow.events[1].sequence == 3 && overflow.dropped_event_count == 1,
          "timeline ring did not retain the newest completed events");
  const std::string overflow_json = overflow.to_json();
  require(overflow_json.contains("\"droppedEventCount\":1") &&
            overflow_json.find("\"sequence\":2") < overflow_json.find("\"sequence\":3"),
          "timeline JSON omitted overflow metadata or retained event order");
  bounded->clear();
  bounded_owner.render(dui::Text{"four"});
  overflow = bounded->snapshot();
  require(overflow.events.size() == 1 && overflow.events[0].sequence == 4 &&
            overflow.dropped_event_count == 0,
          "timeline clear reused IDs or retained its dropped-event count");
  bounded->clear();
  static_cast<void>(bounded_owner.frame(dui::BoxConstraints::tight({80.0, 16.0})));
  overflow = bounded->snapshot();
  require(overflow.events.size() == 2 && overflow.events[0].phase == dui::TimelinePhase::frame &&
            overflow.events[1].phase == dui::TimelinePhase::composite &&
            overflow.dropped_event_count == 3,
          "timeline ring did not evict nested spans by completion before start-order sorting");
}

void timeline_recorder_streams_completed_batches() {
  auto completion_recorder =
    std::make_shared<dui::TimelineRecorder>(8, std::make_shared<StepTimelineClock>());
  dui::BuildOwner completion_owner;
  completion_owner.set_timeline_recorder(completion_recorder);
  completion_owner.render(dui::Text{"completion order"});
  static_cast<void>(completion_owner.layer_frame(dui::BoxConstraints::tight({80.0, 16.0})));
  const auto completion_snapshot = completion_recorder->snapshot();
  const auto completion_batch = completion_recorder->read_completed(8);
  require(completion_snapshot.events.size() == 6 && completion_batch.events.size() == 6 &&
            completion_snapshot.events[1].phase == dui::TimelinePhase::frame &&
            completion_batch.events.back().phase == dui::TimelinePhase::frame &&
            completion_batch.missed_event_count == 0,
          "incremental timeline read did not preserve completion order separately from snapshots");

  bool zero_limit_rejected = false;
  try {
    static_cast<void>(completion_recorder->read_completed(0));
  } catch (const std::invalid_argument&) {
    zero_limit_rejected = true;
  }
  require(zero_limit_rejected, "incremental timeline read accepted a zero limit");

  auto recorder = std::make_shared<dui::TimelineRecorder>(3, std::make_shared<StepTimelineClock>());
  dui::BuildOwner owner;
  owner.set_timeline_recorder(recorder);
  owner.render(dui::Text{"one"});
  owner.render(dui::Text{"two"});
  auto first = recorder->read_completed(1);
  require(first.events.size() == 1 && first.events[0].sequence == 1 &&
            first.missed_event_count == 0,
          "initial incremental timeline page was incorrect");

  owner.render(dui::Text{"three"});
  owner.render(dui::Text{"four"});
  owner.render(dui::Text{"five"});
  auto overflow = recorder->read_completed(2, first.next_cursor);
  require(overflow.events.size() == 2 && overflow.events[0].sequence == 3 &&
            overflow.events[1].sequence == 4 && overflow.missed_event_count == 1,
          "incremental timeline read did not report an exact overwrite gap");
  auto remainder = recorder->read_completed(2, overflow.next_cursor);
  require(remainder.events.size() == 1 && remainder.events[0].sequence == 5 &&
            remainder.missed_event_count == 0,
          "limited incremental timeline read skipped its remaining event");
  const auto empty = recorder->read_completed(2, remainder.next_cursor);
  require(empty.events.empty() && empty.missed_event_count == 0,
          "up-to-date incremental timeline read was not empty");

  owner.render(dui::Text{"six"});
  recorder->clear();
  auto cleared = recorder->read_completed(2, remainder.next_cursor);
  require(cleared.events.empty() && cleared.missed_event_count == 1,
          "incremental timeline read did not report an empty clear gap");
  owner.render(dui::Text{"seven"});
  auto continued = recorder->read_completed(2, cleared.next_cursor);
  require(continued.events.size() == 1 && continued.events[0].sequence == 7 &&
            continued.missed_event_count == 0,
          "incremental timeline read did not continue after a clear gap");
  const auto late_reader = recorder->read_completed(2);
  require(late_reader.events.size() == 1 && late_reader.events[0].sequence == 7 &&
            late_reader.missed_event_count == 6,
          "a fresh incremental reader did not report unavailable history");

  auto other = std::make_shared<dui::TimelineRecorder>(2, std::make_shared<StepTimelineClock>());
  bool mismatched_cursor_rejected = false;
  try {
    static_cast<void>(other->read_completed(1, continued.next_cursor));
  } catch (const std::invalid_argument&) {
    mismatched_cursor_rejected = true;
  }
  require(mismatched_cursor_rejected, "incremental timeline accepted a foreign cursor");

  std::weak_ptr<dui::TimelineRecorder> recorder_lifetime = recorder;
  const auto detached = std::move(continued);
  owner.set_timeline_recorder(nullptr);
  recorder.reset();
  require(recorder_lifetime.expired() && detached.events.size() == 1 &&
            detached.events[0].sequence == 7,
          "incremental timeline cursor retained its recorder or batch storage was not detached");
  bool expired_cursor_rejected = false;
  try {
    static_cast<void>(other->read_completed(1, detached.next_cursor));
  } catch (const std::invalid_argument&) {
    expired_cursor_rejected = true;
  }
  require(expired_cursor_rejected, "incremental timeline accepted an expired foreign cursor");
}

class GroupedNumberPunctuation final : public std::numpunct<char> {
private:
  char do_thousands_sep() const override { return '_'; }
  std::string do_grouping() const override { return "\3"; }
};

void timeline_json_is_canonical_and_strict() {
  const dui::TimelineSnapshot empty;
  require(empty.to_json() == "{\"version\":1,\"droppedEventCount\":0,\"events\":[]}",
          "empty timeline JSON was not canonical");
  require(empty.to_chrome_trace_json() ==
            "{\"traceEvents\":[{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":1,"
            "\"args\":{\"name\":\"DUI UI\"}},{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,"
            "\"tid\":2,\"args\":{\"name\":\"DUI Raster\"}}],\"displayTimeUnit\":\"ns\","
            "\"duiDroppedEventCount\":0,\"duiTimeOriginNs\":\"0\"}",
          "empty Chrome trace JSON was not deterministic or self-describing");

  dui::TimelineEvent root_event;
  root_event.sequence = 1;
  root_event.span_id = 1;
  root_event.start = std::chrono::nanoseconds{-1};
  const dui::TimelineSnapshot root_snapshot{{root_event}, 0};
  require(root_snapshot.to_json() ==
            "{\"version\":1,\"droppedEventCount\":0,\"events\":[{\"sequence\":1,\"spanId\":"
            "1,\"parentSpanId\":0,\"frameId\":0,\"lane\":\"ui\",\"phase\":\"reconcile\","
            "\"outcome\":\"completed\",\"startNs\":-1,\"durationNs\":0,\"pass\":0,"
            "\"workCount\":0}]}",
          "timeline JSON changed its root parent/frame sentinels or signed time encoding");

  using NanosecondsRep = std::chrono::nanoseconds::rep;
  const auto maximum_id = std::numeric_limits<std::uint64_t>::max();
  const auto maximum_size = std::numeric_limits<std::size_t>::max();
  const auto minimum_time = std::numeric_limits<NanosecondsRep>::min();
  const auto maximum_duration = std::numeric_limits<NanosecondsRep>::max();
  const dui::TimelineEvent event{maximum_id,
                                 maximum_id - 1,
                                 maximum_id - 2,
                                 maximum_id - 3,
                                 dui::TimelineLane::ui,
                                 dui::TimelinePhase::lazy_realization,
                                 dui::TimelineOutcome::failed,
                                 std::chrono::nanoseconds{minimum_time},
                                 std::chrono::nanoseconds{maximum_duration},
                                 maximum_size,
                                 maximum_size - 1};
  const dui::TimelineSnapshot encoded{{event}, maximum_size};
  const std::string expected =
    "{\"version\":1,\"droppedEventCount\":" + std::to_string(maximum_size) +
    ",\"events\":[{\"sequence\":" + std::to_string(maximum_id) +
    ",\"spanId\":" + std::to_string(maximum_id - 1) +
    ",\"parentSpanId\":" + std::to_string(maximum_id - 2) +
    ",\"frameId\":" + std::to_string(maximum_id - 3) +
    ",\"lane\":\"ui\",\"phase\":\"lazyRealization\",\"outcome\":\"failed\",\"startNs\":" +
    std::to_string(minimum_time) + ",\"durationNs\":" + std::to_string(maximum_duration) +
    ",\"pass\":" + std::to_string(maximum_size) +
    ",\"workCount\":" + std::to_string(maximum_size - 1) + "}]}";
  require(encoded.to_json() == expected && encoded.to_json() == expected,
          "timeline JSON lost integer limits, field order, or byte determinism");

  const std::locale previous_locale = std::locale();
  std::string localized;
  try {
    std::locale::global(std::locale{previous_locale, new GroupedNumberPunctuation});
    localized = encoded.to_json();
    std::locale::global(previous_locale);
  } catch (...) {
    std::locale::global(previous_locale);
    throw;
  }
  require(localized == expected, "timeline JSON depended on the process numeric locale");

  const std::array phases{
    dui::TimelinePhase::reconcile, dui::TimelinePhase::build,
    dui::TimelinePhase::frame,     dui::TimelinePhase::synchronize_render_tree,
    dui::TimelinePhase::layout,    dui::TimelinePhase::lazy_realization,
    dui::TimelinePhase::composite};
  const std::array<std::string_view, 7> names{
    "reconcile", "build",           "frame",    "synchronizeRenderTree",
    "layout",    "lazyRealization", "composite"};
  dui::TimelineSnapshot enumerated;
  for (std::size_t index = 0; index < phases.size(); ++index) {
    dui::TimelineEvent value;
    value.sequence = phases.size() - index;
    value.span_id = index + 1;
    value.phase = phases[index];
    value.outcome = index == 0 ? dui::TimelineOutcome::completed : dui::TimelineOutcome::failed;
    enumerated.events.push_back(value);
  }
  const std::string enumeration_json = enumerated.to_json();
  const std::string enumeration_trace = enumerated.to_chrome_trace_json();
  std::size_t previous_position = 0;
  for (std::string_view name : names) {
    const std::size_t position = enumeration_json.find("\"phase\":\"" + std::string{name} + '"');
    require(position != std::string::npos && position >= previous_position,
            "timeline JSON omitted an enum spelling or reordered events");
    require(enumeration_trace.contains("\"name\":\"" + std::string{name} + '"'),
            "Chrome trace JSON omitted a UI phase spelling");
    previous_position = position;
  }
  require(enumeration_json.contains("\"outcome\":\"completed\"") &&
            enumeration_json.contains("\"outcome\":\"failed\"") &&
            enumeration_json.find("\"sequence\":7") < enumeration_json.find("\"sequence\":1"),
          "timeline JSON omitted outcomes or sorted the public event vector");

  const std::array raster_phases{dui::TimelinePhase::raster_frame,
                                 dui::TimelinePhase::surface_acquire, dui::TimelinePhase::rasterize,
                                 dui::TimelinePhase::surface_present};
  const std::array raster_outcomes{dui::TimelineOutcome::completed,
                                   dui::TimelineOutcome::unavailable,
                                   dui::TimelineOutcome::out_of_date, dui::TimelineOutcome::lost};
  const std::array<std::string_view, 4> raster_phase_names{"rasterFrame", "surfaceAcquire",
                                                           "rasterize", "surfacePresent"};
  const std::array<std::string_view, 4> raster_outcome_names{"completed", "unavailable",
                                                             "outOfDate", "lost"};
  dui::TimelineSnapshot raster_encoded;
  for (std::size_t index = 0; index < raster_phases.size(); ++index) {
    dui::TimelineEvent value;
    value.sequence = index + 1;
    value.span_id = index + 1;
    value.lane = dui::TimelineLane::raster;
    value.phase = raster_phases[index];
    value.outcome = raster_outcomes[index];
    raster_encoded.events.push_back(value);
  }
  const std::string raster_json = raster_encoded.to_json();
  const std::string raster_trace = raster_encoded.to_chrome_trace_json();
  require(raster_json.starts_with("{\"version\":2") &&
            raster_json.find("\"lane\":\"raster\"") != std::string::npos &&
            raster_trace.contains("\"cat\":\"dui.raster\"") &&
            raster_trace.contains("\"tid\":2,\"args\""),
          "raster timeline JSON did not select or encode its lane");
  for (std::size_t index = 0; index < raster_phase_names.size(); ++index) {
    require(
      raster_json.contains("\"phase\":\"" + std::string{raster_phase_names[index]} + '"') &&
        raster_json.contains("\"outcome\":\"" + std::string{raster_outcome_names[index]} + '"') &&
        raster_trace.contains("\"name\":\"" + std::string{raster_phase_names[index]} + '"') &&
        raster_trace.contains("\"outcome\":\"" + std::string{raster_outcome_names[index]} + '"'),
      "timeline JSON omitted a version-2 phase or outcome spelling");
  }

  dui::TimelineEvent flowed_event;
  flowed_event.sequence = 1;
  flowed_event.span_id = 1;
  flowed_event.frame_id = 2;
  flowed_event.flow_id = std::numeric_limits<std::uint64_t>::max();
  dui::TimelineEvent zero_flow_raster;
  zero_flow_raster.sequence = 2;
  zero_flow_raster.span_id = 2;
  zero_flow_raster.lane = dui::TimelineLane::raster;
  zero_flow_raster.phase = dui::TimelinePhase::raster_frame;
  const dui::TimelineSnapshot flowed{{flowed_event, zero_flow_raster}, 0};
  const std::string flowed_json = flowed.to_json();
  require(flowed_json.starts_with("{\"version\":3") &&
            flowed_json.contains("\"frameId\":2,\"flowId\":18446744073709551615,\"lane\":") &&
            flowed_json.contains("\"frameId\":0,\"flowId\":0,\"lane\":"),
          "timeline JSON did not emit fixed version-3 flow fields for a mixed snapshot");

  dui::TimelineEvent trace_ui{1,
                              2,
                              0,
                              3,
                              dui::TimelineLane::ui,
                              dui::TimelinePhase::frame,
                              dui::TimelineOutcome::failed,
                              std::chrono::nanoseconds{minimum_time + 1},
                              std::chrono::nanoseconds{1001},
                              4,
                              5,
                              maximum_id};
  dui::TimelineEvent trace_nested = trace_ui;
  trace_nested.sequence = 2;
  trace_nested.span_id = 3;
  trace_nested.parent_span_id = 2;
  trace_nested.phase = dui::TimelinePhase::layout;
  trace_nested.start = std::chrono::nanoseconds{minimum_time};
  trace_nested.duration = std::chrono::nanoseconds::zero();
  dui::TimelineEvent trace_raster = trace_ui;
  trace_raster.sequence = 3;
  trace_raster.span_id = 4;
  trace_raster.frame_id = 9;
  trace_raster.lane = dui::TimelineLane::raster;
  trace_raster.phase = dui::TimelinePhase::raster_frame;
  trace_raster.outcome = dui::TimelineOutcome::out_of_date;
  constexpr auto maximum_trace_time = (std::uint64_t{1} << 50) - 1;
  trace_raster.start =
    std::chrono::nanoseconds{minimum_time + static_cast<NanosecondsRep>(maximum_trace_time)};
  trace_raster.duration = std::chrono::nanoseconds{maximum_trace_time};
  dui::TimelineEvent trace_zero = trace_nested;
  trace_zero.sequence = 4;
  trace_zero.span_id = 5;
  trace_zero.parent_span_id = 0;
  trace_zero.phase = dui::TimelinePhase::frame;
  trace_zero.start = std::chrono::nanoseconds{minimum_time + 2};
  trace_zero.flow_id = 0;
  const dui::TimelineSnapshot trace_snapshot{{trace_ui, trace_nested, trace_raster, trace_zero}, 6};
  const std::string trace_json = trace_snapshot.to_chrome_trace_json();
  require(trace_json.contains(
            "{\"name\":\"frame\",\"cat\":\"dui.ui\",\"ph\":\"X\",\"ts\":0.001,\"dur\":1.001,"
            "\"pid\":1,\"tid\":1,\"args\":{\"outcome\":\"failed\",\"sequence\":\"1\","
            "\"spanId\":\"2\",\"parentSpanId\":\"0\",\"frameId\":\"3\",\"pass\":4,"
            "\"workCount\":5,\"flowId\":\"" +
            std::to_string(maximum_id) + "\"}}") &&
            trace_json.contains("\"ph\":\"s\",\"ts\":0.001,\"pid\":1,\"tid\":1,"
                                "\"id\":\"0xffffffffffffffff\","
                                "\"scope\":\"dui\",\"bp\":\"e\"") &&
            trace_json.contains("\"name\":\"layout\",\"cat\":\"dui.ui\",\"ph\":\"X\",\"ts\":0,"
                                "\"dur\":0") &&
            trace_json.contains("\"name\":\"rasterFrame\",\"cat\":\"dui.raster\",\"ph\":\"X\","
                                "\"ts\":1125899906842.623,\"dur\":1125899906842.623") &&
            trace_json.contains("\"ph\":\"f\",\"ts\":1125899906842.623,\"pid\":1,\"tid\":2,") &&
            trace_json.ends_with("\"displayTimeUnit\":\"ns\",\"duiDroppedEventCount\":6,"
                                 "\"duiTimeOriginNs\":\"-9223372036854775808\"}"),
          "Chrome trace JSON lost field order, precise integer time, flow, lane, or drop metadata");
  const auto first_flow_marker = trace_json.find("\"cat\":\"dui.flow\"");
  require(first_flow_marker != std::string::npos &&
            trace_json.find("\"cat\":\"dui.flow\"", first_flow_marker + 1) != std::string::npos &&
            trace_json.find("\"cat\":\"dui.flow\"",
                            trace_json.find("\"cat\":\"dui.flow\"", first_flow_marker + 1) + 1) ==
              std::string::npos,
          "Chrome trace emitted flow markers for nested or zero-flow events");

  std::string localized_trace;
  try {
    std::locale::global(std::locale{previous_locale, new GroupedNumberPunctuation});
    localized_trace = trace_snapshot.to_chrome_trace_json();
    std::locale::global(previous_locale);
  } catch (...) {
    std::locale::global(previous_locale);
    throw;
  }
  require(localized_trace == trace_json && trace_snapshot.to_chrome_trace_json() == trace_json,
          "Chrome trace JSON depended on locale or changed between serializations");

  const auto trace_rejects = [](dui::TimelineSnapshot malformed) {
    try {
      static_cast<void>(malformed.to_chrome_trace_json());
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  trace_raster.start =
    std::chrono::nanoseconds{minimum_time + static_cast<NanosecondsRep>(maximum_trace_time + 1)};
  trace_raster.duration = std::chrono::nanoseconds::zero();
  require(trace_rejects({{trace_raster, trace_nested}, 0}),
          "Chrome trace accepted an inexact relative timestamp range");
  trace_raster.start = std::chrono::nanoseconds{maximum_duration};
  require(trace_rejects({{trace_raster, trace_nested}, 0}),
          "Chrome trace accepted an INT64-limit relative timestamp range");
  trace_raster.start = trace_ui.start;
  trace_raster.duration = std::chrono::nanoseconds{maximum_duration};
  require(trace_rejects({{trace_raster}, 0}), "Chrome trace accepted an inexact duration range");

  const auto rejects = [](dui::TimelineSnapshot malformed) {
    bool rejected_native = false;
    try {
      static_cast<void>(malformed.to_json());
    } catch (const std::invalid_argument&) {
      rejected_native = true;
    }
    bool rejected_trace = false;
    try {
      static_cast<void>(malformed.to_chrome_trace_json());
    } catch (const std::invalid_argument&) {
      rejected_trace = true;
    }
    return rejected_native && rejected_trace;
  };
  dui::TimelineSnapshot malformed{{dui::TimelineEvent{}}, 0};
  malformed.events.front().lane = static_cast<dui::TimelineLane>(99);
  require(rejects(malformed), "timeline JSON accepted an unknown lane");
  malformed.events.front().lane = dui::TimelineLane::ui;
  malformed.events.front().phase = static_cast<dui::TimelinePhase>(99);
  require(rejects(malformed), "timeline JSON accepted an unknown phase");
  malformed.events.front().phase = dui::TimelinePhase::frame;
  malformed.events.front().outcome = static_cast<dui::TimelineOutcome>(99);
  require(rejects(malformed), "timeline JSON accepted an unknown outcome");
  malformed.events.front().outcome = dui::TimelineOutcome::completed;
  malformed.events.front().duration = std::chrono::nanoseconds{-1};
  require(rejects(malformed), "timeline JSON accepted a negative duration");
}

void tooling_report_html_is_safe_and_deterministic() {
  const dui::ToolingReport empty;
  const std::string empty_html = empty.to_html();
  require(empty_html.starts_with("<!doctype html><html lang=\"en\">") &&
            empty_html.ends_with("</body></html>") &&
            empty_html.contains("Content-Security-Policy") &&
            empty_html.contains("default-src 'none'; style-src 'unsafe-inline'") &&
            empty_html.contains("@media(max-width:620px)") &&
            empty_html.contains("No mounted Element tree.") &&
            empty_html.contains("No retained timeline events."),
          "empty tooling report was not a complete secure responsive document");

  dui::InspectorSnapshot inspector;
  inspector.mount_count = 7;
  inspector.unmount_count = 2;
  inspector.pending_build_count = 1;
  inspector.root.emplace();
  inspector.root->id = 9;
  inspector.root->generation = 3;
  inspector.root->name = "root<script>&\"'";
  inspector.root->value = std::string{"line\n"} + static_cast<char>(0x80);
  inspector.root->key = "<key>";
  inspector.root->dirty = true;
  inspector.root->has_focus_node = true;
  inspector.root->focused = true;
  inspector.root->state_slot_count = 2;
  inspector.root->state_values = {{"visible", std::string{"<value>&"}}, {"failed", std::nullopt}};
  inspector.root->render_object = dui::InspectorRenderSnapshot{
    11, dui::InspectorRenderProtocol::box, true, false, true, true, 4, 5};
  inspector.root->children.emplace_back();
  inspector.root->children.back().id = 10;
  inspector.root->children.back().name = "child";
  inspector.root->children.back().state = dui::InspectorElementState::dormant_keep_alive;
  inspector.root->children.back().has_focus_node = true;

  dui::TimelineSnapshot timeline;
  timeline.dropped_event_count = 4;
  const std::array outcomes{dui::TimelineOutcome::completed, dui::TimelineOutcome::unavailable,
                            dui::TimelineOutcome::out_of_date, dui::TimelineOutcome::lost,
                            dui::TimelineOutcome::failed};
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    dui::TimelineEvent event;
    event.sequence = outcomes.size() - index;
    event.span_id = index + 20;
    event.parent_span_id = index;
    event.frame_id = 30 + index;
    event.flow_id = 40 + index;
    event.lane = index == 0 ? dui::TimelineLane::ui : dui::TimelineLane::raster;
    event.phase = index == 0 ? dui::TimelinePhase::frame : dui::TimelinePhase::raster_frame;
    event.outcome = outcomes[index];
    event.start = std::chrono::nanoseconds{-1 + static_cast<std::int64_t>(index)};
    event.duration = std::chrono::nanoseconds{static_cast<std::int64_t>(index)};
    event.pass = index + 50;
    event.work_count = index + 60;
    timeline.events.push_back(event);
  }
  const dui::ToolingReport report{inspector, timeline};
  const std::string html = report.to_html();
  require(
    html == report.to_html() && html.contains("<b>7</b><span>mounts</span>") &&
      html.contains("<b>4</b><span>dropped events</span>") &&
      html.contains("&quot;root&lt;script&gt;&amp;\\&quot;&#39;&quot;") &&
      !html.contains("root<script>") && html.contains("&quot;line\\n\\ufffd&quot;") &&
      html.contains("&quot;&lt;key&gt;&quot;") && html.contains("&quot;&lt;value&gt;&amp;&quot;") &&
      html.contains("<em>unavailable</em>") &&
      html.contains("Render #11</strong> box | layout 4 | paint 5 | flags L-C | boundary") &&
      html.contains("badge focus\">focused") && html.contains("badge focusable\">focusable") &&
      html.contains("badge dormant\">dormant") &&
      html.find("<tr><td>5</td><td>ui</td>") < html.find("<tr><td>1</td><td>raster</td>") &&
      html.contains("outcome-completed") && html.contains("outcome-unavailable") &&
      html.contains("outcome-outOfDate") && html.contains("outcome-lost") &&
      html.contains("outcome-failed") &&
      html.contains("<td>24</td><td>4</td><td>34</td><td>44</td><td>54</td><td>64</td>"),
    "tooling report lost snapshot order, metadata, escaping, state, render, or timeline data");

  const std::locale previous_locale = std::locale();
  std::string localized;
  try {
    std::locale::global(std::locale{previous_locale, new GroupedNumberPunctuation});
    localized = report.to_html();
    std::locale::global(previous_locale);
  } catch (...) {
    std::locale::global(previous_locale);
    throw;
  }
  require(localized == html, "tooling report depended on the process numeric locale");

  dui::ToolingReport deep;
  deep.inspector.root.emplace();
  dui::InspectorNode* cursor = &*deep.inspector.root;
  for (std::size_t depth = 1; depth <= dui::InspectorSnapshot::maximum_depth; ++depth) {
    cursor->children.emplace_back();
    cursor = &cursor->children.back();
  }
  bool deep_rejected = false;
  try {
    static_cast<void>(deep.to_html());
  } catch (const std::length_error&) {
    deep_rejected = true;
  }
  require(deep_rejected, "tooling report rendered beyond the inspector depth bound");

  dui::ToolingReport malformed;
  malformed.timeline.events.emplace_back();
  malformed.timeline.events.front().duration = std::chrono::nanoseconds{-1};
  bool malformed_rejected = false;
  try {
    static_cast<void>(malformed.to_html());
  } catch (const std::invalid_argument&) {
    malformed_rejected = true;
  }
  require(malformed_rejected, "tooling report accepted malformed timeline data");

  malformed.timeline.events.clear();
  malformed.inspector.root.emplace();
  malformed.inspector.root->state = static_cast<dui::InspectorElementState>(99);
  malformed_rejected = false;
  try {
    static_cast<void>(malformed.to_html());
  } catch (const std::invalid_argument&) {
    malformed_rejected = true;
  }
  require(malformed_rejected, "tooling report accepted an unknown Element state");

  malformed.inspector.root->state = dui::InspectorElementState::active;
  malformed.inspector.root->render_object.emplace();
  malformed.inspector.root->render_object->protocol = static_cast<dui::InspectorRenderProtocol>(99);
  malformed_rejected = false;
  try {
    static_cast<void>(malformed.to_html());
  } catch (const std::invalid_argument&) {
    malformed_rejected = true;
  }
  require(malformed_rejected, "tooling report accepted an unknown RenderObject protocol");
}

void duplicate_keys_are_rejected() {
  dui::BuildOwner owner;
  owner.render(ItemList{{{1, "original"}}});
  const auto original_id = only_child(*owner.root()).children().front()->id();
  bool rejected = false;
  try {
    owner.render(ItemList{{{1, "one"}, {1, "duplicate"}}});
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "duplicate ForEach keys were accepted");
  require(only_child(*owner.root()).children().front()->id() == original_id,
          "duplicate-key validation mutated the existing list");
}

} // namespace

int main() {
  try {
    state_survives_and_batches();
    replacing_type_unmounts_element();
    optional_and_choice_reconcile();
    choice_branch_has_distinct_identity();
    keyed_reorder_preserves_identity();
    keyed_component_state_follows_key();
    observable_invalidates_only_readers();
    parent_rebuild_coalesces_dirty_child();
    observable_can_die_before_owner();
    conditional_observable_dependencies_are_replaced();
    environment_invalidates_only_readers();
    nearest_environment_scope_wins();
    missing_environment_is_rejected();
    skipped_equal_component_keeps_latest_descriptor();
    state_write_during_build_remains_scheduled();
    reentrant_root_render_is_rejected();
    stale_state_handle_is_rejected();
    state_handle_rejects_destroyed_owner();
    tree_dump_is_deterministic();
    structured_inspector_is_detached_and_deterministic();
    timeline_recorder_is_deterministic_bounded_and_failure_safe();
    timeline_recorder_streams_completed_batches();
    timeline_json_is_canonical_and_strict();
    tooling_report_html_is_safe_and_deterministic();
    duplicate_keys_are_rejected();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  std::cout << "All DUI core tests passed\n";
  return EXIT_SUCCESS;
}
