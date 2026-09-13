#pragma once

#include "dui/geometry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace dui {

struct DrawTextCommand {
  std::string text;
  Offset offset;

  friend bool operator==(const DrawTextCommand&, const DrawTextCommand&) = default;
};

struct DrawRectCommand {
  Rect rect;
  std::uint32_t color{};

  friend bool operator==(const DrawRectCommand&, const DrawRectCommand&) = default;
};

struct DrawImageCommand {
  std::string asset;
  Rect destination;

  friend bool operator==(const DrawImageCommand&, const DrawImageCommand&) = default;
};

struct PushClipRectCommand {
  Rect rect;

  friend bool operator==(const PushClipRectCommand&, const PushClipRectCommand&) = default;
};

struct PopClipCommand {
  friend bool operator==(PopClipCommand, PopClipCommand) = default;
};

using DisplayCommand = std::variant<DrawTextCommand, DrawRectCommand, DrawImageCommand,
                                    PushClipRectCommand, PopClipCommand>;

class DisplayList {
public:
  explicit DisplayList(std::vector<DisplayCommand> commands = {});

  [[nodiscard]] const std::vector<DisplayCommand>& commands() const { return commands_; }
  [[nodiscard]] std::string dump() const;

private:
  std::vector<DisplayCommand> commands_;
};

class DisplayListBuilder {
public:
  void draw_text(std::string_view text, Offset offset);
  void draw_rect(Rect rect, std::uint32_t color);
  void draw_image(std::string_view asset, Rect destination);
  void push_clip_rect(Rect rect);
  void pop_clip();

  [[nodiscard]] DisplayList build() && {
    if (clip_depth_ != 0) {
      throw std::logic_error("DisplayList contains an unclosed clip");
    }
    return DisplayList{std::move(commands_)};
  }
  [[nodiscard]] bool empty() const { return commands_.empty(); }

private:
  std::vector<DisplayCommand> commands_;
  std::size_t clip_depth_{};
};

class PaintingContext {
public:
  explicit PaintingContext(DisplayListBuilder& builder) : builder_(&builder) {}

  void draw_text(std::string_view text, Offset offset) { builder_->draw_text(text, offset); }

  void draw_rect(Rect rect, std::uint32_t color) { builder_->draw_rect(rect, color); }

  void draw_image(std::string_view asset, Rect destination) {
    builder_->draw_image(asset, destination);
  }

private:
  DisplayListBuilder* builder_;
};

class Layer {
public:
  enum class Kind { container, offset, clip_rect, display_list };

  virtual ~Layer() = default;
  [[nodiscard]] virtual Kind kind() const = 0;
};

using LayerPtr = std::shared_ptr<const Layer>;

class ContainerLayer final : public Layer {
public:
  explicit ContainerLayer(std::vector<LayerPtr> children);

  [[nodiscard]] Kind kind() const override { return Kind::container; }
  [[nodiscard]] const std::vector<LayerPtr>& children() const { return children_; }

private:
  std::vector<LayerPtr> children_;
};

class OffsetLayer final : public Layer {
public:
  OffsetLayer(Offset offset, LayerPtr child);

  [[nodiscard]] Kind kind() const override { return Kind::offset; }
  [[nodiscard]] Offset offset() const { return offset_; }
  [[nodiscard]] const LayerPtr& child() const { return child_; }

private:
  Offset offset_;
  LayerPtr child_;
};

class ClipRectLayer final : public Layer {
public:
  ClipRectLayer(Rect clip_rect, LayerPtr child);

  [[nodiscard]] Kind kind() const override { return Kind::clip_rect; }
  [[nodiscard]] Rect clip_rect() const { return clip_rect_; }
  [[nodiscard]] const LayerPtr& child() const { return child_; }

private:
  Rect clip_rect_;
  LayerPtr child_;
};

class DisplayListLayer final : public Layer {
public:
  explicit DisplayListLayer(DisplayList display_list) : display_list_(std::move(display_list)) {}

  [[nodiscard]] Kind kind() const override { return Kind::display_list; }
  [[nodiscard]] const DisplayList& display_list() const { return display_list_; }

private:
  DisplayList display_list_;
};

class LayerTree {
public:
  LayerTree() = default;
  LayerTree(Size frame_size, LayerPtr root);

  [[nodiscard]] Size frame_size() const { return frame_size_; }
  [[nodiscard]] const LayerPtr& root() const { return root_; }
  [[nodiscard]] DisplayList flatten() const;
  [[nodiscard]] std::string dump() const;

private:
  Size frame_size_{};
  LayerPtr root_;
};

