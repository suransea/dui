#include "dui/ui.hpp"

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

class ArenaMember final : public dui::GestureArenaMember {
public:
    ArenaMember(std::string name, std::vector<std::string>& events) :
        name_(std::move(name)),
        events_(&events) {}

    void accept_gesture(dui::PointerId pointer) override {
        events_->push_back(name_ + ":accept:" + std::to_string(pointer));
    }

    void reject_gesture(dui::PointerId pointer) override {
        events_->push_back(name_ + ":reject:" + std::to_string(pointer));
    }

private:
    std::string name_;
    std::vector<std::string>* events_;
};

void gesture_arena_selects_one_winner() {
    dui::GestureArena arena;
    std::vector<std::string> events;
    auto tap = std::make_shared<ArenaMember>("tap", events);
    auto drag = std::make_shared<ArenaMember>("drag", events);

    arena.add(1, tap);
    arena.add(1, drag);
    arena.close(1);
    arena.accept(1, *drag);

    require(!arena.contains(1), "resolved GestureArena remained active");
    require(
        events == std::vector<std::string>{"drag:accept:1", "tap:reject:1"},
        "GestureArena resolved members in the wrong order"
    );
}

void gesture_arena_promotes_last_member_and_cancels() {
    dui::GestureArena arena;
    std::vector<std::string> events;
    auto first = std::make_shared<ArenaMember>("first", events);
    auto second = std::make_shared<ArenaMember>("second", events);

    arena.add(2, first);
    arena.add(2, second);
    arena.close(2);
    arena.reject(2, *first);
    require(
        events == std::vector<std::string>{"first:reject:2", "second:accept:2"},
        "last GestureArena member was not promoted"
    );

    events.clear();
    arena.add(3, first);
    arena.add(3, second);
    arena.cancel(3);
    require(
        events == std::vector<std::string>{"first:reject:3", "second:reject:3"},
        "GestureArena cancellation did not reject every member"
    );
}

void eager_accept_waits_for_arena_close() {
    dui::GestureArena arena;
    std::vector<std::string> events;
    auto eager = std::make_shared<ArenaMember>("eager", events);
    auto late = std::make_shared<ArenaMember>("late", events);

    arena.add(4, eager);
    arena.accept(4, *eager);
    require(arena.contains(4) && events.empty(), "eager winner resolved before arena close");
    arena.add(4, late);
    arena.close(4);
    require(
        events == std::vector<std::string>{"eager:accept:4", "late:reject:4"},
        "eager winner did not resolve after close"
    );
}

class ReentrantArenaMember final : public dui::GestureArenaMember {
public:
    ReentrantArenaMember(dui::GestureArena& arena, bool& rejected) :
        arena_(&arena),
        rejected_(&rejected) {}

    void accept_gesture(dui::PointerId pointer) override {
        try {
            arena_->add(pointer, std::make_shared<ArenaMember>("new", events_));
        } catch (const std::logic_error&) {
            *rejected_ = true;
        }
    }

    void reject_gesture(dui::PointerId) override {}

private:
    dui::GestureArena* arena_;
    bool* rejected_;
    std::vector<std::string> events_;
};

void arena_recreation_is_blocked_during_callbacks() {
    dui::GestureArena arena;
    bool rejected = false;
    auto member = std::make_shared<ReentrantArenaMember>(arena, rejected);
    arena.add(5, member);
    arena.close(5);
    require(rejected, "GestureArena was recreated reentrantly for the same pointer");
    require(!arena.contains(5), "resolved arena was unexpectedly recreated");
}

class ArenaDestroyingMember final : public dui::GestureArenaMember {
public:
    explicit ArenaDestroyingMember(std::unique_ptr<dui::GestureArena>& arena) : arena_(&arena) {}

    void accept_gesture(dui::PointerId) override { arena_->reset(); }
    void reject_gesture(dui::PointerId) override {}

private:
    std::unique_ptr<dui::GestureArena>* arena_;
};

void arena_can_be_destroyed_from_resolution_callback() {
    auto arena = std::make_unique<dui::GestureArena>();
    auto member = std::make_shared<ArenaDestroyingMember>(arena);
    arena->add(6, member);
    arena->close(6);
    require(arena == nullptr, "GestureArena callback did not destroy its owner");
}

void focus_routes_keys_from_leaf_to_ancestor() {
    dui::FocusManager manager;
    std::vector<std::string> events;
    auto parent = manager.create_node({}, [&](const dui::KeyEvent&) {
        events.push_back("parent");
        return dui::KeyEventResult::handled;
    });
    auto child = manager.create_node(parent, [&](const dui::KeyEvent&) {
        events.push_back("child");
        return dui::KeyEventResult::ignored;
    });

    child->request_focus();
    const auto result = manager.dispatch_key({"Enter", dui::KeyPhase::down});
    require(result == dui::KeyEventResult::handled, "key event was not handled by focus ancestor");
    require(events == std::vector<std::string>{"child", "parent"}, "key event followed the wrong focus path");
}

void focus_falls_back_and_nodes_outlive_manager() {
    std::shared_ptr<dui::FocusNode> surviving;
    {
        dui::FocusManager manager;
        auto parent = manager.create_node();
        auto child = manager.create_node(parent);
        child->request_focus();
        child.reset();
        require(manager.focused_node() == parent, "focus did not fall back to live parent");
        surviving = parent;
    }

    bool rejected = false;
    try {
        surviving->request_focus();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "FocusNode accessed a destroyed FocusManager");
}

