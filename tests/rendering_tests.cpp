#include "dui/ui.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

const dui::Element& only_child(const dui::Element& element) {
    require(element.children().size() == 1, "expected exactly one Element child");
    return *element.children().front();
}

void constraints_validate_and_constrain() {
    const dui::BoxConstraints constraints{10.0, 100.0, 20.0, 80.0};
    require(constraints.constrain({1.0, 200.0}) == dui::Size{10.0, 80.0}, "size was not constrained");
    require(constraints.loosen().min_width() == 0.0, "loosen retained minimum width");

    bool rejected = false;
    try {
        static_cast<void>(dui::BoxConstraints{20.0, 10.0, 0.0, 1.0});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "non-normalized constraints were accepted");
}

void vstack_lays_out_paints_and_hits() {
    dui::BuildOwner owner;
    owner.render(dui::VStack{
        dui::Text{"A"},
        dui::Text{"BB"}
    });

    const dui::DisplayList display = owner.frame(
        dui::BoxConstraints::tight({100.0, 100.0})
    );
    require(
        display.dump() ==
            "text(\"A\", 0.0, 0.0)\n"
            "text(\"BB\", 0.0, 16.0)\n",
        "VStack produced an unexpected DisplayList"
    );

    const auto* stack = dynamic_cast<const dui::RenderVStack*>(owner.root()->render_object());
    require(stack != nullptr, "VStack did not create RenderVStack");
    require(stack->size() == dui::Size{16.0, 32.0}, "VStack size is incorrect");
    require(static_cast<const dui::RenderBox*>(stack->children()[1])->offset() == dui::Offset{0.0, 16.0}, "second child offset is incorrect");
    require(owner.pending_layout_count() == 0, "layout queue was not drained");
    require(owner.pending_paint_count() == 0, "paint queue was not drained");

    require(owner.hit_test({1.0, 1.0}) == stack->children()[0], "first text hit-test failed");
    require(owner.hit_test({1.0, 17.0}) == stack->children()[1], "second text hit-test failed");
    require(owner.hit_test({50.0, 50.0}) == nullptr, "empty VStack area reported a hit");
}

void equivalent_update_reuses_layout_and_display_list() {
    dui::BuildOwner owner;
    owner.render(dui::VStack{dui::Text{"same"}});
    const dui::DisplayList first = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));

    auto* stack = owner.root()->render_object();
    auto* text = only_child(*owner.root()).render_object();
    const auto stack_layouts = stack->layout_count();
    const auto stack_paints = stack->paint_count();
    const auto text_layouts = text->layout_count();
    const auto text_paints = text->paint_count();

    owner.render(dui::VStack{dui::Text{"same"}});
    const dui::DisplayList second = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));

    require(owner.root()->render_object() == stack, "equivalent VStack replaced its RenderObject");
    require(only_child(*owner.root()).render_object() == text, "equivalent Text replaced its RenderObject");
    require(stack->layout_count() == stack_layouts, "equivalent update repeated stack layout");
    require(stack->paint_count() == stack_paints, "equivalent update repeated stack paint");
    require(text->layout_count() == text_layouts, "equivalent update repeated text layout");
    require(text->paint_count() == text_paints, "equivalent update repeated text paint");
    require(first.commands() == second.commands(), "cached DisplayList changed");
}

void text_update_invalidates_layout_and_paint() {
    dui::BuildOwner owner;
    owner.render(dui::VStack{dui::Text{"A"}, dui::Text{"B"}});
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({200.0, 100.0})));

    auto* stack = owner.root()->render_object();
    auto* first = owner.root()->children()[0]->render_object();
    auto* second = owner.root()->children()[1]->render_object();
    const auto first_layouts = first->layout_count();
    const auto second_layouts = second->layout_count();
    const auto stack_layouts = stack->layout_count();

    owner.render(dui::VStack{dui::Text{"A"}, dui::Text{"longer"}});
    require(second->needs_layout(), "changed Text was not marked for layout");
    require(stack->needs_layout(), "Text layout invalidation did not reach parent");

    const auto display = owner.frame(dui::BoxConstraints::tight({200.0, 100.0}));
    require(first->layout_count() == first_layouts, "unchanged sibling was laid out again");
    require(second->layout_count() == second_layouts + 1, "changed Text was not laid out once");
    require(stack->layout_count() == stack_layouts + 1, "parent VStack was not laid out once");
    require(static_cast<dui::RenderBox*>(stack)->size() == dui::Size{48.0, 32.0}, "updated text width did not affect stack size");
    require(display.dump().contains("text(\"longer\", 0.0, 16.0)"), "updated text was not painted");
}

struct OptionalText {
    bool show;

    auto build(dui::BuildContext&) const {
        return dui::VStack{
            dui::Text{"fixed"},
            dui::optional(show, [] { return dui::Text{"optional"}; })
        };
    }
};

