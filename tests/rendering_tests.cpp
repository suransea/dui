#include "dui/ui.hpp"

#include <array>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
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
  owner.render(dui::VStack{dui::Text{"A"}, dui::Text{"BB"}});

  const dui::DisplayList display = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));
  require(display.dump() == "text(\"A\", 0.0, 0.0)\n"
                            "text(\"BB\", 0.0, 16.0)\n",
          "VStack produced an unexpected DisplayList");

  const auto* stack = dynamic_cast<const dui::RenderVStack*>(owner.root()->render_object());
  require(stack != nullptr, "VStack did not create RenderVStack");
  require(stack->size() == dui::Size{16.0, 32.0}, "VStack size is incorrect");
  require(static_cast<const dui::RenderBox*>(stack->children()[1])->offset() ==
            dui::Offset{0.0, 16.0},
          "second child offset is incorrect");
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
  require(only_child(*owner.root()).render_object() == text,
          "equivalent Text replaced its RenderObject");
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
  require(static_cast<dui::RenderBox*>(stack)->size() == dui::Size{48.0, 32.0},
          "updated text width did not affect stack size");
  require(display.dump().contains("text(\"longer\", 0.0, 16.0)"), "updated text was not painted");
}

struct OptionalText {
  bool show;

  auto build(dui::BuildContext&) const {
    return dui::VStack{dui::Text{"fixed"},
                       dui::optional(show, [] { return dui::Text{"optional"}; })};
  }
};

void transparent_elements_flatten_and_unmount_safely() {
  dui::BuildOwner owner;
  owner.render(OptionalText{true});
  const auto first = owner.frame(dui::BoxConstraints::tight({200.0, 100.0}));
  require(first.commands().size() == 2,
          "optional RenderText was not attached through transparent Elements");

  const dui::Element& stack_element = only_child(*owner.root());
  auto* stack = stack_element.render_object();
  auto* fixed = stack_element.children()[0]->render_object();
  require(stack->children().size() == 2, "transparent Optional was not flattened");

  owner.render(OptionalText{false});
  const auto second = owner.frame(dui::BoxConstraints::tight({200.0, 100.0}));
  require(stack_element.render_object() == stack,
          "optional removal replaced the stack RenderObject");
  require(stack->children().size() == 1 && stack->children().front() == fixed,
          "removed RenderObject remained attached");
  require(second.dump() == "text(\"fixed\", 0.0, 0.0)\n",
          "optional removal left stale paint commands");
}

void display_list_rect_is_deterministic() {
  dui::DisplayListBuilder builder;
  builder.draw_rect({{1.0, 2.0}, {3.0, 4.0}}, 0xff00aaffu);
  require(std::move(builder).build().dump() == "rect(1.0, 2.0, 3.0, 4.0, 0xff00aaff)\n",
          "rectangle command dump is not deterministic");
}

void hstack_positions_children_horizontally() {
  dui::BuildOwner owner;
  owner.render(dui::HStack{dui::Text{"A"}, dui::Text{"BB"}});
  const auto display = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));

  const auto* stack = dynamic_cast<const dui::RenderHStack*>(owner.root()->render_object());
  require(stack != nullptr, "HStack did not create RenderHStack");
  require(stack->size() == dui::Size{24.0, 16.0}, "HStack size is incorrect");
  require(static_cast<const dui::RenderBox*>(stack->children()[1])->offset() ==
            dui::Offset{8.0, 0.0},
          "HStack child offset is incorrect");
  require(display.dump() == "text(\"A\", 0.0, 0.0)\n"
                            "text(\"BB\", 8.0, 0.0)\n",
          "HStack produced an unexpected DisplayList");
}

void padding_and_background_compose_render_objects() {
  dui::BuildOwner owner;
  owner.render(dui::background(dui::padding(dui::Text{"X"}, dui::Insets::all(4.0)), 0xff112233u));

  const auto display = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));
  const auto* colored = dynamic_cast<const dui::RenderColoredBox*>(owner.root()->render_object());
  const auto* padded =
    dynamic_cast<const dui::RenderPadding*>(only_child(*owner.root()).render_object());
  require(colored != nullptr && padded != nullptr,
          "structural modifiers created wrong RenderObjects");
  require(colored->size() == dui::Size{16.0, 24.0}, "background size does not match padded child");
  require(static_cast<const dui::RenderBox*>(padded->children().front())->offset() ==
            dui::Offset{4.0, 4.0},
          "padding offset is incorrect");
  require(display.dump() == "rect(0.0, 0.0, 16.0, 24.0, 0xff112233)\n"
                            "text(\"X\", 4.0, 4.0)\n",
          "padding/background DisplayList is incorrect");
}

class NonBoxRenderObject final : public dui::RenderObject {
public:
  explicit NonBoxRenderObject(dui::RenderOwner& owner) : RenderObject(owner) {}
};

template <class T>
concept ExposesBoxGeometry = requires(const T& value) {
                               value.size();
                               value.offset();
                               value.parent_data();
                             };

static_assert(!ExposesBoxGeometry<NonBoxRenderObject>);
static_assert(ExposesBoxGeometry<dui::RenderBox>);

template <class T>
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
  auto build(dui::BuildContext&) const { return dui::Fragment{dui::Text{"component"}}; }
};

struct KeepAliveResource {
  int* destructions;
  bool active{true};

  KeepAliveResource(int& constructions, int& destroyed) : destructions(&destroyed) {
    ++constructions;
  }
  KeepAliveResource(const KeepAliveResource&) = delete;
  KeepAliveResource& operator=(const KeepAliveResource&) = delete;
  KeepAliveResource(KeepAliveResource&& other) noexcept
    : destructions(other.destructions), active(std::exchange(other.active, false)) {}
  ~KeepAliveResource() {
    if (active) {
      ++*destructions;
    }
  }
};

struct BudgetResource {
  int* cancellations;
  int* destructions;
  bool active{true};

  BudgetResource(int& constructions, int& canceled, int& destroyed)
    : cancellations(&canceled), destructions(&destroyed) {
    ++constructions;
  }
  BudgetResource(const BudgetResource&) = delete;
  BudgetResource& operator=(const BudgetResource&) = delete;
  BudgetResource(BudgetResource&& other) noexcept
    : cancellations(other.cancellations), destructions(other.destructions),
      active(std::exchange(other.active, false)) {}
  ~BudgetResource() {
    if (active) {
      ++*destructions;
    }
  }

  void request_stop() noexcept { ++*cancellations; }
};

struct KeepAliveStateItem {
  int id;
  std::vector<std::optional<dui::StateHandle<int>>>* handles;
  std::vector<const KeepAliveResource*>* resources;
  int* resource_constructions;
  int* resource_destructions;
  dui::Signal<int>* signal;
  int* key_events;

  auto build(dui::BuildContext& context) const {
    auto state = context.state<"keep-alive-value">(id * 10);
    handles->at(static_cast<std::size_t>(id)) = state;
    auto& resource = context.resource<"keep-alive-resource">([&] {
      return KeepAliveResource{*resource_constructions, *resource_destructions};
    });
    resources->at(static_cast<std::size_t>(id)) = &resource;
    const int observed = id == 1 ? signal->get() : 0;
    return dui::focusable(
      dui::Text{"keep-alive " + std::to_string(id) + ":" + std::to_string(state.get()) + ":" +
                std::to_string(observed)},
      [events = key_events](const dui::KeyEvent&) {
        ++*events;
        return dui::KeyEventResult::handled;
      },
      id == 1);
  }
};

struct BudgetStateItem {
  int id;
  std::vector<std::optional<dui::StateHandle<int>>>* handles;
  int* resource_constructions;
  int* resource_cancellations;
  int* resource_destructions;
  dui::Signal<int>* signal;

  auto build(dui::BuildContext& context) const {
    auto state = context.state<"budget-value">(id);
    handles->at(static_cast<std::size_t>(id)) = state;
    static_cast<void>(context.resource<"budget-resource">([&] {
      return BudgetResource{*resource_constructions, *resource_cancellations,
                            *resource_destructions};
    }));
    if (id == 0) {
      static_cast<void>(signal->get());
    }
    return dui::focusable(dui::Text{"budget " + std::to_string(id)}, {}, id == 0);
  }
};

