#include "dui/rendering.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>

namespace dui {

namespace {

std::string number(double value) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(1) << value;
  return std::move(output).str();
}

std::optional<Rect> intersect_rects(Rect left, Rect right) {
  const auto valid = [](Rect rect) {
    return std::isfinite(rect.origin.x) && std::isfinite(rect.origin.y) &&
           std::isfinite(rect.size.width) && std::isfinite(rect.size.height) &&
           rect.size.width >= 0.0 && rect.size.height >= 0.0;
  };
  if (!valid(left) || !valid(right)) {
    throw std::overflow_error("Semantics clip bounds must be finite and non-negative");
  }
  const long double x =
    std::max(static_cast<long double>(left.origin.x), static_cast<long double>(right.origin.x));
  const long double y =
    std::max(static_cast<long double>(left.origin.y), static_cast<long double>(right.origin.y));
  const long double right_edge =
    std::min(static_cast<long double>(left.origin.x) + left.size.width,
             static_cast<long double>(right.origin.x) + right.size.width);
  const long double bottom_edge =
    std::min(static_cast<long double>(left.origin.y) + left.size.height,
             static_cast<long double>(right.origin.y) + right.size.height);
  if (right_edge <= x || bottom_edge <= y) {
    return std::nullopt;
  }
  const long double width = right_edge - x;
  const long double height = bottom_edge - y;
  if (width > std::numeric_limits<double>::max() || height > std::numeric_limits<double>::max()) {
    throw std::overflow_error("Semantics clip intersection overflowed");
  }
  return Rect{{static_cast<double>(x), static_cast<double>(y)},
              {static_cast<double>(width), static_cast<double>(height)}};
}

Offset add_semantics_offset(Offset left, Offset right) {
  const long double x = static_cast<long double>(left.x) + right.x;
  const long double y = static_cast<long double>(left.y) + right.y;
  if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > std::numeric_limits<double>::max() ||
      std::abs(y) > std::numeric_limits<double>::max()) {
    throw std::overflow_error("Semantics global offset overflowed");
  }
  return {static_cast<double>(x), static_cast<double>(y)};
}

void append_layer(const Layer& layer, DisplayListBuilder& builder, Offset offset) {
  switch (layer.kind()) {
  case Layer::Kind::container: {
    const auto* container = dynamic_cast<const ContainerLayer*>(&layer);
    if (container == nullptr) {
      throw std::logic_error("Layer kind does not match ContainerLayer type");
    }
    for (const LayerPtr& child : container->children()) {
      append_layer(*child, builder, offset);
    }
    break;
  }
  case Layer::Kind::offset: {
    const auto* translated = dynamic_cast<const OffsetLayer*>(&layer);
    if (translated == nullptr) {
      throw std::logic_error("Layer kind does not match OffsetLayer type");
    }
    append_layer(*translated->child(), builder, offset + translated->offset());
    break;
  }
  case Layer::Kind::clip_rect: {
    const auto* clipped = dynamic_cast<const ClipRectLayer*>(&layer);
    if (clipped == nullptr) {
      throw std::logic_error("Layer kind does not match ClipRectLayer type");
    }
    builder.push_clip_rect({clipped->clip_rect().origin + offset, clipped->clip_rect().size});
    append_layer(*clipped->child(), builder, offset);
    builder.pop_clip();
    break;
  }
  case Layer::Kind::display_list: {
    const auto* display_layer = dynamic_cast<const DisplayListLayer*>(&layer);
    if (display_layer == nullptr) {
      throw std::logic_error("Layer kind does not match DisplayListLayer type");
    }
    const auto& display = display_layer->display_list();
    for (const DisplayCommand& command : display.commands()) {
      std::visit(
        [&](const auto& value) {
          using T = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::same_as<T, DrawTextCommand>) {
            builder.draw_text(value.text, value.offset + offset);
          } else if constexpr (std::same_as<T, DrawRectCommand>) {
            builder.draw_rect({value.rect.origin + offset, value.rect.size}, value.color);
          } else if constexpr (std::same_as<T, DrawImageCommand>) {
            builder.draw_image(value.asset,
                               {value.destination.origin + offset, value.destination.size});
          } else if constexpr (std::same_as<T, PushClipRectCommand>) {
            builder.push_clip_rect({value.rect.origin + offset, value.rect.size});
          } else {
            builder.pop_clip();
          }
        },
        command);
    }
    break;
  }
  }
}

void dump_layer(const Layer& layer, std::ostringstream& output, std::size_t depth) {
  const std::string indent(depth * 2, ' ');
  switch (layer.kind()) {
  case Layer::Kind::container: {
    const auto* container = dynamic_cast<const ContainerLayer*>(&layer);
    if (container == nullptr) {
      throw std::logic_error("Layer kind does not match ContainerLayer type");
    }
    output << indent << "container\n";
    for (const LayerPtr& child : container->children()) {
      dump_layer(*child, output, depth + 1);
    }
    break;
  }
  case Layer::Kind::offset: {
    const auto* translated = dynamic_cast<const OffsetLayer*>(&layer);
    if (translated == nullptr) {
      throw std::logic_error("Layer kind does not match OffsetLayer type");
    }
    output << indent << "offset(" << number(translated->offset().x) << ", "
           << number(translated->offset().y) << ")\n";
    dump_layer(*translated->child(), output, depth + 1);
    break;
  }
  case Layer::Kind::clip_rect: {
    const auto* clipped = dynamic_cast<const ClipRectLayer*>(&layer);
    if (clipped == nullptr) {
      throw std::logic_error("Layer kind does not match ClipRectLayer type");
    }
    output << indent << "clip_rect(" << number(clipped->clip_rect().origin.x) << ", "
           << number(clipped->clip_rect().origin.y) << ", "
           << number(clipped->clip_rect().size.width) << ", "
           << number(clipped->clip_rect().size.height) << ")\n";
    dump_layer(*clipped->child(), output, depth + 1);
    break;
  }
  case Layer::Kind::display_list: {
    const auto* display_layer = dynamic_cast<const DisplayListLayer*>(&layer);
    if (display_layer == nullptr) {
      throw std::logic_error("Layer kind does not match DisplayListLayer type");
    }
    const auto& display = display_layer->display_list();
    output << indent << "display_list(" << display.commands().size() << ")\n";
    break;
  }
  }
}

} // namespace