class RenderOwner;
class BuildOwner;
class RenderObject;
class RenderBox;
class RenderSliver;

enum class SemanticsRole { generic, text, image, button };

enum class SemanticsAction { activate, focus };

struct SemanticsProperties {
  SemanticsRole role{SemanticsRole::generic};
  std::string label;
  std::string value;
  bool enabled{true};
  bool hidden{};
  bool focusable{};
  bool focused{};

  friend bool operator==(const SemanticsProperties&, const SemanticsProperties&) = default;
};

struct SemanticsNode {
  std::uint64_t id{};
  SemanticsRole role{SemanticsRole::generic};
  std::string label;
  std::string value;
  bool enabled{true};
  Rect bounds{};
  std::vector<SemanticsAction> actions;
  std::vector<SemanticsNode> children;
  bool focusable{};
  bool focused{};

  [[nodiscard]] bool supports(SemanticsAction action) const {
    return std::find(actions.begin(), actions.end(), action) != actions.end();
  }
};

struct SemanticsTree {
  std::vector<SemanticsNode> roots;

  [[nodiscard]] const SemanticsNode* find(std::uint64_t id) const;
};

struct SemanticsEntry {
  std::uint64_t id{};
  std::optional<std::uint64_t> parent_id;
  std::size_t child_index{};
  SemanticsRole role{SemanticsRole::generic};
  std::string label;
  std::string value;
  bool enabled{true};
  Rect bounds{};
  std::vector<SemanticsAction> actions;
  bool focusable{};
  bool focused{};

  [[nodiscard]] bool supports(SemanticsAction action) const {
    return std::find(actions.begin(), actions.end(), action) != actions.end();
  }

  friend bool operator==(const SemanticsEntry&, const SemanticsEntry&) = default;
};

enum class SemanticsChangeKind { added, updated, removed };

struct SemanticsChange {
  SemanticsChangeKind kind{};
  SemanticsEntry entry;

  friend bool operator==(const SemanticsChange&, const SemanticsChange&) = default;
};

struct SemanticsUpdate {
  std::vector<SemanticsChange> changes;

  [[nodiscard]] bool empty() const { return changes.empty(); }

  friend bool operator==(const SemanticsUpdate&, const SemanticsUpdate&) = default;
};

class SemanticsDiffer {
public:
  [[nodiscard]] SemanticsUpdate update(const SemanticsTree& tree);
  [[nodiscard]] SemanticsUpdate clear();
  void swap(SemanticsDiffer& other) noexcept { entries_.swap(other.entries_); }
  // Invalidated by a non-empty update or clear.
  [[nodiscard]] std::span<const SemanticsEntry> entries() const { return entries_; }

private:
  std::vector<SemanticsEntry> entries_;
};

class AccessibilityAdapter {
public:
  virtual ~AccessibilityAdapter() = default;

  // The change span is valid only for the duration of this call.
  virtual void apply(std::span<const SemanticsChange> changes) = 0;
};

class AccessibilityBridge {
public:
  AccessibilityBridge();
  ~AccessibilityBridge();

  AccessibilityBridge(const AccessibilityBridge&) = delete;
  AccessibilityBridge& operator=(const AccessibilityBridge&) = delete;
  AccessibilityBridge(AccessibilityBridge&&) = delete;
  AccessibilityBridge& operator=(AccessibilityBridge&&) = delete;

  [[nodiscard]] bool publish(const SemanticsTree& tree, AccessibilityAdapter& adapter);
  [[nodiscard]] bool clear(AccessibilityAdapter& adapter);
  // Invalidated by a successful non-empty publish or clear.
  [[nodiscard]] std::span<const SemanticsEntry> entries() const { return differ_.entries(); }

private:
  struct DeliveryState {
    bool alive{true};
    bool delivering{};
  };

  SemanticsDiffer differ_;
  std::shared_ptr<DeliveryState> delivery_state_;
};

struct HitTestEntry {
  RenderObject* target{};
  Offset local_position{};
};

class HitTestResult {
public:
  void add(HitTestEntry entry) { path_.push_back(entry); }
  [[nodiscard]] const std::vector<HitTestEntry>& path() const { return path_; }
  [[nodiscard]] RenderObject* target() const {
    return path_.empty() ? nullptr : path_.front().target;
  }

private:
  std::vector<HitTestEntry> path_;
};

struct BoxParentData {
  Offset offset{};
};

