# DUI

DUI is an experimental C++ declarative UI framework based on Flutter's
Widget/Element/RenderObject architecture. It combines short-lived, statically
typed view descriptions with a persistent runtime element tree.

The current implementation is intentionally headless. It validates build,
identity, reconciliation, state, dependency tracking, box and Sliver layout,
painting, hit testing, pointer activation, and a platform-neutral `HostWindow`
contract before introducing a production graphics backend or native host.

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

`BuildOwner::inspect()` captures a detached structured snapshot for tooling
without flushing or framing the tree. It includes active and dormant Elements,
identity and lifecycle metadata, safe state/dependency/resource counts, focus
ownership, and RenderObject protocol/dirty/counter metadata. Snapshots support
stable-ID lookup and canonical JSON serialization while retaining no framework
or native pointers. State remains private unless declared with an explicit
inspection formatter; opted-in values are cached as detached strings, refreshed
on state updates, and can be revoked by returning to the ordinary state API.

An optional fixed-capacity `TimelineRecorder` captures correlated UI and raster
spans for reconciliation, frame production, layout stabilization, retained-layer
composition, surface acquisition, rasterization, and presentation. Callers can
inject a monotonic clock for deterministic profiling and safely snapshot or
clear the recorder across those threads. Detached snapshots serialize to
versioned, canonical, locale-independent JSON without reducing nanosecond
timestamps to floating point. Recorded `LayerTree` values carry a weak,
non-owning provenance token so matching raster work shares a cross-lane flow ID
without conflating UI frame IDs with raster ticket IDs. Detached snapshots can
also export Chrome/Perfetto Trace Event JSON with UI/raster tracks, complete
spans, exact decimal microsecond timestamps, and visual cross-thread flow links.
Tooling can poll bounded completion-order batches through recorder-local weak
cursors; overwrite and clear gaps are reported exactly without retaining the
recorder or invoking callbacks under its lock.

`ToolingReport` combines detached inspector and timeline snapshots into one
deterministic, responsive HTML file for offline diagnosis. The report shows the
active/dormant Element hierarchy, opted-in state and RenderObject metadata, and
retained UI/raster events without scripts, remote assets, native handles, or
live runtime dependencies. Dynamic strings use the canonical JSON Unicode rules
before HTML escaping, and a restrictive Content Security Policy prevents them
from becoming markup.

```sh
./build/debug/dui_tooling_report > build/debug/tooling-report.html
```

Opt-in restoration supports replacement-owner hot restart for booleans,
integers, and strings. Restoration IDs are caller-owned and globally unique;
restored values are available during the replacement tree's first build. The
snapshot contains detached tagged values, not Elements, callbacks, `std::any`,
RTTI, resources, or native handles.

```cpp
auto count = context.restorable_state<"count">(0, "app.counter");

auto saved = owner.save_restoration_state();
dui::BuildOwner replacement;
replacement.restore_state(std::move(saved));
replacement.render(App{});
```

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
lazy-cache, and dormant keep-alive content. A conditional Win32 UI Automation
adapter exposes the tree through `WM_GETOBJECT`, including stable providers,
navigation, properties, keyboard-focus state, bounds, and asynchronous Invoke
and focus action routing. Native Windows runtime verification and adapters for
other platforms remain in progress.
Visible `Text`, labeled `Image`, `GestureDetector`, and `FocusView` primitives
also contribute semantics without requiring an explicit wrapper; image asset
identifiers are never used as accessibility labels.
`SemanticsDiffer` flattens successive snapshots into deterministic add, update,
move, and child-first removal records for future native accessibility adapters.
`AccessibilityBridge` delivers those records through a platform-neutral adapter
contract and acknowledges snapshots only after successful delivery, allowing
failed native updates to retry without losing state.
It also exposes owned, generation-tagged prepare/acknowledge/reject transactions
for upcoming host delivery across separate platform and UI executors.
The H0 host coordinator now uses those transactions for optional accessibility
services, including coalesced desired trees, explicit failed-batch retry,
generation-safe adapter replacement and actions, and platform-affine release.
It also exposes an existing-API-compatible text-input proxy that marshals public
sessions, full editing values, geometry, native callbacks, backend replacement,
and shutdown across the UI and platform executors without exposing native
session IDs.
Generation-bearing asynchronous clipboard reads/writes and coalesced system
cursor commands complete the H0 service contract. Replacement cancels old
clipboard requests, reapplies the retained cursor, rejects stale callbacks, and
keeps backend release on the platform executor.

The native roadmap targets Linux, Android, macOS, iOS, and Windows through one
shared host contract. Linux/Wayland is the first executable reference integration;
Android uses a thin Activity/View and JNI bridge, Apple hosts use separate
AppKit/UIKit adapters over shared C++ and Metal seams, and Windows integrates the
existing IMM32/UI Automation work into the same lifecycle. Build-only,
simulator/compositor, and physical-device verification are reported separately;
see [RFC 0002](docs/rfcs/0002-cross-platform-hosts.md).

The implemented H0 contract uses separate platform and UI `TaskRunner`s, weak
generation-checked delegates, validated metrics/input/surface events, coalesced
frame demand, and executor-affine orderly shutdown. Its deterministic fake-host
tests do not claim an H1 native build or H2 native runtime. The earlier
`Backend`/`NativeView` API remains temporarily for existing raster tests and is
not an alternative native lifecycle contract.

The Linux P1a slice adds a platform-neutral Wayland surface state engine for
initial bufferless configuration, coalesced `xdg_surface` serials, integer and
fractional scaling plans, recoverable buffer preparation, and one outstanding
frame callback. This is H0 protocol-state coverage only. A real libwayland,
xkbcommon, `wl_shm`, and headless-Weston integration remains P1b and is not
claimed by builds that lack those SDK/runtime dependencies.
When `wayland-client`, `wayland-protocols`, and `wayland-scanner` are installed,
CMake also enables the H1 `dui::wayland_native` connection target and generates
its protocol bindings from the installed XML. Set
`DUI_REQUIRE_WAYLAND_NATIVE=ON` when absence of that target must fail configure.
This connection/registry/link slice does not yet claim an xdg window or Weston
runtime verification.
P1b.2a adds one xdg toplevel and checked memfd-backed XRGB8888 diagnostic buffers
driven by the shared Wayland state engine. Its compositor test covers initial
configure, coalesced demand producing one frame callback, and `HostWindow`
shutdown notification under headless Weston; this remains a diagnostic surface
rather than a production renderer.
P1b.2b tracks stable `wl_output` objects and applies the maximum entered-output
integer scale, or compositor fractional scale through a per-window viewport
when both optional protocols are advertised. Scale changes repaint the
diagnostic buffer and publish matching physical metrics and device-pixel ratio.
P1b.3a adds seat capability tracking and a primary-button `wl_pointer` adapter.
Its platform-neutral state tests cover complete and canceled pointer streams;
native event H2 remains pending a compositor-side input injector.

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Use the `asan` preset for AddressSanitizer and UndefinedBehaviorSanitizer, or
the `tsan` preset for ThreadSanitizer.

The core requires C++23. C++26 static reflection will be an optional metadata
and tooling enhancement, not a requirement for view composition.
