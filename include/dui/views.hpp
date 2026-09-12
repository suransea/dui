#pragma once

#include "dui/runtime.hpp"

#include <cmath>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace dui {

struct Text {
  std::string value;
  std::function<void()> on_activate;

  explicit Text(std::string text, std::function<void()> callback = {})
    : value(std::move(text)), on_activate(std::move(callback)) {}
};

struct Image {
  std::string asset;
  Size intrinsic_size;
};

template <class... Children> struct VStack {
  std::tuple<Children...> children;

  explicit VStack(Children... values) : children(std::move(values)...) {}
};

template <class... Children> VStack(Children...) -> VStack<Children...>;

template <class... Children> struct HStack {
  std::tuple<Children...> children;

  explicit HStack(Children... values) : children(std::move(values)...) {}
};

template <class... Children> HStack(Children...) -> HStack<Children...>;

template <class... Children> struct Stack {
  std::tuple<Children...> children;

  explicit Stack(Children... values) : children(std::move(values)...) {}
};

template <class... Children> Stack(Children...) -> Stack<Children...>;

template <class... Slivers> struct Viewport {
  double scroll_offset;
  std::tuple<Slivers...> children;

  explicit Viewport(double offset, Slivers... values)
    : scroll_offset(offset), children(std::move(values)...) {}
};

template <class... Slivers> Viewport(double, Slivers...) -> Viewport<Slivers...>;

template <class Child> struct SliverToBoxAdapter {
  Child child;

  explicit SliverToBoxAdapter(Child value) : child(std::move(value)) {}
};

template <class Child> SliverToBoxAdapter(Child) -> SliverToBoxAdapter<Child>;

class SliverCacheExtent {
public:
  explicit SliverCacheExtent(double value) : value_(value) {
    if (!std::isfinite(value) || value < 0.0) {
      throw std::invalid_argument("Sliver cache extent must be finite and non-negative");
    }
  }

  [[nodiscard]] double value() const { return value_; }

private:
  double value_;
};

[[nodiscard]] inline SliverCacheExtent cache_extent(double value) {
  return SliverCacheExtent{value};
}

template <class... Children> struct SliverFixedExtentList {
  double item_extent;
  std::tuple<Children...> children;

  explicit SliverFixedExtentList(double extent, Children... values)
    : item_extent(extent), children(std::move(values)...) {}

  explicit SliverFixedExtentList(double extent, SliverCacheExtent cache, Children... values)
    : item_extent(extent), children(std::move(values)...), cache_extent_(cache.value()) {}

  [[nodiscard]] double cache_extent() const { return cache_extent_; }

private:
  double cache_extent_{};
};

template <class... Children>
SliverFixedExtentList(double, Children...) -> SliverFixedExtentList<Children...>;

template <class... Children>
SliverFixedExtentList(double, SliverCacheExtent, Children...) -> SliverFixedExtentList<Children...>;

template <class... Children> struct Fragment {
  std::tuple<Children...> children;

  explicit Fragment(Children... values) : children(std::move(values)...) {}
};

template <class... Children> Fragment(Children...) -> Fragment<Children...>;

template <class Child, class... Values> struct EnvironmentScope {
  Child child;
  std::tuple<Values...> values;

  EnvironmentScope(Child child_value, Values... environment_values)
    : child(std::move(child_value)), values(std::move(environment_values)...) {}
};

template <class Child, class... Values>
EnvironmentScope(Child, Values...) -> EnvironmentScope<Child, Values...>;

namespace detail {

template <class...> inline constexpr bool unique_types = true;

template <class First, class... Rest>
inline constexpr bool unique_types<First, Rest...> =
  (!std::same_as<First, Rest> && ...) && unique_types<Rest...>;

} // namespace detail

template <class Child, class... Values>
[[nodiscard]] auto with_environment(Child&& child, Values&&... values) {
  static_assert(detail::unique_types<std::decay_t<Values>...>,
                "An EnvironmentScope cannot provide the same type more than once");
  return EnvironmentScope<std::decay_t<Child>, std::decay_t<Values>...>{
    std::forward<Child>(child), std::forward<Values>(values)...};
}

template <class Child> struct Padding {
  Insets insets;
  Child child;
};

template <class Child> Padding(Insets, Child) -> Padding<Child>;

template <class Child> [[nodiscard]] auto padding(Child&& child, Insets insets) {
  return Padding<std::decay_t<Child>>{insets, std::forward<Child>(child)};
}

template <class Child> struct ColoredBox {
  std::uint32_t color;
  Child child;
};

template <class Child> ColoredBox(std::uint32_t, Child) -> ColoredBox<Child>;

template <class Child> [[nodiscard]] auto background(Child&& child, std::uint32_t color) {
  return ColoredBox<std::decay_t<Child>>{color, std::forward<Child>(child)};
}

template <class Child> struct RepaintBoundary {
  Child child;
};

template <class Child> RepaintBoundary(Child) -> RepaintBoundary<Child>;

