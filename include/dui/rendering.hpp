#pragma once

#include "dui/geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
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

using DisplayCommand = std::variant<DrawTextCommand, DrawRectCommand, DrawImageCommand>;

class DisplayList {
public:
    explicit DisplayList(std::vector<DisplayCommand> commands = {}) :
        commands_(std::move(commands)) {}

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

    [[nodiscard]] DisplayList build() && {
        return DisplayList{std::move(commands_)};
    }
    [[nodiscard]] bool empty() const { return commands_.empty(); }

private:
    std::vector<DisplayCommand> commands_;
};

class PaintingContext {
public:
    explicit PaintingContext(DisplayListBuilder& builder) : builder_(&builder) {}

    void draw_text(std::string_view text, Offset offset) {
        builder_->draw_text(text, offset);
    }

    void draw_rect(Rect rect, std::uint32_t color) {
        builder_->draw_rect(rect, color);
    }

    void draw_image(std::string_view asset, Rect destination) {
        builder_->draw_image(asset, destination);
    }

private:
    DisplayListBuilder* builder_;
};

class Layer {
public:
    enum class Kind {
        container,
        offset,
        display_list
    };

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

class DisplayListLayer final : public Layer {
public:
    explicit DisplayListLayer(DisplayList display_list) :
        display_list_(std::move(display_list)) {}

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
class RenderObject;
class RenderBox;

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
    [[nodiscard]] virtual bool has_repaint_boundary() const { return false; }

private:
    friend class RenderOwner;
    friend class RenderBox;

    virtual void validate_child_protocol(const RenderObject&) const;
    [[nodiscard]] virtual bool hit_test_protocol(HitTestResult&, Offset) { return false; }
    virtual void paint_subtree(PaintingContext&, Offset) {}
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
    [[nodiscard]] const BoxConstraints& constraints() const { return *constraints_; }
    void set_size(Size size) { size_ = size; }
    void layout_child(RenderObject& child, BoxConstraints constraints, Offset offset);
    [[nodiscard]] const LayerPtr& boundary_layer() const { return retained_layer_; }

    virtual void perform_layout() = 0;
    virtual void paint(PaintingContext&, Offset) {}
    [[nodiscard]] virtual bool hit_test_self(Offset) const { return false; }

private:
    friend class RenderOwner;

    struct BoundaryChild {
        RenderObject::Id id{};
        Offset offset{};
    };
    using BoundaryChunk = std::variant<DisplayList, BoundaryChild>;

    void validate_child_protocol(const RenderObject& child) const final;
    [[nodiscard]] bool hit_test_protocol(HitTestResult& result, Offset position) override;
    void paint_subtree(PaintingContext& context, Offset parent_offset) override;
    void layout(BoxConstraints constraints);

    std::optional<BoxConstraints> constraints_;
    Size size_{};
    BoxParentData parent_data_;
    std::vector<BoundaryChunk> boundary_chunks_;
    LayerPtr retained_layer_;
};

class RenderText final : public RenderBox {
public:
    RenderText(
        RenderOwner& owner,
        std::string text,
        std::function<void()> on_activate = {}
    );

    void set_text(std::string text);
    void set_on_activate(std::function<void()> callback) {
        on_activate_ = std::move(callback);
    }
    [[nodiscard]] const std::string& text() const { return text_; }

protected:
    void perform_layout() override;
    void paint(PaintingContext& context, Offset offset) override;
    [[nodiscard]] bool hit_test_self(Offset) const override { return true; }
    [[nodiscard]] bool has_activation_handler() const override {
        return static_cast<bool>(on_activate_);
    }
    [[nodiscard]] bool handle_activate() override;

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
    RenderImage(RenderOwner& owner, std::string asset, Size intrinsic_size);

    void set_image(std::string asset, Size intrinsic_size);

protected:
    void perform_layout() override;
    void paint(PaintingContext& context, Offset offset) override;
    [[nodiscard]] bool hit_test_self(Offset) const override { return true; }

private:
    std::string asset_;
    Size intrinsic_size_;
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
    RenderColoredBox(RenderOwner& owner, std::uint32_t color) :
        RenderBox(owner),
        color_(color) {}

    void set_color(std::uint32_t color);

protected:
    void perform_layout() override;
    void paint(PaintingContext& context, Offset offset) override;

private:
    std::uint32_t color_;
};

class RenderActionBox final : public RenderBox {
public:
    RenderActionBox(
        RenderOwner& owner,
        std::function<void()> on_activate,
        bool stops_propagation = false
    ) :
        RenderBox(owner),
        on_activate_(std::move(on_activate)),
        stops_propagation_(stops_propagation) {}

    void set_on_activate(std::function<void()> callback) {
        on_activate_ = std::move(callback);
    }
    void set_stops_propagation(bool value) { stops_propagation_ = value; }

protected:
    void perform_layout() override;
    [[nodiscard]] bool hit_test_self(Offset) const override { return true; }
    [[nodiscard]] bool has_activation_handler() const override {
        return static_cast<bool>(on_activate_);
    }
    [[nodiscard]] bool handle_activate() override;

private:
    std::function<void()> on_activate_;
    bool stops_propagation_{};
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
    [[nodiscard]] RenderObject* resolve(RenderObject::Id id) const;

    [[nodiscard]] std::size_t pending_layout_count() const { return dirty_layout_.size(); }
    [[nodiscard]] std::size_t pending_paint_count() const { return dirty_paint_.size(); }
    [[nodiscard]] std::size_t pending_compositing_count() const {
        return dirty_compositing_.size();
    }
    [[nodiscard]] const RenderView& root() const { return *root_; }

private:
    friend class RenderObject;
    friend class RenderBox;

    void schedule_layout(RenderObject& object);
    void schedule_paint(RenderObject& object);
    void schedule_compositing(RenderObject& object);
    void ensure_boundary(RenderBox& boundary);
    void record_boundary(RenderBox& boundary);
    void compose_boundary(RenderBox& boundary);
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
};

} // namespace dui
