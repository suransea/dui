#include "dui/rendering.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
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
    case Layer::Kind::display_list: {
        const auto* display_layer = dynamic_cast<const DisplayListLayer*>(&layer);
        if (display_layer == nullptr) {
            throw std::logic_error("Layer kind does not match DisplayListLayer type");
        }
        const auto& display = display_layer->display_list();
        for (const DisplayCommand& command : display.commands()) {
            std::visit([&](const auto& value) {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, DrawTextCommand>) {
                    builder.draw_text(value.text, value.offset + offset);
                } else if constexpr (std::same_as<T, DrawRectCommand>) {
                    builder.draw_rect({value.rect.origin + offset, value.rect.size}, value.color);
                } else {
                    builder.draw_image(
                        value.asset,
                        {value.destination.origin + offset, value.destination.size}
                    );
                }
            }, command);
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

std::string DisplayList::dump() const {
    std::ostringstream output;
    for (const DisplayCommand& command : commands_) {
        std::visit([&](const auto& value) {
            using T = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<T, DrawTextCommand>) {
                output << "text(\"" << value.text << "\", "
                    << number(value.offset.x) << ", " << number(value.offset.y) << ")\n";
            } else if constexpr (std::same_as<T, DrawRectCommand>) {
                output << "rect(" << number(value.rect.origin.x) << ", "
                    << number(value.rect.origin.y) << ", "
                    << number(value.rect.size.width) << ", "
                    << number(value.rect.size.height) << ", 0x"
                    << std::hex << std::setw(8) << std::setfill('0') << value.color
                    << std::dec << ")\n";
            } else {
                output << "image(\"" << value.asset << "\", "
                    << number(value.destination.origin.x) << ", "
                    << number(value.destination.origin.y) << ", "
                    << number(value.destination.size.width) << ", "
                    << number(value.destination.size.height) << ")\n";
            }
        }, command);
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

ContainerLayer::ContainerLayer(std::vector<LayerPtr> children) :
    children_(std::move(children)) {
    if (std::ranges::any_of(children_, [](const LayerPtr& child) { return child == nullptr; })) {
        throw std::invalid_argument("ContainerLayer children cannot be null");
    }
}

OffsetLayer::OffsetLayer(Offset offset, LayerPtr child) :
    offset_(offset),
    child_(std::move(child)) {
    if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
        throw std::invalid_argument("OffsetLayer offset must be finite");
    }
    if (child_ == nullptr) {
        throw std::invalid_argument("OffsetLayer child cannot be null");
    }
}

LayerTree::LayerTree(Size frame_size, LayerPtr root) :
    frame_size_(frame_size),
    root_(std::move(root)) {
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
    output << "layer_tree(" << number(frame_size_.width) << ", "
        << number(frame_size_.height) << ")\n";
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
    const bool unchanged = children_.size() == children.size()
        && std::ranges::equal(children_, children);
    if (unchanged) {
        return;
    }

    std::vector<RenderObject*> next{children.begin(), children.end()};
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
        if (std::ranges::find(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(index), child)
            != next.begin() + static_cast<std::ptrdiff_t>(index)) {
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

void RenderBox::layout_child(
    RenderObject& child,
    BoxConstraints child_constraints,
    Offset child_offset
) {
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

void RenderBox::paint_subtree(PaintingContext& context, Offset parent_offset) {
    const Offset absolute_offset = parent_offset + parent_data_.offset;
    paint(context, absolute_offset);
    for (RenderObject* child : children_) {
        child->paint_subtree(context, absolute_offset);
    }
    needs_paint_ = false;
    ++paint_count_;
    owner_->dirty_paint_.erase(this);
}

RenderObject* RenderObject::hit_test(Offset position) {
    HitTestResult result;
    static_cast<void>(hit_test(result, position));
    return result.target();
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

RenderText::RenderText(
    RenderOwner& owner,
    std::string text,
    std::function<void()> on_activate
) :
    RenderBox(owner),
    text_(std::move(text)),
    on_activate_(std::move(on_activate)) {}

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
    set_size(constraints().constrain({
        static_cast<double>(text_.size()) * character_width,
        line_height
    }));
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

RenderImage::RenderImage(RenderOwner& owner, std::string asset, Size intrinsic_size) :
    RenderBox(owner),
    asset_(std::move(asset)),
    intrinsic_size_(intrinsic_size) {
    static_cast<void>(BoxConstraints{}.constrain(intrinsic_size));
}

void RenderImage::set_image(std::string asset, Size intrinsic_size) {
    static_cast<void>(BoxConstraints{}.constrain(intrinsic_size));
    const bool size_changed = intrinsic_size_ != intrinsic_size;
    const bool asset_changed = asset_ != asset;
    if (!size_changed && !asset_changed) {
        return;
    }
    asset_ = std::move(asset);
    intrinsic_size_ = intrinsic_size;
    if (size_changed) {
        mark_needs_layout();
    } else {
        mark_needs_paint();
    }
}

void RenderImage::perform_layout() {
    set_size(constraints().constrain(intrinsic_size_));
}

void RenderImage::paint(PaintingContext& context, Offset offset) {
    context.draw_image(asset_, {offset, size()});
}

void RenderVStack::perform_layout() {
    double width = 0.0;
    double height = 0.0;
    const BoxConstraints child_constraints{
        0.0,
        constraints().max_width(),
        0.0,
        infinity
    };

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
    const BoxConstraints child_constraints{
        0.0,
        infinity,
        0.0,
        constraints().max_height()
    };

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
    if (std::ranges::any_of(values, [](double value) {
        return !std::isfinite(value) || value < 0.0;
    })) {
        throw std::invalid_argument("Padding Insets must be finite and non-negative");
    }
}

} // namespace

RenderPadding::RenderPadding(RenderOwner& owner, Insets insets) :
    RenderBox(owner),
    insets_(insets) {
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
    const BoxConstraints child_constraints{
        reduce(constraints().min_width(), insets_.horizontal()),
        reduce(constraints().max_width(), insets_.horizontal()),
        reduce(constraints().min_height(), insets_.vertical()),
        reduce(constraints().max_height(), insets_.vertical())
    };
    layout_child(child, child_constraints, {insets_.left, insets_.top});
    set_size(constraints().constrain({
        child.size().width + insets_.horizontal(),
        child.size().height + insets_.vertical()
    }));
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

void RenderOwner::schedule_layout(RenderObject& object) {
    dirty_layout_.insert(&object);
}

void RenderOwner::schedule_paint(RenderObject& object) {
    dirty_paint_.insert(&object);
}

void RenderOwner::schedule_compositing(RenderObject& object) {
    dirty_compositing_.insert(&object);
}

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

void RenderOwner::set_roots(std::span<RenderObject* const> roots) {
    root_->set_children(roots);
}

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

    const auto record = [&](auto&& self, RenderBox& object, Offset offset) -> void {
        PaintingContext context{recorder.builder};
        object.paint(context, offset);
        recorder.painted.push_back(&object);
        for (RenderObject* child : object.children_) {
            auto& child_box = static_cast<RenderBox&>(*child);
            if (child_box.is_repaint_boundary()) {
                ensure_boundary(child_box);
                recorder.flush();
                recorder.chunks.emplace_back(RenderBox::BoundaryChild{
                    child_box.id(),
                    offset + child_box.offset()
                });
            } else {
                self(self, child_box, offset + child_box.offset());
            }
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
    std::vector<LayerPtr> layers;
    layers.reserve(boundary.boundary_chunks_.size());
    for (const RenderBox::BoundaryChunk& chunk : boundary.boundary_chunks_) {
        if (const auto* display = std::get_if<DisplayList>(&chunk)) {
            layers.push_back(std::make_shared<DisplayListLayer>(*display));
            continue;
        }
        const auto child_id = std::get<RenderBox::BoundaryChild>(chunk).id;
        auto* child = dynamic_cast<RenderBox*>(resolve(child_id));
        if (child == nullptr || !child->is_repaint_boundary()) {
            throw std::logic_error("Retained boundary slot no longer resolves");
        }
        ensure_boundary(*child);
        layers.push_back(std::make_shared<OffsetLayer>(
            std::get<RenderBox::BoundaryChild>(chunk).offset,
            child->retained_layer_
        ));
    }
    boundary.retained_layer_ = std::make_shared<ContainerLayer>(std::move(layers));
    boundary.needs_compositing_ = false;
    dirty_compositing_.erase(&boundary);
}

LayerTree RenderOwner::layer_frame(BoxConstraints viewport) {
    root_->layout(viewport);
    dirty_layout_.clear();

    if (dirty_paint_.empty()
        && dirty_compositing_.empty()
        && last_layer_tree_.root() != nullptr) {
        return last_layer_tree_;
    }

    ensure_boundary(*root_);
    dirty_paint_.clear();
    dirty_compositing_.clear();
    last_layer_tree_ = LayerTree{root_->size(), root_->retained_layer_};
    return last_layer_tree_;
}

DisplayList RenderOwner::frame(BoxConstraints viewport) {
    return layer_frame(viewport).flatten();
}

RenderObject* RenderOwner::hit_test(Offset position) {
    return root_->hit_test(position);
}

HitTestResult RenderOwner::hit_test_path(Offset position) {
    HitTestResult result;
    static_cast<void>(root_->hit_test(result, position));
    return result;
}

RenderObject* RenderOwner::resolve(RenderObject::Id id) const {
    const auto found = objects_.find(id);
    return found == objects_.end() ? nullptr : found->second;
}

} // namespace dui
