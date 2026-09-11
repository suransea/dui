#pragma once

#include "dui/runtime.hpp"

#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace dui {

struct Text {
    std::string value;
    std::function<void()> on_activate;

    explicit Text(std::string text, std::function<void()> callback = {}) :
        value(std::move(text)),
        on_activate(std::move(callback)) {}
};

struct Image {
    std::string asset;
    Size intrinsic_size;
};

template<class... Children>
struct VStack {
    std::tuple<Children...> children;

    explicit VStack(Children... values) : children(std::move(values)...) {}
};

template<class... Children>
VStack(Children...) -> VStack<Children...>;

template<class... Children>
struct HStack {
    std::tuple<Children...> children;

    explicit HStack(Children... values) : children(std::move(values)...) {}
};

template<class... Children>
HStack(Children...) -> HStack<Children...>;

template<class... Children>
struct Stack {
    std::tuple<Children...> children;

    explicit Stack(Children... values) : children(std::move(values)...) {}
};

template<class... Children>
Stack(Children...) -> Stack<Children...>;

template<class... Children>
struct Fragment {
    std::tuple<Children...> children;

    explicit Fragment(Children... values) : children(std::move(values)...) {}
};

template<class... Children>
Fragment(Children...) -> Fragment<Children...>;

template<class Child, class... Values>
struct EnvironmentScope {
    Child child;
    std::tuple<Values...> values;

    EnvironmentScope(Child child_value, Values... environment_values) :
        child(std::move(child_value)),
        values(std::move(environment_values)...) {}
};

template<class Child, class... Values>
EnvironmentScope(Child, Values...) -> EnvironmentScope<Child, Values...>;

namespace detail {

template<class...>
inline constexpr bool unique_types = true;

template<class First, class... Rest>
inline constexpr bool unique_types<First, Rest...> =
    (!std::same_as<First, Rest> && ...) && unique_types<Rest...>;

} // namespace detail

template<class Child, class... Values>
[[nodiscard]] auto with_environment(Child&& child, Values&&... values) {
    static_assert(
        detail::unique_types<std::decay_t<Values>...>,
        "An EnvironmentScope cannot provide the same type more than once"
    );
    return EnvironmentScope<std::decay_t<Child>, std::decay_t<Values>...>{
        std::forward<Child>(child),
        std::forward<Values>(values)...
    };
}

template<class Child>
struct Padding {
    Insets insets;
    Child child;
};

template<class Child>
Padding(Insets, Child) -> Padding<Child>;

template<class Child>
[[nodiscard]] auto padding(Child&& child, Insets insets) {
    return Padding<std::decay_t<Child>>{
        insets,
        std::forward<Child>(child)
    };
}

template<class Child>
struct ColoredBox {
    std::uint32_t color;
    Child child;
};

template<class Child>
ColoredBox(std::uint32_t, Child) -> ColoredBox<Child>;

template<class Child>
[[nodiscard]] auto background(Child&& child, std::uint32_t color) {
    return ColoredBox<std::decay_t<Child>>{
        color,
        std::forward<Child>(child)
    };
}

template<class Child>
struct RepaintBoundary {
    Child child;
};

template<class Child>
RepaintBoundary(Child) -> RepaintBoundary<Child>;

template<class Child>
[[nodiscard]] auto repaint_boundary(Child&& child) {
    return RepaintBoundary<std::decay_t<Child>>{std::forward<Child>(child)};
}

template<class Child>
struct GestureDetector {
    Child child;
    std::function<void()> on_tap;
};

template<class Child>
GestureDetector(Child, std::function<void()>) -> GestureDetector<Child>;

template<class Child, class Callback>
[[nodiscard]] auto on_tap(Child&& child, Callback&& callback) {
    return GestureDetector<std::decay_t<Child>>{
        std::forward<Child>(child),
        std::function<void()>{std::forward<Callback>(callback)}
    };
}