DisplayList::DisplayList(std::vector<DisplayCommand> commands) : commands_(std::move(commands)) {
  std::size_t clip_depth = 0;
  for (const DisplayCommand& command : commands_) {
    if (std::holds_alternative<PushClipRectCommand>(command)) {
      ++clip_depth;
    } else if (std::holds_alternative<PopClipCommand>(command)) {
      if (clip_depth == 0) {
        throw std::invalid_argument("DisplayList clip stack underflow");
      }
      --clip_depth;
    }
  }
  if (clip_depth != 0) {
    throw std::invalid_argument("DisplayList contains an unclosed clip");
  }
}

std::string DisplayList::dump() const {
  std::ostringstream output;
  for (const DisplayCommand& command : commands_) {
    std::visit(
      [&](const auto& value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::same_as<T, DrawTextCommand>) {
          output << "text(\"" << value.text << "\", " << number(value.offset.x) << ", "
                 << number(value.offset.y) << ")\n";
        } else if constexpr (std::same_as<T, DrawRectCommand>) {
          output << "rect(" << number(value.rect.origin.x) << ", " << number(value.rect.origin.y)
                 << ", " << number(value.rect.size.width) << ", " << number(value.rect.size.height)
                 << ", 0x" << std::hex << std::setw(8) << std::setfill('0') << value.color
                 << std::dec << ")\n";
        } else if constexpr (std::same_as<T, DrawImageCommand>) {
          output << "image(\"" << value.asset << "\", " << number(value.destination.origin.x)
                 << ", " << number(value.destination.origin.y) << ", "
                 << number(value.destination.size.width) << ", "
                 << number(value.destination.size.height) << ")\n";
        } else if constexpr (std::same_as<T, PushClipRectCommand>) {
          output << "push_clip_rect(" << number(value.rect.origin.x) << ", "
                 << number(value.rect.origin.y) << ", " << number(value.rect.size.width) << ", "
                 << number(value.rect.size.height) << ")\n";
        } else {
          output << "pop_clip()\n";
        }
      },
      command);
  }
  return std::move(output).str();
}

void DisplayListBuilder::draw_text(std::string_view text, Offset offset) {
  commands_.emplace_back(DrawTextCommand{std::string(text), offset});
}

void DisplayListBuilder::draw_rect(Rect rect, std::uint32_t color) {
  commands_.emplace_back(DrawRectCommand{rect, color});
}

void DisplayListBuilder::draw_image(std::string_view asset, Rect destination) {
  commands_.emplace_back(DrawImageCommand{std::string(asset), destination});
}

void DisplayListBuilder::push_clip_rect(Rect rect) {
  static_cast<void>(BoxConstraints{}.constrain(rect.size));
  if (!std::isfinite(rect.origin.x) || !std::isfinite(rect.origin.y)) {
    throw std::invalid_argument("Clip rectangle origin must be finite");
  }
  commands_.emplace_back(PushClipRectCommand{rect});
  ++clip_depth_;
}

void DisplayListBuilder::pop_clip() {
  if (clip_depth_ == 0) {
    throw std::logic_error("DisplayList clip stack underflow");
  }
  commands_.emplace_back(PopClipCommand{});
  --clip_depth_;
}

ContainerLayer::ContainerLayer(std::vector<LayerPtr> children) : children_(std::move(children)) {
  if (std::ranges::any_of(children_, [](const LayerPtr& child) { return child == nullptr; })) {
    throw std::invalid_argument("ContainerLayer children cannot be null");
  }
}

OffsetLayer::OffsetLayer(Offset offset, LayerPtr child)
  : offset_(offset), child_(std::move(child)) {
  if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
    throw std::invalid_argument("OffsetLayer offset must be finite");
  }
  if (child_ == nullptr) {
    throw std::invalid_argument("OffsetLayer child cannot be null");
  }
}

ClipRectLayer::ClipRectLayer(Rect clip_rect, LayerPtr child)
  : clip_rect_(clip_rect), child_(std::move(child)) {
  static_cast<void>(BoxConstraints{}.constrain(clip_rect.size));
  if (!std::isfinite(clip_rect.origin.x) || !std::isfinite(clip_rect.origin.y)) {
    throw std::invalid_argument("ClipRectLayer origin must be finite");
  }
  if (child_ == nullptr) {
    throw std::invalid_argument("ClipRectLayer child cannot be null");
  }
}

LayerTree::LayerTree(Size frame_size, LayerPtr root)
  : frame_size_(frame_size), root_(std::move(root)) {
  static_cast<void>(BoxConstraints{}.constrain(frame_size));
  if (root_ == nullptr) {
    throw std::invalid_argument("LayerTree root cannot be null");
  }
}

DisplayList LayerTree::flatten() const {
  DisplayListBuilder builder;
  if (root_ != nullptr) {
    append_layer(*root_, builder, {});
  }
  return std::move(builder).build();
}

std::string LayerTree::dump() const {
  if (root_ == nullptr) {
    return "<empty>\n";
  }
  std::ostringstream output;
  output << "layer_tree(" << number(frame_size_.width) << ", " << number(frame_size_.height)
         << ")\n";
  dump_layer(*root_, output, 1);
  return std::move(output).str();
}

RenderObject::RenderObject(RenderOwner& owner) : owner_(&owner), id_(owner.register_object(*this)) {
  owner.schedule_layout(*this);
  owner.schedule_paint(*this);
}

RenderObject::~RenderObject() {
  if (parent_ != nullptr) {
    std::erase(parent_->children_, this);
    parent_->mark_needs_layout();
    parent_ = nullptr;
  }
  for (RenderObject* child : children_) {
    if (child->parent_ == this) {
      child->parent_ = nullptr;
    }
  }
  detach_from_owner();
}

void RenderObject::detach_from_owner() {
  if (owner_ != nullptr) {
    owner_->forget(*this);
    owner_ = nullptr;
  }
}

void RenderObject::mark_needs_layout() {
  if (owner_ == nullptr) {
    throw std::logic_error("RenderObject owner no longer exists");
  }
  if (!needs_layout_) {
    needs_layout_ = true;
    owner_->schedule_layout(*this);
  }
  mark_needs_paint();
  if (parent_ != nullptr) {
    parent_->mark_needs_layout();
  }
}

