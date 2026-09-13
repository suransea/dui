#include "dui/ui.hpp"

#include <algorithm>
#include <locale>
#include <mutex>
#include <ranges>
#include <string_view>
#include <utility>

namespace dui {

namespace {

class SteadyTimelineClock final : public TimelineClock {
public:
  std::chrono::nanoseconds now() const noexcept override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch());
  }
};

} // namespace

class TimelineRecorder::Impl {
public:
  Impl(std::size_t capacity, std::shared_ptr<TimelineClock> clock)
    : events_(capacity), clock_(std::move(clock)) {
    if (capacity == 0) {
      throw std::invalid_argument("TimelineRecorder capacity must be positive");
    }
    if (clock_ == nullptr) {
      throw std::invalid_argument("TimelineRecorder requires a clock");
    }
  }

  [[nodiscard]] TimelineEvent begin(TimelineLane lane, TimelinePhase phase,
                                    std::uint64_t parent_span_id, std::uint64_t frame_id,
                                    std::size_t pass, std::size_t work_count) noexcept {
    std::uint64_t sequence;
    std::uint64_t span_id;
    {
      std::lock_guard lock{state_mutex_};
      sequence = next_nonzero(next_sequence_);
      span_id = next_nonzero(next_span_id_);
    }
    std::chrono::nanoseconds start;
    {
      std::lock_guard lock{clock_mutex_};
      start = clock_->now();
    }
    return TimelineEvent{
      sequence, span_id, parent_span_id, frame_id,  lane, phase, TimelineOutcome::completed,
      start,    {},      pass,           work_count};
  }

  void finish(TimelineEvent event, TimelineOutcome outcome) noexcept {
    std::chrono::nanoseconds end;
    {
      std::lock_guard lock{clock_mutex_};
      end = clock_->now();
    }
    event.outcome = outcome;
    event.duration = end >= event.start ? end - event.start : std::chrono::nanoseconds{};
    std::lock_guard lock{state_mutex_};
    if (event_count_ < events_.size()) {
      events_[(first_event_ + event_count_) % events_.size()] = event;
      ++event_count_;
      return;
    }
    events_[first_event_] = event;
    first_event_ = (first_event_ + 1) % events_.size();
    ++dropped_event_count_;
  }

  [[nodiscard]] TimelineSnapshot snapshot() const {
    TimelineSnapshot result;
    {
      std::lock_guard lock{state_mutex_};
      result.events.reserve(event_count_);
      for (std::size_t index = 0; index < event_count_; ++index) {
        result.events.push_back(events_[(first_event_ + index) % events_.size()]);
      }
      result.dropped_event_count = dropped_event_count_;
    }
    std::ranges::sort(result.events, {}, &TimelineEvent::sequence);
    return result;
  }

  void clear() noexcept {
    std::lock_guard lock{state_mutex_};
    first_event_ = 0;
    event_count_ = 0;
    dropped_event_count_ = 0;
  }

  [[nodiscard]] std::uint64_t next_frame_id() noexcept {
    std::lock_guard lock{state_mutex_};
    return next_nonzero(next_frame_id_);
  }

private:
  static std::uint64_t next_nonzero(std::uint64_t& value) noexcept {
    std::uint64_t result = value++;
    if (result == 0) {
      result = value++;
    }
    return result;
  }

  std::vector<TimelineEvent> events_;
  std::shared_ptr<TimelineClock> clock_;
  mutable std::mutex state_mutex_;
  mutable std::mutex clock_mutex_;
  std::size_t first_event_{};
  std::size_t event_count_{};
  std::size_t dropped_event_count_{};
  std::uint64_t next_sequence_{1};
  std::uint64_t next_span_id_{1};
  std::uint64_t next_frame_id_{1};
};

TimelineRecorder::TimelineRecorder(std::size_t capacity)
  : TimelineRecorder(capacity, std::make_shared<SteadyTimelineClock>()) {}

TimelineRecorder::TimelineRecorder(std::size_t capacity, std::shared_ptr<TimelineClock> clock)
  : impl_(std::make_unique<Impl>(capacity, std::move(clock))) {}

TimelineRecorder::~TimelineRecorder() = default;

TimelineSnapshot TimelineRecorder::snapshot() const { return impl_->snapshot(); }

void TimelineRecorder::clear() noexcept { impl_->clear(); }

TimelineEvent TimelineRecorder::begin(TimelineLane lane, TimelinePhase phase,
                                      std::uint64_t parent_span_id, std::uint64_t frame_id,
                                      std::size_t pass, std::size_t work_count) noexcept {
  return impl_->begin(lane, phase, parent_span_id, frame_id, pass, work_count);
}