static_assert(dui::detail::has_sliver_protocol<SliverComponent>());
static_assert(!dui::detail::has_box_protocol<SliverComponent>());
static_assert(dui::detail::has_box_protocol<BoxComponent>());
static_assert(!dui::detail::has_sliver_protocol<BoxComponent>());
static_assert(
  dui::detail::has_sliver_protocol<dui::SliverFixedExtentList<dui::Text, BoxComponent>>());
static_assert(dui::detail::has_single_box_protocol<dui::Text>());
static_assert(dui::detail::has_single_box_protocol<BoxComponent>());
static_assert(!dui::detail::has_single_box_protocol<dui::Optional<dui::Text>>());
static_assert(!dui::detail::has_single_box_protocol<dui::Fragment<dui::Text, dui::Text>>());

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
    require(parent.children().size() == 1 && parent.children().front() == &child,
            "failed update mutated children");
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
    owner.render(dui::padding(dui::Text{"invalid"}, dui::Insets{-1.0, 0.0, 0.0, 0.0}));
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

  require(initial.dump() == "image(\"first.png\", 0.0, 0.0, 20.0, 10.0)\n",
          "Image produced an unexpected DisplayList");

  owner.render(dui::Image{"second.png", {20.0, 10.0}});
  require(!image->needs_layout() && image->needs_paint(),
          "pixel-only Image update invalidated layout");
  const auto pixels = owner.frame(dui::BoxConstraints::tight({100.0, 100.0}));
  require(image->layout_count() == layouts, "pixel-only Image update repeated layout");
  require(pixels.dump().contains("second.png"), "Image asset update was not painted");

  owner.render(dui::Image{"second.png", {40.0, 30.0}});
  require(image->needs_layout(), "Image intrinsic-size update did not invalidate layout");
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));
  require(static_cast<dui::RenderBox*>(image)->size() == dui::Size{40.0, 30.0},
          "Image intrinsic size was not applied");
}

class TestNativeView final : public dui::NativeView {
public:
  [[nodiscard]] dui::NativeViewId id() const override { return 42; }
  [[nodiscard]] dui::ViewMetrics metrics() const override { return {{800.0, 600.0}, 2.0}; }
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
  require(view.last_display.dump().contains("submitted"),
          "Renderer submitted the wrong DisplayList");
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
  dui::LayerTree tree{{100.0, 100.0}, std::make_shared<dui::ContainerLayer>(std::move(children))};

  require(tree.flatten().dump() == "text(\"A\", 0.0, 0.0)\n"
                                   "text(\"B\", 0.0, 16.0)\n"
                                   "text(\"C\", 0.0, 32.0)\n",
          "LayerTree did not preserve layer order and offsets");
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
  owner.render(dui::VStack{dui::Text{"A"}, dui::repaint_boundary(dui::Text{"B"}), dui::Text{"C"}});
  const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  require(tree.flatten().dump() == "text(\"A\", 0.0, 0.0)\n"
                                   "text(\"B\", 0.0, 16.0)\n"
                                   "text(\"C\", 0.0, 32.0)\n",
          "repaint boundary changed interleaved paint order");
}