template <class Child> [[nodiscard]] auto repaint_boundary(Child&& child) {
  return RepaintBoundary<std::decay_t<Child>>{std::forward<Child>(child)};
}

template <class Child> struct GestureDetector {
  Child child;
  std::function<void()> on_tap;
};

template <class Child> GestureDetector(Child, std::function<void()>) -> GestureDetector<Child>;

template <class Child, class Callback>
[[nodiscard]] auto on_tap(Child&& child, Callback&& callback) {
  return GestureDetector<std::decay_t<Child>>{
    std::forward<Child>(child), std::function<void()>{std::forward<Callback>(callback)}};
}

template <class Child> struct FocusView {
  Child child;
  FocusNode::KeyHandler on_key;
  bool can_focus{true};
  bool autofocus{};
};

template <class Child> FocusView(Child, FocusNode::KeyHandler, bool, bool) -> FocusView<Child>;

template <class Child>
[[nodiscard]] auto focusable(Child&& child, FocusNode::KeyHandler on_key = {},
                             bool autofocus = false) {
  return FocusView<std::decay_t<Child>>{std::forward<Child>(child), std::move(on_key), true,
                                        autofocus};
}

template <class V> struct Optional {
  std::optional<V> child;
};

template <class Builder> [[nodiscard]] auto optional(bool condition, Builder&& builder) {
  using V = std::decay_t<std::invoke_result_t<Builder>>;
  if (condition) {
    return Optional<V>{std::invoke(std::forward<Builder>(builder))};
  }
  return Optional<V>{std::nullopt};
}

template <class Left, class Right> struct Choice {
  std::variant<Left, Right> child;
};

template <class LeftBuilder, class RightBuilder>
[[nodiscard]] auto choose(bool condition, LeftBuilder&& left_builder,
                          RightBuilder&& right_builder) {
  using Left = std::decay_t<std::invoke_result_t<LeftBuilder>>;
  using Right = std::decay_t<std::invoke_result_t<RightBuilder>>;
  using Result = Choice<Left, Right>;

  if (condition) {
    return Result{std::variant<Left, Right>{std::in_place_index<0>,
                                            std::invoke(std::forward<LeftBuilder>(left_builder))}};
  }
  return Result{std::variant<Left, Right>{std::in_place_index<1>,
                                          std::invoke(std::forward<RightBuilder>(right_builder))}};
}

template <class Range, class KeyFunction, class Builder> struct ForEach {
  Range items;
  KeyFunction key_function;
  Builder builder;

  template <class R, class K, class B>
  ForEach(R&& range, K&& key_fn, B&& child_builder)
    : items(std::forward<R>(range)), key_function(std::forward<K>(key_fn)),
      builder(std::forward<B>(child_builder)) {}
};

template <class R, class K, class B>
ForEach(R&&, K&&, B&&) -> ForEach<std::decay_t<R>, std::decay_t<K>, std::decay_t<B>>;

struct NeverKeepAlive {
  template <class Item> [[nodiscard]] constexpr bool operator()(const Item&) const { return false; }
};

template <class Item, class KeyFunction, class Builder, class KeepAlive = NeverKeepAlive>
class LazyForEach {
public:
  template <class Predicate> [[nodiscard]] auto keep_alive_when(Predicate&& predicate) && {
    using Policy = std::decay_t<Predicate>;
    static_assert(std::predicate<const Policy&, const Item&>,
                  "Lazy Sliver keep-alive policy must be a predicate for const items");
    Policy policy{std::forward<Predicate>(predicate)};
    std::unordered_set<Key, detail::KeyHash> keep_alive_keys;
    keep_alive_keys.reserve(items_.size());
    for (std::size_t index = 0; index < items_.size(); ++index) {
      if (std::invoke(policy, items_[index])) {
        keep_alive_keys.insert(keys_[index]);
      }
    }
    return LazyForEach<Item, KeyFunction, Builder, std::decay_t<Predicate>>{
      std::move(items_), std::move(keys_), std::move(builder_), std::move(keep_alive_keys)};
  }

  [[nodiscard]] const std::vector<Item>& items() const { return items_; }
  [[nodiscard]] const std::vector<Key>& keys() const { return keys_; }
  [[nodiscard]] const Builder& builder() const { return builder_; }
  [[nodiscard]] const std::unordered_set<Key, detail::KeyHash>& keep_alive_keys() const {
    return keep_alive_keys_;
  }

private:
  template <class, class, class, class> friend class LazyForEach;
  template <std::ranges::input_range Range, class K, class B>
  friend auto lazy_for_each(Range&&, K&&, B&&);

  LazyForEach(std::vector<Item> items, std::vector<Key> keys, Builder builder,
              std::unordered_set<Key, detail::KeyHash> keep_alive_keys)
    : items_(std::move(items)), keys_(std::move(keys)), builder_(std::move(builder)),
      keep_alive_keys_(std::move(keep_alive_keys)) {}