void transparent_elements_flatten_and_unmount_safely() {
    dui::BuildOwner owner;
    owner.render(OptionalText{true});
    const auto first = owner.frame(dui::BoxConstraints::tight({200.0, 100.0}));
    require(first.commands().size() == 2, "optional RenderText was not attached through transparent Elements");

    const dui::Element& stack_element = only_child(*owner.root());
    auto* stack = stack_element.render_object();
    auto* fixed = stack_element.children()[0]->render_object();
    require(stack->children().size() == 2, "transparent Optional was not flattened");

    owner.render(OptionalText{false});
    const auto second = owner.frame(dui::BoxConstraints::tight({200.0, 100.0}));
    require(stack_element.render_object() == stack, "optional removal replaced the stack RenderObject");
    require(stack->children().size() == 1 && stack->children().front() == fixed, "removed RenderObject remained attached");
    require(second.dump() == "text(\"fixed\", 0.0, 0.0)\n", "optional removal left stale paint commands");
}

void display_list_rect_is_deterministic() {
    dui::DisplayListBuilder builder;
    builder.draw_rect({{1.0, 2.0}, {3.0, 4.0}}, 0xff00aaffu);
    require(
        std::move(builder).build().dump() == "rect(1.0, 2.0, 3.0, 4.0, 0xff00aaff)\n",
        "rectangle command dump is not deterministic"
    );
}

void hstack_positions_children_horizontally() {
    dui::BuildOwner owner;
    owner.render(dui::HStack{dui::Text{"A"}, dui::Text{"BB"}});
    const auto display = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));

    const auto* stack = dynamic_cast<const dui::RenderHStack*>(owner.root()->render_object());
    require(stack != nullptr, "HStack did not create RenderHStack");
    require(stack->size() == dui::Size{24.0, 16.0}, "HStack size is incorrect");
    require(static_cast<const dui::RenderBox*>(stack->children()[1])->offset() == dui::Offset{8.0, 0.0}, "HStack child offset is incorrect");
    require(
        display.dump() ==
            "text(\"A\", 0.0, 0.0)\n"
            "text(\"BB\", 8.0, 0.0)\n",
        "HStack produced an unexpected DisplayList"
    );
}

void padding_and_background_compose_render_objects() {
    dui::BuildOwner owner;
    owner.render(dui::background(
        dui::padding(dui::Text{"X"}, dui::Insets::all(4.0)),
        0xff112233u
    ));

    const auto display = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));
    const auto* colored = dynamic_cast<const dui::RenderColoredBox*>(owner.root()->render_object());
    const auto* padded = dynamic_cast<const dui::RenderPadding*>(
        only_child(*owner.root()).render_object()
    );
    require(colored != nullptr && padded != nullptr, "structural modifiers created wrong RenderObjects");
    require(colored->size() == dui::Size{16.0, 24.0}, "background size does not match padded child");
    require(static_cast<const dui::RenderBox*>(padded->children().front())->offset() == dui::Offset{4.0, 4.0}, "padding offset is incorrect");
    require(
        display.dump() ==
            "rect(0.0, 0.0, 16.0, 24.0, 0xff112233)\n"
            "text(\"X\", 4.0, 4.0)\n",
        "padding/background DisplayList is incorrect"
    );
}

class NonBoxRenderObject final : public dui::RenderObject {
public:
    explicit NonBoxRenderObject(dui::RenderOwner& owner) : RenderObject(owner) {}
};

template<class T>
concept ExposesBoxGeometry = requires(const T& value) {
    value.size();
    value.offset();
    value.parent_data();
};

static_assert(!ExposesBoxGeometry<NonBoxRenderObject>);
static_assert(ExposesBoxGeometry<dui::RenderBox>);

template<class T>
concept ExposesSliverGeometry = requires(const T& value) {
    value.geometry();
    value.parent_data();
};

static_assert(!ExposesSliverGeometry<NonBoxRenderObject>);
static_assert(!ExposesSliverGeometry<dui::RenderBox>);
static_assert(ExposesSliverGeometry<dui::RenderSliver>);

struct SliverComponent {
    auto build(dui::BuildContext&) const {
        return dui::Fragment{dui::SliverToBoxAdapter{dui::Text{"component"}}};
    }
};

struct BoxComponent {
    auto build(dui::BuildContext&) const {
        return dui::Fragment{dui::Text{"component"}};
    }
};

static_assert(dui::detail::has_sliver_protocol<SliverComponent>());
static_assert(!dui::detail::has_box_protocol<SliverComponent>());
static_assert(dui::detail::has_box_protocol<BoxComponent>());
static_assert(!dui::detail::has_sliver_protocol<BoxComponent>());

void render_tree_rejects_invalid_children_without_mutation() {
    dui::RenderOwner owner;
    dui::RenderOwner other_owner;
    dui::RenderVStack parent{owner};
    dui::RenderVStack child{owner};
    dui::RenderText leaf{owner, "leaf"};
    dui::RenderText foreign{other_owner, "foreign"};
    NonBoxRenderObject incompatible{owner};

    std::array<dui::RenderObject*, 1> original{&child};
    parent.set_children(original);

    const auto expect_rejected = [&](std::span<dui::RenderObject* const> proposed) {
        bool rejected = false;
        try {
            parent.set_children(proposed);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "invalid RenderObject children were accepted");
        require(parent.children().size() == 1 && parent.children().front() == &child, "failed update mutated children");
        require(child.parent() == &parent, "failed update detached the original child");
    };

    std::array<dui::RenderObject*, 1> null_child{nullptr};
    expect_rejected(null_child);
    std::array<dui::RenderObject*, 1> self_child{&parent};
    expect_rejected(self_child);
    std::array<dui::RenderObject*, 2> duplicate{&leaf, &leaf};
    expect_rejected(duplicate);
    std::array<dui::RenderObject*, 1> cross_owner{&foreign};
    expect_rejected(cross_owner);
    std::array<dui::RenderObject*, 1> wrong_protocol{&incompatible};
    expect_rejected(wrong_protocol);

    std::array<dui::RenderObject*, 1> cycle{&parent};
    bool cycle_rejected = false;
    try {
        child.set_children(cycle);
    } catch (const std::logic_error&) {
        cycle_rejected = true;
    }
    require(cycle_rejected, "RenderObject ancestor cycle was accepted");
}