void repaint_boundary_isolates_ancestors_and_siblings() {
  dui::BuildOwner owner;
  const auto make_view = [](std::uint32_t color) {
    return dui::VStack{dui::Text{"fixed"},
                       dui::repaint_boundary(dui::background(dui::Text{"inside"}, color))};
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
    return dui::VStack{dui::Image{"spacer", {10.0, height}},
                       dui::repaint_boundary(dui::Text{"moved"})};
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
  require(moved.flatten().dump().contains("text(\"moved\", 0.0, 30.0)"),
          "moved layer used stale offset");
}

void boundary_records_accumulated_non_boundary_offset() {
  dui::BuildOwner owner;
  owner.render(
    dui::VStack{dui::Text{"above"}, dui::padding(dui::repaint_boundary(dui::Text{"nested"}),
                                                 dui::Insets{4.0, 6.0, 0.0, 0.0})});
  const auto tree = owner.layer_frame(dui::BoxConstraints::tight({100.0, 100.0}));
  require(tree.flatten().dump().contains("text(\"nested\", 4.0, 22.0)"),
          "boundary lost accumulated non-boundary ancestor offset");
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
  require(recovered.flatten().dump() == stable.flatten().dump(),
          "valid frame did not recover after layout failure");
}

void sliver_values_validate_protocol_invariants() {
  const dui::SliverConstraints constraints{4.0, 16.0, 20.0, 100.0, 20.0};
  require(constraints.as_box_constraints() == dui::BoxConstraints{100.0, 100.0, 0.0, dui::infinity},
          "SliverConstraints produced incorrect box constraints");
  const dui::SliverGeometry geometry{32.0, 20.0, 32.0, 20.0, true};
  require(geometry.scroll_extent() == 32.0 && geometry.has_visual_overflow(),
          "SliverGeometry lost values");

  bool constraints_rejected = false;
  try {
    static_cast<void>(
      dui::SliverConstraints{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0, 0.0});
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
    static_cast<void>(dui::DisplayList{std::vector<dui::DisplayCommand>{dui::PopClipCommand{}}});
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
  owner.render(dui::Viewport{8.0, dui::SliverToBoxAdapter{dui::Text{"A"}},
                             dui::SliverToBoxAdapter{dui::Text{"B"}}});
  const auto first = owner.layer_frame(dui::BoxConstraints::tight({100.0, 20.0}));
  require(first.flatten().dump() == "push_clip_rect(0.0, 0.0, 100.0, 20.0)\n"
                                    "text(\"A\", 0.0, -8.0)\n"
                                    "text(\"B\", 0.0, 8.0)\n"
                                    "pop_clip()\n",
          "Viewport produced incorrect clipped paint output");

  auto* viewport = dynamic_cast<dui::RenderViewport*>(owner.root()->render_object());
  require(viewport != nullptr, "Viewport did not create RenderViewport");
  require(viewport->size() == dui::Size{100.0, 20.0}, "Viewport size is incorrect");
  require(viewport->max_scroll_extent() == 12.0, "Viewport max scroll extent is incorrect");
  auto* first_sliver = dynamic_cast<dui::RenderSliver*>(viewport->children()[0]);
  auto* second_sliver = dynamic_cast<dui::RenderSliver*>(viewport->children()[1]);
  require(first_sliver != nullptr && second_sliver != nullptr, "Viewport children are not Slivers");
  require(first_sliver->geometry().paint_extent() == 8.0,
          "leading Sliver paint extent is incorrect");
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

  owner.render(dui::Viewport{0.0, dui::SliverToBoxAdapter{BoxComponent{}}});
  const auto box_component = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(box_component.dump().contains("text(\"component\", 0.0, 0.0)"),
          "box component protocol did not propagate into Sliver adapter");
}

void viewport_scroll_update_preserves_identity_and_culls_offscreen_content() {
  dui::BuildOwner owner;
  const auto make_view = [](double offset) {
    return dui::Viewport{offset, dui::SliverToBoxAdapter{dui::Text{"A"}},
                         dui::SliverToBoxAdapter{dui::Text{"B"}}};
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
  require(viewport->layout_count() == viewport_layouts + 1,
          "scroll update did not relayout viewport once");
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
    return dui::Viewport{offset,
                         dui::SliverToBoxAdapter{dui::repaint_boundary(dui::Text{"retained"})}};
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

void fixed_extent_sliver_virtualizes_layout_paint_and_hit_testing() {
  dui::BuildOwner owner;
  const auto make_view = [](double offset) {
    return dui::Viewport{offset, dui::SliverFixedExtentList{16.0, dui::Text{"A"}, dui::Text{"B"},
                                                            dui::Text{"C"}, dui::Text{"D"},
                                                            dui::Text{"E"}}};
  };
  owner.render(make_view(16.0));
  const auto first = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  auto* viewport = dynamic_cast<dui::RenderViewport*>(owner.root()->render_object());
  auto* list = dynamic_cast<dui::RenderSliverFixedExtentList*>(viewport->children().front());
  require(list != nullptr, "SliverFixedExtentList did not create its RenderSliver");
  require(list->first_visible_index() == 1 && list->visible_child_count() == 2,
          "fixed-extent Sliver calculated the wrong visible range");
  require(list->geometry().scroll_extent() == 80.0, "fixed-extent Sliver scroll extent is wrong");
  require(viewport->max_scroll_extent() == 48.0, "fixed-extent viewport max scroll is wrong");
  require(first.dump() == "push_clip_rect(0.0, 0.0, 100.0, 32.0)\n"
                          "text(\"B\", 0.0, 0.0)\n"
                          "text(\"C\", 0.0, 16.0)\n"
                          "pop_clip()\n",
          "fixed-extent Sliver painted outside its visible range");
  require(list->children()[0]->layout_count() == 0, "leading offscreen item was laid out");
  require(list->children()[1]->layout_count() == 1, "first visible item was not laid out once");
  require(list->children()[2]->layout_count() == 1, "second visible item was not laid out once");
  require(list->children()[3]->layout_count() == 0, "trailing offscreen item was laid out");
  require(owner.hit_test({1.0, 1.0}) == list->children()[1], "first visible list item did not hit");
  require(owner.hit_test({1.0, 17.0}) == list->children()[2],
          "second visible list item did not hit");
  require(owner.hit_test({1.0, 32.0}) == nullptr, "list hit at the viewport trailing edge");

  owner.render(make_view(32.0));
  const auto second = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list->first_visible_index() == 2 && list->visible_child_count() == 2,
          "scrolling did not advance the visible range at an exact item boundary");
  require(!second.dump().contains("text(\"B\""), "item leaving visibility was still painted");
  require(second.dump().contains("text(\"C\", 0.0, 0.0)"), "retained visible item was misplaced");
  require(second.dump().contains("text(\"D\", 0.0, 16.0)"), "newly visible item was not painted");
  require(list->children()[1]->layout_count() == 1, "offscreen item was laid out again");
  require(list->children()[2]->layout_count() == 1, "retained item repeated layout unnecessarily");
  require(list->children()[3]->layout_count() == 1, "newly visible item was not laid out");

  owner.render(make_view(100.0));
  const auto overscrolled = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list->visible_child_count() == 0, "overscrolled list retained visible children");
  require(!overscrolled.dump().contains("text("), "overscrolled list painted a child");
}

void fixed_extent_sliver_rejects_invalid_extent_before_mutation() {
  dui::RenderOwner owner;
  dui::RenderSliverFixedExtentList list{owner, 16.0};
  for (const double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
    bool rejected = false;
    try {
      list.set_item_extent(invalid);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected, "invalid fixed item extent was accepted");
    require(list.item_extent() == 16.0, "invalid item extent mutated the previous value");
  }
}

void fixed_extent_sliver_respects_fractional_boundaries() {
  dui::BuildOwner owner;
  const double scroll_boundary = 3.0 * 0.1;
  const double viewport_extent = 5.0 * 0.1 - scroll_boundary;
  owner.render(dui::Viewport{
    scroll_boundary, dui::SliverFixedExtentList{0.1, dui::Text{"A"}, dui::Text{"B"}, dui::Text{"C"},
                                                dui::Text{"D"}, dui::Text{"E"}, dui::Text{"F"}}});
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({1.0, viewport_extent})));
  auto* list = dynamic_cast<dui::RenderSliverFixedExtentList*>(
    owner.root()->render_object()->children().front());
  require(list->first_visible_index() == 3 && list->visible_child_count() == 2,
          "fractional item boundary selected the wrong visible range");
  require(list->children()[2]->layout_count() == 0,
          "fractional leading boundary laid out the previous item");
  require(list->children()[5]->layout_count() == 0,
          "fractional trailing boundary laid out the next item");
  require(owner.hit_test({0.5, 0.0}) == list->children()[3],
          "fractional leading boundary hit the previous item");
  require(owner.hit_test({0.5, 0.1}) == list->children()[4],
          "fractional item boundary hit the wrong item");

  owner.render(dui::Viewport{
    5.0 * 0.1, dui::SliverFixedExtentList{0.1, dui::Text{"A"}, dui::Text{"B"}, dui::Text{"C"},
                                          dui::Text{"D"}, dui::Text{"E"}, dui::Text{"F"}}});
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({1.0, 0.1})));
  require(list->first_visible_index() == 5 && list->visible_child_count() == 1,
          "computed double item boundary produced platform-dependent indexing");
  require(owner.hit_test({0.5, 0.0}) == list->children()[5],
          "computed double item boundary hit the previous item");

  dui::BuildOwner large_owner;
  large_owner.render(
    dui::Viewport{1.0e18 - 128.0,
                  dui::SliverFixedExtentList{1.0e18, dui::Text{"large A"}, dui::Text{"large B"}}});
  static_cast<void>(large_owner.frame(dui::BoxConstraints::tight({1.0, 1000.0})));
  auto* large_list = dynamic_cast<dui::RenderSliverFixedExtentList*>(
    large_owner.root()->render_object()->children().front());
  require(large_list->first_visible_index() == 0 && large_list->visible_child_count() == 2,
          "large-coordinate arithmetic erased a genuine visible range");

  dui::BuildOwner local_owner;
  local_owner.render(dui::Viewport{
    1.0e18, dui::SliverFixedExtentList{1.0e18, dui::Text{"before"}, dui::Text{"visible"}}});
  const auto local_frame = local_owner.frame(dui::BoxConstraints::tight({1.0, 1.0}));
  auto* local_list = dynamic_cast<dui::RenderSliverFixedExtentList*>(
    local_owner.root()->render_object()->children().front());
  require(local_list->first_visible_index() == 1 && local_list->visible_child_count() == 1,
          "large scroll offset absorbed a viewport-local paint extent");
  require(local_frame.dump().contains("text(\"visible\", 0.0, 0.0)"),
          "large scroll offset failed to paint local content");
  require(local_owner.hit_test({0.5, 0.5}) == local_list->children()[1],
          "large scroll offset absorbed a viewport-local hit coordinate");
}

void dirty_fixed_extent_sliver_rejects_stale_hit_range() {
  dui::RenderOwner owner;
  dui::RenderViewport viewport{owner, 16.0};
  dui::RenderSliverFixedExtentList list{owner, 16.0};
  dui::RenderText first{owner, "A"};
  dui::RenderText second{owner, "B"};
  dui::RenderText third{owner, "C"};
  std::array<dui::RenderObject*, 3> initial_children{&first, &second, &third};
  list.set_children(initial_children);
  std::array<dui::RenderObject*, 1> viewport_children{&list};
  viewport.set_children(viewport_children);
  std::array<dui::RenderObject*, 1> roots{&viewport};
  owner.set_roots(roots);
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  require(list.first_visible_index() == 1, "test did not establish a nonzero cached range");

  std::array<dui::RenderObject*, 1> shortened{&first};
  list.set_children(shortened);
  dui::HitTestResult result;
  require(!list.hit_test(result, {1.0, 1.0}), "dirty Sliver used a stale hit-test range");
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  require(list.visible_child_count() == 0, "shortened overscrolled list did not recover");
}

