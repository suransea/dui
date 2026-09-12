#pragma once

#include "dui/async.hpp"
#include "dui/foundation.hpp"
#include "dui/input.hpp"
#include "dui/rendering.hpp"

#include <algorithm>
#include <any>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dui {

class BuildContext;
class BuildOwner;
class Element;
class DependencySource;

struct OwnerLifetime {
  BuildOwner* owner{};
};

class DependencySource {
public:
  virtual ~DependencySource();

  DependencySource(const DependencySource&) = delete;
  DependencySource& operator=(const DependencySource&) = delete;

protected:
  DependencySource() = default;
  void notify_dependents();

private:
  friend class BuildContext;
  friend class BuildOwner;

  struct Subscriber {
    BuildOwner* owner;
    std::uint64_t id;
    std::uint64_t generation;
  };

  void subscribe(BuildOwner& owner, Element& element);
  void unsubscribe(BuildOwner& owner, std::uint64_t id);

  std::vector<Subscriber> subscribers_;
};

namespace detail {
struct ElementAccess;
}

template <class T> class Signal;

template <class T> class StateHandle;

template <class V>
void reconcile_child(std::unique_ptr<Element>&, const V&, BuildOwner&, Element*, Key);

class Element {
public:
  using Id = std::uint64_t;

  [[nodiscard]] Id id() const { return id_; }
  [[nodiscard]] std::uint64_t generation() const { return generation_; }
  [[nodiscard]] std::size_t depth() const { return depth_; }
  [[nodiscard]] TypeToken view_type() const { return view_type_; }
  [[nodiscard]] const Key& key() const { return key_; }
  [[nodiscard]] const std::string& debug_name() const { return debug_name_; }
  [[nodiscard]] const std::string& debug_value() const { return debug_value_; }
  [[nodiscard]] std::size_t update_count() const { return update_count_; }
  [[nodiscard]] bool dirty() const { return dirty_; }
  [[nodiscard]] const std::vector<std::unique_ptr<Element>>& children() const { return children_; }
  [[nodiscard]] std::size_t kept_alive_child_count() const {
    return lazy_kept_alive_children_.size();
  }
  [[nodiscard]] const RenderObject* render_object() const { return render_object_.get(); }
  [[nodiscard]] RenderObject* render_object() { return render_object_.get(); }

private:
  friend class BuildContext;
  friend class BuildOwner;
  friend class DependencySource;
  friend struct detail::ElementAccess;
  template <class> friend class Signal;
  template <class> friend class StateHandle;

  template <class V>
  friend void reconcile_child(std::unique_ptr<Element>&, const V&, BuildOwner&, Element*, Key);

  struct StateSlot {
    std::string name;
    TypeToken type;
    std::any value;
  };

  struct EnvironmentSlot final : DependencySource {
    TypeToken type;
    std::any value;

    template <class T>
    explicit EnvironmentSlot(T initial)
      : type(type_token<T>()), value(std::make_any<T>(std::move(initial))) {}

    template <class T> void update(T next) {
      T& current = std::any_cast<T&>(value);
      if constexpr (std::equality_comparable<T>) {
        if (current == next) {
          return;
        }
      }
      current = std::move(next);
      notify_dependents();
    }
  };

  struct ResourceValue {
    virtual ~ResourceValue() = default;
    virtual void request_cancel() noexcept {}
  };

  template <class T> struct ResourceHolder final : ResourceValue {
    template <class U> explicit ResourceHolder(U&& initial) : value(std::forward<U>(initial)) {}

    void request_cancel() noexcept override {
      if constexpr (requires(T & resource) {
                      { resource.request_stop() } noexcept;
                    }) {
        value.request_stop();
      }
    }

    T value;
  };

  struct ResourceSlot {
    std::string name;
    TypeToken type;
    std::unique_ptr<ResourceValue> value;
  };

  using LazyRangeRealizer = void (*)(Element&, BuildOwner&, std::size_t, std::size_t,
                                     std::uint64_t);