  std::vector<Item> items_;
  std::vector<Key> keys_;
  Builder builder_;
  std::unordered_set<Key, detail::KeyHash> keep_alive_keys_;
};

template <std::ranges::input_range Range, class KeyFunction, class Builder>
[[nodiscard]] auto lazy_for_each(Range&& range, KeyFunction&& key_function, Builder&& builder) {
  using Item = std::ranges::range_value_t<Range>;
  std::vector<Item> items;
  if constexpr (std::ranges::sized_range<Range>) {
    items.reserve(std::ranges::size(range));
  }
  if constexpr (std::is_lvalue_reference_v<Range&&>) {
    for (const auto& item : range) {
      items.push_back(item);
    }
  } else {
    auto iterator = std::ranges::begin(range);
    const auto end = std::ranges::end(range);
    for (; iterator != end; ++iterator) {
      items.push_back(std::ranges::iter_move(iterator));
    }
  }

  std::vector<Key> keys;
  keys.reserve(items.size());
  std::unordered_set<Key, detail::KeyHash> unique_keys;
  unique_keys.reserve(items.size());
  for (const auto& item : items) {
    Key key = make_key(std::invoke(key_function, item));
    if (!unique_keys.insert(key).second) {
      throw std::logic_error("ForEach contains a duplicate key: " + key.to_string());
    }
    keys.push_back(std::move(key));
  }
  return LazyForEach<Item, std::decay_t<KeyFunction>, std::decay_t<Builder>>{
    std::move(items), std::move(keys), std::forward<Builder>(builder), {}};
}

template <class T>
concept Component =
  std::copy_constructible<T> && requires(const T& value, BuildContext& context) {
                                  value.build(context);
                                  requires(!std::is_void_v<decltype(value.build(context))>);
                                };

template <class> inline constexpr bool is_builtin_view = false;

template <> inline constexpr bool is_builtin_view<Text> = true;

template <> inline constexpr bool is_builtin_view<Image> = true;

template <class... Children> inline constexpr bool is_builtin_view<VStack<Children...>> = true;

template <class... Children> inline constexpr bool is_builtin_view<HStack<Children...>> = true;

template <class... Children> inline constexpr bool is_builtin_view<Stack<Children...>> = true;

template <class... Slivers> inline constexpr bool is_builtin_view<Viewport<Slivers...>> = true;

template <class Child> inline constexpr bool is_builtin_view<SliverToBoxAdapter<Child>> = true;

template <class... Children>
inline constexpr bool is_builtin_view<SliverFixedExtentList<Children...>> = true;

template <class... Children> inline constexpr bool is_builtin_view<Fragment<Children...>> = true;

template <class Child, class... Values>
inline constexpr bool is_builtin_view<EnvironmentScope<Child, Values...>> = true;

template <class Child> inline constexpr bool is_builtin_view<Padding<Child>> = true;

template <class Child> inline constexpr bool is_builtin_view<ColoredBox<Child>> = true;

template <class Child> inline constexpr bool is_builtin_view<RepaintBoundary<Child>> = true;

template <class Child> inline constexpr bool is_builtin_view<GestureDetector<Child>> = true;

template <class Child> inline constexpr bool is_builtin_view<FocusView<Child>> = true;

template <class V> inline constexpr bool is_builtin_view<Optional<V>> = true;

template <class Left, class Right>
inline constexpr bool is_builtin_view<Choice<Left, Right>> = true;

template <class Range, class KeyFunction, class Builder>
inline constexpr bool is_builtin_view<ForEach<Range, KeyFunction, Builder>> = true;

template <class T>
concept View = is_builtin_view<std::remove_cvref_t<T>> || Component<std::remove_cvref_t<T>>;

namespace detail {

template <class T> struct ViewProtocol {
  static constexpr bool box = false;
  static constexpr bool sliver = false;
};

template <> struct ViewProtocol<Text> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <> struct ViewProtocol<Image> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class... C> struct ViewProtocol<VStack<C...>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class... C> struct ViewProtocol<HStack<C...>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class... C> struct ViewProtocol<Stack<C...>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class... S> struct ViewProtocol<Viewport<S...>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class C> struct ViewProtocol<SliverToBoxAdapter<C>> {
  static constexpr bool box = false;
  static constexpr bool sliver = true;
};
template <class... C> struct ViewProtocol<SliverFixedExtentList<C...>> {
  static constexpr bool box = false;
  static constexpr bool sliver = true;
};
template <class C> struct ViewProtocol<Padding<C>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class C> struct ViewProtocol<ColoredBox<C>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class C> struct ViewProtocol<RepaintBoundary<C>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class C> struct ViewProtocol<GestureDetector<C>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};
template <class C> struct ViewProtocol<FocusView<C>> {
  static constexpr bool box = true;
  static constexpr bool sliver = false;
};

template <class T> consteval bool has_box_protocol();

template <class T> consteval bool has_sliver_protocol();