template<class Child>
struct FocusView {
    Child child;
    FocusNode::KeyHandler on_key;
    bool can_focus{true};
    bool autofocus{};
};

template<class Child>
FocusView(Child, FocusNode::KeyHandler, bool, bool) -> FocusView<Child>;

template<class Child>
[[nodiscard]] auto focusable(
    Child&& child,
    FocusNode::KeyHandler on_key = {},
    bool autofocus = false
) {
    return FocusView<std::decay_t<Child>>{
        std::forward<Child>(child),
        std::move(on_key),
        true,
        autofocus
    };
}

template<class V>
struct Optional {
    std::optional<V> child;
};

template<class Builder>
[[nodiscard]] auto optional(bool condition, Builder&& builder) {
    using V = std::decay_t<std::invoke_result_t<Builder>>;
    if (condition) {
        return Optional<V>{std::invoke(std::forward<Builder>(builder))};
    }
    return Optional<V>{std::nullopt};
}

template<class Left, class Right>
struct Choice {
    std::variant<Left, Right> child;
};

template<class LeftBuilder, class RightBuilder>
[[nodiscard]] auto choose(
    bool condition,
    LeftBuilder&& left_builder,
    RightBuilder&& right_builder
) {
    using Left = std::decay_t<std::invoke_result_t<LeftBuilder>>;
    using Right = std::decay_t<std::invoke_result_t<RightBuilder>>;
    using Result = Choice<Left, Right>;

    if (condition) {
        return Result{std::variant<Left, Right>{
            std::in_place_index<0>,
            std::invoke(std::forward<LeftBuilder>(left_builder))
        }};
    }
    return Result{std::variant<Left, Right>{
        std::in_place_index<1>,
        std::invoke(std::forward<RightBuilder>(right_builder))
    }};
}

template<class Range, class KeyFunction, class Builder>
struct ForEach {
    Range items;
    KeyFunction key_function;
    Builder builder;

    template<class R, class K, class B>
    ForEach(R&& range, K&& key_fn, B&& child_builder) :
        items(std::forward<R>(range)),
        key_function(std::forward<K>(key_fn)),
        builder(std::forward<B>(child_builder)) {}
};

template<class R, class K, class B>
ForEach(R&&, K&&, B&&) -> ForEach<std::decay_t<R>, std::decay_t<K>, std::decay_t<B>>;

template<class T>
concept Component = std::copy_constructible<T> && requires(const T& value, BuildContext& context) {
    value.build(context);
    requires (!std::is_void_v<decltype(value.build(context))>);
};

template<class>
inline constexpr bool is_builtin_view = false;

template<>
inline constexpr bool is_builtin_view<Text> = true;

template<>
inline constexpr bool is_builtin_view<Image> = true;

template<class... Children>
inline constexpr bool is_builtin_view<VStack<Children...>> = true;

template<class... Children>
inline constexpr bool is_builtin_view<HStack<Children...>> = true;

template<class... Children>
inline constexpr bool is_builtin_view<Stack<Children...>> = true;

template<class... Children>
inline constexpr bool is_builtin_view<Fragment<Children...>> = true;

template<class Child, class... Values>
inline constexpr bool is_builtin_view<EnvironmentScope<Child, Values...>> = true;

template<class Child>
inline constexpr bool is_builtin_view<Padding<Child>> = true;

template<class Child>
inline constexpr bool is_builtin_view<ColoredBox<Child>> = true;

template<class Child>
inline constexpr bool is_builtin_view<RepaintBoundary<Child>> = true;

template<class Child>
inline constexpr bool is_builtin_view<GestureDetector<Child>> = true;

template<class Child>
inline constexpr bool is_builtin_view<FocusView<Child>> = true;

template<class V>
inline constexpr bool is_builtin_view<Optional<V>> = true;

template<class Left, class Right>
inline constexpr bool is_builtin_view<Choice<Left, Right>> = true;