  Element(Id id, std::uint64_t generation, std::size_t depth, TypeToken view_type, Key key,
          std::string debug_name)
    : id_(id), generation_(generation), depth_(depth), view_type_(view_type), key_(std::move(key)),
      debug_name_(std::move(debug_name)) {}

  Id id_{};
  std::uint64_t generation_{};
  std::size_t depth_{};
  TypeToken view_type_{};
  Key key_;
  std::string debug_name_;
  std::string debug_value_;
  std::size_t update_count_{};
  bool dirty_{};
  bool rebuilding_dirty_{};
  Element* parent_{};
  std::vector<std::unique_ptr<Element>> children_;
  std::vector<std::unique_ptr<Element>> lazy_kept_alive_children_;
  std::unordered_map<std::uint64_t, StateSlot> state_;
  std::vector<DependencySource*> dependencies_;
  std::unordered_map<const void*, std::unique_ptr<EnvironmentSlot>> environment_;
  std::unordered_map<std::uint64_t, ResourceSlot> resources_;
  std::any descriptor_;
  std::vector<Key> lazy_keys_;
  std::unordered_set<Key, detail::KeyHash> lazy_keep_alive_keys_;
  std::uint64_t lazy_revision_{};
  LazyRangeRealizer lazy_range_realizer_{};
  void (*rebuild_)(Element&, BuildOwner&){};
  std::optional<std::size_t> active_branch_;
  std::unique_ptr<RenderObject> render_object_;
  std::shared_ptr<FocusNode> focus_node_;
};

class BuildOwner {
public:
  enum class PointerPhase { down, move, up, cancel };

  struct PointerEvent {
    std::int64_t pointer{};
    PointerPhase phase{};
    Offset position{};
  };

  BuildOwner();
  ~BuildOwner();

  BuildOwner(const BuildOwner&) = delete;
  BuildOwner& operator=(const BuildOwner&) = delete;

  template <class V> void render(const V& view);

  void flush();
  [[nodiscard]] LayerTree layer_frame(BoxConstraints viewport);
  [[nodiscard]] DisplayList frame(BoxConstraints viewport);
  [[nodiscard]] RenderObject* hit_test(Offset position);
  void dispatch_pointer(PointerEvent event);
  [[nodiscard]] KeyEventResult dispatch_key(const KeyEvent& event) {
    return focus_manager_.dispatch_key(event);
  }
  [[nodiscard]] std::shared_ptr<FocusNode> focused_node() const {
    return focus_manager_.focused_node();
  }

  [[nodiscard]] const Element* root() const { return root_.get(); }
  [[nodiscard]] Element* root() { return root_.get(); }
  [[nodiscard]] std::string dump_tree() const;
  [[nodiscard]] std::size_t mount_count() const { return mount_count_; }
  [[nodiscard]] std::size_t unmount_count() const { return unmount_count_; }
  [[nodiscard]] std::size_t pending_build_count() const { return dirty_.size(); }
  [[nodiscard]] std::size_t pending_layout_count() const {
    return render_owner_.pending_layout_count();
  }
  [[nodiscard]] std::size_t pending_paint_count() const {
    return render_owner_.pending_paint_count();
  }
  [[nodiscard]] std::size_t pending_compositing_count() const {
    return render_owner_.pending_compositing_count();
  }

private:
  struct PointerTapRoute;

  friend class BuildContext;
  friend class DependencySource;
  friend struct detail::ElementAccess;
  template <class> friend class Signal;
  template <class> friend class StateHandle;

  template <class V>
  friend void reconcile_child(std::unique_ptr<Element>&, const V&, BuildOwner&, Element*, Key);

  [[nodiscard]] std::unique_ptr<Element> make_element(TypeToken type, Key key,
                                                      std::string debug_name, Element* parent);