template <class T> consteval bool has_single_box_protocol();

template <class T> struct SingleBoxProtocol {
  static constexpr bool value = false;
};

template <> struct SingleBoxProtocol<Text> {
  static constexpr bool value = true;
};
template <> struct SingleBoxProtocol<Image> {
  static constexpr bool value = true;
};
template <class... C> struct SingleBoxProtocol<VStack<C...>> {
  static constexpr bool value = true;
};
template <class... C> struct SingleBoxProtocol<HStack<C...>> {
  static constexpr bool value = true;
};
template <class... C> struct SingleBoxProtocol<Stack<C...>> {
  static constexpr bool value = true;
};
template <class... S> struct SingleBoxProtocol<Viewport<S...>> {
  static constexpr bool value = true;
};
template <class C> struct SingleBoxProtocol<Padding<C>> {
  static constexpr bool value = true;
};
template <class C> struct SingleBoxProtocol<ColoredBox<C>> {
  static constexpr bool value = true;
};
template <class C> struct SingleBoxProtocol<RepaintBoundary<C>> {
  static constexpr bool value = true;
};
template <class C> struct SingleBoxProtocol<GestureDetector<C>> {
  static constexpr bool value = true;
};
template <class C> struct SingleBoxProtocol<FocusView<C>> {
  static constexpr bool value = true;
};
template <class C, class... V> struct SingleBoxProtocol<EnvironmentScope<C, V...>> {
  static constexpr bool value = has_single_box_protocol<C>();
};
template <class... C> struct SingleBoxProtocol<Fragment<C...>> {
  static constexpr bool value = sizeof...(C) == 1 && (has_single_box_protocol<C>() && ...);
};
template <class L, class R> struct SingleBoxProtocol<Choice<L, R>> {
  static constexpr bool value = has_single_box_protocol<L>() && has_single_box_protocol<R>();
};

template <class... C> struct ViewProtocol<Fragment<C...>> {
  static constexpr bool box = (has_box_protocol<C>() && ...);
  static constexpr bool sliver = (has_sliver_protocol<C>() && ...);
};

template <class C, class... V> struct ViewProtocol<EnvironmentScope<C, V...>> {
  static constexpr bool box = has_box_protocol<C>();
  static constexpr bool sliver = has_sliver_protocol<C>();
};

template <class V> struct ViewProtocol<Optional<V>> {
  static constexpr bool box = has_box_protocol<V>();
  static constexpr bool sliver = has_sliver_protocol<V>();
};

template <class L, class R> struct ViewProtocol<Choice<L, R>> {
  static constexpr bool box = has_box_protocol<L>() && has_box_protocol<R>();
  static constexpr bool sliver = has_sliver_protocol<L>() && has_sliver_protocol<R>();
};

template <class Range, class KeyFunction, class Builder>
struct ViewProtocol<ForEach<Range, KeyFunction, Builder>> {
  using Child =
    std::decay_t<std::invoke_result_t<const Builder&, std::ranges::range_reference_t<const Range>>>;
  static constexpr bool box = has_box_protocol<Child>();
  static constexpr bool sliver = has_sliver_protocol<Child>();
};

template <class T> consteval bool has_box_protocol() {
  using V = std::remove_cvref_t<T>;
  if constexpr (Component<V>) {
    using Child = decltype(std::declval<const V&>().build(std::declval<BuildContext&>()));
    return has_box_protocol<Child>();
  } else {
    return ViewProtocol<V>::box;
  }
}

template <class T> consteval bool has_sliver_protocol() {
  using V = std::remove_cvref_t<T>;
  if constexpr (Component<V>) {
    using Child = decltype(std::declval<const V&>().build(std::declval<BuildContext&>()));
    return has_sliver_protocol<Child>();
  } else {
    return ViewProtocol<V>::sliver;
  }
}

template <class T> consteval bool has_single_box_protocol() {
  using V = std::remove_cvref_t<T>;
  if constexpr (Component<V>) {
    using Child = decltype(std::declval<const V&>().build(std::declval<BuildContext&>()));
    return has_single_box_protocol<Child>();
  } else {
    return SingleBoxProtocol<V>::value;
  }
}