void RenderObject::mark_needs_paint() {
  if (owner_ == nullptr) {
    throw std::logic_error("RenderObject owner no longer exists");
  }
  if (owner_->paint_depth_ != 0) {
    throw std::logic_error("RenderObject invalidation is not allowed during paint");
  }
  if (!needs_paint_) {
    needs_paint_ = true;
    owner_->schedule_paint(*this);
  }
  if (!is_repaint_boundary() && parent_ != nullptr) {
    parent_->mark_needs_paint();
    return;
  }
  if (is_repaint_boundary()) {
    needs_compositing_ = false;
    for (RenderObject* ancestor = parent_; ancestor != nullptr; ancestor = ancestor->parent_) {
      if (ancestor->is_repaint_boundary() && !ancestor->needs_compositing_) {
        ancestor->needs_compositing_ = true;
        owner_->schedule_compositing(*ancestor);
      }
    }
  }
}

void RenderObject::validate_child_protocol(const RenderObject&) const {
  throw std::logic_error("This RenderObject protocol does not accept children");
}

void RenderObject::set_children(std::span<RenderObject* const> children) {
  if (owner_ == nullptr) {
    throw std::logic_error("RenderObject owner no longer exists");
  }
  const bool unchanged =
    children_.size() == children.size() && std::ranges::equal(children_, children);
  if (unchanged) {
    return;
  }

  std::vector<RenderObject*> next{children.begin(), children.end()};
  validate_child_count(next.size());
  for (std::size_t index = 0; index < next.size(); ++index) {
    RenderObject* child = next[index];
    if (child == nullptr) {
      throw std::invalid_argument("A RenderObject child cannot be null");
    }
    if (child->owner_ != owner_) {
      throw std::logic_error("A RenderObject child must have the same owner");
    }
    if (child == this) {
      throw std::logic_error("A RenderObject cannot be its own child");
    }
    if (child->parent_ != nullptr && child->parent_ != this) {
      throw std::logic_error("RenderObject cannot have more than one parent");
    }
    validate_child_protocol(*child);
    for (RenderObject* ancestor = this; ancestor != nullptr; ancestor = ancestor->parent_) {
      if (ancestor == child) {
        throw std::logic_error("A RenderObject tree cannot contain a cycle");
      }
    }
    if (std::ranges::find(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(index), child) !=
        next.begin() + static_cast<std::ptrdiff_t>(index)) {
      throw std::logic_error("A RenderObject child cannot occur more than once");
    }
  }

  for (RenderObject* child : children_) {
    if (child->parent_ == this) {
      child->parent_ = nullptr;
    }
  }
  children_ = std::move(next);
  for (RenderObject* child : children_) {
    child->parent_ = this;
  }
  mark_needs_layout();
}

bool RenderObject::hit_test(HitTestResult& result, Offset position) {
  return hit_test_protocol(result, position);
}

void RenderBox::validate_child_protocol(const RenderObject& child) const {
  if (accepts_sliver_children_) {
    if (dynamic_cast<const RenderSliver*>(&child) == nullptr) {
      throw std::logic_error("A RenderViewport can only adopt RenderSliver children");
    }
    return;
  }
  if (dynamic_cast<const RenderBox*>(&child) == nullptr) {
    throw std::logic_error("A RenderBox can only adopt another RenderBox");
  }
}

void RenderBox::layout(BoxConstraints constraints) {
  if (!needs_layout_ && constraints_ == constraints) {
    return;
  }
  const std::optional<BoxConstraints> previous_constraints = constraints_;
  const Size previous_size = size_;
  const bool constraints_changed = constraints_ != constraints;
  constraints_ = constraints;
  needs_layout_ = true;
  try {
    perform_layout();
    size_ = constraints.constrain(size_);
  } catch (...) {
    constraints_ = previous_constraints;
    size_ = previous_size;
    owner_->schedule_layout(*this);
    throw;
  }
  if (constraints_changed || size_ != previous_size) {
    mark_needs_paint();
  }
  needs_layout_ = false;
  ++layout_count_;
  owner_->dirty_layout_.erase(this);
}

void RenderBox::layout_child(RenderObject& child, BoxConstraints child_constraints,
                             Offset child_offset) {
  auto* child_box = dynamic_cast<RenderBox*>(&child);
  if (child_box == nullptr) {
    throw std::logic_error("RenderBox layout requires a RenderBox child");
  }
  if (child_box->parent_data_.offset != child_offset) {
    child_box->parent_data_.offset = child_offset;
    if (child_box->is_repaint_boundary()) {
      mark_needs_paint();
    } else {
      child_box->mark_needs_paint();
    }
  }
  child_box->layout(child_constraints);
}

RenderObject* RenderObject::hit_test(Offset position) {
  HitTestResult result;
  static_cast<void>(hit_test(result, position));
  return result.target();
}

void RenderSliver::validate_child_protocol(const RenderObject& child) const {
  if (!accepts_box_children_ || dynamic_cast<const RenderBox*>(&child) == nullptr) {
    throw std::logic_error("This RenderSliver can only adopt RenderBox children");
  }
}

void RenderSliver::layout(SliverConstraints constraints) {
  if (!needs_layout_ && constraints_ == constraints) {
    return;
  }
  const std::optional<SliverConstraints> previous_constraints = constraints_;
  const SliverGeometry previous_geometry = geometry_;
  const bool constraints_changed = constraints_ != constraints;
  constraints_ = constraints;
  needs_layout_ = true;
  try {
    perform_layout();
    if (geometry_.paint_extent() > constraints.remaining_paint_extent()) {
      throw std::logic_error("Sliver paint extent exceeds its remaining paint extent");
    }
  } catch (...) {
    constraints_ = previous_constraints;
    geometry_ = previous_geometry;
    owner_->schedule_layout(*this);
    throw;
  }
  if (constraints_changed || geometry_ != previous_geometry) {
    mark_needs_paint();
  }
  needs_layout_ = false;
  ++layout_count_;
  owner_->dirty_layout_.erase(this);
}

void RenderSliver::layout_box_child(RenderObject& child, BoxConstraints child_constraints,
                                    Offset child_offset) {
  auto* child_box = dynamic_cast<RenderBox*>(&child);
  if (child_box == nullptr) {
    throw std::logic_error("RenderSliver box layout requires a RenderBox child");
  }
  const Offset previous_offset = child_box->parent_data_.offset;
  try {
    child_box->layout(child_constraints);
    child_box->parent_data_.offset = child_offset;
  } catch (...) {
    child_box->parent_data_.offset = previous_offset;
    throw;
  }
  if (previous_offset != child_offset) {
    if (child_box->is_repaint_boundary()) {
      mark_needs_paint();
    } else {
      child_box->mark_needs_paint();
    }
  }
}