void cross_manager_focus_parent_is_rejected() {
    dui::FocusManager first;
    dui::FocusManager second;
    auto foreign = first.create_node();
    bool rejected = false;
    try {
        static_cast<void>(second.create_node(foreign));
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "cross-manager Focus parent was accepted");
}

void focus_handler_can_replace_itself_safely() {
    dui::FocusManager manager;
    auto node = manager.create_node();
    std::weak_ptr<dui::FocusNode> weak_node = node;
    int calls = 0;
    node->set_key_handler([weak_node, &calls](const dui::KeyEvent&) {
        ++calls;
        weak_node.lock()->set_key_handler([&calls](const dui::KeyEvent&) {
            ++calls;
            return dui::KeyEventResult::handled;
        });
        return dui::KeyEventResult::ignored;
    });
    node->request_focus();

    require(
        manager.dispatch_key({"A", dui::KeyPhase::down}) == dui::KeyEventResult::ignored,
        "first self-replacing key handler returned the wrong result"
    );
    require(
        manager.dispatch_key({"A", dui::KeyPhase::down}) == dui::KeyEventResult::handled,
        "replacement key handler was not installed"
    );
    require(calls == 2, "self-replacing key handler call count is incorrect");
}

void focus_reparents_around_destroyed_intermediate_node() {
    dui::FocusManager manager;
    auto root = manager.create_node();
    auto middle = manager.create_node(root, {}, false);
    auto leaf = manager.create_node(middle);
    leaf->request_focus();

    middle.reset();
    require(leaf->parent_id() == root->id(), "Focus child was not reparented to live ancestor");
    leaf.reset();
    require(manager.focused_node() == root, "focus did not fall back through removed intermediate node");
}

void text_editing_ranges_use_utf8_boundaries() {
    const std::string text = "A\xc3\xa9Z";
    require(dui::TextRange{0, 3}.valid_for(text), "valid UTF-8 byte range was rejected");
    require(!dui::TextRange{2, 3}.valid_for(text), "UTF-8 continuation-byte boundary was accepted");
    require(!dui::TextRange{3, 2}.valid_for(text), "reversed text range was accepted");

    const dui::TextEditingValue valid{text, {1, 3}, dui::TextRange{1, 3}};
    const dui::TextEditingValue invalid{text, {1, 9}, std::nullopt};
    require(valid.valid(), "valid text editing value was rejected");
    require(!invalid.valid(), "out-of-bounds text editing value was accepted");

    require(dui::is_valid_utf8("plain"), "ASCII was rejected as invalid UTF-8");
    require(dui::is_valid_utf8("\xf0\x9f\x98\x80"), "four-byte UTF-8 was rejected");
    require(!dui::is_valid_utf8("\x80"), "lone UTF-8 continuation byte was accepted");
    require(!dui::is_valid_utf8("\xe2\x82"), "truncated UTF-8 sequence was accepted");
    require(!dui::is_valid_utf8("\xc0\x80"), "overlong UTF-8 sequence was accepted");
    require(!dui::is_valid_utf8("\xed\xa0\x80"), "UTF-8 surrogate encoding was accepted");
    require(!dui::is_valid_utf8("\xf4\x90\x80\x80"), "out-of-range UTF-8 scalar was accepted");
    require(
        !dui::TextRange{0, 1}.valid_for("\x80"),
        "range validation accepted malformed UTF-8 text"
    );
}

class InputClient final : public dui::TextInputClient {
public:
    void update_editing_value(dui::TextEditingValue value) override {
        last_value = std::move(value);
    }
    void perform_action(dui::TextInputAction action) override {
        last_action = action;
    }

    std::optional<dui::TextEditingValue> last_value;
    std::optional<dui::TextInputAction> last_action;
};

class InputBackend final : public dui::TextInputBackend {
public:
    [[nodiscard]] dui::TextInputSessionId start_text_input(
        std::weak_ptr<dui::TextInputClient> input_client,
        dui::TextInputConfiguration,
        dui::TextEditingValue
    ) override {
        client = std::move(input_client);
        return ++session;
    }
    void update_editing_state(dui::TextInputSessionId, dui::TextEditingValue) override {}
    void stop_text_input(dui::TextInputSessionId stopped) override { stopped_session = stopped; }
    void set_editable_rect(dui::TextInputSessionId, dui::Rect) override {}

    std::weak_ptr<dui::TextInputClient> client;
    dui::TextInputSessionId session{};
    dui::TextInputSessionId stopped_session{};
};

void text_input_backend_holds_weak_client_session() {
    InputBackend backend;
    auto client = std::make_shared<InputClient>();
    const auto session = backend.start_text_input(
        client,
        {},
        {"", {}, std::nullopt}
    );
    require(!backend.client.expired(), "live IME client was not retained weakly");
    client.reset();
    require(backend.client.expired(), "IME backend retained a destroyed client");
    backend.stop_text_input(session);
    require(backend.stopped_session == session, "IME session identity was not preserved");
}

} // namespace

int main() {
    try {
        gesture_arena_selects_one_winner();
        gesture_arena_promotes_last_member_and_cancels();
        eager_accept_waits_for_arena_close();
        arena_recreation_is_blocked_during_callbacks();
        arena_can_be_destroyed_from_resolution_callback();
        focus_routes_keys_from_leaf_to_ancestor();
        focus_falls_back_and_nodes_outlive_manager();
        cross_manager_focus_parent_is_rejected();
        focus_handler_can_replace_itself_safely();
        focus_reparents_around_destroyed_intermediate_node();
        text_editing_ranges_use_utf8_boundaries();
        text_input_backend_holds_weak_client_session();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "All DUI input tests passed\n";
    return EXIT_SUCCESS;
}