struct SliverParentData {
  Offset paint_offset{};
};

class RenderObject {
public:
  using Id = std::uint64_t;

  explicit RenderObject(RenderOwner& owner);
  virtual ~RenderObject();

  RenderObject(const RenderObject&) = delete;
  RenderObject& operator=(const RenderObject&) = delete;

  [[nodiscard]] Id id() const { return id_; }
  [[nodiscard]] RenderObject* parent() const { return parent_; }
  [[nodiscard]] const std::vector<RenderObject*>& children() const { return children_; }
  [[nodiscard]] bool needs_layout() const { return needs_layout_; }
  [[nodiscard]] bool needs_paint() const { return needs_paint_; }
  [[nodiscard]] bool needs_compositing() const { return needs_compositing_; }
  [[nodiscard]] bool is_repaint_boundary() const { return has_repaint_boundary(); }
  [[nodiscard]] std::size_t layout_count() const { return layout_count_; }
  [[nodiscard]] std::size_t paint_count() const { return paint_count_; }

  void mark_needs_layout();
  void mark_needs_paint();
  void set_children(std::span<RenderObject* const> children);

  [[nodiscard]] RenderObject* hit_test(Offset position);
  [[nodiscard]] bool hit_test(HitTestResult& result, Offset position);
  [[nodiscard]] bool can_activate() const { return has_activation_handler(); }
  [[nodiscard]] bool activate() { return handle_activate(); }

protected:
  [[nodiscard]] virtual bool has_activation_handler() const { return false; }
  [[nodiscard]] virtual bool handle_activate() { return false; }
  [[nodiscard]] virtual bool handle_semantics_action(SemanticsAction action);
  [[nodiscard]] virtual bool has_focus_handler() const { return false; }
  [[nodiscard]] virtual bool handle_focus() { return false; }
  [[nodiscard]] virtual bool has_repaint_boundary() const { return false; }
  [[nodiscard]] virtual std::optional<SemanticsProperties> semantics_properties() const {
    return std::nullopt;
  }
  virtual void paint(PaintingContext&, Offset) {}

private:
  friend class RenderOwner;
  friend class RenderBox;
  friend class RenderSliver;

  virtual void validate_child_protocol(const RenderObject&) const;
  virtual void validate_child_count(std::size_t) const {}
  [[nodiscard]] virtual bool hit_test_protocol(HitTestResult&, Offset) { return false; }
  [[nodiscard]] virtual Offset paint_offset() const { return {}; }
  [[nodiscard]] virtual std::optional<Rect> paint_clip(Offset) const { return std::nullopt; }
  [[nodiscard]] virtual std::pair<std::size_t, std::size_t> paint_child_range() const {
    return {0, children_.size()};
  }
  void detach_from_owner();

  RenderOwner* owner_;
  Id id_{};
  RenderObject* parent_{};
  std::vector<RenderObject*> children_;
  bool needs_layout_{true};
  bool needs_paint_{true};
  bool needs_compositing_{true};
  std::size_t layout_count_{};
  std::size_t paint_count_{};
};

class RenderBox : public RenderObject {
public:
  explicit RenderBox(RenderOwner& owner) : RenderObject(owner) {}

  [[nodiscard]] Size size() const { return size_; }
  [[nodiscard]] Offset offset() const { return parent_data_.offset; }
  [[nodiscard]] const BoxParentData& parent_data() const { return parent_data_; }

protected:
  RenderBox(RenderOwner& owner, bool accepts_sliver_children)
    : RenderObject(owner), accepts_sliver_children_(accepts_sliver_children) {}
  [[nodiscard]] const BoxConstraints& constraints() const { return *constraints_; }
  void set_size(Size size) { size_ = size; }
  void layout_child(RenderObject& child, BoxConstraints constraints, Offset offset);
  [[nodiscard]] const LayerPtr& boundary_layer() const { return retained_layer_; }

  virtual void perform_layout() = 0;
  [[nodiscard]] virtual bool hit_test_self(Offset) const { return false; }

private:
  friend class RenderOwner;
  friend class RenderSliver;

  struct BoundaryChild {
    RenderObject::Id id{};
    Offset offset{};
  };
  struct BoundaryClipBegin {
    Rect rect;
  };
  struct BoundaryClipEnd {};
  using BoundaryChunk =
    std::variant<DisplayList, BoundaryChild, BoundaryClipBegin, BoundaryClipEnd>;