template <class V> [[nodiscard]] constexpr std::string_view view_name() {
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

template <class... Children> void update_view(Element&, const VStack<Children...>&, BuildOwner&);

template <class... Children> void update_view(Element&, const HStack<Children...>&, BuildOwner&);

template <class... Children> void update_view(Element&, const Stack<Children...>&, BuildOwner&);

template <class... Slivers> void update_view(Element&, const Viewport<Slivers...>&, BuildOwner&);

template <class Child> void update_view(Element&, const SliverToBoxAdapter<Child>&, BuildOwner&);

template <class... Children>
void update_view(Element&, const SliverFixedExtentList<Children...>&, BuildOwner&);

template <class Item, class KeyFunction, class Builder, class KeepAlive>
void update_view(Element&,
                 const SliverFixedExtentList<LazyForEach<Item, KeyFunction, Builder, KeepAlive>>&,
                 BuildOwner&);

template <class... Children> void update_view(Element&, const Fragment<Children...>&, BuildOwner&);

template <class Child, class... Values>
void update_view(Element&, const EnvironmentScope<Child, Values...>&, BuildOwner&);

template <class Child> void update_view(Element&, const Padding<Child>&, BuildOwner&);

template <class Child> void update_view(Element&, const ColoredBox<Child>&, BuildOwner&);

template <class Child> void update_view(Element&, const RepaintBoundary<Child>&, BuildOwner&);

template <class Child> void update_view(Element&, const GestureDetector<Child>&, BuildOwner&);

template <class Child> void update_view(Element&, const FocusView<Child>&, BuildOwner&);

template <class V> void update_view(Element&, const Optional<V>&, BuildOwner&);

template <class Left, class Right>
void update_view(Element&, const Choice<Left, Right>&, BuildOwner&);

template <class Range, class KeyFunction, class Builder>
void update_view(Element&, const ForEach<Range, KeyFunction, Builder>&, BuildOwner&);

template <class V> [[nodiscard]] std::string debug_name(const V&) {
  if constexpr (std::same_as<std::remove_cvref_t<V>, Text>) {
    return "Text";
  } else if constexpr (std::same_as<std::remove_cvref_t<V>, Image>) {
    return "Image";
  } else if constexpr (requires {
                         typename std::tuple_size<decltype(std::declval<V>().children)>;
                       }) {
    return "Group";
  } else if constexpr (Component<V>) {
    return "Component";
  } else {
    return "View";
  }
}

template <class... Children> [[nodiscard]] std::string debug_name(const VStack<Children...>&) {
  return "VStack";
}

template <class... Children> [[nodiscard]] std::string debug_name(const HStack<Children...>&) {
  return "HStack";
}

template <class... Children> [[nodiscard]] std::string debug_name(const Stack<Children...>&) {
  return "Stack";
}

template <class... Slivers> [[nodiscard]] std::string debug_name(const Viewport<Slivers...>&) {
  return "Viewport";
}

template <class Child> [[nodiscard]] std::string debug_name(const SliverToBoxAdapter<Child>&) {
  return "SliverToBoxAdapter";
}

template <class... Children>
[[nodiscard]] std::string debug_name(const SliverFixedExtentList<Children...>&) {
  return "SliverFixedExtentList";
}

template <class... Children> [[nodiscard]] std::string debug_name(const Fragment<Children...>&) {
  return "Fragment";
}

template <class Child, class... Values>
[[nodiscard]] std::string debug_name(const EnvironmentScope<Child, Values...>&) {
  return "Environment";
}

template <class Child> [[nodiscard]] std::string debug_name(const Padding<Child>&) {
  return "Padding";
}

template <class Child> [[nodiscard]] std::string debug_name(const ColoredBox<Child>&) {
  return "ColoredBox";
}

template <class Child> [[nodiscard]] std::string debug_name(const RepaintBoundary<Child>&) {
  return "RepaintBoundary";
}

template <class Child> [[nodiscard]] std::string debug_name(const GestureDetector<Child>&) {
  return "GestureDetector";
}

template <class Child> [[nodiscard]] std::string debug_name(const FocusView<Child>&) {
  return "Focus";
}

template <class V> [[nodiscard]] std::string debug_name(const Optional<V>&) { return "Optional"; }

template <class Left, class Right>
[[nodiscard]] std::string debug_name(const Choice<Left, Right>&) {
  return "Choice";
}

template <class Range, class KeyFunction, class Builder>
[[nodiscard]] std::string debug_name(const ForEach<Range, KeyFunction, Builder>&) {
  return "ForEach";
}

} // namespace detail