bool RenderSliver::hit_test_protocol(HitTestResult& result, Offset position) {
  if (!constraints_.has_value() || position.x < 0.0 ||
      position.x >= constraints().cross_axis_extent() || position.y < 0.0 ||
      position.y >= geometry_.hit_test_extent()) {
    return false;
  }
  for (RenderObject* child : children() | std::views::reverse) {
    const auto& child_box = static_cast<const RenderBox&>(*child);
    if (child->hit_test(result, position - child_box.offset())) {
      result.add({this, position});
      return true;
    }
  }
  return false;
}

RenderViewport::RenderViewport(RenderOwner& owner, double scroll_offset)
  : RenderBox(owner, true), scroll_offset_(scroll_offset) {
  if (!std::isfinite(scroll_offset) || scroll_offset < 0.0) {
    throw std::invalid_argument("Viewport scroll offset must be finite and non-negative");
  }
}

void RenderViewport::set_scroll_offset(double scroll_offset) {
  if (!std::isfinite(scroll_offset) || scroll_offset < 0.0) {
    throw std::invalid_argument("Viewport scroll offset must be finite and non-negative");
  }
  if (scroll_offset_ == scroll_offset) {
    return;
  }
  scroll_offset_ = scroll_offset;
  mark_needs_layout();
}

void RenderViewport::perform_layout() {
  const Size viewport = constraints().biggest();
  if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height)) {
    throw std::logic_error("RenderViewport requires finite viewport constraints");
  }
  set_size(viewport);

  double preceding = 0.0;
  for (RenderObject* child : children()) {
    auto& sliver = static_cast<RenderSliver&>(*child);
    const double paint_origin = std::max(0.0, preceding - scroll_offset_);
    const double remaining = std::max(0.0, viewport.height - paint_origin);
    const double local_scroll = std::max(0.0, scroll_offset_ - preceding);
    const Offset previous_offset = sliver.parent_data_.paint_offset;
    sliver.layout({local_scroll, preceding, remaining, viewport.width, viewport.height});
    sliver.parent_data_.paint_offset = {0.0, paint_origin};
    if (previous_offset != sliver.parent_data_.paint_offset) {
      sliver.mark_needs_paint();
    }
    preceding += sliver.geometry().scroll_extent();
    if (!std::isfinite(preceding)) {
      throw std::overflow_error("Viewport scroll extent overflowed");
    }
  }
  max_scroll_extent_ = std::max(0.0, preceding - viewport.height);
}

bool RenderViewport::hit_test_protocol(HitTestResult& result, Offset position) {
  if (!size().contains(position)) {
    return false;
  }
  for (RenderObject* child : children() | std::views::reverse) {
    const auto& sliver = static_cast<const RenderSliver&>(*child);
    if (child->hit_test(result, position - sliver.parent_data().paint_offset)) {
      result.add({this, position});
      return true;
    }
  }
  return false;
}

void RenderSliverToBoxAdapter::perform_layout() {
  if (children().empty()) {
    set_geometry({});
    return;
  }
  auto& child = static_cast<RenderBox&>(*children().front());
  layout_box_child(child, constraints().as_box_constraints(),
                   {0.0, -constraints().scroll_offset()});
  const double scroll_extent = child.size().height;
  const double visible = std::min(std::max(0.0, scroll_extent - constraints().scroll_offset()),
                                  constraints().remaining_paint_extent());
  set_geometry({scroll_extent, visible, scroll_extent, visible,
                constraints().scroll_offset() > 0.0 || visible < scroll_extent});
}

void RenderSliverToBoxAdapter::validate_child_count(std::size_t count) const {
  if (count > 1) {
    throw std::logic_error("RenderSliverToBoxAdapter accepts at most one RenderBox child");
  }
}

