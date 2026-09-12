# DUI

DUI is an experimental C++ declarative UI framework based on Flutter's
Widget/Element/RenderObject architecture. It combines short-lived, statically
typed view descriptions with a persistent runtime element tree.

The current implementation is intentionally headless. It validates build,
identity, reconciliation, state, dependency tracking, box and Sliver layout,
painting, hit testing, pointer activation, and the NativeView/Renderer boundary
before introducing a production graphics backend.

Frames are represented by immutable retained `LayerTree` snapshots. Declarative
`RepaintBoundary` Views isolate paint work, while the compatibility renderer
flattens retained layers into the deterministic DisplayList used by tests. An
owned raster worker submits immutable snapshots asynchronously and provides
quiescent stop/join semantics. Raster submission captures surface metrics and
returns a shared completion ticket for presentation, retry, cancellation, or
terminal failure.

A vertical `Viewport` is the explicit box-to-Sliver boundary. It accepts
`SliverToBoxAdapter` children, computes scroll and paint geometry, clips retained
output, and preserves hit-test coordinates while scrolling.
`SliverFixedExtentList` uses
constant-time visible-index calculation and limits layout, paint traversal, and
render-tree hit testing to the visible range. Its current declarative children
can use `lazy_for_each` to snapshot model data while constructing, reconciling,
and synchronizing only the visible/cache keyed Elements. An explicit
`cache_extent(...)` argument retains items intersecting that many logical pixels
before and after the viewport while keeping them out of layout, paint, and hit
testing until visible. An owning lazy source can additionally use
`keep_alive_when(predicate)` to retain selected realized Element subtrees after
they leave that bounded range; dormant entries preserve state, resources,
dependencies, and focus identity without remaining attached to the RenderSliver.
`keep_alive_limit(count)` optionally bounds that dormant bucket with
least-recently-used eviction; active and cache-range children do not consume the
limit, and omitting it preserves unbounded policy-selected retention.
Ordinary `ForEach` remains the explicit eager path.

```cpp
struct Counter {
  auto build(dui::BuildContext& context) const {
    auto count = context.state<"count">(0);

    return dui::VStack{
      dui::Text{"Count: " + std::to_string(count.get())},
      dui::Text{"Increment",
                [count]() mutable { count.update([](int value) { return value + 1; }); }},
    };
  }
};
```

See [RFC 0001](docs/rfcs/0001-cpp-declarative-ui-architecture.md) for the
architecture and implementation roadmap.

Implemented layout Views include `VStack`, `HStack`, `Stack`, `Padding`,
`ColoredBox`, `Viewport`, and `SliverToBoxAdapter`; primitives include `Text`
and `Image`. `SliverFixedExtentList` provides the first scrolling-list protocol.
A deterministic DisplayList serves as the test renderer.

The interaction foundation includes typed Environment values, Signals, named
RAII resources, cancellation, GestureArena, focus/key routing, IME contracts,
tickers, animation controllers, declarative gesture/focus Views, and lazy
Task coroutines with a deterministic executor. Declarative taps are arbitrated
through GestureArena, and a conditional Win32 IMM32 adapter provides native
text input. Native Windows IME verification and adapters for other platforms
remain in progress.

Explicit `semantics(...)` wrappers produce immutable platform-neutral semantics
trees with stable IDs, roles, labels, values, actions, and globally clipped
bounds. Collection follows visible paint ranges, excluding hidden, clipped,
lazy-cache, and dormant keep-alive content. Native accessibility bridges such as
macOS Accessibility and Windows UI Automation are not implemented yet.
Visible `Text`, labeled `Image`, `GestureDetector`, and `FocusView` primitives
also contribute semantics without requiring an explicit wrapper; image asset
identifiers are never used as accessibility labels.
`SemanticsDiffer` flattens successive snapshots into deterministic add, update,
move, and child-first removal records for future native accessibility adapters.

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Use the `asan` preset for AddressSanitizer and UndefinedBehaviorSanitizer.

The core requires C++23. C++26 static reflection will be an optional metadata
and tooling enhancement, not a requirement for view composition.