namespace detail {

inline void update_view(Element& element, const Text& text, BuildOwner& owner) {
  ElementAccess::debug_value(element) = text.value;
  auto& render_text =
    ElementAccess::ensure_render_object<RenderText>(element, owner, text.value, text.on_activate);
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
  ElementAccess::ensure_render_object<RenderImage>(element, owner, image.asset,
                                                   image.intrinsic_size)
    .set_image(image.asset, image.intrinsic_size);
  auto& children = ElementAccess::children(element);
  while (!children.empty()) {
    ElementAccess::unmount(owner, children.back());
    children.pop_back();
  }
}

template <class Tuple>
void update_static_children(Element& element, const Tuple& tuple, BuildOwner& owner) {
  auto& children = ElementAccess::children(element);
  std::size_t index = 0;
  std::apply(
    [&](const auto&... child) {
      (
        [&] {
          if (children.size() <= index) {
            children.push_back(nullptr);
          }
          reconcile_child(children[index], child, owner, &element, Key{});
          ++index;
        }(),
        ...);
    },
    tuple);

  while (children.size() > index) {
    ElementAccess::unmount(owner, children.back());
    children.pop_back();
  }
}

template <class... Children>
void update_view(Element& element, const VStack<Children...>& view, BuildOwner& owner) {
  static_assert((has_box_protocol<Children>() && ...), "VStack children must use the box protocol");
  static_cast<void>(ElementAccess::ensure_render_object<RenderVStack>(element, owner));
  update_static_children(element, view.children, owner);
}

template <class... Children>
void update_view(Element& element, const HStack<Children...>& view, BuildOwner& owner) {
  static_assert((has_box_protocol<Children>() && ...), "HStack children must use the box protocol");
  static_cast<void>(ElementAccess::ensure_render_object<RenderHStack>(element, owner));
  update_static_children(element, view.children, owner);
}

template <class... Children>
void update_view(Element& element, const Stack<Children...>& view, BuildOwner& owner) {
  static_assert((has_box_protocol<Children>() && ...), "Stack children must use the box protocol");
  static_cast<void>(ElementAccess::ensure_render_object<RenderStack>(element, owner));
  update_static_children(element, view.children, owner);
}

template <class... Slivers>
void update_view(Element& element, const Viewport<Slivers...>& view, BuildOwner& owner) {
  static_assert((has_sliver_protocol<Slivers>() && ...),
                "Viewport children must use the Sliver protocol");
  ElementAccess::ensure_render_object<RenderViewport>(element, owner, view.scroll_offset)
    .set_scroll_offset(view.scroll_offset);
  update_static_children(element, view.children, owner);
}

template <class Child>
void update_view(Element& element, const SliverToBoxAdapter<Child>& view, BuildOwner& owner) {
  static_assert(has_box_protocol<Child>(), "SliverToBoxAdapter child must use the box protocol");
  static_cast<void>(ElementAccess::ensure_render_object<RenderSliverToBoxAdapter>(element, owner));
  auto& children = ElementAccess::children(element);
  if (children.empty()) {
    children.push_back(nullptr);
  }
  reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template <class... Children>
void update_view(Element& element, const SliverFixedExtentList<Children...>& view,
                 BuildOwner& owner) {
  static_assert((has_box_protocol<Children>() && ...),
                "SliverFixedExtentList children must use the box protocol");
  if (!std::isfinite(view.cache_extent()) || view.cache_extent() < 0.0) {
    throw std::invalid_argument("Sliver cache extent must be finite and non-negative");
  }
  auto& render = ElementAccess::ensure_render_object<RenderSliverFixedExtentList>(element, owner,
                                                                                  view.item_extent);
  render.set_item_extent(view.item_extent);
  render.set_cache_extent(view.cache_extent());
  render.clear_lazy_model();
  update_static_children(element, view.children, owner);
}

template <class Item, class KeyFunction, class Builder, class KeepAlive>
void realize_lazy_fixed_extent_range(Element& element, BuildOwner& owner, std::size_t first,
                                     std::size_t end, std::uint64_t revision) {
  using Source = LazyForEach<Item, KeyFunction, Builder, KeepAlive>;
  using Child = std::decay_t<std::invoke_result_t<const Builder&, const Item&>>;

  const auto& source = std::any_cast<const Source&>(ElementAccess::descriptor(element));
  const auto& keys = ElementAccess::lazy_keys(element);
  if (end > keys.size() || first > end) {
    throw std::logic_error("Lazy Sliver requested an invalid child range");
  }

  struct PendingChild {
    Key key;
    Child view;
  };
  std::vector<PendingChild> pending;
  pending.reserve(end - first);
  auto item = source.items().begin() + static_cast<std::ptrdiff_t>(first);
  for (std::size_t index = first; index < end; ++index, ++item) {
    pending.push_back({keys[index], std::invoke(source.builder(), *item)});
  }

  auto& element_children = ElementAccess::children(element);
  auto& kept_alive_children = ElementAccess::lazy_kept_alive_children(element);
  std::unordered_map<Key, std::size_t, KeyHash> previous_indices;
  previous_indices.reserve(element_children.size());
  for (std::size_t index = 0; index < element_children.size(); ++index) {
    previous_indices.emplace(element_children[index]->key(), index);
  }
  std::unordered_map<Key, std::size_t, KeyHash> kept_alive_indices;
  kept_alive_indices.reserve(kept_alive_children.size());
  for (std::size_t index = 0; index < kept_alive_children.size(); ++index) {
    kept_alive_indices.emplace(kept_alive_children[index]->key(), index);
  }
  std::vector<std::unique_ptr<Element>> next;
  next.reserve(pending.size());
  std::vector<std::unique_ptr<Element>> next_kept_alive;
  next_kept_alive.reserve(element_children.size() + kept_alive_children.size());
  std::vector<std::unique_ptr<Element>> previous = std::move(element_children);
  std::vector<std::unique_ptr<Element>> previous_kept_alive = std::move(kept_alive_children);
  for (const PendingChild& pending_child : pending) {
    std::unique_ptr<Element> child;
    const auto found = previous_indices.find(pending_child.key);
    if (found != previous_indices.end() &&
        previous[found->second]->view_type() == type_token<Child>()) {
      child = std::move(previous[found->second]);
    } else {
      const auto kept_alive = kept_alive_indices.find(pending_child.key);
      if (kept_alive != kept_alive_indices.end() &&
          previous_kept_alive[kept_alive->second]->view_type() == type_token<Child>()) {
        child = std::move(previous_kept_alive[kept_alive->second]);
      }
    }
    try {
      reconcile_child(child, pending_child.view, owner, &element, pending_child.key);
    } catch (...) {
      ElementAccess::unmount(owner, child);
      for (auto& remaining : previous) {
        ElementAccess::unmount(owner, remaining);
      }
      for (auto& remaining : previous_kept_alive) {
        ElementAccess::unmount(owner, remaining);
      }
      for (auto& mounted : next) {
        ElementAccess::unmount(owner, mounted);
      }
      element_children.clear();
      kept_alive_children.clear();
      throw;
    }
    next.push_back(std::move(child));
  }
  const auto retain_or_unmount = [&](std::unique_ptr<Element>& child) {
    if (child == nullptr) {
      return;
    }
    if (ElementAccess::lazy_keep_alive_keys(element).contains(child->key())) {
      next_kept_alive.push_back(std::move(child));
    } else {
      ElementAccess::unmount(owner, child);
    }
  };
  for (auto& child : previous) {
    retain_or_unmount(child);
  }
  for (auto& child : previous_kept_alive) {
    retain_or_unmount(child);
  }
  element_children = std::move(next);
  kept_alive_children = std::move(next_kept_alive);
  auto* render = dynamic_cast<RenderSliverFixedExtentList*>(ElementAccess::render_object(element));
  if (render == nullptr) {
    throw std::logic_error("Lazy Sliver Element lost its RenderSliver");
  }
  render->set_mounted_range(first, end - first, revision);
}

template <class Item, class KeyFunction, class Builder, class KeepAlive>
void update_view(
  Element& element,
  const SliverFixedExtentList<LazyForEach<Item, KeyFunction, Builder, KeepAlive>>& view,
  BuildOwner& owner) {
  using Source = LazyForEach<Item, KeyFunction, Builder, KeepAlive>;
  static_assert(std::copy_constructible<Source>,
                "Lazy Sliver ForEach must own a copyable deferred source");
  using Child = std::decay_t<std::invoke_result_t<const Builder&, const Item&>>;
  static_assert(has_single_box_protocol<Child>(),
                "Lazy Sliver items must produce exactly one box-protocol RenderObject");
  static_assert(std::predicate<const KeepAlive&, const Item&>,
                "Lazy Sliver keep-alive policy must be a predicate for const items");
  if (!std::isfinite(view.item_extent) || view.item_extent <= 0.0) {
    throw std::invalid_argument("Sliver fixed item extent must be finite and positive");
  }
  if (!std::isfinite(view.cache_extent()) || view.cache_extent() < 0.0) {
    throw std::invalid_argument("Sliver cache extent must be finite and non-negative");
  }

  const Source& source = std::get<0>(view.children);
  std::vector<Key> keys = source.keys();
  std::unordered_set<Key, KeyHash> keep_alive_keys = source.keep_alive_keys();
  auto descriptor = std::make_any<Source>(source);
  auto& render = ElementAccess::ensure_render_object<RenderSliverFixedExtentList>(element, owner,
                                                                                  view.item_extent);
  render.set_item_extent(view.item_extent);
  render.set_cache_extent(view.cache_extent());
  const std::uint64_t revision = ElementAccess::install_lazy_model(
    element, std::move(descriptor), std::move(keys), std::move(keep_alive_keys),
    &realize_lazy_fixed_extent_range<Item, KeyFunction, Builder, KeepAlive>);
  auto& kept_alive_children = ElementAccess::lazy_kept_alive_children(element);
  std::erase_if(kept_alive_children, [&](auto& child) {
    if (ElementAccess::lazy_keep_alive_keys(element).contains(child->key())) {
      return false;
    }
    ElementAccess::unmount(owner, child);
    return true;
  });
  render.set_lazy_model(source.items().size(), revision);
}

template <class... Children>
void update_view(Element& element, const Fragment<Children...>& view, BuildOwner& owner) {
  update_static_children(element, view.children, owner);
}

template <class Child, class... Values>
void update_view(Element& element, const EnvironmentScope<Child, Values...>& view,
                 BuildOwner& owner) {
  std::apply([&](const auto&... value) { (ElementAccess::set_environment(element, value), ...); },
             view.values);

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

template <class Child>
void update_view(Element& element, const Padding<Child>& view, BuildOwner& owner) {
  static_assert(has_box_protocol<Child>(), "Padding child must use the box protocol");
  ElementAccess::ensure_render_object<RenderPadding>(element, owner, view.insets)
    .set_insets(view.insets);
  auto& children = ElementAccess::children(element);
  if (children.empty()) {
    children.push_back(nullptr);
  }
  reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template <class Child>
void update_view(Element& element, const ColoredBox<Child>& view, BuildOwner& owner) {
  static_assert(has_box_protocol<Child>(), "ColoredBox child must use the box protocol");
  ElementAccess::ensure_render_object<RenderColoredBox>(element, owner, view.color)
    .set_color(view.color);
  auto& children = ElementAccess::children(element);
  if (children.empty()) {
    children.push_back(nullptr);
  }
  reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template <class Child>
void update_view(Element& element, const RepaintBoundary<Child>& view, BuildOwner& owner) {
  static_assert(has_box_protocol<Child>(), "RepaintBoundary child must use the box protocol");
  ElementAccess::ensure_render_object<RenderRepaintBoundary>(element, owner);
  auto& children = ElementAccess::children(element);
  if (children.empty()) {
    children.push_back(nullptr);
  }
  reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template <class Child>
void update_view(Element& element, const GestureDetector<Child>& view, BuildOwner& owner) {
  static_assert(has_box_protocol<Child>(), "GestureDetector child must use the box protocol");
  auto& action =
    ElementAccess::ensure_render_object<RenderActionBox>(element, owner, view.on_tap, false);
  action.set_on_activate(view.on_tap);
  action.set_stops_propagation(false);
  auto& children = ElementAccess::children(element);
  if (children.empty()) {
    children.push_back(nullptr);
  }
  reconcile_child(children.front(), view.child, owner, &element, Key{});
}

template <class Child>
void update_view(Element& element, const FocusView<Child>& view, BuildOwner& owner) {
  static_assert(has_box_protocol<Child>(), "FocusView child must use the box protocol");
  const bool is_new_focus_node = !ElementAccess::has_focus_node(element);
  const std::shared_ptr<FocusNode> focus_node =
    ElementAccess::ensure_focus_node(element, owner, view.on_key, view.can_focus);
  std::weak_ptr<FocusNode> weak_focus = focus_node;
  auto request_focus = [weak_focus] {
    if (const auto node = weak_focus.lock()) {
      node->request_focus();
    }
  };
  auto& action =
    ElementAccess::ensure_render_object<RenderActionBox>(element, owner, request_focus, true);
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

template <class V> void update_view(Element& element, const Optional<V>& view, BuildOwner& owner) {
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

template <class Left, class Right>
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
  std::visit(
    [&](const auto& child) { reconcile_child(children.front(), child, owner, &element, Key{}); },
    view.child);
}

template <class Range, class KeyFunction, class Builder>
void update_view(Element& element, const ForEach<Range, KeyFunction, Builder>& view,
                 BuildOwner& owner) {
  using ItemReference = std::ranges::range_reference_t<const Range>;
  using Child = std::decay_t<std::invoke_result_t<const Builder&, ItemReference>>;

  struct PendingChild {
    Key key;
    Child view;
  };

  std::vector<PendingChild> pending;

  for (const auto& item : view.items) {
    Key item_key = make_key(std::invoke(view.key_function, item));
    const auto duplicate = std::ranges::find_if(
      pending, [&](const PendingChild& child) { return child.key == item_key; });
    if (duplicate != pending.end()) {
      throw std::logic_error("ForEach contains a duplicate key: " + item_key.to_string());
    }
    pending.push_back({std::move(item_key), std::invoke(view.builder, item)});
  }

  std::vector<std::unique_ptr<Element>> next;
  next.reserve(pending.size());
  auto& element_children = ElementAccess::children(element);
  std::vector<std::unique_ptr<Element>> previous = std::move(element_children);

  for (const auto& pending_child : pending) {
    auto found = std::ranges::find_if(previous, [&](const auto& candidate) {
      return candidate != nullptr && candidate->key() == pending_child.key &&
             candidate->view_type() == type_token<Child>();
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

template <Component V> void update_view(Element& element, const V& component, BuildOwner& owner) {
  auto& descriptor = ElementAccess::descriptor(element);
  if constexpr (std::equality_comparable<V>) {
    const bool equivalent =
      descriptor.has_value() && std::any_cast<const V&>(descriptor) == component;
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

template <class V>
void reconcile_child(std::unique_ptr<Element>& slot, const V& view, BuildOwner& owner,
                     Element* parent, Key key) {
  using View = std::remove_cvref_t<V>;
  const bool reusable =
    slot != nullptr && slot->view_type_ == type_token<View>() && slot->key_ == key;

  if (!reusable) {
    owner.unmount(slot);
    slot = owner.make_element(type_token<View>(), std::move(key), detail::debug_name(view), parent);
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

template <class V> void BuildOwner::render(const V& view) {
  static_assert(View<V>, "BuildOwner::render requires a DUI View or Component");
  static_assert(detail::has_box_protocol<V>(), "BuildOwner root must use the box protocol");
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