void render_object_safely_outlives_owner() {
    std::unique_ptr<dui::RenderText> text;
    {
        dui::RenderOwner owner;
        text = std::make_unique<dui::RenderText>(owner, "detached later");
    }

    bool rejected = false;
    try {
        text->mark_needs_paint();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected, "orphaned RenderObject retained a dangling owner");
    text.reset();
}

void invalid_padding_is_rejected() {
    dui::BuildOwner owner;
    bool rejected = false;
    try {
        owner.render(dui::padding(
            dui::Text{"invalid"},
            dui::Insets{-1.0, 0.0, 0.0, 0.0}
        ));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "negative padding was accepted");
}

void hit_test_refreshes_layout_and_pointer_dispatches() {
    dui::BuildOwner owner;
    int activations = 0;
    owner.render(dui::Text{"A", [&] { ++activations; }});

    bool pre_frame_rejected = false;
    try {
        static_cast<void>(owner.hit_test({1.0, 1.0}));
    } catch (const std::logic_error&) {
        pre_frame_rejected = true;
    }
    require(pre_frame_rejected, "hit testing before a frame was accepted");

    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));
    owner.dispatch_pointer({1, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
    owner.dispatch_pointer({1, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(activations == 1, "pointer down/up did not activate Text");

    owner.render(dui::Text{"longer", [&] { ++activations; }});
    require(owner.hit_test({40.0, 1.0}) != nullptr, "hit test used stale geometry after update");

    owner.dispatch_pointer({2, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
    owner.render(dui::VStack{dui::Text{"replacement"}});
    owner.dispatch_pointer({2, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(activations == 1, "removed pointer target was activated");
}

void image_updates_distinguish_pixels_from_geometry() {
    dui::BuildOwner owner;
    owner.render(dui::Image{"first.png", {20.0, 10.0}});
    const auto initial = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));
    auto* image = owner.root()->render_object();
    const auto layouts = image->layout_count();

    require(
        initial.dump() == "image(\"first.png\", 0.0, 0.0, 20.0, 10.0)\n",
        "Image produced an unexpected DisplayList"
    );

    owner.render(dui::Image{"second.png", {20.0, 10.0}});
    require(!image->needs_layout() && image->needs_paint(), "pixel-only Image update invalidated layout");
    const auto pixels = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(image->layout_count() == layouts, "pixel-only Image update repeated layout");
    require(pixels.dump().contains("second.png"), "Image asset update was not painted");

    owner.render(dui::Image{"second.png", {40.0, 30.0}});
    require(image->needs_layout(), "Image intrinsic-size update did not invalidate layout");
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));
    require(static_cast<dui::RenderBox*>(image)->size() == dui::Size{40.0, 30.0}, "Image intrinsic size was not applied");
}

class TestNativeView final : public dui::NativeView {
public:
    [[nodiscard]] dui::NativeViewId id() const override { return 42; }
    [[nodiscard]] dui::ViewMetrics metrics() const override {
        return {{800.0, 600.0}, 2.0};
    }
    void present(dui::DisplayList display_list) override {
        last_display = std::move(display_list);
        ++present_count;
    }

    dui::DisplayList last_display;
    int present_count{};
};

void renderer_flattens_layer_tree_to_native_view() {
    dui::BuildOwner owner;
    owner.render(dui::Text{"submitted"});
    auto layers = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));

    TestNativeView view;
    dui::DisplayListRenderer renderer;
    renderer.render(view, std::move(layers));

    require(view.id() == 42, "NativeView identity is incorrect");
    require(view.metrics().device_pixel_ratio == 2.0, "NativeView metrics are incorrect");
    require(view.present_count == 1, "Renderer did not present exactly one frame");
    require(view.last_display.dump().contains("submitted"), "Renderer submitted the wrong DisplayList");
}

dui::LayerPtr text_layer(std::string text) {
    dui::DisplayListBuilder builder;
    builder.draw_text(text, {});
    return std::make_shared<dui::DisplayListLayer>(std::move(builder).build());
}

void layer_tree_flattens_ordered_offsets() {
    std::vector<dui::LayerPtr> children;
    children.push_back(text_layer("A"));
    children.push_back(std::make_shared<dui::OffsetLayer>(dui::Offset{0.0, 16.0}, text_layer("B")));
    children.push_back(std::make_shared<dui::OffsetLayer>(dui::Offset{0.0, 32.0}, text_layer("C")));
    dui::LayerTree tree{
        {100.0, 100.0},
        std::make_shared<dui::ContainerLayer>(std::move(children))
    };

    require(
        tree.flatten().dump()
            == "text(\"A\", 0.0, 0.0)\n"
               "text(\"B\", 0.0, 16.0)\n"
               "text(\"C\", 0.0, 32.0)\n",
        "LayerTree did not preserve layer order and offsets"
    );
    require(tree.dump().contains("container"), "LayerTree dump omitted container structure");
}

void clean_layer_frame_reuses_immutable_snapshot() {
    dui::BuildOwner owner;
    owner.render(dui::Text{"old"});
    const auto first = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    const auto second = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(first.root() == second.root(), "clean frame did not reuse retained root layer");

    owner.render(dui::Text{"new"});
    const auto replacement = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(first.root() != replacement.root(), "dirty frame reused stale root layer");
    require(first.flatten().dump().contains("old"), "old immutable snapshot changed after update");
    require(replacement.flatten().dump().contains("new"), "replacement snapshot has stale content");

    owner.render(dui::VStack{});
    static_cast<void>(owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0})));
    require(first.flatten().dump().contains("old"), "snapshot depended on unmounted RenderObject");
}