void TimelineRecorder::finish(TimelineEvent event, TimelineOutcome outcome) noexcept {
  impl_->finish(event, outcome);
}

std::uint64_t TimelineRecorder::next_frame_id() noexcept { return impl_->next_frame_id(); }

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

BuildOwner::TimelineSpan::TimelineSpan(BuildOwner& owner,
                                       std::shared_ptr<TimelineRecorder> recorder,
                                       TimelineEvent event)
  : owner_(&owner), recorder_(std::move(recorder)), event_(event), active_(true) {}

BuildOwner::TimelineSpan::~TimelineSpan() {
  if (active_) {
    owner_->finish_timeline(recorder_, event_, TimelineOutcome::failed);
  }
}

BuildOwner::TimelineSpan::TimelineSpan(TimelineSpan&& other) noexcept
  : owner_(std::exchange(other.owner_, nullptr)), recorder_(std::move(other.recorder_)),
    event_(other.event_), active_(std::exchange(other.active_, false)) {}

void BuildOwner::TimelineSpan::complete() noexcept {
  if (active_) {
    owner_->finish_timeline(recorder_, event_, TimelineOutcome::completed);
    active_ = false;
  }
}

void BuildOwner::TimelineSpan::fail() noexcept {
  if (active_) {
    owner_->finish_timeline(recorder_, event_, TimelineOutcome::failed);
    active_ = false;
  }
}

BuildOwner::TimelineSpan BuildOwner::begin_timeline(TimelinePhase phase,
                                                    std::uint64_t parent_span_id,
                                                    std::uint64_t frame_id, std::size_t pass,
                                                    std::size_t work_count) noexcept {
  const std::shared_ptr<TimelineRecorder> recorder = timeline_recorder_;
  if (recorder == nullptr) {
    return {};
  }
  return TimelineSpan{
    *this, recorder,
    recorder->begin(TimelineLane::ui, phase, parent_span_id, frame_id, pass, work_count)};
}

void BuildOwner::finish_timeline(const std::shared_ptr<TimelineRecorder>& recorder,
                                 TimelineEvent event, TimelineOutcome outcome) noexcept {
  recorder->finish(event, outcome);
}

std::uint64_t BuildOwner::next_timeline_frame_id() noexcept {
  return timeline_recorder_ == nullptr ? 0 : timeline_recorder_->next_frame_id();
}