void lazy_fixed_extent_list_builds_only_visible_keyed_elements() {
  struct Item {
    int id;
  };

  std::vector<Item> items;
  for (int id = 0; id < 100; ++id) {
    items.push_back({id});
  }
  int builder_calls = 0;
  auto builder = [&](const Item& item) {
    ++builder_calls;
    return dui::Text{"item " + std::to_string(item.id)};
  };
  const auto make_view = [&](const std::vector<Item>& values, double offset) {
    return dui::Viewport{offset, dui::SliverFixedExtentList{
                                   16.0, dui::lazy_for_each(values, dui::key<&Item::id>, builder)}};
  };

  dui::BuildOwner owner;
  owner.render(make_view(items, 160.0));
  require(builder_calls == 0, "lazy Sliver invoked an item builder during root render");
  auto* list_element = owner.root()->children().front().get();
  require(list_element->children().empty(), "lazy Sliver eagerly mounted item Elements");

  const auto first = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(builder_calls == 2, "first lazy frame did not build exactly the visible range");
  require(list_element->children().size() == 2, "first lazy frame mounted the wrong item count");
  require(list_element->children()[0]->key() == dui::Key{std::int64_t{10}} &&
            list_element->children()[1]->key() == dui::Key{std::int64_t{11}},
          "deep initial scroll built the wrong keyed items");
  require(!first.dump().contains("item 0"), "deep initial scroll built or painted index zero");
  require(first.dump().contains("text(\"item 10\", 0.0, 0.0)") &&
            first.dump().contains("text(\"item 11\", 0.0, 16.0)"),
          "lazy first frame painted incomplete content");

  auto* viewport = dynamic_cast<dui::RenderViewport*>(owner.root()->render_object());
  auto* list = dynamic_cast<dui::RenderSliverFixedExtentList*>(viewport->children().front());
  require(list->logical_child_count() == 100 && list->first_mounted_index() == 10,
          "lazy RenderSliver lost logical or mounted range metadata");
  require(list->children().size() == 2, "render synchronization visited eager lazy-list children");
  require(list->children()[0]->paint_count() == 1 && list->children()[1]->paint_count() == 1,
          "lazy probe pass painted before child realization stabilized");

  const auto retained_id = list_element->children()[1]->id();
  const auto mounts = owner.mount_count();
  const auto unmounts = owner.unmount_count();
  owner.render(make_view(items, 176.0));
  require(builder_calls == 2, "lazy Sliver invoked item builders before layout requested a range");
  const auto second = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(builder_calls == 4, "scrolling did not build exactly the next visible range");
  require(list_element->children()[0]->key() == dui::Key{std::int64_t{11}} &&
            list_element->children()[0]->id() == retained_id,
          "overlapping lazy item did not retain keyed Element identity");
  require(list_element->children()[1]->key() == dui::Key{std::int64_t{12}},
          "scrolling mounted the wrong trailing item");
  require(owner.mount_count() == mounts + 1 && owner.unmount_count() == unmounts + 1,
          "lazy range transition did not mount and evict exactly one item");
  require(!second.dump().contains("item 10") && second.dump().contains("item 12"),
          "lazy range transition painted stale content");

  std::vector<Item> duplicate = items;
  duplicate[90].id = duplicate[91].id;
  const auto cached_first_id = list_element->children()[0]->id();
  const auto cached_second_id = list_element->children()[1]->id();
  const int calls_before_duplicate = builder_calls;
  bool rejected = false;
  try {
    owner.render(make_view(duplicate, 176.0));
  } catch (const std::logic_error&) {
    rejected = true;
  }
  require(rejected, "duplicate keys outside the lazy range were accepted");
  require(builder_calls == calls_before_duplicate,
          "duplicate-key validation invoked a lazy item builder");
  require(list_element->children()[0]->id() == cached_first_id &&
            list_element->children()[1]->id() == cached_second_id,
          "duplicate-key rejection mutated the mounted lazy range");
  const auto recovered = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(recovered.commands() == second.commands(),
          "duplicate-key rejection did not preserve the previous lazy frame");
}

void lazy_source_owns_temporary_data_and_preserves_eager_foreach() {
  struct Item {
    int id;
  };
  const auto item_builder = [](const Item& item) {
    return dui::Text{"owned " + std::to_string(item.id)};
  };

  dui::BuildOwner lazy_owner;
  lazy_owner.render(dui::Viewport{
    0.0,
    dui::SliverFixedExtentList{16.0, dui::lazy_for_each(std::initializer_list<Item>{{1}, {2}, {3}},
                                                        dui::key<&Item::id>, item_builder)}});
  const auto lazy_frame = lazy_owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(lazy_frame.dump().contains("owned 1") && lazy_frame.dump().contains("owned 2"),
          "lazy source did not retain an owned copy of temporary data");

  int eager_builds = 0;
  auto eager_builder = [&](const Item& item) {
    ++eager_builds;
    return dui::Text{"eager " + std::to_string(item.id)};
  };
  const std::vector<Item> eager_items{{1}, {2}, {3}};
  dui::BuildOwner eager_owner;
  eager_owner.render(dui::Viewport{
    0.0,
    dui::SliverFixedExtentList{16.0, dui::cache_extent(32.0),
                               dui::ForEach{eager_items, dui::key<&Item::id>, eager_builder}}});
  require(eager_builds == 3, "ordinary ForEach no longer preserved eager behavior");
  const auto eager_frame = eager_owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(eager_frame.dump().contains("eager 1") && eager_frame.dump().contains("eager 2"),
          "eager ForEach compatibility path produced incorrect output");
}