template<class Range, class KeyFunction, class Builder>
inline constexpr bool is_builtin_view<ForEach<Range, KeyFunction, Builder>> = true;

template<class T>
concept View = is_builtin_view<std::remove_cvref_t<T>> || Component<std::remove_cvref_t<T>>;

namespace detail {

template<class V>
[[nodiscard]] constexpr std::string_view view_name() {
    if constexpr (std::same_as<std::remove_cvref_t<V>, Text>) {
        return "Text";
    } else if constexpr (std::same_as<std::remove_cvref_t<V>, Image>) {
        return "Image";
    } else if constexpr (requires { typename std::remove_cvref_t<V>::dui_never_exists; }) {
        return "Unknown";
    } else {
        return "Component";
    }
}

template<class... Children>
void update_view(Element&, const VStack<Children...>&, BuildOwner&);

template<class... Children>
void update_view(Element&, const HStack<Children...>&, BuildOwner&);

template<class... Children>
void update_view(Element&, const Stack<Children...>&, BuildOwner&);

template<class... Children>
void update_view(Element&, const Fragment<Children...>&, BuildOwner&);

template<class Child, class... Values>
void update_view(Element&, const EnvironmentScope<Child, Values...>&, BuildOwner&);

template<class Child>
void update_view(Element&, const Padding<Child>&, BuildOwner&);

template<class Child>
void update_view(Element&, const ColoredBox<Child>&, BuildOwner&);

template<class Child>
void update_view(Element&, const RepaintBoundary<Child>&, BuildOwner&);

template<class Child>
void update_view(Element&, const GestureDetector<Child>&, BuildOwner&);

template<class Child>
void update_view(Element&, const FocusView<Child>&, BuildOwner&);

template<class V>
void update_view(Element&, const Optional<V>&, BuildOwner&);

template<class Left, class Right>
void update_view(Element&, const Choice<Left, Right>&, BuildOwner&);

template<class Range, class KeyFunction, class Builder>
void update_view(Element&, const ForEach<Range, KeyFunction, Builder>&, BuildOwner&);

template<class V>
[[nodiscard]] std::string debug_name(const V&) {
    if constexpr (std::same_as<std::remove_cvref_t<V>, Text>) {
        return "Text";
    } else if constexpr (std::same_as<std::remove_cvref_t<V>, Image>) {
        return "Image";
    } else if constexpr (requires { typename std::tuple_size<decltype(std::declval<V>().children)>; }) {
        return "Group";
    } else if constexpr (Component<V>) {
        return "Component";
    } else {
        return "View";
    }
}

template<class... Children>
[[nodiscard]] std::string debug_name(const VStack<Children...>&) {
    return "VStack";
}

template<class... Children>
[[nodiscard]] std::string debug_name(const HStack<Children...>&) {
    return "HStack";
}

template<class... Children>
[[nodiscard]] std::string debug_name(const Stack<Children...>&) {
    return "Stack";
}

template<class... Children>
[[nodiscard]] std::string debug_name(const Fragment<Children...>&) {
    return "Fragment";
}

template<class Child, class... Values>
[[nodiscard]] std::string debug_name(const EnvironmentScope<Child, Values...>&) {
    return "Environment";
}

template<class Child>
[[nodiscard]] std::string debug_name(const Padding<Child>&) {
    return "Padding";
}

template<class Child>
[[nodiscard]] std::string debug_name(const ColoredBox<Child>&) {
    return "ColoredBox";
}

template<class Child>
[[nodiscard]] std::string debug_name(const RepaintBoundary<Child>&) {
    return "RepaintBoundary";
}

template<class Child>
[[nodiscard]] std::string debug_name(const GestureDetector<Child>&) {
    return "GestureDetector";
}

template<class Child>
[[nodiscard]] std::string debug_name(const FocusView<Child>&) {
    return "Focus";
}

template<class V>
[[nodiscard]] std::string debug_name(const Optional<V>&) {
    return "Optional";
}

template<class Left, class Right>
[[nodiscard]] std::string debug_name(const Choice<Left, Right>&) {
    return "Choice";
}

template<class Range, class KeyFunction, class Builder>
[[nodiscard]] std::string debug_name(const ForEach<Range, KeyFunction, Builder>&) {
    return "ForEach";
}

} // namespace detail