void BuildOwner::set_timeline_recorder(std::shared_ptr<TimelineRecorder> recorder) {
  if (reconciling_ || framing_) {
    throw std::logic_error(
      "BuildOwner cannot change its timeline recorder during reconciliation or framing");
  }
  timeline_recorder_ = std::move(recorder);
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

void BuildOwner::flush() { flush_with_timeline(0, 0); }

void BuildOwner::flush_with_timeline(std::uint64_t parent_span_id, std::uint64_t frame_id) {
  if (reconciling_) {
    throw std::logic_error("BuildOwner does not allow reentrant render or flush");
  }
  reconciling_ = true;
  auto timeline = begin_timeline(TimelinePhase::build, parent_span_id, frame_id, 0, dirty_.size());
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
    timeline.complete();
    reconciling_ = false;
  } catch (...) {
    timeline.fail();
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
  const std::uint64_t frame_id = next_timeline_frame_id();
  auto frame_timeline = begin_timeline(TimelinePhase::frame, 0, frame_id, 0, 0);
  try {
    flush_with_timeline(frame_timeline.id(), frame_id);
    constexpr std::size_t max_realization_rounds = 16;
    std::size_t realization_rounds = 0;
    for (;;) {
      {
        auto timeline = begin_timeline(TimelinePhase::synchronize_render_tree, frame_timeline.id(),
                                       frame_id, realization_rounds, 0);
        synchronize_render_tree();
        timeline.complete();
      }
      {
        auto timeline = begin_timeline(TimelinePhase::layout, frame_timeline.id(), frame_id,
                                       realization_rounds, render_owner_.pending_layout_count());
        render_owner_.layout(viewport);
        timeline.complete();
      }
      if (!has_pending_lazy_children()) {
        break;
      }
      if (realization_rounds == max_realization_rounds) {
        throw std::logic_error("Lazy child layout did not stabilize");
      }
      {
        auto timeline = begin_timeline(TimelinePhase::lazy_realization, frame_timeline.id(),
                                       frame_id, realization_rounds, 1);
        if (!realize_lazy_children()) {
          throw std::logic_error("Lazy child request disappeared during realization");
        }
        timeline.complete();
      }
      ++realization_rounds;
      frame_timeline.set_work_count(realization_rounds);
      flush_with_timeline(frame_timeline.id(), frame_id);
    }
    LayerTree result;
    {
      auto timeline = begin_timeline(TimelinePhase::composite, frame_timeline.id(), frame_id, 0,
                                     render_owner_.pending_paint_count() +
                                       render_owner_.pending_compositing_count());
      result = render_owner_.composite_frame();
      timeline.complete();
    }
    last_viewport_ = viewport;
    frame_timeline.complete();
    framing_ = false;
    return result;
  } catch (...) {
    frame_timeline.fail();
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

void append_json_string(std::ostringstream& output, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  output << '"';
  for (std::size_t index = 0; index < value.size();) {
    const auto character = static_cast<unsigned char>(value[index]);
    if (character >= 0x80u) {
      std::size_t length = 0;
      bool valid_second = true;
      if (character >= 0xc2u && character <= 0xdfu) {
        length = 2;
      } else if (character >= 0xe0u && character <= 0xefu) {
        length = 3;
        if (index + 1 < value.size()) {
          const auto second = static_cast<unsigned char>(value[index + 1]);
          valid_second = character == 0xe0u   ? second >= 0xa0u && second <= 0xbfu
                         : character == 0xedu ? second >= 0x80u && second <= 0x9fu
                                              : second >= 0x80u && second <= 0xbfu;
        }
      } else if (character >= 0xf0u && character <= 0xf4u) {
        length = 4;
        if (index + 1 < value.size()) {
          const auto second = static_cast<unsigned char>(value[index + 1]);
          valid_second = character == 0xf0u   ? second >= 0x90u && second <= 0xbfu
                         : character == 0xf4u ? second >= 0x80u && second <= 0x8fu
                                              : second >= 0x80u && second <= 0xbfu;
        }
      }
      bool valid = length != 0 && index + length <= value.size() && valid_second;
      for (std::size_t continuation = 1; valid && continuation < length; ++continuation) {
        const auto byte = static_cast<unsigned char>(value[index + continuation]);
        valid = byte >= 0x80u && byte <= 0xbfu;
      }
      if (!valid) {
        output << "\\ufffd";
        ++index;
        continue;
      }
      output.write(value.data() + index, static_cast<std::streamsize>(length));
      index += length;
      continue;
    }
    switch (character) {
    case '"':
      output << "\\\"";
      break;
    case '\\':
      output << "\\\\";
      break;
    case '\b':
      output << "\\b";
      break;
    case '\f':
      output << "\\f";
      break;
    case '\n':
      output << "\\n";
      break;
    case '\r':
      output << "\\r";
      break;
    case '\t':
      output << "\\t";
      break;
    default:
      if (character < 0x20u) {
        output << "\\u00" << hex[character >> 4u] << hex[character & 0x0fu];
      } else {
        output << static_cast<char>(character);
      }
      break;
    }
    ++index;
  }
  output << '"';
}

void append_json_bool(std::ostringstream& output, bool value) {
  output << (value ? "true" : "false");
}

std::string_view inspector_state_name(InspectorElementState state) {
  switch (state) {
  case InspectorElementState::active:
    return "active";
  case InspectorElementState::dormant_keep_alive:
    return "dormantKeepAlive";
  }
  return "active";
}

std::string_view inspector_protocol_name(InspectorRenderProtocol protocol) {
  switch (protocol) {
  case InspectorRenderProtocol::object:
    return "object";
  case InspectorRenderProtocol::box:
    return "box";
  case InspectorRenderProtocol::sliver:
    return "sliver";
  }
  return "object";
}

std::string_view timeline_lane_name(TimelineLane lane) {
  switch (lane) {
  case TimelineLane::ui:
    return "ui";
  case TimelineLane::raster:
    return "raster";
  }
  throw std::invalid_argument("Timeline event has an unknown lane");
}

std::string_view timeline_phase_name(TimelinePhase phase) {
  switch (phase) {
  case TimelinePhase::reconcile:
    return "reconcile";
  case TimelinePhase::build:
    return "build";
  case TimelinePhase::frame:
    return "frame";
  case TimelinePhase::synchronize_render_tree:
    return "synchronizeRenderTree";
  case TimelinePhase::layout:
    return "layout";
  case TimelinePhase::lazy_realization:
    return "lazyRealization";
  case TimelinePhase::composite:
    return "composite";
  case TimelinePhase::raster_frame:
    return "rasterFrame";
  case TimelinePhase::surface_acquire:
    return "surfaceAcquire";
  case TimelinePhase::rasterize:
    return "rasterize";
  case TimelinePhase::surface_present:
    return "surfacePresent";
  }
  throw std::invalid_argument("Timeline event has an unknown phase");
}

std::string_view timeline_outcome_name(TimelineOutcome outcome) {
  switch (outcome) {
  case TimelineOutcome::completed:
    return "completed";
  case TimelineOutcome::unavailable:
    return "unavailable";
  case TimelineOutcome::out_of_date:
    return "outOfDate";
  case TimelineOutcome::lost:
    return "lost";
  case TimelineOutcome::failed:
    return "failed";
  }
  throw std::invalid_argument("Timeline event has an unknown outcome");
}

void append_json_node(std::ostringstream& output, const InspectorNode& node, std::size_t depth) {
  if (depth >= InspectorSnapshot::maximum_depth) {
    throw std::length_error("Inspector snapshot exceeds its maximum serialization depth");
  }
  output << "{\"id\":" << node.id << ",\"generation\":" << node.generation
         << ",\"depth\":" << node.depth << ",\"name\":";
  append_json_string(output, node.name);
  output << ",\"value\":";
  append_json_string(output, node.value);
  output << ",\"key\":";
  append_json_string(output, node.key);
  output << ",\"state\":";
  append_json_string(output, inspector_state_name(node.state));
  output << ",\"updateCount\":" << node.update_count << ",\"dirty\":";
  append_json_bool(output, node.dirty);
  output << ",\"stateSlotCount\":" << node.state_slot_count
         << ",\"dependencyCount\":" << node.dependency_count
         << ",\"environmentCount\":" << node.environment_count
         << ",\"resourceCount\":" << node.resource_count << ",\"hasFocusNode\":";
  append_json_bool(output, node.has_focus_node);
  output << ",\"focused\":";
  append_json_bool(output, node.focused);
  output << ",\"renderObject\":";
  if (!node.render_object.has_value()) {
    output << "null";
  } else {
    const InspectorRenderSnapshot& render = *node.render_object;
    output << "{\"id\":" << render.id << ",\"protocol\":";
    append_json_string(output, inspector_protocol_name(render.protocol));
    output << ",\"needsLayout\":";
    append_json_bool(output, render.needs_layout);
    output << ",\"needsPaint\":";
    append_json_bool(output, render.needs_paint);
    output << ",\"needsCompositing\":";
    append_json_bool(output, render.needs_compositing);
    output << ",\"repaintBoundary\":";
    append_json_bool(output, render.repaint_boundary);
    output << ",\"layoutCount\":" << render.layout_count << ",\"paintCount\":" << render.paint_count
           << '}';
  }
  output << ",\"children\":[";
  for (std::size_t index = 0; index < node.children.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    append_json_node(output, node.children[index], depth + 1);
  }
  output << "]}";
}

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

const InspectorNode* InspectorNode::find(Element::Id search_id) const {
  std::vector<const InspectorNode*> pending{this};
  while (!pending.empty()) {
    const InspectorNode* node = pending.back();
    pending.pop_back();
    if (node->id == search_id) {
      return node;
    }
    for (auto child = node->children.rbegin(); child != node->children.rend(); ++child) {
      pending.push_back(&*child);
    }
  }
  return nullptr;
}

const InspectorNode* InspectorSnapshot::find(Element::Id id) const {
  return root.has_value() ? root->find(id) : nullptr;
}

std::string InspectorSnapshot::to_json() const {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "{\"mountCount\":" << mount_count << ",\"unmountCount\":" << unmount_count
         << ",\"pendingBuildCount\":" << pending_build_count
         << ",\"pendingLayoutCount\":" << pending_layout_count
         << ",\"pendingPaintCount\":" << pending_paint_count
         << ",\"pendingCompositingCount\":" << pending_compositing_count << ",\"root\":";
  if (root.has_value()) {
    append_json_node(output, *root, 0);
  } else {
    output << "null";
  }
  output << '}';
  return std::move(output).str();
}

std::string TimelineSnapshot::to_json() const {
  std::size_t version = 1;
  for (const TimelineEvent& event : events) {
    static_cast<void>(timeline_lane_name(event.lane));
    static_cast<void>(timeline_phase_name(event.phase));
    static_cast<void>(timeline_outcome_name(event.outcome));
    if (event.duration < std::chrono::nanoseconds::zero()) {
      throw std::invalid_argument("Timeline event duration cannot be negative");
    }
    if (event.lane == TimelineLane::raster || event.phase == TimelinePhase::raster_frame ||
        event.phase == TimelinePhase::surface_acquire || event.phase == TimelinePhase::rasterize ||
        event.phase == TimelinePhase::surface_present ||
        event.outcome == TimelineOutcome::unavailable ||
        event.outcome == TimelineOutcome::out_of_date || event.outcome == TimelineOutcome::lost) {
      version = 2;
    }
  }
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << "{\"version\":" << version << ",\"droppedEventCount\":" << dropped_event_count
         << ",\"events\":[";
  for (std::size_t index = 0; index < events.size(); ++index) {
    const TimelineEvent& event = events[index];
    if (index != 0) {
      output << ',';
    }
    output << "{\"sequence\":" << event.sequence << ",\"spanId\":" << event.span_id
           << ",\"parentSpanId\":" << event.parent_span_id << ",\"frameId\":" << event.frame_id
           << ",\"lane\":";
    append_json_string(output, timeline_lane_name(event.lane));
    output << ",\"phase\":";
    append_json_string(output, timeline_phase_name(event.phase));
    output << ",\"outcome\":";
    append_json_string(output, timeline_outcome_name(event.outcome));
    output << ",\"startNs\":" << event.start.count() << ",\"durationNs\":" << event.duration.count()
           << ",\"pass\":" << event.pass << ",\"workCount\":" << event.work_count << '}';
  }
  output << "]}";
  return std::move(output).str();
}

InspectorSnapshot BuildOwner::inspect() const {
  if (reconciling_ || framing_) {
    throw std::logic_error("BuildOwner cannot be inspected during reconciliation or framing");
  }

  InspectorSnapshot snapshot{mount_count_,
                             unmount_count_,
                             dirty_.size(),
                             render_owner_.pending_layout_count(),
                             render_owner_.pending_paint_count(),
                             render_owner_.pending_compositing_count(),
                             std::nullopt};
  const std::shared_ptr<FocusNode> focused_node = focus_manager_.focused_node();
  const auto collect = [&](auto&& self, const Element& element, InspectorElementState state,
                           std::size_t traversal_depth) -> InspectorNode {
    if (traversal_depth >= InspectorSnapshot::maximum_depth) {
      throw std::length_error("Element tree exceeds the inspector maximum depth");
    }
    std::optional<InspectorRenderSnapshot> render_snapshot;
    if (const RenderObject* render = element.render_object_.get(); render != nullptr) {
      InspectorRenderProtocol protocol = InspectorRenderProtocol::object;
      if (dynamic_cast<const RenderBox*>(render) != nullptr) {
        protocol = InspectorRenderProtocol::box;
      } else if (dynamic_cast<const RenderSliver*>(render) != nullptr) {
        protocol = InspectorRenderProtocol::sliver;
      }
      render_snapshot = InspectorRenderSnapshot{render->id(),
                                                protocol,
                                                render->needs_layout(),
                                                render->needs_paint(),
                                                render->needs_compositing(),
                                                render->is_repaint_boundary(),
                                                render->layout_count(),
                                                render->paint_count()};
    }

    InspectorNode node{element.id_,
                       element.generation_,
                       element.depth_,
                       element.debug_name_,
                       element.debug_value_,
                       element.key_.empty() ? std::string{} : element.key_.to_string(),
                       state,
                       element.update_count_,
                       element.dirty_,
                       element.state_.size(),
                       element.dependencies_.size(),
                       element.environment_.size(),
                       element.resources_.size(),
                       element.focus_node_ != nullptr,
                       element.focus_node_ != nullptr && element.focus_node_ == focused_node,
                       std::move(render_snapshot),
                       {}};
    node.children.reserve(element.children_.size() + element.lazy_kept_alive_children_.size());
    for (const auto& child : element.children_) {
      if (child != nullptr) {
        node.children.push_back(self(self, *child, state, traversal_depth + 1));
      }
    }
    for (const auto& child : element.lazy_kept_alive_children_) {
      if (child != nullptr) {
        node.children.push_back(
          self(self, *child, InspectorElementState::dormant_keep_alive, traversal_depth + 1));
      }
    }
    return node;
  };
  if (root_ != nullptr) {
    snapshot.root = collect(collect, *root_, InspectorElementState::active, 0);
  }
  return snapshot;
}

std::string BuildOwner::dump_tree() const {
  std::ostringstream output;
  if (root_ != nullptr) {
    append_tree(*root_, output, 0);
  }
  return std::move(output).str();
}

} // namespace dui