void lazy_fixed_extent_cache_retains_bounded_keyed_range() {
  struct Item {
    int id;
  };

  std::vector<Item> items;
  for (int id = 0; id < 100; ++id) {
    items.push_back({id});
  }
  int builder_calls = 0;
  auto builder = [&](const Item& item) {
    ++builder_calls;
    return dui::Text{"cached " + std::to_string(item.id)};
  };
  const auto make_view = [&](const std::vector<Item>& values, double offset, double cache) {
    return dui::Viewport{
      offset, dui::SliverFixedExtentList{16.0, dui::cache_extent(cache),
                                         dui::lazy_for_each(values, dui::key<&Item::id>, builder)}};
  };

  dui::BuildOwner owner;
  owner.render(make_view(items, 160.0, 16.0));
  const auto first = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  auto* list_element = owner.root()->children().front().get();
  auto* viewport = dynamic_cast<dui::RenderViewport*>(owner.root()->render_object());
  auto* list = dynamic_cast<dui::RenderSliverFixedExtentList*>(viewport->children().front());
  require(list->cache_extent() == 16.0 && list->first_mounted_index() == 9,
          "lazy Sliver lost its configured cache extent or leading cached index");
  require(builder_calls == 4 && list_element->children().size() == 4,
          "lazy Sliver did not mount the exact cache range");
  require(list_element->children()[0]->key() == dui::Key{std::int64_t{9}} &&
            list_element->children()[3]->key() == dui::Key{std::int64_t{12}},
          "lazy Sliver mounted incorrect exact-boundary cache items");
  require(first.dump().contains("cached 10") && first.dump().contains("cached 11") &&
            !first.dump().contains("cached 9") && !first.dump().contains("cached 12"),
          "cached but invisible items leaked into paint output");
  require(list->children()[0]->layout_count() == 0 && list->children()[0]->paint_count() == 0 &&
            list->children()[3]->layout_count() == 0 && list->children()[3]->paint_count() == 0,
          "cached but invisible items were laid out or painted");

  const auto promoted_id = list_element->children()[3]->id();
  auto* promoted_render = list_element->children()[3]->render_object();
  const auto mounts = owner.mount_count();
  const auto unmounts = owner.unmount_count();
  owner.render(make_view(items, 176.0, 16.0));
  const auto second = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list_element->children().front()->key() == dui::Key{std::int64_t{10}} &&
            list_element->children().back()->key() == dui::Key{std::int64_t{13}},
          "one-item scroll did not shift the cache range symmetrically");
  require(list_element->children()[2]->key() == dui::Key{std::int64_t{12}} &&
            list_element->children()[2]->id() == promoted_id &&
            list_element->children()[2]->render_object() == promoted_render,
          "cached item did not retain keyed identity when promoted to visible");
  require(owner.mount_count() == mounts + 1 && owner.unmount_count() == unmounts + 1,
          "one-item cache shift did not mount and evict exactly one Element");
  require(second.dump().contains("cached 11") && second.dump().contains("cached 12") &&
            !second.dump().contains("cached 10") && !second.dump().contains("cached 13"),
          "shifted cache range changed the visible paint range");
  require(owner.hit_test({1.0, 17.0}) == promoted_render,
          "promoted cached item did not participate in visible hit testing");

  const auto shrink_mounts = owner.mount_count();
  const auto shrink_unmounts = owner.unmount_count();
  owner.render(make_view(items, 176.0, 0.0));
  [[maybe_unused]] const auto shrunk = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list_element->children().size() == 2 && list->first_mounted_index() == 11 &&
            list_element->children()[1]->id() == promoted_id,
          "shrinking cache to zero did not preserve the visible keyed range");
  require(owner.mount_count() == shrink_mounts && owner.unmount_count() == shrink_unmounts + 2,
          "shrinking cache to zero did not evict exactly the cached Elements");

  const auto stable_first_id = list_element->children()[0]->id();
  const auto root_updates = owner.root()->update_count();
  const auto list_updates = list_element->update_count();
  for (const double invalid_cache : std::array{-1.0, std::numeric_limits<double>::infinity(),
                                               std::numeric_limits<double>::quiet_NaN()}) {
    bool invalid_rejected = false;
    try {
      owner.render(make_view(items, 0.0, invalid_cache));
    } catch (const std::invalid_argument&) {
      invalid_rejected = true;
    }
    require(invalid_rejected && viewport->scroll_offset() == 176.0 && list->cache_extent() == 0.0 &&
              list_element->children()[0]->id() == stable_first_id &&
              owner.root()->update_count() == root_updates &&
              list_element->update_count() == list_updates,
            "invalid cache extent mutated the retained lazy tree");
  }
  bool direct_invalid_rejected = false;
  try {
    list->set_cache_extent(std::numeric_limits<double>::infinity());
  } catch (const std::invalid_argument&) {
    direct_invalid_rejected = true;
  }
  require(direct_invalid_rejected && list->cache_extent() == 0.0,
          "RenderSliver accepted or committed an invalid cache extent");

  dui::BuildOwner empty_owner;
  bool empty_invalid_rejected = false;
  try {
    empty_owner.render(make_view(items, 0.0, -1.0));
  } catch (const std::invalid_argument&) {
    empty_invalid_rejected = true;
  }
  require(empty_invalid_rejected && empty_owner.root() == nullptr,
          "invalid initial cache extent left a partial Element tree");

  std::vector<Item> short_items{{0}, {1}, {2}, {3}, {4}};
  owner.render(make_view(short_items, 0.0, 16.0));
  [[maybe_unused]] const auto leading = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list_element->children().size() == 3 &&
            list_element->children().front()->key() == dui::Key{std::int64_t{0}} &&
            list_element->children().back()->key() == dui::Key{std::int64_t{2}},
          "leading cache range was not clamped to the list start");

  owner.render(make_view(short_items, 48.0, 16.0));
  [[maybe_unused]] const auto trailing = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list_element->children().size() == 3 && list->first_mounted_index() == 2 &&
            list_element->children().back()->key() == dui::Key{std::int64_t{4}},
          "trailing cache range was not clamped to the list end");

  owner.render(make_view(short_items, 88.0, 16.0));
  const auto near_overscroll = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->children().size() == 1 && list->first_mounted_index() == 4 &&
            list_element->children().front()->key() == dui::Key{std::int64_t{4}} &&
            near_overscroll.dump().find("cached") == std::string::npos,
          "near overscroll did not retain only the intersecting leading cache item");

  const auto overscroll_unmounts = owner.unmount_count();
  owner.render(make_view(short_items, 200.0, 16.0));
  const auto overscrolled = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list_element->children().empty() &&
            overscrolled.dump().find("cached") == std::string::npos,
          "far overscroll retained or painted an out-of-range cached item");
  require(owner.unmount_count() == overscroll_unmounts + 1,
          "far overscroll did not dispose the previous bounded cache range");

  owner.render(make_view({}, 0.0, 16.0));
  [[maybe_unused]] const auto empty = owner.frame(dui::BoxConstraints::tight({100.0, 32.0}));
  require(list->logical_child_count() == 0 && list_element->children().empty(),
          "empty lazy model produced a non-empty cache range");

  std::vector<Item> large_items{{0}, {1}};
  dui::BuildOwner precise_owner;
  precise_owner.render(dui::Viewport{
    1.0e18,
    dui::SliverFixedExtentList{1.0e18, dui::cache_extent(0.0),
                               dui::lazy_for_each(large_items, dui::key<&Item::id>, builder)}});
  const auto precise = precise_owner.frame(dui::BoxConstraints::tight({100.0, 1.0}));
  auto* precise_list_element = precise_owner.root()->children().front().get();
  require(precise_list_element->children().size() == 1 &&
            precise_list_element->children().front()->key() == dui::Key{std::int64_t{1}} &&
            precise.dump().contains("cached 1"),
          "large-coordinate cache arithmetic lost the visible item");

  const double huge_extent = std::numeric_limits<double>::max() / 8.0;
  std::vector<Item> huge_items{{0}, {1}, {2}, {3}, {4}, {5}, {6}, {7}};
  dui::BuildOwner overflow_owner;
  overflow_owner.render(dui::Viewport{
    huge_extent * 6.0,
    dui::SliverFixedExtentList{huge_extent, dui::cache_extent(huge_extent * 2.5),
                               dui::lazy_for_each(huge_items, dui::key<&Item::id>, builder)}});
  [[maybe_unused]] const auto overflow_safe =
    overflow_owner.frame(dui::BoxConstraints::tight({100.0, huge_extent / 2.0}));
  auto* overflow_list_element = overflow_owner.root()->children().front().get();
  require(overflow_list_element->children().size() == 5 &&
            overflow_list_element->children().front()->key() == dui::Key{std::int64_t{3}} &&
            overflow_list_element->children().back()->key() == dui::Key{std::int64_t{7}},
          "cache range overflow did not clamp to the logical list end");

  dui::BuildOwner fractional_owner;
  fractional_owner.render(make_view(items, 160.5, 0.75));
  const auto fractional = fractional_owner.frame(dui::BoxConstraints::tight({100.0, 31.0}));
  auto* fractional_list = fractional_owner.root()->children().front().get();
  require(fractional_list->children().size() == 4 &&
            fractional_list->children().front()->key() == dui::Key{std::int64_t{9}} &&
            fractional_list->children().back()->key() == dui::Key{std::int64_t{12}} &&
            !fractional.dump().contains("cached 9") && !fractional.dump().contains("cached 12"),
          "fractional cache boundaries mounted or painted the wrong items");
}