  void validate_child_protocol(const RenderObject& child) const final;
  [[nodiscard]] bool hit_test_protocol(HitTestResult& result, Offset position) override;
  [[nodiscard]] Offset paint_offset() const override { return parent_data_.offset; }
  void layout(BoxConstraints constraints);

  std::optional<BoxConstraints> constraints_;
  Size size_{};
  BoxParentData parent_data_;
  std::vector<BoundaryChunk> boundary_chunks_;
  LayerPtr retained_layer_;
  bool accepts_sliver_children_{};
};

class RenderSliver : public RenderObject {
public:
  explicit RenderSliver(RenderOwner& owner, bool accepts_box_children = false)
    : RenderObject(owner), accepts_box_children_(accepts_box_children) {}

  [[nodiscard]] const SliverGeometry& geometry() const { return geometry_; }
  [[nodiscard]] const SliverParentData& parent_data() const { return parent_data_; }

protected:
  [[nodiscard]] const SliverConstraints& constraints() const { return *constraints_; }
  [[nodiscard]] bool has_constraints() const { return constraints_.has_value(); }
  void set_geometry(SliverGeometry geometry) { geometry_ = geometry; }
  void layout_box_child(RenderObject& child, BoxConstraints constraints, Offset offset);
  virtual void perform_layout() = 0;

private:
  friend class RenderViewport;

  void validate_child_protocol(const RenderObject& child) const final;
  [[nodiscard]] bool hit_test_protocol(HitTestResult& result, Offset position) override;
  [[nodiscard]] Offset paint_offset() const override { return parent_data_.paint_offset; }
  [[nodiscard]] std::pair<std::size_t, std::size_t> paint_child_range() const override {
    return geometry_.paint_extent() > 0.0
             ? std::pair<std::size_t, std::size_t>{0, children().size()}
             : std::pair<std::size_t, std::size_t>{0, 0};
  }
  void layout(SliverConstraints constraints);

  std::optional<SliverConstraints> constraints_;
  SliverGeometry geometry_;
  SliverParentData parent_data_;
  bool accepts_box_children_{};
};

class RenderViewport final : public RenderBox {
public:
  explicit RenderViewport(RenderOwner& owner, double scroll_offset = 0.0);

  void set_scroll_offset(double scroll_offset);
  [[nodiscard]] double scroll_offset() const { return scroll_offset_; }
  [[nodiscard]] double max_scroll_extent() const { return max_scroll_extent_; }

protected:
  void perform_layout() override;

private:
  [[nodiscard]] bool hit_test_protocol(HitTestResult& result, Offset position) override;
  [[nodiscard]] std::optional<Rect> paint_clip(Offset offset) const override {
    return Rect{offset, size()};
  }

  double scroll_offset_{};
  double max_scroll_extent_{};
};

class RenderSliverToBoxAdapter final : public RenderSliver {
public:
  explicit RenderSliverToBoxAdapter(RenderOwner& owner) : RenderSliver(owner, true) {}

protected:
  void perform_layout() override;

private:
  void validate_child_count(std::size_t count) const override;
};

class RenderSliverFixedExtentList final : public RenderSliver {
public:
  struct ChildRange {
    std::size_t first{};
    std::size_t end{};
    std::uint64_t revision{};

    friend bool operator==(const ChildRange&, const ChildRange&) = default;
  };

  RenderSliverFixedExtentList(RenderOwner& owner, double item_extent);

  void set_item_extent(double item_extent);
  void set_cache_extent(double cache_extent);
  void set_lazy_model(std::size_t logical_child_count, std::uint64_t revision);
  void set_mounted_range(std::size_t first, std::size_t count, std::uint64_t revision);
  void clear_lazy_model();
  [[nodiscard]] double item_extent() const { return item_extent_; }
  [[nodiscard]] double cache_extent() const { return cache_extent_; }
  [[nodiscard]] std::size_t first_visible_index() const { return first_visible_index_; }
  [[nodiscard]] std::size_t visible_child_count() const { return visible_child_count_; }
  [[nodiscard]] std::size_t logical_child_count() const {
    return lazy_ ? logical_child_count_ : children().size();
  }
  [[nodiscard]] std::size_t first_mounted_index() const { return first_mounted_index_; }
  [[nodiscard]] std::optional<ChildRange> requested_child_range() const {
    return requested_child_range_;
  }

protected:
  void perform_layout() override;

private:
  [[nodiscard]] bool hit_test_protocol(HitTestResult& result, Offset position) override;
  [[nodiscard]] std::pair<std::size_t, std::size_t> paint_child_range() const override {
    return {first_visible_child_, first_visible_child_ + visible_child_count_};
  }