namespace {

void validate_item_extent(double item_extent) {
  if (!std::isfinite(item_extent) || item_extent <= 0.0) {
    throw std::invalid_argument("Sliver fixed item extent must be finite and positive");
  }
}

void validate_cache_extent(double cache_extent) {
  if (!std::isfinite(cache_extent) || cache_extent < 0.0) {
    throw std::invalid_argument("Sliver cache extent must be finite and non-negative");
  }
}

std::size_t first_item_at(double offset, double item_extent, std::size_t child_count,
                          double scroll_extent) {
  if (child_count == 0 || offset >= scroll_extent) {
    return child_count;
  }
  std::size_t first = 0;
  std::size_t last = child_count;
  while (first < last) {
    const std::size_t middle = first + (last - first) / 2;
    const double item_start = static_cast<double>(middle) * item_extent;
    if (item_start <= offset) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first == 0 ? 0 : first - 1;
}

std::size_t first_item_starting_at_or_after_viewport_position(double scroll_offset,
                                                              long double viewport_position,
                                                              double item_extent,
                                                              std::size_t child_count) {
  std::size_t first = 0;
  std::size_t last = child_count;
  while (first < last) {
    const std::size_t middle = first + (last - first) / 2;
    const double item_start = static_cast<double>(middle) * item_extent;
    const long double relative_start = static_cast<long double>(item_start) - scroll_offset;
    if (relative_start < viewport_position) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first;
}

std::size_t first_item_intersecting_leading_cache(double scroll_offset, double cache_extent,
                                                  double item_extent, std::size_t child_count,
                                                  double scroll_extent) {
  if (child_count == 0 || static_cast<long double>(scroll_offset) - scroll_extent >=
                            static_cast<long double>(cache_extent)) {
    return child_count;
  }
  if (scroll_offset <= cache_extent) {
    return 0;
  }

  const long double leading_position = -static_cast<long double>(cache_extent);
  std::size_t first = 0;
  std::size_t last = child_count;
  while (first < last) {
    const std::size_t middle = first + (last - first) / 2;
    const double item_start = static_cast<double>(middle) * item_extent;
    const long double relative_start = static_cast<long double>(item_start) - scroll_offset;
    if (relative_start <= leading_position) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first == 0 ? 0 : first - 1;
}

std::size_t item_at_viewport_position(double scroll_offset, double viewport_position,
                                      double item_extent, std::size_t child_count) {
  std::size_t first = 0;
  std::size_t last = child_count;
  while (first < last) {
    const std::size_t middle = first + (last - first) / 2;
    const double item_start = static_cast<double>(middle) * item_extent;
    const long double relative_start = static_cast<long double>(item_start) - scroll_offset;
    if (relative_start <= static_cast<long double>(viewport_position)) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first == 0 ? child_count : first - 1;
}

} // namespace

RenderSliverFixedExtentList::RenderSliverFixedExtentList(RenderOwner& owner, double item_extent)
  : RenderSliver(owner, true), item_extent_(item_extent) {
  validate_item_extent(item_extent);
}

void RenderSliverFixedExtentList::set_item_extent(double item_extent) {
  validate_item_extent(item_extent);
  if (item_extent_ == item_extent) {
    return;
  }
  item_extent_ = item_extent;
  mark_needs_layout();
}

void RenderSliverFixedExtentList::set_cache_extent(double cache_extent) {
  validate_cache_extent(cache_extent);
  if (cache_extent_ == cache_extent) {
    return;
  }
  cache_extent_ = cache_extent;
  mark_needs_layout();
}

void RenderSliverFixedExtentList::set_lazy_model(std::size_t logical_child_count,
                                                 std::uint64_t revision) {
  if (lazy_ && logical_child_count_ == logical_child_count && model_revision_ == revision) {
    return;
  }
  lazy_ = true;
  logical_child_count_ = logical_child_count;
  model_revision_ = revision;
  requested_child_range_.reset();
  mark_needs_layout();
}

void RenderSliverFixedExtentList::set_mounted_range(std::size_t first, std::size_t count,
                                                    std::uint64_t revision) {
  if (!lazy_ || revision != model_revision_ || first > logical_child_count_ ||
      count > logical_child_count_ - first) {
    throw std::logic_error("Mounted Sliver range does not match the lazy model");
  }
  first_mounted_index_ = first;
  mounted_child_count_ = count;
  mounted_revision_ = revision;
  requested_child_range_.reset();
  mark_needs_layout();
}

void RenderSliverFixedExtentList::clear_lazy_model() {
  if (!lazy_) {
    return;
  }
  lazy_ = false;
  logical_child_count_ = 0;
  first_mounted_index_ = 0;
  mounted_child_count_ = 0;
  model_revision_ = 0;
  mounted_revision_ = 0;
  requested_child_range_.reset();
  mark_needs_layout();
}

void RenderSliverFixedExtentList::perform_layout() {
  const std::size_t logical_count = lazy_ ? logical_child_count_ : children().size();
  const double child_count = static_cast<double>(logical_count);
  const double scroll_extent = child_count * item_extent_;
  if (!std::isfinite(scroll_extent)) {
    throw std::overflow_error("Sliver fixed-extent list scroll extent overflowed");
  }

  const double visible_extent =
    std::min(std::max(0.0, scroll_extent - constraints().scroll_offset()),
             constraints().remaining_paint_extent());
  first_visible_index_ =
    first_item_at(constraints().scroll_offset(), item_extent_, logical_count, scroll_extent);
  if (visible_extent == 0.0) {
    visible_child_count_ = 0;
  } else {
    const std::size_t trailing_index = first_item_starting_at_or_after_viewport_position(
      constraints().scroll_offset(), visible_extent, item_extent_, logical_count);
    visible_child_count_ = trailing_index - first_visible_index_;
  }

  const std::size_t first_cached_index = first_item_intersecting_leading_cache(
    constraints().scroll_offset(), cache_extent_, item_extent_, logical_count, scroll_extent);
  const long double trailing_cache_position =
    static_cast<long double>(constraints().remaining_paint_extent()) + cache_extent_;
  const std::size_t cached_end = first_item_starting_at_or_after_viewport_position(
    constraints().scroll_offset(), trailing_cache_position, item_extent_, logical_count);
  if (lazy_ &&
      (mounted_revision_ != model_revision_ || first_mounted_index_ != first_cached_index ||
       mounted_child_count_ != cached_end - first_cached_index ||
       mounted_child_count_ != children().size())) {
    requested_child_range_ = ChildRange{first_cached_index, cached_end, model_revision_};
    first_visible_child_ = 0;
    visible_child_count_ = 0;
    set_geometry({scroll_extent, visible_extent, scroll_extent, 0.0,
                  constraints().scroll_offset() > 0.0 || visible_extent < scroll_extent});
    return;
  }

  requested_child_range_.reset();
  first_visible_child_ = lazy_ ? first_visible_index_ - first_mounted_index_ : first_visible_index_;

  const BoxConstraints child_constraints{constraints().cross_axis_extent(),
                                         constraints().cross_axis_extent(), item_extent_,
                                         item_extent_};
  const std::size_t end = first_visible_child_ + visible_child_count_;
  for (std::size_t child_index = first_visible_child_; child_index < end; ++child_index) {
    const std::size_t logical_index = lazy_ ? first_mounted_index_ + child_index : child_index;
    const double item_start = static_cast<double>(logical_index) * item_extent_;
    layout_box_child(*children()[child_index], child_constraints,
                     {0.0, static_cast<double>(static_cast<long double>(item_start) -
                                               constraints().scroll_offset())});
  }
  set_geometry({scroll_extent, visible_extent, scroll_extent, visible_extent,
                constraints().scroll_offset() > 0.0 || visible_extent < scroll_extent});
}

bool RenderSliverFixedExtentList::hit_test_protocol(HitTestResult& result, Offset position) {
  const std::size_t visible_end = first_visible_index_ + visible_child_count_;
  const std::size_t child_end = first_visible_child_ + visible_child_count_;
  if (needs_layout() || !has_constraints() || first_visible_index_ > visible_end ||
      child_end > children().size() || position.x < 0.0 ||
      position.x >= constraints().cross_axis_extent() || position.y < 0.0 ||
      position.y >= geometry().hit_test_extent()) {
    return false;
  }
  const std::size_t candidate = item_at_viewport_position(constraints().scroll_offset(), position.y,
                                                          item_extent_, logical_child_count());
  if (candidate < first_visible_index_ || candidate >= visible_end) {
    return false;
  }
  const std::size_t child_index = lazy_ ? candidate - first_mounted_index_ : candidate;
  if (child_index >= child_end) {
    return false;
  }
  auto* child = static_cast<RenderBox*>(children()[child_index]);
  const Offset local_position{position.x, position.y - child->offset().y};
  if (child->hit_test(result, local_position)) {
    result.add({this, position});
    return true;
  }
  return false;
}

bool RenderBox::hit_test_protocol(HitTestResult& result, Offset position) {
  if (!size_.contains(position)) {
    return false;
  }

  for (RenderObject* child : children_ | std::views::reverse) {
    const auto& child_box = static_cast<const RenderBox&>(*child);
    if (child->hit_test(result, position - child_box.parent_data_.offset)) {
      result.add({this, position});
      return true;
    }
  }
  if (hit_test_self(position)) {
    result.add({this, position});
    return true;
  }
  return false;
}

bool RenderObject::handle_semantics_action(SemanticsAction action) {
  switch (action) {
  case SemanticsAction::activate:
    if (!has_activation_handler()) {
      return false;
    }
    static_cast<void>(handle_activate());
    return true;
  }
  return false;
}

RenderText::RenderText(RenderOwner& owner, std::string text, std::function<void()> on_activate)
  : RenderBox(owner), text_(std::move(text)), on_activate_(std::move(on_activate)) {}

void RenderText::set_text(std::string text) {
  if (text_ == text) {
    return;
  }
  text_ = std::move(text);
  mark_needs_layout();
}

void RenderText::perform_layout() {
  constexpr double character_width = 8.0;
  constexpr double line_height = 16.0;
  set_size(
    constraints().constrain({static_cast<double>(text_.size()) * character_width, line_height}));
}

void RenderText::paint(PaintingContext& context, Offset offset) {
  context.draw_text(text_, offset);
}

bool RenderText::handle_activate() {
  if (on_activate_) {
    auto callback = on_activate_;
    callback();
  }
  return false;
}

RenderImage::RenderImage(RenderOwner& owner, std::string asset, Size intrinsic_size,
                         std::string semantics_label)
  : RenderBox(owner), asset_(std::move(asset)), intrinsic_size_(intrinsic_size),
    semantics_label_(std::move(semantics_label)) {
  static_cast<void>(BoxConstraints{}.constrain(intrinsic_size));
}

void RenderImage::set_image(std::string asset, Size intrinsic_size, std::string semantics_label) {
  static_cast<void>(BoxConstraints{}.constrain(intrinsic_size));
  const bool size_changed = intrinsic_size_ != intrinsic_size;
  const bool asset_changed = asset_ != asset;
  const bool semantics_changed = semantics_label_ != semantics_label;
  if (!size_changed && !asset_changed && !semantics_changed) {
    return;
  }
  asset_ = std::move(asset);
  intrinsic_size_ = intrinsic_size;
  semantics_label_ = std::move(semantics_label);
  if (size_changed) {
    mark_needs_layout();
  } else if (asset_changed) {
    mark_needs_paint();
  }
}

void RenderImage::perform_layout() { set_size(constraints().constrain(intrinsic_size_)); }

void RenderImage::paint(PaintingContext& context, Offset offset) {
  context.draw_image(asset_, {offset, size()});
}

void RenderVStack::perform_layout() {
  double width = 0.0;
  double height = 0.0;
  const BoxConstraints child_constraints{0.0, constraints().max_width(), 0.0, infinity};

  for (RenderObject* child : children()) {
    auto& child_box = static_cast<RenderBox&>(*child);
    layout_child(child_box, child_constraints, {0.0, height});
    width = std::max(width, child_box.size().width);
    height += child_box.size().height;
  }
  set_size(constraints().constrain({width, height}));
}

void RenderHStack::perform_layout() {
  double width = 0.0;
  double height = 0.0;
  const BoxConstraints child_constraints{0.0, infinity, 0.0, constraints().max_height()};

  for (RenderObject* child : children()) {
    auto& child_box = static_cast<RenderBox&>(*child);
    layout_child(child_box, child_constraints, {width, 0.0});
    width += child_box.size().width;
    height = std::max(height, child_box.size().height);
  }
  set_size(constraints().constrain({width, height}));
}

void RenderStack::perform_layout() {
  double width = 0.0;
  double height = 0.0;
  const BoxConstraints child_constraints = constraints().loosen();
  for (RenderObject* child : children()) {
    auto& child_box = static_cast<RenderBox&>(*child);
    layout_child(child_box, child_constraints, {});
    width = std::max(width, child_box.size().width);
    height = std::max(height, child_box.size().height);
  }
  set_size(constraints().constrain({width, height}));
}

namespace {

void validate_insets(Insets insets) {
  const double values[] = {insets.left, insets.top, insets.right, insets.bottom};
  if (std::ranges::any_of(values,
                          [](double value) { return !std::isfinite(value) || value < 0.0; })) {
    throw std::invalid_argument("Padding Insets must be finite and non-negative");
  }
}

} // namespace

RenderPadding::RenderPadding(RenderOwner& owner, Insets insets)
  : RenderBox(owner), insets_(insets) {
  validate_insets(insets);
}

void RenderPadding::set_insets(Insets insets) {
  validate_insets(insets);
  if (insets_ == insets) {
    return;
  }
  insets_ = insets;
  mark_needs_layout();
}

void RenderPadding::perform_layout() {
  if (children().empty()) {
    set_size(constraints().constrain({insets_.horizontal(), insets_.vertical()}));
    return;
  }

  auto& child = static_cast<RenderBox&>(*children().front());
  const auto reduce = [](double value, double amount) {
    return std::isfinite(value) ? std::max(0.0, value - amount) : infinity;
  };
  const BoxConstraints child_constraints{reduce(constraints().min_width(), insets_.horizontal()),
                                         reduce(constraints().max_width(), insets_.horizontal()),
                                         reduce(constraints().min_height(), insets_.vertical()),
                                         reduce(constraints().max_height(), insets_.vertical())};
  layout_child(child, child_constraints, {insets_.left, insets_.top});
  set_size(constraints().constrain(
    {child.size().width + insets_.horizontal(), child.size().height + insets_.vertical()}));
}

void RenderColoredBox::set_color(std::uint32_t color) {
  if (color_ == color) {
    return;
  }
  color_ = color;
  mark_needs_paint();
}

void RenderColoredBox::perform_layout() {
  if (children().empty()) {
    set_size(constraints().constrain({}));
    return;
  }
  auto& child = static_cast<RenderBox&>(*children().front());
  layout_child(child, constraints().loosen(), {});
  set_size(constraints().constrain(child.size()));
}

void RenderColoredBox::paint(PaintingContext& context, Offset offset) {
  context.draw_rect({offset, size()}, color_);
}

void RenderActionBox::perform_layout() {
  if (children().empty()) {
    set_size(constraints().constrain({}));
    return;
  }
  auto& child = static_cast<RenderBox&>(*children().front());
  layout_child(child, constraints().loosen(), {});
  set_size(constraints().constrain(child.size()));
}

void RenderRepaintBoundary::perform_layout() {
  if (children().empty()) {
    set_size(constraints().constrain({}));
    return;
  }
  auto& child = static_cast<RenderBox&>(*children().front());
  layout_child(child, constraints().loosen(), {});
  set_size(constraints().constrain(child.size()));
}

bool RenderActionBox::handle_activate() {
  const bool stops_propagation = stops_propagation_;
  if (on_activate_) {
    auto callback = on_activate_;
    callback();
  }
  return stops_propagation;
}

void RenderSemanticsBox::perform_layout() {
  if (children().empty()) {
    set_size(constraints().constrain({}));
    return;
  }
  auto& child = static_cast<RenderBox&>(*children().front());
  layout_child(child, constraints().loosen(), {});
  set_size(constraints().constrain(child.size()));
}

bool RenderSemanticsBox::handle_activate() {
  if (!on_activate_) {
    return false;
  }
  auto callback = on_activate_;
  callback();
  return true;
}

void RenderSemanticsBox::validate_child_count(std::size_t count) const {
  if (count > 1) {
    throw std::logic_error("RenderSemanticsBox accepts at most one RenderBox child");
  }
}

void RenderView::perform_layout() {
  const Size viewport = constraints().biggest();
  if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height)) {
    throw std::logic_error("RenderView requires finite viewport constraints");
  }

  for (RenderObject* child : children()) {
    layout_child(static_cast<RenderBox&>(*child), BoxConstraints::loose(viewport), {});
  }
  set_size(viewport);
}

RenderOwner::RenderOwner() : root_(std::make_unique<RenderView>(*this)) {}

RenderOwner::~RenderOwner() {
  for (const auto& [id, object] : objects_) {
    static_cast<void>(id);
    object->parent_ = nullptr;
    object->children_.clear();
    object->owner_ = nullptr;
  }
  dirty_layout_.clear();
  dirty_paint_.clear();
  dirty_compositing_.clear();
  objects_.clear();
  root_.reset();
}

void RenderOwner::schedule_layout(RenderObject& object) { dirty_layout_.insert(&object); }

void RenderOwner::schedule_paint(RenderObject& object) { dirty_paint_.insert(&object); }

void RenderOwner::schedule_compositing(RenderObject& object) { dirty_compositing_.insert(&object); }

RenderObject::Id RenderOwner::register_object(RenderObject& object) {
  const RenderObject::Id id = next_id_++;
  objects_.emplace(id, &object);
  return id;
}

void RenderOwner::forget(RenderObject& object) {
  dirty_layout_.erase(&object);
  dirty_paint_.erase(&object);
  dirty_compositing_.erase(&object);
  objects_.erase(object.id_);
}

void RenderOwner::set_roots(std::span<RenderObject* const> roots) { root_->set_children(roots); }

void RenderOwner::ensure_boundary(RenderBox& boundary) {
  if (!boundary.is_repaint_boundary()) {
    throw std::logic_error("Retained layer requested for a non-boundary RenderBox");
  }
  if (boundary.needs_paint_ || boundary.retained_layer_ == nullptr) {
    record_boundary(boundary);
  } else if (boundary.needs_compositing_) {
    compose_boundary(boundary);
  }
}

void RenderOwner::record_boundary(RenderBox& boundary) {
  struct Recorder {
    DisplayListBuilder builder;
    std::vector<RenderBox::BoundaryChunk> chunks;
    std::vector<RenderObject*> painted;

    void flush() {
      if (!builder.empty()) {
        chunks.emplace_back(std::move(builder).build());
        builder = DisplayListBuilder{};
      }
    }
  } recorder;

  const auto record = [&](auto&& self, RenderObject& object, Offset offset) -> void {
    const std::optional<Rect> clip = object.paint_clip(offset);
    if (clip.has_value()) {
      recorder.flush();
      recorder.chunks.emplace_back(RenderBox::BoundaryClipBegin{*clip});
    }
    PaintingContext context{recorder.builder};
    object.paint(context, offset);
    recorder.painted.push_back(&object);
    const auto [first_child, last_child] = object.paint_child_range();
    if (first_child > last_child || last_child > object.children_.size()) {
      throw std::logic_error("RenderObject returned an invalid paint child range");
    }
    for (std::size_t index = first_child; index < last_child; ++index) {
      RenderObject* child = object.children_[index];
      const Offset child_offset = offset + child->paint_offset();
      if (auto* child_box = dynamic_cast<RenderBox*>(child);
          child_box != nullptr && child_box->is_repaint_boundary()) {
        ensure_boundary(*child_box);
        recorder.flush();
        recorder.chunks.emplace_back(RenderBox::BoundaryChild{child_box->id(), child_offset});
      } else {
        self(self, *child, child_offset);
      }
    }
    if (clip.has_value()) {
      recorder.flush();
      recorder.chunks.emplace_back(RenderBox::BoundaryClipEnd{});
    }
  };

  ++paint_depth_;
  try {
    record(record, boundary, {});
  } catch (...) {
    --paint_depth_;
    throw;
  }
  --paint_depth_;
  recorder.flush();

  auto previous_chunks = std::move(boundary.boundary_chunks_);
  boundary.boundary_chunks_ = std::move(recorder.chunks);
  try {
    compose_boundary(boundary);
  } catch (...) {
    boundary.boundary_chunks_ = previous_chunks;
    throw;
  }
  for (RenderObject* object : recorder.painted) {
    object->needs_paint_ = false;
    ++object->paint_count_;
    dirty_paint_.erase(object);
  }
  boundary.needs_compositing_ = false;
  dirty_compositing_.erase(&boundary);
}

void RenderOwner::compose_boundary(RenderBox& boundary) {
  struct LayerFrame {
    std::optional<Rect> clip;
    std::vector<LayerPtr> layers;
  };
  std::vector<LayerFrame> frames(1);
  frames.front().layers.reserve(boundary.boundary_chunks_.size());
  for (const RenderBox::BoundaryChunk& chunk : boundary.boundary_chunks_) {
    if (const auto* display = std::get_if<DisplayList>(&chunk)) {
      frames.back().layers.push_back(std::make_shared<DisplayListLayer>(*display));
      continue;
    }
    if (const auto* clip = std::get_if<RenderBox::BoundaryClipBegin>(&chunk)) {
      frames.push_back({clip->rect, {}});
      continue;
    }
    if (std::holds_alternative<RenderBox::BoundaryClipEnd>(chunk)) {
      if (frames.size() == 1 || !frames.back().clip.has_value()) {
        throw std::logic_error("Retained boundary clip chunks are unbalanced");
      }
      LayerFrame clipped = std::move(frames.back());
      frames.pop_back();
      auto contents = std::make_shared<ContainerLayer>(std::move(clipped.layers));
      frames.back().layers.push_back(
        std::make_shared<ClipRectLayer>(*clipped.clip, std::move(contents)));
      continue;
    }
    const auto child_id = std::get<RenderBox::BoundaryChild>(chunk).id;
    auto* child = dynamic_cast<RenderBox*>(resolve(child_id));
    if (child == nullptr || !child->is_repaint_boundary()) {
      throw std::logic_error("Retained boundary slot no longer resolves");
    }
    ensure_boundary(*child);
    frames.back().layers.push_back(std::make_shared<OffsetLayer>(
      std::get<RenderBox::BoundaryChild>(chunk).offset, child->retained_layer_));
  }
  if (frames.size() != 1) {
    throw std::logic_error("Retained boundary clip chunks are unbalanced");
  }
  boundary.retained_layer_ = std::make_shared<ContainerLayer>(std::move(frames.front().layers));
  boundary.needs_compositing_ = false;
  dirty_compositing_.erase(&boundary);
}

void RenderOwner::layout(BoxConstraints viewport) {
  root_->layout(viewport);
  dirty_layout_.clear();
}

LayerTree RenderOwner::composite_frame() {
  if (dirty_paint_.empty() && dirty_compositing_.empty() && last_layer_tree_.root() != nullptr) {
    return last_layer_tree_;
  }

  ensure_boundary(*root_);
  dirty_paint_.clear();
  dirty_compositing_.clear();
  last_layer_tree_ = LayerTree{root_->size(), root_->retained_layer_};
  has_completed_frame_ = true;
  return last_layer_tree_;
}

LayerTree RenderOwner::layer_frame(BoxConstraints viewport) {
  layout(viewport);
  return composite_frame();
}

DisplayList RenderOwner::frame(BoxConstraints viewport) { return layer_frame(viewport).flatten(); }

RenderObject* RenderOwner::hit_test(Offset position) { return root_->hit_test(position); }

HitTestResult RenderOwner::hit_test_path(Offset position) {
  HitTestResult result;
  static_cast<void>(root_->hit_test(result, position));
  return result;
}

const SemanticsNode* SemanticsTree::find(std::uint64_t id) const {
  const auto find_in = [&](auto&& self, const SemanticsNode& node) -> const SemanticsNode* {
    if (node.id == id) {
      return &node;
    }
    for (const SemanticsNode& child : node.children) {
      if (const SemanticsNode* found = self(self, child); found != nullptr) {
        return found;
      }
    }
    return nullptr;
  };
  for (const SemanticsNode& root : roots) {
    if (const SemanticsNode* found = find_in(find_in, root); found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

SemanticsTree RenderOwner::semantics_tree() const {
  if (!has_completed_frame_ || !dirty_layout_.empty()) {
    throw std::logic_error("semantics_tree requires completed current layout");
  }
  const auto collect = [&](auto&& self, const RenderObject& object, Offset offset,
                           std::optional<Rect> clip) -> std::vector<SemanticsNode> {
    if (const auto object_clip = object.paint_clip(offset); object_clip.has_value()) {
      if (clip.has_value()) {
        clip = intersect_rects(*clip, *object_clip);
        if (!clip.has_value()) {
          return {};
        }
      } else {
        clip = object_clip;
      }
    }

    const auto properties = object.semantics_properties();
    if (properties.has_value() && properties->hidden) {
      return {};
    }

    const auto [first_child, last_child] = object.paint_child_range();
    if (first_child > last_child || last_child > object.children_.size()) {
      throw std::logic_error("RenderObject returned an invalid semantics child range");
    }
    std::vector<SemanticsNode> descendants;
    for (std::size_t index = first_child; index < last_child; ++index) {
      const RenderObject& child = *object.children_[index];
      auto child_nodes =
        self(self, child, add_semantics_offset(offset, child.paint_offset()), clip);
      descendants.insert(descendants.end(), std::make_move_iterator(child_nodes.begin()),
                         std::make_move_iterator(child_nodes.end()));
    }
    if (!properties.has_value()) {
      return descendants;
    }

    const auto* box = dynamic_cast<const RenderBox*>(&object);
    if (box == nullptr) {
      throw std::logic_error("Semantics nodes currently require RenderBox geometry");
    }
    Rect bounds{offset, box->size()};
    if (clip.has_value()) {
      const auto clipped_bounds = intersect_rects(bounds, *clip);
      if (!clipped_bounds.has_value()) {
        return {};
      }
      bounds = *clipped_bounds;
    }

    SemanticsNode node{object.id_,
                       properties->role,
                       properties->label,
                       properties->value,
                       properties->enabled,
                       bounds,
                       {},
                       std::move(descendants)};
    if (properties->enabled && object.has_activation_handler()) {
      node.actions.push_back(SemanticsAction::activate);
    }
    std::vector<SemanticsNode> result;
    result.push_back(std::move(node));
    return result;
  };

  return SemanticsTree{collect(collect, *root_, {}, Rect{{}, root_->size()})};
}

bool RenderOwner::perform_semantics_action(std::uint64_t id, SemanticsAction action) {
  const SemanticsTree tree = semantics_tree();
  const SemanticsNode* node = tree.find(id);
  if (node == nullptr || !node->supports(action)) {
    return false;
  }
  RenderObject* object = resolve(id);
  if (object == nullptr) {
    return false;
  }
  const auto properties = object->semantics_properties();
  if (!properties.has_value() || properties->hidden || !properties->enabled) {
    return false;
  }
  switch (action) {
  case SemanticsAction::activate:
    return object->handle_semantics_action(action);
  }
  return false;
}

RenderObject* RenderOwner::resolve(RenderObject::Id id) const {
  const auto found = objects_.find(id);
  return found == objects_.end() ? nullptr : found->second;
}

} // namespace dui