void lazy_keep_alive_preserves_state_outside_the_cache_range() {
  struct Item {
    int id;
  };

  std::vector<Item> items{{0}, {1}, {2}, {3}, {4}};
  std::vector<std::optional<dui::StateHandle<int>>> handles(items.size());
  std::vector<const KeepAliveResource*> resources(items.size());
  int resource_constructions = 0;
  int resource_destructions = 0;
  dui::Signal<int> keep_alive_signal{0};
  int key_events = 0;
  bool keep_enabled = true;
  int policy_calls = 0;
  int builder_calls = 0;
  auto builder = [&](const Item& item) {
    ++builder_calls;
    return KeepAliveStateItem{item.id,
                              &handles,
                              &resources,
                              &resource_constructions,
                              &resource_destructions,
                              &keep_alive_signal,
                              &key_events};
  };
  auto policy = [&](const Item& item) {
    ++policy_calls;
    return keep_enabled && (item.id == 1 || item.id == 3);
  };
  const auto make_view = [&](const std::vector<Item>& values, double offset) {
    auto source = dui::lazy_for_each(values, dui::key<&Item::id>, builder).keep_alive_when(policy);
    return dui::Viewport{
      offset, dui::SliverFixedExtentList{16.0, dui::cache_extent(0.0), std::move(source)}};
  };

  dui::BuildOwner owner;
  owner.render(make_view(items, 16.0));
  require(policy_calls == 5 && builder_calls == 0,
          "keep-alive policy or item builder ran in the wrong model phase");
  const auto first = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  auto* list_element = owner.root()->children().front().get();
  auto* viewport = dynamic_cast<dui::RenderViewport*>(owner.root()->render_object());
  auto* list = dynamic_cast<dui::RenderSliverFixedExtentList*>(viewport->children().front());
  auto* item_element = list_element->children().front().get();
  const auto retained_id = item_element->id();
  auto* retained_render = item_element->children().front()->render_object();
  auto* retained_hit_render = item_element->children().front()->children().front()->render_object();
  const auto* retained_resource = resources[1];
  const auto focused_id = owner.focused_node()->id();
  require(first.dump().contains("keep-alive 1:10") && handles[1].has_value() &&
            resource_constructions == 1 && resource_destructions == 0,
          "initial keep-alive item did not mount with state");

  handles[1]->set(41);
  const auto updated = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(updated.dump().contains("keep-alive 1:41"),
          "active keep-alive item did not rebuild its state");

  const auto unmounts_before_keep = owner.unmount_count();
  owner.render(make_view(items, 32.0));
  const auto second = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->kept_alive_child_count() == 1 && list_element->children().size() == 1 &&
            list_element->children().front()->key() == dui::Key{std::int64_t{2}} &&
            list->children().size() == 1 && owner.unmount_count() == unmounts_before_keep,
          "policy-selected item was unmounted or attached to the active RenderSliver");
  require(resource_constructions == 2 && resource_destructions == 0,
          "dormant keep-alive resource was reconstructed or destroyed");
  require(!second.dump().contains("keep-alive 1"),
          "dormant keep-alive item leaked into paint output");
  require(list_element->kept_alive_child_count() == 1 && builder_calls == 2 &&
            !handles[3].has_value(),
          "never-realized policy-selected item entered the keep-alive bucket");
  const auto dormant_snapshot = owner.inspect();
  const auto* inspected_list = dormant_snapshot.find(list_element->id());
  require(inspected_list != nullptr && inspected_list->render_object.has_value() &&
            inspected_list->render_object->protocol == dui::InspectorRenderProtocol::sliver &&
            inspected_list->children.size() == 2 && inspected_list->children[0].key == "2" &&
            inspected_list->children[0].state == dui::InspectorElementState::active &&
            inspected_list->children[1].key == "1" &&
            inspected_list->children[1].state == dui::InspectorElementState::dormant_keep_alive &&
            !inspected_list->children[1].children.empty() &&
            inspected_list->children[1].children.front().state ==
              dui::InspectorElementState::dormant_keep_alive,
          "structured inspector omitted or reordered a dormant keep-alive subtree");

  handles[1]->set(42);
  keep_alive_signal.set(7);
  require(owner.pending_build_count() == 1,
          "dormant keep-alive dependency did not receive invalidation");
  const auto dormant_update = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(handles[1]->get() == 42 && !dormant_update.dump().contains("keep-alive 1"),
          "dormant state was discarded or painted while detached");
  require(owner.focused_node() != nullptr && owner.focused_node()->id() == focused_id &&
            owner.dispatch_key({"A", dui::KeyPhase::down}) == dui::KeyEventResult::handled &&
            key_events == 1 && dormant_snapshot.find(retained_id) != nullptr &&
            dormant_snapshot.find(retained_id)->state ==
              dui::InspectorElementState::dormant_keep_alive,
          "dormant keep-alive item lost focused key dispatch");

  std::vector<Item> duplicate = items;
  duplicate.back().id = 3;
  const auto calls_before_duplicate = policy_calls;
  const auto root_updates_before_invalid = owner.root()->update_count();
  bool duplicate_rejected = false;
  try {
    owner.render(make_view(duplicate, 0.0));
  } catch (const std::logic_error&) {
    duplicate_rejected = true;
  }
  require(duplicate_rejected && policy_calls == calls_before_duplicate &&
            list_element->kept_alive_child_count() == 1 && viewport->scroll_offset() == 32.0 &&
            owner.root()->update_count() == root_updates_before_invalid,
          "duplicate keys invoked keep-alive policy or mutated dormant Elements");

  const auto different_builder = [](const Item& item) {
    return dui::Text{"invalid " + std::to_string(item.id)};
  };
  bool different_source_rejected = false;
  try {
    auto source = dui::lazy_for_each(duplicate, dui::key<&Item::id>, different_builder)
                    .keep_alive_when([](const Item&) { return true; });
    owner.render(dui::Viewport{0.0, dui::SliverFixedExtentList{16.0, std::move(source)}});
  } catch (const std::logic_error&) {
    different_source_rejected = true;
  }
  require(different_source_rejected &&
            owner.root()->update_count() == root_updates_before_invalid &&
            list_element->kept_alive_child_count() == 1,
          "invalid different-type lazy source replaced the retained tree");

  bool throwing_policy_rejected = false;
  try {
    auto source =
      dui::lazy_for_each(items, dui::key<&Item::id>, different_builder)
        .keep_alive_when([](const Item&) -> bool { throw std::runtime_error("policy failed"); });
    owner.render(dui::Viewport{0.0, dui::SliverFixedExtentList{16.0, std::move(source)}});
  } catch (const std::runtime_error&) {
    throwing_policy_rejected = true;
  }
  require(throwing_policy_rejected && owner.root()->update_count() == root_updates_before_invalid &&
            list_element->kept_alive_child_count() == 1,
          "throwing keep-alive policy mutated the retained tree");

  owner.render(make_view(items, 16.0));
  const auto restored = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  item_element = list_element->children().front().get();
  require(item_element->id() == retained_id &&
            item_element->children().front()->render_object() == retained_render &&
            resources[1] == retained_resource && restored.dump().contains("keep-alive 1:42:7") &&
            list_element->kept_alive_child_count() == 0 && owner.focused_node() != nullptr &&
            owner.focused_node()->id() == focused_id,
          "restored keep-alive item lost Element, RenderObject, or state identity");
  require(owner.hit_test({1.0, 1.0}) == retained_hit_render,
          "restored keep-alive item did not rejoin hit testing");

  owner.render(make_view(items, 32.0));
  [[maybe_unused]] const auto kept_again = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->kept_alive_child_count() == 1,
          "restored item did not return to the keep-alive bucket");
  const auto destructions_before_policy_cancel = resource_destructions;
  keep_enabled = false;
  owner.render(make_view(items, 32.0));
  bool cancelled_handle_rejected = false;
  try {
    static_cast<void>(handles[1]->get());
  } catch (const std::logic_error&) {
    cancelled_handle_rejected = true;
  }
  require(cancelled_handle_rejected && list_element->kept_alive_child_count() == 0 &&
            resource_destructions == destructions_before_policy_cancel + 1 &&
            owner.focused_node() == nullptr,
          "policy cancellation did not immediately unmount dormant state and resources");
  keep_alive_signal.set(8);
  require(owner.pending_build_count() == 0,
          "purged dormant keep-alive item remained subscribed to its dependency");
  [[maybe_unused]] const auto policy_cancelled =
    owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));

  keep_enabled = true;
  owner.render(make_view(items, 16.0));
  [[maybe_unused]] const auto recreated = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto recreated_id = list_element->children().front()->id();
  owner.render(make_view(items, 32.0));
  [[maybe_unused]] const auto retained_for_deletion =
    owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  auto deleted_handle = *handles[1];
  const auto destructions_before_deletion = resource_destructions;
  std::erase_if(items, [](const Item& item) { return item.id == 1; });
  owner.render(make_view(items, 16.0));
  bool deleted_handle_rejected = false;
  try {
    static_cast<void>(deleted_handle.get());
  } catch (const std::logic_error&) {
    deleted_handle_rejected = true;
  }
  require(deleted_handle_rejected && list_element->kept_alive_child_count() == 0 &&
            resource_destructions == destructions_before_deletion + 1,
          "model deletion did not immediately unmount dormant state and resources");
  [[maybe_unused]] const auto deleted = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->children().front()->id() != recreated_id,
          "model deletion restored a stale keep-alive Element");

  std::optional<dui::StateHandle<int>> destruction_handle;
  int owner_resource_constructions = 0;
  int owner_resource_destructions = 0;
  {
    std::vector<Item> destruction_items{{0}, {1}, {2}};
    std::vector<std::optional<dui::StateHandle<int>>> destruction_handles(destruction_items.size());
    std::vector<const KeepAliveResource*> destruction_resources(destruction_items.size());
    int destruction_key_events = 0;
    dui::Signal<int> destruction_signal{0};
    auto destruction_builder = [&](const Item& item) {
      return KeepAliveStateItem{item.id,
                                &destruction_handles,
                                &destruction_resources,
                                &owner_resource_constructions,
                                &owner_resource_destructions,
                                &destruction_signal,
                                &destruction_key_events};
    };
    auto destruction_policy = [](const Item& item) { return item.id == 1; };
    const auto destruction_view = [&](double offset) {
      auto source = dui::lazy_for_each(destruction_items, dui::key<&Item::id>, destruction_builder)
                      .keep_alive_when(destruction_policy);
      return dui::Viewport{offset, dui::SliverFixedExtentList{16.0, std::move(source)}};
    };
    dui::BuildOwner destruction_owner;
    destruction_owner.render(destruction_view(16.0));
    [[maybe_unused]] const auto destruction_active =
      destruction_owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
    destruction_handle = destruction_handles[1];
    destruction_owner.render(destruction_view(32.0));
    [[maybe_unused]] const auto destruction_dormant =
      destruction_owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
    require(destruction_owner.root()->children().front()->kept_alive_child_count() == 1,
            "owner destruction test did not create a dormant Element");
  }
  bool destruction_handle_rejected = false;
  try {
    static_cast<void>(destruction_handle->get());
  } catch (const std::logic_error&) {
    destruction_handle_rejected = true;
  }
  require(destruction_handle_rejected && owner_resource_constructions == 2 &&
            owner_resource_destructions == 2,
          "BuildOwner destruction did not unmount dormant state and resources");
}