void declarative_boundary_preserves_interleaved_paint_order() {
    dui::BuildOwner owner;
    owner.render(dui::VStack{
        dui::Text{"A"},
        dui::repaint_boundary(dui::Text{"B"}),
        dui::Text{"C"}
    });
    const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(
        tree.flatten().dump()
            == "text(\"A\", 0.0, 0.0)\n"
               "text(\"B\", 0.0, 16.0)\n"
               "text(\"C\", 0.0, 32.0)\n",
        "repaint boundary changed interleaved paint order"
    );
}

void repaint_boundary_isolates_ancestors_and_siblings() {
    dui::BuildOwner owner;
    const auto make_view = [](std::uint32_t color) {
        return dui::VStack{
            dui::Text{"fixed"},
            dui::repaint_boundary(dui::background(dui::Text{"inside"}, color))
        };
    };
    owner.render(make_view(0xff000000u));
    const auto first = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));

    auto* stack = static_cast<dui::RenderVStack*>(owner.root()->render_object());
    auto* fixed = static_cast<dui::RenderText*>(stack->children()[0]);
    auto* boundary = static_cast<dui::RenderRepaintBoundary*>(stack->children()[1]);
    auto* colored = static_cast<dui::RenderColoredBox*>(boundary->children().front());
    const auto old_layer = boundary->retained_layer();
    const auto stack_paints = stack->paint_count();
    const auto fixed_paints = fixed->paint_count();
    const auto boundary_paints = boundary->paint_count();
    const auto colored_paints = colored->paint_count();

    owner.render(make_view(0xffffffffu));
    require(boundary->needs_paint(), "nearest repaint boundary was not dirtied");
    require(!stack->needs_paint(), "paint dirtiness escaped repaint boundary");
    const auto second = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));

    require(stack->paint_count() == stack_paints, "clean ancestor repainted");
    require(fixed->paint_count() == fixed_paints, "clean sibling repainted");
    require(boundary->paint_count() == boundary_paints + 1, "dirty boundary did not repaint once");
    require(colored->paint_count() == colored_paints + 1, "dirty boundary content was not repainted");
    require(boundary->retained_layer() != old_layer, "dirty boundary retained stale layer identity");
    require(first.flatten().dump().contains("0xff000000"), "old layer snapshot was mutated");
    require(second.flatten().dump().contains("0xffffffff"), "new boundary content was not composed");
}

void moving_boundary_reuses_local_layer() {
    dui::BuildOwner owner;
    const auto make_view = [](double height) {
        return dui::VStack{
            dui::Image{"spacer", {10.0, height}},
            dui::repaint_boundary(dui::Text{"moved"})
        };
    };
    owner.render(make_view(10.0));
    static_cast<void>(owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0})));
    auto* stack = static_cast<dui::RenderVStack*>(owner.root()->render_object());
    auto* boundary = static_cast<dui::RenderRepaintBoundary*>(stack->children()[1]);
    auto* text = static_cast<dui::RenderText*>(boundary->children().front());
    const auto layer = boundary->retained_layer();
    const auto text_paints = text->paint_count();

    owner.render(make_view(30.0));
    const auto moved = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(boundary->offset() == dui::Offset{0.0, 30.0}, "boundary offset did not follow layout");
    require(boundary->retained_layer() == layer, "movement repainted boundary-local content");
    require(text->paint_count() == text_paints, "movement repainted clean boundary descendant");
    require(moved.flatten().dump().contains("text(\"moved\", 0.0, 30.0)"), "moved layer used stale offset");
}

void boundary_records_accumulated_non_boundary_offset() {
    dui::BuildOwner owner;
    owner.render(dui::VStack{
        dui::Text{"above"},
        dui::padding(
            dui::repaint_boundary(dui::Text{"nested"}),
            dui::Insets{4.0, 6.0, 0.0, 0.0}
        )
    });
    const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(
        tree.flatten().dump().contains("text(\"nested\", 4.0, 22.0)"),
        "boundary lost accumulated non-boundary ancestor offset"
    );
}