  void unmount(std::unique_ptr<Element>& element);
  void mark_dirty(Element::Id id, std::uint64_t generation);
  [[nodiscard]] Element* resolve(Element::Id id, std::uint64_t generation) const;
  void clear_dependencies(Element& element);
  void synchronize_render_tree();
  [[nodiscard]] bool has_pending_lazy_children() const;
  [[nodiscard]] bool realize_lazy_children();
  void forget_dependency(Element::Id id, std::uint64_t generation, DependencySource& dependency);

  template <class T>
  [[nodiscard]] T& state(Element::Id id, std::uint64_t generation, std::uint64_t slot);

  std::unique_ptr<Element> root_;
  std::unordered_map<Element::Id, Element*> registry_;
  std::unordered_set<Element::Id> dirty_;
  Element::Id next_id_{1};
  std::size_t mount_count_{};
  std::size_t unmount_count_{};
  std::shared_ptr<OwnerLifetime> lifetime_;
  RenderOwner render_owner_;
  std::optional<BoxConstraints> last_viewport_;
  GestureArena gesture_arena_;
  std::unordered_map<PointerId, std::shared_ptr<PointerTapRoute>> pointer_tap_routes_;
  FocusManager focus_manager_;
  bool reconciling_{};
  bool framing_{};
};

namespace detail {

struct ElementAccess {
  static std::vector<std::unique_ptr<Element>>& children(Element& element) {
    return element.children_;
  }

  static std::string& debug_value(Element& element) { return element.debug_value_; }

  static std::any& descriptor(Element& element) { return element.descriptor_; }

  static RenderObject* render_object(Element& element) { return element.render_object_.get(); }

  static std::vector<Key>& lazy_keys(Element& element) { return element.lazy_keys_; }

  static std::vector<std::unique_ptr<Element>>& lazy_kept_alive_children(Element& element) {
    return element.lazy_kept_alive_children_;
  }

  static const std::unordered_set<Key, KeyHash>& lazy_keep_alive_keys(const Element& element) {
    return element.lazy_keep_alive_keys_;
  }

  static std::uint64_t install_lazy_model(Element& element, std::any descriptor,
                                          std::vector<Key> keys,
                                          std::unordered_set<Key, KeyHash> keep_alive_keys,
                                          Element::LazyRangeRealizer realizer) {
    element.descriptor_ = std::move(descriptor);
    element.lazy_keys_ = std::move(keys);
    element.lazy_keep_alive_keys_ = std::move(keep_alive_keys);
    element.lazy_range_realizer_ = realizer;
    ++element.lazy_revision_;
    if (element.lazy_revision_ == 0) {
      ++element.lazy_revision_;
    }
    return element.lazy_revision_;
  }

  static bool dirty(const Element& element) { return element.dirty_; }

  static bool rebuilding_dirty(const Element& element) { return element.rebuilding_dirty_; }

  static void set_rebuild(Element& element, void (*rebuild)(Element&, BuildOwner&)) {
    element.rebuild_ = rebuild;
  }

  static void rebuild(Element& element, BuildOwner& owner) { element.rebuild_(element, owner); }

  static std::optional<std::size_t>& active_branch(Element& element) {
    return element.active_branch_;
  }

  static bool has_focus_node(const Element& element) { return element.focus_node_ != nullptr; }

  static std::shared_ptr<FocusNode> ensure_focus_node(Element& element, BuildOwner& owner,
                                                      FocusNode::KeyHandler handler,
                                                      bool can_focus) {
    if (element.focus_node_ == nullptr) {
      std::shared_ptr<FocusNode> parent;
      for (Element* ancestor = element.parent_; ancestor != nullptr; ancestor = ancestor->parent_) {
        if (ancestor->focus_node_ != nullptr) {
          parent = ancestor->focus_node_;
          break;
        }
      }
      element.focus_node_ =
        owner.focus_manager_.create_node(std::move(parent), std::move(handler), can_focus);
    } else {
      element.focus_node_->set_key_handler(std::move(handler));
      element.focus_node_->set_can_focus(can_focus);
    }
    return element.focus_node_;
  }