void lazy_keep_alive_builder_failure_preserves_retry_state() {
  struct Item {
    int id;
  };
  const std::vector<Item> items{{0}, {1}, {2}, {3}};
  bool fail = false;
  auto builder = [&](const Item& item) {
    if (fail && item.id == 3) {
      throw std::runtime_error("lazy builder failed");
    }
    return dui::Text{"retry " + std::to_string(item.id)};
  };
  const auto make_view = [&](double offset) {
    auto source = dui::lazy_for_each(items, dui::key<&Item::id>, builder)
                    .keep_alive_limit(2)
                    .keep_alive_when([](const Item&) { return true; });
    return dui::Viewport{offset, dui::SliverFixedExtentList{16.0, std::move(source)}};
  };

  dui::BuildOwner owner;
  owner.render(make_view(0.0));
  [[maybe_unused]] const auto initial = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  auto* list_element = owner.root()->children().front().get();
  const auto zero_id = list_element->children().front()->id();
  owner.render(make_view(16.0));
  [[maybe_unused]] const auto item_one = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto one_id = list_element->children().front()->id();
  owner.render(make_view(32.0));
  [[maybe_unused]] const auto established = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto two_id = list_element->children().front()->id();
  auto* two_render = list_element->children().front()->render_object();
  require(list_element->kept_alive_child_count() == 2,
          "builder-failure test did not establish an ordered dormant LRU queue");

  fail = true;
  owner.render(make_view(48.0));
  bool failed = false;
  try {
    static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 16.0})));
  } catch (const std::runtime_error&) {
    failed = true;
  }
  require(failed && list_element->children().front()->id() == two_id &&
            list_element->children().front()->render_object() == two_render &&
            list_element->kept_alive_child_count() == 2,
          "lazy builder failure mutated active ownership or the prior LRU queue");

  fail = false;
  owner.render(make_view(48.0));
  const auto retried = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(retried.dump().contains("retry 3") && list_element->kept_alive_child_count() == 2,
          "lazy builder failure did not recover on a valid retry");
  owner.render(make_view(16.0));
  [[maybe_unused]] const auto restored_one = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->children().front()->id() == one_id,
          "builder failure or retry reordered the prior LRU queue");
  owner.render(make_view(0.0));
  [[maybe_unused]] const auto recreated_zero =
    owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->children().front()->id() != zero_id,
          "post-retry budget did not evict the original oldest entry");
}

void lazy_keep_alive_limit_evicts_oldest_dormant_elements() {
  struct Item {
    int id;
  };
  const std::vector<Item> items{{0}, {1}, {2}, {3}, {4}};
  std::vector<std::optional<dui::StateHandle<int>>> handles(items.size());
  int resource_constructions = 0;
  int resource_cancellations = 0;
  int resource_destructions = 0;
  dui::Signal<int> budget_signal{0};
  auto builder = [&](const Item& item) {
    return BudgetStateItem{item.id,
                           &handles,
                           &resource_constructions,
                           &resource_cancellations,
                           &resource_destructions,
                           &budget_signal};
  };
  const auto make_view = [&](double offset, std::size_t limit) {
    auto source = dui::lazy_for_each(items, dui::key<&Item::id>, builder)
                    .keep_alive_limit(limit)
                    .keep_alive_when([](const Item&) { return true; });
    return dui::Viewport{offset, dui::SliverFixedExtentList{16.0, std::move(source)}};
  };

  dui::BuildOwner owner;
  owner.render(make_view(0.0, 2));
  [[maybe_unused]] const auto item_zero = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  auto* list_element = owner.root()->children().front().get();
  const auto zero_id = list_element->children().front()->id();
  require(owner.focused_node() != nullptr && resource_constructions == 1,
          "budget lifecycle test did not establish focus and resource state");

  owner.render(make_view(16.0, 2));
  [[maybe_unused]] const auto item_one = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto one_id = list_element->children().front()->id();
  owner.render(make_view(32.0, 2));
  [[maybe_unused]] const auto item_two = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  const auto two_id = list_element->children().front()->id();
  require(list_element->kept_alive_child_count() == 2,
          "keep-alive limit did not retain the allowed dormant count");

  const auto unmounts_before_first_eviction = owner.unmount_count();
  owner.render(make_view(48.0, 2));
  [[maybe_unused]] const auto item_three = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->kept_alive_child_count() == 2 &&
            owner.unmount_count() > unmounts_before_first_eviction,
          "keep-alive limit did not evict the oldest dormant subtree");
  bool zero_handle_rejected = false;
  try {
    static_cast<void>(handles[0]->get());
  } catch (const std::logic_error&) {
    zero_handle_rejected = true;
  }
  budget_signal.set(1);
  require(zero_handle_rejected && resource_cancellations == 1 && resource_destructions == 1 &&
            owner.focused_node() == nullptr && owner.pending_build_count() == 0,
          "budget eviction did not fully unmount the oldest dormant subtree");

  owner.render(make_view(16.0, 2));
  [[maybe_unused]] const auto restored_one = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->children().front()->id() == one_id,
          "budgeted keep-alive did not restore retained Element identity");
  owner.render(make_view(64.0, 2));
  [[maybe_unused]] const auto item_four = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  owner.render(make_view(32.0, 2));
  [[maybe_unused]] const auto recreated_two =
    owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->children().front()->id() != two_id,
          "least-recently-used dormant Element survived a newer re-eviction");
  owner.render(make_view(16.0, 2));
  [[maybe_unused]] const auto restored_one_again =
    owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->children().front()->id() == one_id &&
            list_element->children().front()->id() != zero_id,
          "restored then re-evicted Element did not become most recent");

  const auto unmounts_before_shrink = owner.unmount_count();
  owner.render(make_view(16.0, 1));
  require(list_element->kept_alive_child_count() == 1 &&
            owner.unmount_count() > unmounts_before_shrink,
          "shrinking keep-alive limit did not synchronously evict oldest excess state");
  bool four_handle_rejected = false;
  try {
    static_cast<void>(handles[4]->get());
  } catch (const std::logic_error&) {
    four_handle_rejected = true;
  }
  require(four_handle_rejected, "limit shrink left the oldest dormant StateHandle valid");
  owner.render(make_view(16.0, 3));
  require(list_element->kept_alive_child_count() == 1,
          "increasing keep-alive limit resurrected an evicted Element");
  owner.render(make_view(48.0, 3));
  [[maybe_unused]] const auto refill_three = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  owner.render(make_view(64.0, 3));
  [[maybe_unused]] const auto refill_four = owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  require(list_element->kept_alive_child_count() == 3,
          "larger keep-alive limit did not admit new dormant Elements");
  const auto unmounts_before_zero = owner.unmount_count();
  owner.render(make_view(64.0, 0));
  require(list_element->kept_alive_child_count() == 0 &&
            owner.unmount_count() > unmounts_before_zero,
          "zero keep-alive limit did not synchronously evict multiple dormant Elements");
  bool two_handle_rejected = false;
  try {
    static_cast<void>(handles[2]->get());
  } catch (const std::logic_error&) {
    two_handle_rejected = true;
  }
  require(two_handle_rejected, "zero keep-alive limit left dormant state valid");
  owner.render(make_view(16.0, 3));
  require(list_element->kept_alive_child_count() == 0,
          "post-zero limit growth resurrected an evicted Element");

  const auto unlimited_view = [&](double offset) {
    auto source = dui::lazy_for_each(items, dui::key<&Item::id>, [](const Item& item) {
                    return dui::Text{"unlimited " + std::to_string(item.id)};
                  }).keep_alive_when([](const Item&) { return true; });
    return dui::Viewport{offset, dui::SliverFixedExtentList{16.0, std::move(source)}};
  };
  dui::BuildOwner unlimited_owner;
  for (std::size_t index = 0; index < 4; ++index) {
    unlimited_owner.render(unlimited_view(static_cast<double>(index) * 16.0));
    [[maybe_unused]] const auto frame =
      unlimited_owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  }
  require(unlimited_owner.root()->children().front()->kept_alive_child_count() == 3,
          "omitted keep-alive limit changed unbounded compatibility behavior");

  const auto cached_view = [&](double offset) {
    auto source = dui::lazy_for_each(items, dui::key<&Item::id>,
                                     [](const Item& item) {
                                       return dui::Text{"cached budget " + std::to_string(item.id)};
                                     })
                    .keep_alive_when([](const Item&) { return true; })
                    .keep_alive_limit(1);
    return dui::Viewport{
      offset, dui::SliverFixedExtentList{16.0, dui::cache_extent(16.0), std::move(source)}};
  };
  dui::BuildOwner cached_owner;
  for (std::size_t index = 1; index < 4; ++index) {
    cached_owner.render(cached_view(static_cast<double>(index) * 16.0));
    [[maybe_unused]] const auto frame =
      cached_owner.frame(dui::BoxConstraints::tight({100.0, 16.0}));
  }
  const auto* cached_list = cached_owner.root()->children().front().get();
  require(cached_list->children().size() == 3 && cached_list->kept_alive_child_count() == 1,
          "active cache-range children incorrectly consumed the dormant keep-alive limit");
}