void failed_layout_does_not_poison_constraints_cache() {
    dui::BuildOwner owner;
    owner.render(dui::Text{"stable"});
    const auto stable = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    const dui::BoxConstraints invalid_viewport{};
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool rejected = false;
        try {
            static_cast<void>(owner.layer_frame(invalid_viewport));
        } catch (const std::logic_error&) {
            rejected = true;
        }
        require(rejected, "failed layout constraints were cached as a successful layout");
    }
    const auto recovered = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(recovered.flatten().dump() == stable.flatten().dump(), "valid frame did not recover after layout failure");
}

void sliver_values_validate_protocol_invariants() {
    const dui::SliverConstraints constraints{4.0, 16.0, 20.0, 100.0, 20.0};
    require(
        constraints.as_box_constraints() == dui::BoxConstraints{100.0, 100.0, 0.0, dui::infinity},
        "SliverConstraints produced incorrect box constraints"
    );
    const dui::SliverGeometry geometry{32.0, 20.0, 32.0, 20.0, true};
    require(geometry.scroll_extent() == 32.0 && geometry.has_visual_overflow(), "SliverGeometry lost values");

    bool constraints_rejected = false;
    try {
        static_cast<void>(dui::SliverConstraints{
            std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0, 0.0
        });
    } catch (const std::invalid_argument&) {
        constraints_rejected = true;
    }
    require(constraints_rejected, "NaN SliverConstraints were accepted");

    bool geometry_rejected = false;
    try {
        static_cast<void>(dui::SliverGeometry{10.0, 8.0, 7.0, 8.0});
    } catch (const std::invalid_argument&) {
        geometry_rejected = true;
    }
    require(geometry_rejected, "non-normalized SliverGeometry was accepted");
}

void sliver_pre_layout_hit_test_and_malformed_clips_are_rejected_safely() {
    dui::RenderOwner owner;
    dui::RenderSliverToBoxAdapter sliver{owner};
    dui::HitTestResult result;
    require(!sliver.hit_test(result, {0.0, 0.0}), "unlaid-out Sliver reported a hit");

    dui::DisplayListBuilder underflow;
    bool pop_rejected = false;
    try {
        underflow.pop_clip();
    } catch (const std::logic_error&) {
        pop_rejected = true;
    }
    require(pop_rejected, "DisplayListBuilder accepted clip-stack underflow");

    dui::DisplayListBuilder unclosed;
    unclosed.push_clip_rect({{}, {10.0, 10.0}});
    bool build_rejected = false;
    try {
        static_cast<void>(std::move(unclosed).build());
    } catch (const std::logic_error&) {
        build_rejected = true;
    }
    require(build_rejected, "DisplayListBuilder accepted an unclosed clip");

    bool direct_rejected = false;
    try {
        static_cast<void>(dui::DisplayList{
            std::vector<dui::DisplayCommand>{dui::PopClipCommand{}}
        });
    } catch (const std::invalid_argument&) {
        direct_rejected = true;
    }
    require(direct_rejected, "DisplayList accepted a malformed public command stream");
}