  double item_extent_;
  double cache_extent_{};
  bool lazy_{};
  std::size_t logical_child_count_{};
  std::size_t first_mounted_index_{};
  std::size_t mounted_child_count_{};
  std::uint64_t model_revision_{};
  std::uint64_t mounted_revision_{};
  std::optional<ChildRange> requested_child_range_;
  std::size_t first_visible_index_{};
  std::size_t first_visible_child_{};
  std::size_t visible_child_count_{};
};

class RenderText final : public RenderBox {
public:
  RenderText(RenderOwner& owner, std::string text, std::function<void()> on_activate = {});

  void set_text(std::string text);
  void set_on_activate(std::function<void()> callback) { on_activate_ = std::move(callback); }
  [[nodiscard]] const std::string& text() const { return text_; }

protected:
  void perform_layout() override;
  void paint(PaintingContext& context, Offset offset) override;
  [[nodiscard]] bool hit_test_self(Offset) const override { return true; }
  [[nodiscard]] bool has_activation_handler() const override {
    return static_cast<bool>(on_activate_);
  }
  [[nodiscard]] bool handle_activate() override;
  [[nodiscard]] std::optional<SemanticsProperties> semantics_properties() const override {
    if (text_.empty()) {
      return std::nullopt;
    }
    return SemanticsProperties{SemanticsRole::text, text_, {}, true, false, false, false};
  }

private:
  std::string text_;
  std::function<void()> on_activate_;
};

class RenderVStack final : public RenderBox {
public:
  explicit RenderVStack(RenderOwner& owner) : RenderBox(owner) {}

protected:
  void perform_layout() override;
};

class RenderImage final : public RenderBox {
public:
  RenderImage(RenderOwner& owner, std::string asset, Size intrinsic_size,
              std::string semantics_label = {});

  void set_image(std::string asset, Size intrinsic_size, std::string semantics_label = {});

protected:
  void perform_layout() override;
  void paint(PaintingContext& context, Offset offset) override;
  [[nodiscard]] bool hit_test_self(Offset) const override { return true; }
  [[nodiscard]] std::optional<SemanticsProperties> semantics_properties() const override {
    if (semantics_label_.empty()) {
      return std::nullopt;
    }
    return SemanticsProperties{
      SemanticsRole::image, semantics_label_, {}, true, false, false, false};
  }

private:
  std::string asset_;
  Size intrinsic_size_;
  std::string semantics_label_;
};

class RenderHStack final : public RenderBox {
public:
  explicit RenderHStack(RenderOwner& owner) : RenderBox(owner) {}

protected:
  void perform_layout() override;
};

class RenderStack final : public RenderBox {
public:
  explicit RenderStack(RenderOwner& owner) : RenderBox(owner) {}

protected:
  void perform_layout() override;
};

class RenderPadding final : public RenderBox {
public:
  RenderPadding(RenderOwner& owner, Insets insets);

  void set_insets(Insets insets);

protected:
  void perform_layout() override;

private:
  Insets insets_;
};

class RenderColoredBox final : public RenderBox {
public:
  RenderColoredBox(RenderOwner& owner, std::uint32_t color) : RenderBox(owner), color_(color) {}

  void set_color(std::uint32_t color);

protected:
  void perform_layout() override;
  void paint(PaintingContext& context, Offset offset) override;

private:
  std::uint32_t color_;
};

class RenderActionBox final : public RenderBox {
public:
  RenderActionBox(RenderOwner& owner, std::function<void()> on_activate,
                  bool stops_propagation = false)
    : RenderBox(owner), on_activate_(std::move(on_activate)),
      stops_propagation_(stops_propagation) {}

  void set_on_activate(std::function<void()> callback) { on_activate_ = std::move(callback); }
  void set_stops_propagation(bool value) { stops_propagation_ = value; }
  void set_semantics_enabled(bool value) { semantics_enabled_ = value; }
  void set_semantics_focus(bool focusable, std::function<bool()> focused) {
    semantics_focusable_ = focusable;
    semantics_focused_ = std::move(focused);
  }

protected:
  void perform_layout() override;
  [[nodiscard]] bool hit_test_self(Offset) const override { return true; }
  [[nodiscard]] bool has_activation_handler() const override {
    return static_cast<bool>(on_activate_);
  }
  [[nodiscard]] bool handle_activate() override;
  [[nodiscard]] bool has_focus_handler() const override {
    return semantics_focusable_ && static_cast<bool>(on_activate_);
  }
  [[nodiscard]] bool handle_focus() override;
  [[nodiscard]] std::optional<SemanticsProperties> semantics_properties() const override {
    return SemanticsProperties{SemanticsRole::button,
                               {},
                               {},
                               semantics_enabled_ && static_cast<bool>(on_activate_),
                               false,
                               semantics_focusable_,
                               semantics_focused_ && semantics_focused_()};
  }

private:
  std::function<void()> on_activate_;
  bool stops_propagation_{};
  bool semantics_enabled_{true};
  bool semantics_focusable_{};
  std::function<bool()> semantics_focused_;
};