void nested_boundary_recomposes_without_repainting_outer_content() {
  dui::BuildOwner owner;
  const auto make_view = [](std::uint32_t color) {
    return dui::repaint_boundary(dui::background(
      dui::repaint_boundary(dui::background(dui::Text{"nested"}, color)), 0xff112233u));
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
  require(outer_color->paint_count() == outer_color_paints,
          "outer content repainted during recomposition");
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
  require(result.target() == stack->children()[1],
          "Stack did not hit the last-painted child first");
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
  require(display.dump().starts_with("rect(0.0, 0.0, 40.0, 16.0, 0xffffffff)"),
          "new color was not painted");
}

void declarative_gesture_detector_bubbles_taps() {
  dui::BuildOwner owner;
  std::vector<std::string> taps;
  owner.render(dui::on_tap(dui::on_tap(dui::Text{"tap"}, [&] { taps.push_back("inner"); }),
                           [&] { taps.push_back("outer"); }));
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

  owner.dispatch_pointer({9, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
  require(taps.empty(), "GestureDetector activated before pointer up");
  owner.dispatch_pointer({9, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
  require(taps == std::vector<std::string>{"inner", "outer"},
          "declarative GestureDetector did not bubble in leaf-to-root order");
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
  owner.render(dui::on_tap(dui::padding(dui::Text{"A"}, dui::Insets::all(10.0)), [&] { ++taps; }));
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

  owner.dispatch_pointer({15, dui::BuildOwner::PointerPhase::down, {11.0, 11.0}});
  owner.dispatch_pointer({15, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
  require(taps == 1, "tap was rejected while pointer remained inside GestureDetector");
}

void declarative_focus_view_requests_focus_and_routes_key() {
  dui::BuildOwner owner;
  std::vector<std::string> keys;
  owner.render(dui::focusable(dui::focusable(dui::Text{"focus"},
                                             [&](const dui::KeyEvent&) {
                                               keys.push_back("inner");
                                               return dui::KeyEventResult::handled;
                                             }),
                              [&](const dui::KeyEvent&) {
                                keys.push_back("outer");
                                return dui::KeyEventResult::handled;
                              }));
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

  owner.dispatch_pointer({10, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
  owner.dispatch_pointer({10, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
  require(owner.focused_node() != nullptr, "FocusView did not request focus on activation");
  require(owner.dispatch_key({"Enter", dui::KeyPhase::down}) == dui::KeyEventResult::handled,
          "focused declarative View did not handle key");
  require(keys == std::vector<std::string>{"inner"},
          "outer FocusView replaced the deeper focus target");
}

void gesture_callback_can_replace_itself_and_reenter_pointer_dispatch() {
  dui::BuildOwner owner;
  std::vector<std::string> taps;
  owner.render(dui::on_tap(
    dui::on_tap(dui::Text{"tap"},
                [&] {
                  taps.push_back("inner");
                  owner.dispatch_pointer({11, dui::BuildOwner::PointerPhase::cancel, {}});
                  owner.render(dui::Text{"replacement"});
                }),
    [&] { taps.push_back("outer"); }));
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));

  owner.dispatch_pointer({11, dui::BuildOwner::PointerPhase::down, {1.0, 1.0}});
  owner.dispatch_pointer({11, dui::BuildOwner::PointerPhase::up, {1.0, 1.0}});
  require(taps == std::vector<std::string>{"inner"},
          "stale gesture ancestor ran after tree replacement");
}

void autofocus_is_not_reapplied_during_reconciliation() {
  dui::BuildOwner owner;
  std::vector<std::string> keys;
  auto make_view = [&] {
    return dui::HStack{dui::focusable(
                         dui::Text{"A"},
                         [&](const dui::KeyEvent&) {
                           keys.push_back("A");
                           return dui::KeyEventResult::handled;
                         },
                         true),
                       dui::focusable(dui::Text{"B"}, [&](const dui::KeyEvent&) {
                         keys.push_back("B");
                         return dui::KeyEventResult::handled;
                       })};
  };
  owner.render(make_view());
  static_cast<void>(owner.frame(dui::BoxConstraints::tight({100.0, 100.0})));
  owner.dispatch_pointer({12, dui::BuildOwner::PointerPhase::down, {9.0, 1.0}});
  owner.dispatch_pointer({12, dui::BuildOwner::PointerPhase::up, {9.0, 1.0}});

  owner.render(make_view());
  require(owner.dispatch_key({"Enter", dui::KeyPhase::down}) == dui::KeyEventResult::handled,
          "focused sibling did not handle key after reconciliation");
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
    fixed_extent_sliver_virtualizes_layout_paint_and_hit_testing();
    fixed_extent_sliver_rejects_invalid_extent_before_mutation();
    fixed_extent_sliver_respects_fractional_boundaries();
    dirty_fixed_extent_sliver_rejects_stale_hit_range();
    lazy_fixed_extent_list_builds_only_visible_keyed_elements();
    lazy_source_owns_temporary_data_and_preserves_eager_foreach();
    lazy_fixed_extent_cache_retains_bounded_keyed_range();
    lazy_keep_alive_preserves_state_outside_the_cache_range();
    lazy_keep_alive_builder_failure_preserves_retry_state();
    lazy_keep_alive_limit_evicts_oldest_dormant_elements();
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