void render_protocols_reject_wrong_children_transactionally() {
    dui::RenderOwner owner;
    dui::RenderVStack box{owner};
    dui::RenderViewport viewport{owner};
    dui::RenderSliverToBoxAdapter sliver{owner};
    dui::RenderSliverToBoxAdapter other_sliver{owner};
    dui::RenderText first{owner, "first"};
    dui::RenderText second{owner, "second"};

    std::array<dui::RenderObject*, 1> original{&first};
    sliver.set_children(original);

    const auto rejected_without_mutation = [&](dui::RenderObject& parent,
                                                std::span<dui::RenderObject* const> proposed) {
        bool rejected = false;
        try {
            parent.set_children(proposed);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        require(rejected, "incompatible render protocol was accepted");
    };

    std::array<dui::RenderObject*, 1> sliver_child{&sliver};
    rejected_without_mutation(box, sliver_child);
    std::array<dui::RenderObject*, 1> box_child{&second};
    rejected_without_mutation(viewport, box_child);
    std::array<dui::RenderObject*, 1> wrong_sliver_child{&other_sliver};
    rejected_without_mutation(sliver, wrong_sliver_child);
    std::array<dui::RenderObject*, 2> too_many{&first, &second};
    rejected_without_mutation(sliver, too_many);
    require(sliver.children().size() == 1 && sliver.children().front() == &first,
            "failed Sliver child update mutated the original tree");
    require(first.parent() == &sliver && second.parent() == nullptr,
            "failed Sliver child update changed parent links");
}

void viewport_lays_out_clips_paints_and_hits_slivers() {
    dui::BuildOwner owner;
    owner.render(dui::Viewport{
        8.0,
        dui::SliverToBoxAdapter{dui::Text{"A"}},
        dui::SliverToBoxAdapter{dui::Text{"B"}}
    });
    const auto first = owner.layer_frame(dui::BoxConstraints::tight({100.0, 20.0}));
    require(
        first.flatten().dump() ==
            "push_clip_rect(0.0, 0.0, 100.0, 20.0)\n"
            "text(\"A\", 0.0, -8.0)\n"
            "text(\"B\", 0.0, 8.0)\n"
            "pop_clip()\n",
        "Viewport produced incorrect clipped paint output"
    );

    auto* viewport = dynamic_cast<dui::RenderViewport*>(owner.root()->render_object());
    require(viewport != nullptr, "Viewport did not create RenderViewport");
    require(viewport->size() == dui::Size{100.0, 20.0}, "Viewport size is incorrect");
    require(viewport->max_scroll_extent() == 12.0, "Viewport max scroll extent is incorrect");
    auto* first_sliver = dynamic_cast<dui::RenderSliver*>(viewport->children()[0]);
    auto* second_sliver = dynamic_cast<dui::RenderSliver*>(viewport->children()[1]);
    require(first_sliver != nullptr && second_sliver != nullptr, "Viewport children are not Slivers");
    require(first_sliver->geometry().paint_extent() == 8.0, "leading Sliver paint extent is incorrect");
    require(second_sliver->parent_data().paint_offset == dui::Offset{0.0, 8.0},
            "second Sliver paint offset is incorrect");
    require(owner.hit_test({1.0, 1.0}) == first_sliver->children().front(),
            "partially visible leading Sliver did not hit");
    require(owner.hit_test({1.0, 9.0}) == second_sliver->children().front(),
            "second visible Sliver did not hit");
    require(owner.hit_test({1.0, 20.0}) == nullptr, "Viewport trailing edge was hit");
}

void view_protocols_propagate_through_components_and_fragments() {
    dui::BuildOwner owner;
    owner.render(dui::Viewport{0.0, SliverComponent{}});
    const auto frame = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
    require(frame.dump().contains("text(\"component\", 0.0, 0.0)"),
            "Sliver component protocol did not propagate through transparent Elements");

    owner.render(dui::Viewport{
        0.0,
        dui::SliverToBoxAdapter{BoxComponent{}}
    });
    const auto box_component = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
    require(box_component.dump().contains("text(\"component\", 0.0, 0.0)"),
            "box component protocol did not propagate into Sliver adapter");
}

void viewport_scroll_update_preserves_identity_and_culls_offscreen_content() {
    dui::BuildOwner owner;
    const auto make_view = [](double offset) {
        return dui::Viewport{
            offset,
            dui::SliverToBoxAdapter{dui::Text{"A"}},
            dui::SliverToBoxAdapter{dui::Text{"B"}}
        };
    };
    owner.render(make_view(0.0));
    const auto initial = owner.layer_frame(dui::BoxConstraints::tight({100.0, 16.0}));
    auto* viewport = owner.root()->render_object();
    auto* first_sliver = viewport->children()[0];
    auto* second_sliver = viewport->children()[1];
    const auto viewport_layouts = viewport->layout_count();

    owner.render(make_view(16.0));
    const auto scrolled = owner.layer_frame(dui::BoxConstraints::tight({100.0, 16.0}));
    require(owner.root()->render_object() == viewport, "scroll update replaced RenderViewport");
    require(viewport->children()[0] == first_sliver && viewport->children()[1] == second_sliver,
            "scroll update replaced RenderSlivers");
    require(viewport->layout_count() == viewport_layouts + 1, "scroll update did not relayout viewport once");
    require(!scrolled.flatten().dump().contains("text(\"A\""), "fully offscreen Sliver was painted");
    require(scrolled.flatten().dump().contains("text(\"B\", 0.0, 0.0)"),
            "visible Sliver used the wrong scroll translation");
    require(initial.flatten().dump().contains("text(\"A\", 0.0, 0.0)"),
            "scroll update mutated an older LayerTree snapshot");

    const auto clean_layouts = viewport->layout_count();
    owner.render(make_view(16.0));
    static_cast<void>(owner.layer_frame(dui::BoxConstraints::tight({100.0, 16.0})));
    require(viewport->layout_count() == clean_layouts, "equivalent Viewport update repeated layout");
}

void viewport_scroll_reuses_repaint_boundary_layer() {
    dui::BuildOwner owner;
    const auto make_view = [](double offset) {
        return dui::Viewport{
            offset,
            dui::SliverToBoxAdapter{dui::repaint_boundary(dui::Text{"retained"})}
        };
    };
    owner.render(make_view(0.0));
    static_cast<void>(owner.layer_frame(dui::BoxConstraints::tight({100.0, 8.0})));
    auto* viewport = owner.root()->render_object();
    auto* sliver = viewport->children().front();
    auto* boundary = dynamic_cast<dui::RenderRepaintBoundary*>(sliver->children().front());
    require(boundary != nullptr, "Sliver repaint boundary was not created");
    const auto retained = boundary->retained_layer();
    const auto paints = boundary->paint_count();

    owner.render(make_view(4.0));
    const auto moved = owner.layer_frame(dui::BoxConstraints::tight({100.0, 8.0}));
    require(boundary->retained_layer() == retained, "scrolling repainted boundary-local content");
    require(boundary->paint_count() == paints, "scrolling increased repaint-boundary paint count");
    require(moved.flatten().dump().contains("text(\"retained\", 0.0, -4.0)"),
            "scrolling did not recompose the retained boundary at its new offset");
}

void nested_boundary_recomposes_without_repainting_outer_content() {
    dui::BuildOwner owner;
    const auto make_view = [](std::uint32_t color) {
        return dui::repaint_boundary(dui::background(
            dui::repaint_boundary(dui::background(dui::Text{"nested"}, color)),
            0xff112233u
        ));
    };
    owner.render(make_view(0xff000000u));
    const auto first = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
    auto* outer = static_cast<dui::RenderRepaintBoundary*>(owner.root()->render_object());
    auto* outer_color = static_cast<dui::RenderColoredBox*>(outer->children().front());
    auto* inner = static_cast<dui::RenderRepaintBoundary*>(outer_color->children().front());
    const auto outer_layer = outer->retained_layer();
    const auto inner_layer = inner->retained_layer();
    const auto outer_paints = outer->paint_count();
    const auto outer_color_paints = outer_color->paint_count();
    const auto inner_paints = inner->paint_count();

    owner.render(make_view(0xffffffffu));
    require(!outer->needs_paint() && outer->needs_compositing(), "inner repaint dirtied outer paint");
    require(owner.pending_compositing_count() != 0, "inner repaint did not schedule composition");
    const auto second = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));

    require(outer->paint_count() == outer_paints, "outer boundary repainted during recomposition");
    require(outer_color->paint_count() == outer_color_paints, "outer content repainted during recomposition");
    require(inner->paint_count() == inner_paints + 1, "inner boundary was not repainted");
    require(inner->retained_layer() != inner_layer, "inner boundary retained stale layer");
    require(outer->retained_layer() != outer_layer, "outer boundary did not recompose child layer");
    require(first.flatten().dump().contains("0xff000000"), "old nested snapshot was mutated");
    require(second.flatten().dump().contains("0xffffffff"), "new nested snapshot has stale content");
    require(owner.pending_compositing_count() == 0, "composition queue did not drain");
}