  template <class T> static void set_environment(Element& element, T value) {
    using Value = std::decay_t<T>;
    static_assert(std::copy_constructible<Value>,
                  "DUI Environment values must currently be copy constructible");

    const void* token = type_token<Value>().value;
    const auto found = element.environment_.find(token);
    if (found == element.environment_.end()) {
      element.environment_.emplace(token,
                                   std::make_unique<Element::EnvironmentSlot>(std::move(value)));
      return;
    }
    if (found->second->type != type_token<Value>()) {
      throw std::logic_error("Environment type token collision");
    }
    found->second->template update<Value>(std::move(value));
  }

  template <class Render, class... Arguments>
  static Render& ensure_render_object(Element& element, BuildOwner& owner,
                                      Arguments&&... arguments) {
    if (element.render_object_ == nullptr) {
      element.render_object_ =
        std::make_unique<Render>(owner.render_owner_, std::forward<Arguments>(arguments)...);
    }
    return static_cast<Render&>(*element.render_object_);
  }

  static void clear_dependencies(BuildOwner& owner, Element& element) {
    owner.clear_dependencies(element);
  }

  static void unmount(BuildOwner& owner, std::unique_ptr<Element>& element) {
    owner.unmount(element);
  }
};

} // namespace detail

class BuildContext {
public:
  BuildContext(BuildOwner& owner, Element& element) : owner_(&owner), element_(&element) {}

  template <fixed_string Name, class T>
  [[nodiscard]] StateHandle<std::decay_t<T>> state(T&& initial);

  template <class T> [[nodiscard]] const T& watch(Signal<T>& signal);

  template <class T> [[nodiscard]] const T& environment();

  template <fixed_string Name, class Factory> [[nodiscard]] auto& resource(Factory&& factory);

  template <fixed_string Name> [[nodiscard]] CancellationToken cancellation();

  [[nodiscard]] Element::Id element_id() const { return element_->id_; }

private:
  template <class> friend class Signal;
  friend class BuildScope;

  void observe(DependencySource& dependency);

  BuildOwner* owner_;
  Element* element_;
};

namespace detail {
inline thread_local BuildContext* active_build_context{};
}

class BuildScope {
public:
  explicit BuildScope(BuildContext& context) : previous_(detail::active_build_context) {
    detail::active_build_context = &context;
  }

  ~BuildScope() { detail::active_build_context = previous_; }

  BuildScope(const BuildScope&) = delete;
  BuildScope& operator=(const BuildScope&) = delete;

private:
  BuildContext* previous_;
};

template <class T> class Signal final : public DependencySource {
public:
  explicit Signal(T value) : value_(std::move(value)) {}

  Signal(const Signal&) = delete;
  Signal& operator=(const Signal&) = delete;

  [[nodiscard]] const T& get() {
    if (detail::active_build_context != nullptr) {
      detail::active_build_context->observe(*this);
    }
    return value_;
  }

  void set(T value) {
    if constexpr (std::equality_comparable<T>) {
      if (value_ == value) {
        return;
      }
    }
    value_ = std::move(value);
    notify();
  }

  template <class F> void update(F&& operation) {
    set(std::invoke(std::forward<F>(operation), value_));
  }

private:
  void notify() { notify_dependents(); }
  T value_;
};

template <class T> class StateHandle {
public:
  [[nodiscard]] const T& get() const {
    return owner().template state<T>(element_, generation_, slot_);
  }

  void set(T value) {
    BuildOwner& build_owner = owner();
    T& current = build_owner.template state<T>(element_, generation_, slot_);
    if constexpr (std::equality_comparable<T>) {
      if (current == value) {
        return;
      }
    }
    current = std::move(value);
    build_owner.mark_dirty(element_, generation_);
  }

  template <class F> void update(F&& operation) {
    set(std::invoke(std::forward<F>(operation), get()));
  }

private:
  friend class BuildContext;

  StateHandle(std::weak_ptr<OwnerLifetime> lifetime, Element::Id element, std::uint64_t generation,
              std::uint64_t slot)
    : lifetime_(std::move(lifetime)), element_(element), generation_(generation), slot_(slot) {}

  [[nodiscard]] BuildOwner& owner() const {
    const auto lifetime = lifetime_.lock();
    if (lifetime == nullptr || lifetime->owner == nullptr) {
      throw std::logic_error("StateHandle owner no longer exists");
    }
    return *lifetime->owner;
  }

  std::weak_ptr<OwnerLifetime> lifetime_;
  Element::Id element_{};
  std::uint64_t generation_{};
  std::uint64_t slot_{};
};