class RenderSemanticsBox final : public RenderBox {
public:
  RenderSemanticsBox(RenderOwner& owner, SemanticsProperties properties,
                     std::function<void()> on_activate = {})
    : RenderBox(owner), properties_(std::move(properties)), on_activate_(std::move(on_activate)) {}

  void set_semantics(SemanticsProperties properties, std::function<void()> on_activate) {
    properties_ = std::move(properties);
    on_activate_ = std::move(on_activate);
  }

protected:
  void perform_layout() override;
  [[nodiscard]] bool has_activation_handler() const override {
    return static_cast<bool>(on_activate_);
  }
  [[nodiscard]] bool handle_activate() override;
  [[nodiscard]] std::optional<SemanticsProperties> semantics_properties() const override {
    return properties_;
  }

private:
  void validate_child_count(std::size_t count) const override;

  SemanticsProperties properties_;
  std::function<void()> on_activate_;
};

class RenderRepaintBoundary final : public RenderBox {
public:
  explicit RenderRepaintBoundary(RenderOwner& owner) : RenderBox(owner) {}

  [[nodiscard]] const LayerPtr& retained_layer() const { return boundary_layer(); }

protected:
  void perform_layout() override;
  [[nodiscard]] bool has_repaint_boundary() const override { return true; }
};

class RenderView final : public RenderBox {
public:
  explicit RenderView(RenderOwner& owner) : RenderBox(owner) {}

protected:
  void perform_layout() override;
  [[nodiscard]] bool has_repaint_boundary() const override { return true; }
};

class RenderOwner {
public:
  RenderOwner();
  ~RenderOwner();

  RenderOwner(const RenderOwner&) = delete;
  RenderOwner& operator=(const RenderOwner&) = delete;

  void set_roots(std::span<RenderObject* const> roots);
  [[nodiscard]] LayerTree layer_frame(BoxConstraints viewport);
  [[nodiscard]] DisplayList frame(BoxConstraints viewport);
  [[nodiscard]] RenderObject* hit_test(Offset position);
  [[nodiscard]] HitTestResult hit_test_path(Offset position);
  [[nodiscard]] SemanticsTree semantics_tree() const;
  [[nodiscard]] bool perform_semantics_action(std::uint64_t id, SemanticsAction action);
  [[nodiscard]] RenderObject* resolve(RenderObject::Id id) const;

  [[nodiscard]] std::size_t pending_layout_count() const { return dirty_layout_.size(); }
  [[nodiscard]] std::size_t pending_paint_count() const { return dirty_paint_.size(); }
  [[nodiscard]] std::size_t pending_compositing_count() const { return dirty_compositing_.size(); }
  [[nodiscard]] const RenderView& root() const { return *root_; }

private:
  friend class BuildOwner;
  friend class RenderObject;
  friend class RenderBox;
  friend class RenderSliver;

  void schedule_layout(RenderObject& object);
  void schedule_paint(RenderObject& object);
  void schedule_compositing(RenderObject& object);
  void ensure_boundary(RenderBox& boundary);
  void record_boundary(RenderBox& boundary);
  void compose_boundary(RenderBox& boundary);
  void layout(BoxConstraints viewport);
  [[nodiscard]] LayerTree composite_frame();
  [[nodiscard]] RenderObject::Id register_object(RenderObject& object);
  void forget(RenderObject& object);

  std::unordered_set<RenderObject*> dirty_layout_;
  std::unordered_set<RenderObject*> dirty_paint_;
  std::unordered_set<RenderObject*> dirty_compositing_;
  std::unordered_map<RenderObject::Id, RenderObject*> objects_;
  RenderObject::Id next_id_{1};
  std::size_t paint_depth_{};
  std::unique_ptr<RenderView> root_;
  LayerTree last_layer_tree_;
  bool has_completed_frame_{};
};

} // namespace dui