void stack_hit_test_uses_reverse_paint_order_and_records_path() {
    dui::BuildOwner owner;
    owner.render(dui::Stack{dui::Text{"bottom"}, dui::Text{"top"}});
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

    auto* stack = owner.root()->render_object();
    dui::HitTestResult result;
    require(stack->parent()->hit_test(result, {1.0, 1.0}), "overlapping Stack was not hit");
    require(result.target() == stack->children()[1], "Stack did not hit the last-painted child first");
    require(result.path().size() == 3, "hit-test path does not contain leaf, stack, and RenderView");
    require(result.path()[1].target == stack, "hit-test path has incorrect ancestor order");
}

void color_update_repaints_without_layout() {
    dui::BuildOwner owner;
    owner.render(dui::background(dui::Text{"color"}, 0xff000000u));
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

    auto* colored = owner.root()->render_object();
    const auto layouts = colored->layout_count();
    const auto paints = colored->paint_count();
    owner.render(dui::background(dui::Text{"color"}, 0xffffffffu));

    require(!colored->needs_layout(), "color-only update invalidated layout");
    require(colored->needs_paint(), "color-only update did not invalidate paint");
    const auto display = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));
    require(colored->layout_count() == layouts, "color-only update repeated layout");
    require(colored->paint_count() == paints + 1, "color-only update did not repaint once");
    require(display.dump().starts_with("rect(0.0, 0.0, 40.0, 16.0, 0xffffffff)"), "new color was not painted");
}