template <class T>
T& BuildOwner::state(Element::Id id, std::uint64_t generation, std::uint64_t slot) {
  Element* element = resolve(id, generation);
  if (element == nullptr) {
    throw std::logic_error("StateHandle refers to an unmounted Element");
  }
  auto found = element->state_.find(slot);
  if (found == element->state_.end() || found->second.type != type_token<T>()) {
    throw std::logic_error("StateHandle has an invalid state slot or type");
  }
  return std::any_cast<T&>(found->second.value);
}

template <fixed_string Name, class T>
StateHandle<std::decay_t<T>> BuildContext::state(T&& initial) {
  using Value = std::decay_t<T>;
  static_assert(std::copy_constructible<Value>,
                "DUI state values must currently be copy constructible");
  constexpr auto slot = hash_name(Name.view());

  auto iterator = element_->state_.find(slot);
  if (iterator != element_->state_.end() && iterator->second.name != Name.view()) {
    throw std::logic_error("Named state hash collision");
  }
  if (iterator != element_->state_.end() && iterator->second.type != type_token<Value>()) {
    throw std::logic_error("Named state changed type");
  }
  if (iterator == element_->state_.end()) {
    auto value = std::make_any<Value>(std::forward<T>(initial));
    iterator = element_->state_
                 .emplace(slot, Element::StateSlot{std::string(Name.view()), type_token<Value>(),
                                                   std::move(value)})
                 .first;
  }

  return StateHandle<Value>{owner_->lifetime_, element_->id_, element_->generation_, slot};
}

template <class T> const T& BuildContext::watch(Signal<T>& signal) {
  observe(signal);
  return signal.get();
}

template <class T> const T& BuildContext::environment() {
  for (Element* ancestor = element_; ancestor != nullptr; ancestor = ancestor->parent_) {
    const auto found = ancestor->environment_.find(type_token<T>().value);
    if (found == ancestor->environment_.end()) {
      continue;
    }
    Element::EnvironmentSlot& slot = *found->second;
    if (slot.type != type_token<T>()) {
      throw std::logic_error("Environment type token mismatch");
    }
    observe(slot);
    return std::any_cast<const T&>(slot.value);
  }
  throw std::logic_error("Requested Environment value was not provided");
}

template <fixed_string Name, class Factory> auto& BuildContext::resource(Factory&& factory) {
  using Value = std::remove_cvref_t<std::invoke_result_t<Factory>>;
  static_assert(!std::is_void_v<Value>, "A DUI resource factory must return a value");
  constexpr auto slot = hash_name(Name.view());

  auto found = element_->resources_.find(slot);
  if (found != element_->resources_.end() && found->second.name != Name.view()) {
    throw std::logic_error("Named resource hash collision");
  }
  if (found != element_->resources_.end() && found->second.type != type_token<Value>()) {
    throw std::logic_error("Named resource changed type");
  }
  if (found == element_->resources_.end()) {
    Value value = std::invoke(std::forward<Factory>(factory));
    auto holder = std::make_unique<Element::ResourceHolder<Value>>(std::move(value));
    found = element_->resources_
              .emplace(slot, Element::ResourceSlot{std::string(Name.view()), type_token<Value>(),
                                                   std::move(holder)})
              .first;
  }

  return static_cast<Element::ResourceHolder<Value>&>(*found->second.value).value;
}

template <fixed_string Name> CancellationToken BuildContext::cancellation() {
  return resource<Name>([] { return CancellationSource{}; }).token();
}

} // namespace dui
