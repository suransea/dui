#include "dui/ui.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

namespace dui {

struct BuildOwner::PointerTapRoute final : GestureArenaMember {
  explicit PointerTapRoute(std::vector<RenderObject::Id> value) : route(std::move(value)) {}

  void accept_gesture(PointerId pointer) override {
    if (pointer == pointer_id && !rejected) {
      accepted = true;
    }
  }

  void reject_gesture(PointerId pointer) override {
    if (pointer == pointer_id) {
      accepted = false;
      rejected = true;
    }
  }

  PointerId pointer_id{};
  std::vector<RenderObject::Id> route;
  bool accepted{};
  bool rejected{};
};

DependencySource::~DependencySource() {
  for (const Subscriber& subscriber : subscribers_) {
    subscriber.owner->forget_dependency(subscriber.id, subscriber.generation, *this);
  }
}

void DependencySource::subscribe(BuildOwner& owner, Element& element) {
  const auto found = std::ranges::find_if(subscribers_, [&](const Subscriber& subscriber) {
    return subscriber.owner == &owner && subscriber.id == element.id_;
  });
  if (found == subscribers_.end()) {
    subscribers_.push_back({&owner, element.id_, element.generation_});
  }
}

void DependencySource::unsubscribe(BuildOwner& owner, std::uint64_t id) {
  std::erase_if(subscribers_, [&](const Subscriber& subscriber) {
    return subscriber.owner == &owner && subscriber.id == id;
  });
}

void DependencySource::notify_dependents() {
  std::erase_if(subscribers_, [](const Subscriber& subscriber) {
    if (subscriber.owner->resolve(subscriber.id, subscriber.generation) == nullptr) {
      return true;
    }
    subscriber.owner->mark_dirty(subscriber.id, subscriber.generation);
    return false;
  });
}

std::string Key::to_string() const {
  return std::visit(
    [](const auto& value) -> std::string {
      using T = std::remove_cvref_t<decltype(value)>;
      if constexpr (std::same_as<T, std::monostate>) {
        return "<none>";
      } else if constexpr (std::same_as<T, std::string>) {
        return value;
      } else {
        return std::to_string(value);
      }
    },
    value_);
}

BuildOwner::BuildOwner() : lifetime_(std::make_shared<OwnerLifetime>()) { lifetime_->owner = this; }

BuildOwner::~BuildOwner() {
  unmount(root_);
  lifetime_->owner = nullptr;
}

std::unique_ptr<Element> BuildOwner::make_element(TypeToken type, Key key, std::string debug_name,
                                                  Element* parent) {
  const auto id = next_id_++;
  auto element =
    std::unique_ptr<Element>{new Element{id, id, parent == nullptr ? 0 : parent->depth_ + 1, type,
                                         std::move(key), std::move(debug_name)}};
  element->parent_ = parent;
  registry_.emplace(id, element.get());
  ++mount_count_;
  return element;
}

void BuildOwner::clear_dependencies(Element& element) {
  for (DependencySource* dependency : element.dependencies_) {
    dependency->unsubscribe(*this, element.id_);
  }
  element.dependencies_.clear();
}

void BuildOwner::forget_dependency(Element::Id id, std::uint64_t generation,
                                   DependencySource& dependency) {
  Element* element = resolve(id, generation);
  if (element == nullptr) {
    return;
  }
  std::erase(element->dependencies_, &dependency);
}

void BuildOwner::unmount(std::unique_ptr<Element>& element) {
  if (element == nullptr) {
    return;
  }

  for (auto& [slot, resource] : element->resources_) {
    static_cast<void>(slot);
    resource.value->request_cancel();
  }
  for (auto& child : element->children_) {
    unmount(child);
  }
  for (auto& child : element->lazy_kept_alive_children_) {
    unmount(child);
  }
  clear_dependencies(*element);
  dirty_.erase(element->id_);
  registry_.erase(element->id_);
  ++unmount_count_;
  element.reset();
}

void BuildOwner::mark_dirty(Element::Id id, std::uint64_t generation) {
  Element* element = resolve(id, generation);
  if (element == nullptr || element->rebuild_ == nullptr) {
    return;
  }
  dirty_.insert(id);
  element->dirty_ = true;
}

Element* BuildOwner::resolve(Element::Id id, std::uint64_t generation) const {
  const auto found = registry_.find(id);
  if (found == registry_.end() || found->second->generation_ != generation) {
    return nullptr;
  }
  return found->second;
}

void BuildOwner::flush() {
  if (reconciling_) {
    throw std::logic_error("BuildOwner does not allow reentrant render or flush");
  }
  reconciling_ = true;
  try {
    while (!dirty_.empty()) {
      std::vector<Element::Id> pending{dirty_.begin(), dirty_.end()};
      dirty_.clear();
      std::ranges::sort(pending, {}, [&](Element::Id id) {
        const auto found = registry_.find(id);
        return found == registry_.end() ? std::size_t{} : found->second->depth_;
      });

      for (Element::Id id : pending) {
        const auto found = registry_.find(id);
        if (found == registry_.end()) {
          continue;
        }
        Element& element = *found->second;
        if (!element.dirty_ || element.rebuild_ == nullptr) {
          continue;
        }
        element.dirty_ = false;
        element.rebuild_(element, *this);
      }
    }
    reconciling_ = false;
  } catch (...) {
    reconciling_ = false;
    throw;
  }
}

namespace {

void collect_render_objects(Element& element, std::vector<RenderObject*>& output) {
  if (RenderObject* render_object = element.render_object(); render_object != nullptr) {
    output.push_back(render_object);
    std::vector<RenderObject*> render_children;
    for (const auto& child : element.children()) {
      if (child != nullptr) {
        collect_render_objects(*child, render_children);
      }
    }
    render_object->set_children(render_children);
    return;
  }

  for (const auto& child : element.children()) {
    if (child != nullptr) {
      collect_render_objects(*child, output);
    }
  }
}

} // namespace

void BuildOwner::synchronize_render_tree() {
  std::vector<RenderObject*> roots;
  if (root_ != nullptr) {
    collect_render_objects(*root_, roots);
  }
  render_owner_.set_roots(roots);
}

bool BuildOwner::has_pending_lazy_children() const {
  const auto visit = [&](auto&& self, const Element& element) -> bool {
    if (element.lazy_range_realizer_ != nullptr) {
      const auto* render =
        dynamic_cast<const RenderSliverFixedExtentList*>(element.render_object_.get());
      if (render == nullptr) {
        throw std::logic_error("Lazy child manager requires RenderSliverFixedExtentList");
      }
      const auto request = render->requested_child_range();
      if (request.has_value() && request->revision == element.lazy_revision_) {
        return true;
      }
    }
    return std::ranges::any_of(
      element.children_, [&](const auto& child) { return child != nullptr && self(self, *child); });
  };
  return root_ != nullptr && visit(visit, *root_);
}

bool BuildOwner::realize_lazy_children() {
  if (reconciling_) {
    throw std::logic_error("BuildOwner does not allow reentrant lazy realization");
  }
  bool realized = false;
  reconciling_ = true;
  try {
    const auto visit = [&](auto&& self, Element& element) -> void {
      if (element.lazy_range_realizer_ != nullptr) {
        auto* render = dynamic_cast<RenderSliverFixedExtentList*>(element.render_object_.get());
        if (render == nullptr) {
          throw std::logic_error("Lazy child manager requires RenderSliverFixedExtentList");
        }
        const auto request = render->requested_child_range();
        if (request.has_value() && request->revision == element.lazy_revision_) {
          element.lazy_range_realizer_(element, *this, request->first, request->end,
                                       request->revision);
          realized = true;
        }
      }
      for (const auto& child : element.children_) {
        if (child != nullptr) {
          self(self, *child);
        }
      }
    };
    if (root_ != nullptr) {
      visit(visit, *root_);
    }
    reconciling_ = false;
  } catch (...) {
    reconciling_ = false;
    throw;
  }
  return realized;
}

LayerTree BuildOwner::layer_frame(BoxConstraints viewport) {
  if (framing_) {
    throw std::logic_error("BuildOwner does not allow reentrant frame production");
  }
  framing_ = true;
  try {
    flush();
    constexpr std::size_t max_realization_rounds = 16;
    std::size_t realization_rounds = 0;
    for (;;) {
      synchronize_render_tree();
      render_owner_.layout(viewport);
      if (!has_pending_lazy_children()) {
        break;
      }
      if (realization_rounds == max_realization_rounds) {
        throw std::logic_error("Lazy child layout did not stabilize");
      }
      if (!realize_lazy_children()) {
        throw std::logic_error("Lazy child request disappeared during realization");
      }
      flush();
      ++realization_rounds;
    }
    LayerTree result = render_owner_.composite_frame();
    last_viewport_ = viewport;
    framing_ = false;
    return result;
  } catch (...) {
    framing_ = false;
    throw;
  }
}

DisplayList BuildOwner::frame(BoxConstraints viewport) { return layer_frame(viewport).flatten(); }

RenderObject* BuildOwner::hit_test(Offset position) {
  if (!last_viewport_.has_value()) {
    throw std::logic_error("hit_test requires a completed frame");
  }
  static_cast<void>(layer_frame(*last_viewport_));
  return render_owner_.hit_test(position);
}

SemanticsTree BuildOwner::semantics_tree() {
  if (!last_viewport_.has_value()) {
    throw std::logic_error("semantics_tree requires a completed frame");
  }
  static_cast<void>(layer_frame(*last_viewport_));
  return render_owner_.semantics_tree();
}

bool BuildOwner::perform_semantics_action(std::uint64_t id, SemanticsAction action) {
  if (!last_viewport_.has_value()) {
    throw std::logic_error("perform_semantics_action requires a completed frame");
  }
  static_cast<void>(layer_frame(*last_viewport_));
  return render_owner_.perform_semantics_action(id, action);
}

void BuildOwner::dispatch_pointer(PointerEvent event) {
  const std::shared_ptr<OwnerLifetime> lifetime = lifetime_;
  HitTestResult hit_path;
  if (event.phase != PointerPhase::cancel) {
    if (!last_viewport_.has_value()) {
      throw std::logic_error("pointer dispatch requires a completed frame");
    }
    static_cast<void>(layer_frame(*last_viewport_));
    hit_path = render_owner_.hit_test_path(event.position);
  }
  RenderObject* target = hit_path.target();

  switch (event.phase) {
  case PointerPhase::down: {
    if (const auto stale = pointer_tap_routes_.find(event.pointer);
        stale != pointer_tap_routes_.end()) {
      pointer_tap_routes_.erase(stale);
      gesture_arena_.cancel(event.pointer);
    }
    if (target != nullptr) {
      std::vector<RenderObject::Id> route;
      route.reserve(hit_path.path().size());
      for (const HitTestEntry& entry : hit_path.path()) {
        if (entry.target->can_activate()) {
          route.push_back(entry.target->id());
        }
      }
      if (route.empty()) {
        break;
      }
      auto tap_route = std::make_shared<PointerTapRoute>(std::move(route));
      tap_route->pointer_id = event.pointer;
      pointer_tap_routes_.emplace(event.pointer, tap_route);
      try {
        gesture_arena_.add(event.pointer, tap_route);
      } catch (...) {
        pointer_tap_routes_.erase(event.pointer);
        gesture_arena_.cancel(event.pointer);
        throw;
      }
    }
    break;
  }
  case PointerPhase::up: {
    const auto down = pointer_tap_routes_.find(event.pointer);
    std::shared_ptr<PointerTapRoute> tap_route;
    if (down != pointer_tap_routes_.end()) {
      tap_route = std::move(down->second);
      pointer_tap_routes_.erase(down);
    }
    if (tap_route == nullptr) {
      gesture_arena_.cancel(event.pointer);
      break;
    }
    std::unordered_set<RenderObject::Id> current_targets;
    for (const HitTestEntry& entry : hit_path.path()) {
      if (entry.target->can_activate()) {
        current_targets.insert(entry.target->id());
      }
    }
    std::vector<RenderObject::Id> activation_route;
    for (RenderObject::Id id : tap_route->route) {
      if (current_targets.contains(id)) {
        activation_route.push_back(id);
      }
    }
    if (!activation_route.empty()) {
      gesture_arena_.accept(event.pointer, *tap_route);
      gesture_arena_.close(event.pointer);
    } else {
      gesture_arena_.reject(event.pointer, *tap_route);
    }
    if (tap_route->accepted) {
      for (RenderObject::Id id : activation_route) {
        if (lifetime->owner == nullptr) {
          return;
        }
        if (RenderObject* object = render_owner_.resolve(id); object != nullptr) {
          const bool stops_propagation = object->activate();
          if (lifetime->owner == nullptr) {
            return;
          }
          if (stops_propagation) {
            break;
          }
        }
      }
    }
    break;
  }
  case PointerPhase::cancel: {
    const auto route = pointer_tap_routes_.find(event.pointer);
    if (route != pointer_tap_routes_.end()) {
      pointer_tap_routes_.erase(route);
    }
    gesture_arena_.cancel(event.pointer);
    break;
  }
  case PointerPhase::move:
    break;
  }
}

void BuildContext::observe(DependencySource& dependency) {
  if (std::ranges::find(element_->dependencies_, &dependency) == element_->dependencies_.end()) {
    element_->dependencies_.push_back(&dependency);
    dependency.subscribe(*owner_, *element_);
  }
}

namespace {

void append_tree(const Element& element, std::ostringstream& output, std::size_t indent) {
  output << std::string(indent, ' ') << element.debug_name() << '#' << element.id();
  if (!element.key().empty()) {
    output << " key=" << element.key().to_string();
  }
  if (!element.debug_value().empty()) {
    output << " value=\"" << element.debug_value() << '"';
  }
  output << " updates=" << element.update_count() << '\n';

  for (const auto& child : element.children()) {
    if (child != nullptr) {
      append_tree(*child, output, indent + 2);
    }
  }
}

} // namespace

std::string BuildOwner::dump_tree() const {
  std::ostringstream output;
  if (root_ != nullptr) {
    append_tree(*root_, output, 0);
  }
  return std::move(output).str();
}

} // namespace dui