void declarative_gesture_detector_bubbles_taps() {
    dui::BuildOwner owner;
    std::vector<std::string> taps;
    owner.render(dui::on_tap(
        dui::on_tap(
            dui::Text{"tap"},
            [&] { taps.push_back("inner"); }
        ),
        [&] { taps.push_back("outer"); }
    ));
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

    owner.dispatch_pointer({9, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
    require(taps.empty(), "GestureDetector activated before pointer up");
    owner.dispatch_pointer({9, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(
        taps == std::vector<std::string>{"inner", "outer"},
        "declarative GestureDetector did not bubble in leaf-to-root order"
    );
}

void declarative_gesture_cancel_suppresses_tap() {
    dui::BuildOwner owner;
    int taps = 0;
    owner.render(dui::on_tap(dui::Text{"tap"}, [&] { ++taps; }));
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

    owner.dispatch_pointer({14, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
    owner.dispatch_pointer({14, dui::BuildOwner::PointerPhase::cancel, {}});
    owner.dispatch_pointer({14, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(taps == 0, "cancelled GestureDetector activated");
}

void gesture_remains_active_across_descendants_of_same_detector() {
    dui::BuildOwner owner;
    int taps = 0;
    owner.render(dui::on_tap(
        dui::padding(dui::Text{"A"}, dui::Insets::all(10.0)),
        [&] { ++taps; }
    ));
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

    owner.dispatch_pointer({15, dui::BuildOwner::PointerPhase::down, {11.0, 11.0}});
    owner.dispatch_pointer({15, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(taps == 1, "tap was rejected while pointer remained inside GestureDetector");
}

void declarative_focus_view_requests_focus_and_routes_key() {
    dui::BuildOwner owner;
    std::vector<std::string> keys;
    owner.render(dui::focusable(
        dui::focusable(
            dui::Text{"focus"},
            [&](const dui::KeyEvent&) {
                keys.push_back("inner");
                return dui::KeyEventResult::handled;
            }
        ),
        [&](const dui::KeyEvent&) {
            keys.push_back("outer");
            return dui::KeyEventResult::handled;
        }
    ));
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

    owner.dispatch_pointer({10, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
    owner.dispatch_pointer({10, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(owner.focused_node() != nullptr, "FocusView did not request focus on activation");
    require(
        owner.dispatch_key({"Enter", dui::KeyPhase::down}) == dui::KeyEventResult::handled,
        "focused declarative View did not handle key"
    );
    require(keys == std::vector<std::string>{"inner"}, "outer FocusView replaced the deeper focus target");
}

void gesture_callback_can_replace_itself_and_reenter_pointer_dispatch() {
    dui::BuildOwner owner;
    std::vector<std::string> taps;
    owner.render(dui::on_tap(
        dui::on_tap(dui::Text{"tap"}, [&] {
            taps.push_back("inner");
            owner.dispatch_pointer({11, dui::BuildOwner::PointerPhase::cancel, {}});
            owner.render(dui::Text{"replacement"});
        }),
        [&] { taps.push_back("outer"); }
    ));
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

    owner.dispatch_pointer({11, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
    owner.dispatch_pointer({11, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(taps == std::vector<std::string>{"inner"}, "stale gesture ancestor ran after tree replacement");
}

void autofocus_is_not_reapplied_during_reconciliation() {
    dui::BuildOwner owner;
    std::vector<std::string> keys;
    auto make_view = [&] {
        return dui::HStack{
            dui::focusable(
                dui::Text{"A"},
                [&](const dui::KeyEvent&) {
                    keys.push_back("A");
                    return dui::KeyEventResult::handled;
                },
                true
            ),
            dui::focusable(dui::Text{"B"}, [&](const dui::KeyEvent&) {
                keys.push_back("B");
                return dui::KeyEventResult::handled;
            })
        };
    };
    owner.render(make_view());
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));
    owner.dispatch_pointer({12, dui::BuildOwner::PointerPhase::down, {9.0, 1.0}});
    owner.dispatch_pointer({12, dui::BuildOwner::PointerPhase::up, {9.0, 1.0}});

    owner.render(make_view());
    require(
        owner.dispatch_key({"Enter", dui::KeyPhase::down}) == dui::KeyEventResult::handled,
        "focused sibling did not handle key after reconciliation"
    );
    require(keys == std::vector<std::string>{"B"}, "autofocus stole focus during reconciliation");
}

void text_callback_can_destroy_its_build_owner() {
    auto owner = std::make_unique<dui::BuildOwner>();
    owner->render(dui::Text{"destroy", [&] { owner.reset(); }});
    static_cast<void>(owner->frame(dui::BoxConstraints::tight({100.0, 100.0})));
    owner->dispatch_pointer({13, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
    owner->dispatch_pointer({13, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
    require(owner == nullptr, "Text callback did not destroy its BuildOwner");
}

} // namespace

int main() {
    try {
        constraints_validate_and_constrain();
        vstack_lays_out_paints_and_hits();
        equivalent_update_reuses_layout_and_display_list();
        text_update_invalidates_layout_and_paint();
        transparent_elements_flatten_and_unmount_safely();
        display_list_rect_is_deterministic();
        hstack_positions_children_horizontally();
        padding_and_background_compose_render_objects();
        render_tree_rejects_invalid_children_without_mutation();
        render_object_safely_outlives_owner();
        invalid_padding_is_rejected();
        hit_test_refreshes_layout_and_pointer_dispatches();
        image_updates_distinguish_pixels_from_geometry();
        renderer_flattens_layer_tree_to_native_view();
        layer_tree_flattens_ordered_offsets();
        clean_layer_frame_reuses_immutable_snapshot();
        declarative_boundary_preserves_interleaved_paint_order();
        repaint_boundary_isolates_ancestors_and_siblings();
        moving_boundary_reuses_local_layer();
        boundary_records_accumulated_non_boundary_offset();
        failed_layout_does_not_poison_constraints_cache();
        sliver_values_validate_protocol_invariants();
        sliver_pre_layout_hit_test_and_malformed_clips_are_rejected_safely();
        render_protocols_reject_wrong_children_transactionally();
        viewport_lays_out_clips_paints_and_hits_slivers();
        view_protocols_propagate_through_components_and_fragments();
        viewport_scroll_update_preserves_identity_and_culls_offscreen_content();
        viewport_scroll_reuses_repaint_boundary_layer();
        nested_boundary_recomposes_without_repainting_outer_content();
        stack_hit_test_uses_reverse_paint_order_and_records_path();
        color_update_repaints_without_layout();
        declarative_gesture_detector_bubbles_taps();
        declarative_gesture_cancel_suppresses_tap();
        gesture_remains_active_across_descendants_of_same_detector();
        declarative_focus_view_requests_focus_and_routes_key();
        gesture_callback_can_replace_itself_and_reenter_pointer_dispatch();
        autofocus_is_not_reapplied_during_reconciliation();
        text_callback_can_destroy_its_build_owner();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "All DUI rendering tests passed\n";
    return EXIT_SUCCESS;
}
