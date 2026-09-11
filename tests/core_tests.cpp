#include "dui/ui.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
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
    require(only_child(*owner.root()).debug_value() == "count=2", "state rebuild produced stale output");

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
                left,
                [] { return dui::Text{"left"}; },
                [] { return dui::VStack{dui::Text{"right"}}; }
            )
        };
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
    require(only_child(*updated_group.children()[1]).id() != left_id, "choice branch type was not replaced");
    require(owner.unmount_count() >= 2, "conditional removals were not unmounted");

    owner.render(ConditionalComponent{true, false});
    const dui::Element& restored_group = only_child(*owner.root());
    require(only_child(*restored_group.children()[0]).id() != optional_child_id, "removed optional child retained identity");
}

struct SameTypeChoice {
    bool first;

    auto build(dui::BuildContext&) const {
        return dui::VStack{
            dui::choose(
                first,
                [] { return dui::Text{"first"}; },
                [] { return dui::Text{"second"}; }
            ),
            dui::Text{"stable sibling"}
        };
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
    require(only_child(*updated_group.children()[0]).id() != branch_id, "same-type Choice branch retained identity");
    require(updated_group.children()[1]->id() == sibling_id, "Choice switch replaced an unaffected sibling");
    require(owner.unmount_count() == unmounts + 1, "Choice switch unmounted more than the active branch");
}

struct Item {
    int id;
    std::string label;
};

struct ItemList {
    std::vector<Item> items;

    auto build(dui::BuildContext&) const {
        return dui::ForEach{
            items,
            dui::key<&Item::id>,
            [](const Item& item) { return dui::Text{item.label}; }
        };
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
        return dui::ForEach{
            items,
            dui::key<&Item::id>,
            [states = states](const Item& item) {
                return StatefulItem{item, states};
            }
        };
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
    require(only_child(*reordered.children()[0]).debug_value() == "TWO=99", "state did not follow its item key");
    require(only_child(*reordered.children()[1]).debug_value() == "ONE=10", "another item's state was corrupted");
}

struct ObservedLabel {
    dui::Signal<std::string>* value;

    auto build(dui::BuildContext&) const {
        return dui::Text{value->get()};
    }
};

struct StaticLabel {
    auto build(dui::BuildContext&) const {
        return dui::Text{"static"};
    }
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

    require(root.children()[0]->children()[0]->debug_value() == "after", "observed component did not rebuild");
    require(root.children()[0]->children()[0]->update_count() == observed_updates + 1, "reader update count is incorrect");
    require(root.children()[1]->children()[0]->update_count() == static_updates, "unrelated component rebuilt");
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
    require(owner.root()->debug_value() == "safe replacement", "destroyed observable left a dangling dependency");
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
    require(owner.pending_build_count() == 0, "component remained subscribed to an unread observable");
    second.set(20);
    require(owner.pending_build_count() == 1, "component did not subscribe to its new observable");
    owner.flush();
    require(only_child(*owner.root()).debug_value() == "20", "conditional observable rebuild was stale");
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
            dui::VStack{
                ThemeReader{reader_builds},
                EnvironmentIndependent{independent_builds}
            },
            Theme{theme.get()}
        );
    }
};

void environment_invalidates_only_readers() {
    dui::BuildOwner owner;
    std::optional<dui::StateHandle<std::string>> theme;
    int reader_builds = 0;
    int independent_builds = 0;
    owner.render(ThemeProvider{&theme, &reader_builds, &independent_builds});

    require(reader_builds == 1 && independent_builds == 1, "environment subtree did not initially build once");
    theme->set("dark");
    owner.flush();

    require(reader_builds == 2, "Environment reader was not invalidated");
    require(independent_builds == 1, "Environment update rebuilt an equal non-reader component");
    const dui::Element& environment = only_child(*owner.root());
    const dui::Element& stack = only_child(environment);
    require(only_child(*stack.children()[0]).debug_value() == "dark", "Environment reader saw a stale value");
}

void nearest_environment_scope_wins() {
    dui::BuildOwner owner;
    int builds = 0;
    owner.render(dui::with_environment(
        dui::with_environment(
            ThemeReader{&builds},
            Theme{"inner"}
        ),
        Theme{"outer"}
    ));

    const dui::Element& outer = *owner.root();
    const dui::Element& inner = only_child(outer);
    const dui::Element& reader = only_child(inner);
    require(only_child(reader).debug_value() == "inner", "Environment lookup ignored nearest provider");
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

    friend bool operator==(
        const EqualComponentWithIgnoredConfig& left,
        const EqualComponentWithIgnoredConfig& right
    ) {
        return left.invalidation == right.invalidation
            && left.observed_config == right.observed_config;
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
    require(only_child(*owner.root()).debug_value() == "outer", "reentrant render corrupted the outer build");
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
    owner.render(dui::VStack{
        dui::Text{"alpha"},
        dui::Optional<dui::Text>{dui::Text{"beta"}}
    });

    const std::string expected =
        "VStack#1 updates=1\n"
        "  Text#2 value=\"alpha\" updates=1\n"
        "  Optional#3 updates=1\n"
        "    Text#4 value=\"beta\" updates=1\n";
    require(owner.dump_tree() == expected, "tree inspection output is not deterministic");
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
    require(
        only_child(*owner.root()).children().front()->id() == original_id,
        "duplicate-key validation mutated the existing list"
    );
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
        duplicate_keys_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "All DUI core tests passed\n";
    return EXIT_SUCCESS;
}