namespace detail {

inline void update_view(Element& element, const Text& text, BuildOwner& owner) {
    ElementAccess::debug_value(element) = text.value;
    auto& render_text = ElementAccess::ensure_render_object<RenderText>(
        element,
        owner,
        text.value,
        text.on_activate
    );
    render_text.set_text(text.value);
    render_text.set_on_activate(text.on_activate);
    auto& children = ElementAccess::children(element);
    while (!children.empty()) {
        ElementAccess::unmount(owner, children.back());
        children.pop_back();
    }
}

inline void update_view(Element& element, const Image& image, BuildOwner& owner) {
    ElementAccess::debug_value(element) = image.asset;
    ElementAccess::ensure_render_object<RenderImage>(
        element,
        owner,
        image.asset,
        image.intrinsic_size
    ).set_image(image.asset, image.intrinsic_size);
    auto& children = ElementAccess::children(element);
    while (!children.empty()) {
        ElementAccess::unmount(owner, children.back());
        children.pop_back();
    }
}

template<class Tuple>
void update_static_children(Element& element, const Tuple& tuple, BuildOwner& owner) {
    auto& children = ElementAccess::children(element);
    std::size_t index = 0;
    std::apply([&](const auto&... child) {
        ([&] {
            if (children.size() <= index) {
                children.push_back(nullptr);
            }
            reconcile_child(children[index], child, owner, &element, Key{});
            ++index;
        }(), ...);
    }, tuple);

    while (children.size() > index) {
        ElementAccess::unmount(owner, children.back());
        children.pop_back();
    }
}

template<class... Children>
void update_view(Element& element, const VStack<Children...>& view, BuildOwner& owner) {
    static_cast<void>(ElementAccess::ensure_render_object<RenderVStack>(element, owner));
    update_static_children(element, view.children, owner);
}

template<class... Children>
void update_view(Element& element, const HStack<Children...>& view, BuildOwner& owner) {
    static_cast<void>(ElementAccess::ensure_render_object<RenderHStack>(element, owner));
    update_static_children(element, view.children, owner);
}

template<class... Children>
void update_view(Element& element, const Stack<Children...>& view, BuildOwner& owner) {
    static_cast<void>(ElementAccess::ensure_render_object<RenderStack>(element, owner));
    update_static_children(element, view.children, owner);
}

template<class... Children>
void update_view(Element& element, const Fragment<Children...>& view, BuildOwner& owner) {
    update_static_children(element, view.children, owner);
}

template<class Child, class... Values>
void update_view(
    Element& element,
    const EnvironmentScope<Child, Values...>& view,
    BuildOwner& owner
) {
    std::apply([&](const auto&... value) {
        (ElementAccess::set_environment(element, value), ...);
    }, view.values);

    auto& children = ElementAccess::children(element);
    if (children.empty()) {
        children.push_back(nullptr);
    }
    reconcile_child(children.front(), view.child, owner, &element, Key{});
    while (children.size() > 1) {
        ElementAccess::unmount(owner, children.back());
        children.pop_back();
    }
}

template<class Child>
void update_view(Element& element, const Padding<Child>& view, BuildOwner& owner) {
    ElementAccess::ensure_render_object<RenderPadding>(element, owner, view.insets)
        .set_insets(view.insets);
    auto& children = ElementAccess::children(element);
    if (children.empty()) {
        children.push_back(nullptr);
    }
    reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template<class Child>
void update_view(Element& element, const ColoredBox<Child>& view, BuildOwner& owner) {
    ElementAccess::ensure_render_object<RenderColoredBox>(element, owner, view.color)
        .set_color(view.color);
    auto& children = ElementAccess::children(element);
    if (children.empty()) {
        children.push_back(nullptr);
    }
    reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template<class Child>
void update_view(Element& element, const RepaintBoundary<Child>& view, BuildOwner& owner) {
    ElementAccess::ensure_render_object<RenderRepaintBoundary>(element, owner);
    auto& children = ElementAccess::children(element);
    if (children.empty()) {
        children.push_back(nullptr);
    }
    reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template<class Child>
void update_view(Element& element, const GestureDetector<Child>& view, BuildOwner& owner) {
    auto& action = ElementAccess::ensure_render_object<RenderActionBox>(
        element,
        owner,
        view.on_tap,
        false
    );
    action.set_on_activate(view.on_tap);
    action.set_stops_propagation(false);
    auto& children = ElementAccess::children(element);
    if (children.empty()) {
        children.push_back(nullptr);
    }
    reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template<class Child>
void update_view(Element& element, const FocusView<Child>& view, BuildOwner& owner) {
    const bool is_new_focus_node = !ElementAccess::has_focus_node(element);
    const std::shared_ptr<FocusNode> focus_node = ElementAccess::ensure_focus_node(
        element,
        owner,
        view.on_key,
        view.can_focus
    );
    std::weak_ptr<FocusNode> weak_focus = focus_node;
    auto request_focus = [weak_focus] {
        if (const auto node = weak_focus.lock()) {
            node->request_focus();
        }
    };
    auto& action = ElementAccess::ensure_render_object<RenderActionBox>(
        element,
        owner,
        request_focus,
        true
    );
    action.set_on_activate(std::move(request_focus));
    action.set_stops_propagation(true);
    if (is_new_focus_node && view.autofocus && view.can_focus) {
        focus_node->request_focus();
    }

    auto& children = ElementAccess::children(element);
    if (children.empty()) {
        children.push_back(nullptr);
    }
    reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template<class V>
void update_view(Element& element, const Optional<V>& view, BuildOwner& owner) {
    auto& children = ElementAccess::children(element);
    if (!view.child.has_value()) {
        if (!children.empty()) {
            ElementAccess::unmount(owner, children.front());
            children.clear();
        }
        return;
    }

    if (children.empty()) {
        children.push_back(nullptr);
    }
    reconcile_child(children.front(), *view.child, owner, &element, Key{});
}

template<class Left, class Right>
void update_view(Element& element, const Choice<Left, Right>& view, BuildOwner& owner) {
    auto& children = ElementAccess::children(element);
    auto& active_branch = ElementAccess::active_branch(element);
    if (active_branch.has_value() && *active_branch != view.child.index() && !children.empty()) {
        ElementAccess::unmount(owner, children.front());
        children.clear();
    }
    active_branch = view.child.index();
    if (children.empty()) {
        children.push_back(nullptr);
    }
    std::visit([&](const auto& child) {
        reconcile_child(children.front(), child, owner, &element, Key{});
    }, view.child);
}

template<class Range, class KeyFunction, class Builder>
void update_view(
    Element& element,
    const ForEach<Range, KeyFunction, Builder>& view,
    BuildOwner& owner
) {
    using ItemReference = std::ranges::range_reference_t<const Range>;
    using Child = std::decay_t<std::invoke_result_t<const Builder&, ItemReference>>;

    struct PendingChild {
        Key key;
        Child view;
    };

    std::vector<PendingChild> pending;

    for (const auto& item : view.items) {
        Key item_key = make_key(std::invoke(view.key_function, item));
        const auto duplicate = std::ranges::find_if(pending, [&](const PendingChild& child) {
            return child.key == item_key;
        });
        if (duplicate != pending.end()) {
            throw std::logic_error("ForEach contains a duplicate key: " + item_key.to_string());
        }
        pending.push_back({
            std::move(item_key),
            std::invoke(view.builder, item)
        });
    }

    std::vector<std::unique_ptr<Element>> next;
    next.reserve(pending.size());
    auto& element_children = ElementAccess::children(element);
    std::vector<std::unique_ptr<Element>> previous = std::move(element_children);

    for (const auto& pending_child : pending) {
        auto found = std::ranges::find_if(previous, [&](const auto& candidate) {
            return candidate != nullptr
                && candidate->key() == pending_child.key
                && candidate->view_type() == type_token<Child>();
        });

        std::unique_ptr<Element> child;
        if (found != previous.end()) {
            child = std::move(*found);
        }
        try {
            reconcile_child(child, pending_child.view, owner, &element, pending_child.key);
        } catch (...) {
            ElementAccess::unmount(owner, child);
            for (auto& remaining : previous) {
                ElementAccess::unmount(owner, remaining);
            }
            for (auto& mounted : next) {
                ElementAccess::unmount(owner, mounted);
            }
            element_children.clear();
            throw;
        }
        next.push_back(std::move(child));
    }

    for (auto& child : previous) {
        ElementAccess::unmount(owner, child);
    }
    element_children = std::move(next);
}

template<Component V>
void update_view(Element& element, const V& component, BuildOwner& owner) {
    auto& descriptor = ElementAccess::descriptor(element);
    if constexpr (std::equality_comparable<V>) {
        const bool equivalent = descriptor.has_value()
            && std::any_cast<const V&>(descriptor) == component;
        descriptor = component;
        if (equivalent && !ElementAccess::rebuilding_dirty(element)) {
            return;
        }
    } else {
        descriptor = component;
    }
    ElementAccess::set_rebuild(element, [](Element& current, BuildOwner& build_owner) {
        ElementAccess::clear_dependencies(build_owner, current);
        BuildContext context{build_owner, current};
        BuildScope scope{context};
        const auto& value = std::any_cast<const V&>(ElementAccess::descriptor(current));
        auto child = value.build(context);
        static_assert(View<decltype(child)>, "Component::build must return a DUI View value");

        auto& children = ElementAccess::children(current);
        if (children.empty()) {
            children.push_back(nullptr);
        }
        reconcile_child(children.front(), child, build_owner, &current, Key{});
        while (children.size() > 1) {
            ElementAccess::unmount(build_owner, children.back());
            children.pop_back();
        }
    });
    ElementAccess::rebuild(element, owner);
}

} // namespace detail

template<class V>
void reconcile_child(
    std::unique_ptr<Element>& slot,
    const V& view,
    BuildOwner& owner,
    Element* parent,
    Key key
) {
    using View = std::remove_cvref_t<V>;
    const bool reusable = slot != nullptr
        && slot->view_type_ == type_token<View>()
        && slot->key_ == key;

    if (!reusable) {
        owner.unmount(slot);
        slot = owner.make_element(
            type_token<View>(),
            std::move(key),
            detail::debug_name(view),
            parent
        );
    }

    ++slot->update_count_;
    owner.dirty_.erase(slot->id_);
    slot->rebuilding_dirty_ = slot->dirty_;
    slot->dirty_ = false;
    try {
        detail::update_view(*slot, view, owner);
        slot->rebuilding_dirty_ = false;
    } catch (...) {
        slot->rebuilding_dirty_ = false;
        throw;
    }
}

template<class V>
void BuildOwner::render(const V& view) {
    static_assert(View<V>, "BuildOwner::render requires a DUI View or Component");
    if (reconciling_) {
        throw std::logic_error("BuildOwner does not allow reentrant render or flush");
    }
    reconciling_ = true;
    try {
        reconcile_child(root_, view, *this, nullptr, Key{});
        reconciling_ = false;
    } catch (...) {
        reconciling_ = false;
        throw;
    }
}

} // namespace dui
