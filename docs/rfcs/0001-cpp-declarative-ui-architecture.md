# RFC 0001: C++ Declarative UI Architecture

- Status: Accepted for prototype
- Target: DUI 0.1
- Last updated: 2026-09-10

## Summary

DUI will use Flutter's three-tree architecture while adapting its public model
to C++ value semantics. A component's explicit `build(BuildContext&)` function
produces a short-lived, statically typed View description. Reconciliation
updates a persistent Element tree, which owns identity, state, dependencies,
and lifecycle. RenderObject nodes perform layout, painting, hit testing, and
semantics. Backend and Renderer interfaces isolate the framework from window
systems and graphics implementations.

The design takes the following ideas:

| Source | Adopted idea | Deliberate difference |
| --- | --- | --- |
| Flutter | Widget/Element/RenderObject, constraints, keys, dirty phases | Views are values; no GC object per widget |
| Shaft | Backend/Renderer boundary, multiple roots, observed builds | No ARC graph or unchecked existential casts |
| SwiftUI | Static composition, modifiers, environment | No compiler-only result builder or wrapper explosion |
| Compose | Read dependency tracking and local values | State identity is named, not positional |
| React | Unidirectional updates, batching, priorities, boundaries | No universal allocated VDOM or hook ordering |
| ArkUI | Explicit component build and state-driven UI | No mandatory source annotation transform |

## Scope

The project will provide:

- declarative View values and explicit component build functions;
- persistent Element identity and reconciliation;
- typed local state and observable dependency tracking;
- constrained box and sliver layout protocols;
- retained layers and display-list painting;
- input, gestures, focus, text input, and semantics;
- platform and renderer abstractions;
- headless testing and developer inspection;
- selective type erasure for runtime and ABI boundaries.

The initial prototype excludes production graphics, mobile embedding, hot code
reload, accessibility platform bridges, and binary-stable plugins.

## Flutter Engine relationship

Flutter's stable embedder API starts and hosts an engine whose framework is a
Dart isolate. It does not expose a supported entry point for an external C++
framework to submit Flutter framework RenderObjects or engine LayerTrees.
Depending directly on engine-internal Shell, Rasterizer, DisplayList, or layer
types would force DUI to track a Flutter Engine fork.

DUI therefore ports the framework architecture but defines independent
`Backend`, `NativeView`, `Renderer`, `LayerTree`, and `DisplayList` contracts.
Skia and Impeller can be renderer implementations. A Flutter Engine adapter is
permitted as an experimental backend, but engine-private APIs cannot leak into
the framework core.

## Architecture

```text
Component::build(BuildContext&)
              |
              v
ephemeral, typed View description
              |
              v reconcile
persistent Element tree
              |
              v update
persistent RenderObject tree
              |
              v paint
LayerTree and DisplayList
              |
              v
Renderer and platform Backend
```

### View

A View is an immutable or logically immutable value describing configuration.
Static child groups store heterogeneous children in tuples. `Optional<V>` and
`Choice<L, R>` model runtime control flow. `ForEach` retains a range, key
extractor, and child factory and is traversed directly during reconciliation.

Views normally have automatic storage duration and disappear after reconcile.
They must not own persistent framework state.

### Element

An Element is persistent and heterogeneous. It owns child Elements, state
slots, observed dependencies, lifecycle, and the bridge to a RenderObject. A
static operation table per View type provides mount/update/debug behavior
without requiring every public View to inherit a virtual base.

An existing Element is reusable when its parent slot, View type token, and Key
match. Static children use structural positions. Dynamic collections use
explicit keys; index identity is an opt-in fallback. Global subtree-moving keys
are deferred until a concrete use case justifies their complexity.

### RenderObject

RenderObjects are persistent runtime-polymorphic objects. They implement
constraints-based layout, painting, hit testing, parent data, semantics, and
dirty propagation. Widget-to-render-object configuration remains statically
associated so updates do not require unchecked downcasts.

The rendering pipeline has separate build, layout, compositing, paint, and
semantics queues. Relayout and repaint boundaries constrain invalidation.

## Public composition API

```cpp
struct TodoPage {
  std::vector<Todo> todos;

  auto build(dui::BuildContext& context) const {
    auto filter = context.state<"filter">(Filter::all);

    return dui::VStack{
      FilterBar{filter.get(), [filter](Filter next) mutable { filter.set(next); }},
      dui::ForEach{todos, dui::key<&Todo::id>, [](const Todo& todo) { return TodoRow{todo}; }},
      dui::optional(todos.empty(), [] { return EmptyState{"No todos"}; })};
  }
};
```

Normal control flow is represented explicitly because standard C++ reflection
cannot inspect and rewrite statements in a function body. Compile-time control
flow may use ordinary `if constexpr`. Runtime heterogeneous branches use
`Choice`, and runtime collections use `ForEach`.

## State and reactivity

Local state is stored in the component Element and addressed by a compile-time
name:

```cpp
auto count = context.state<"count">(0);
```

Named slots avoid React hook-order constraints and survive source insertions
that do not rename the state. A StateHandle contains owner, Element ID, slot,
and generation. It never owns an Element. Access after unmount is rejected.

Observable reads during build register the current Element as a dependent.
Writes invalidate only live dependents. Every rebuild clears and recreates its
read set, allowing conditional dependencies. Updates are transactionally
batched and flushed in Element depth order.

The UI and Element trees are confined to a UI executor. Workers return values
through messages. Coroutine tasks are attached to Element lifetime through
cancellation tokens and RAII.

## Ownership

- Build-time Views are values.
- Parents uniquely own child Elements.
- Elements uniquely own or reference their RenderObjects according to render
  tree attachment rules.
- Parent pointers are non-owning.
- State and callback handles use IDs plus generations, not owning pointers.
- Frame-temporary allocations use PMR arenas.
- `shared_ptr` is not the default ownership mechanism.

The prototype may use standard-library allocation while preserving these
semantics. Arena and small-buffer optimizations follow profiling.

## Type erasure

Static composition is the default. `AnyView`, `AnyComponent`, and backend ABI
handles are explicit escape hatches for routes, plugins, scripting, and compile
isolation. Owning erasure should use small-buffer storage when practical. The
persistent Element tree is necessarily heterogeneous and uses one indirect
operation-table dispatch per updated node.

## Modifiers

Property modifiers such as text color are fused into configuration values and
do not create Elements. Structural modifiers such as padding, clipping,
gestures, and repaint boundaries create explicit Views only when they affect
layout, hit testing, lifecycle, or painting. Adjacent compatible modifiers are
normalized to limit template depth.

## Backend and threads

The framework defines platform-neutral Backend and Renderer contracts. A
typical host uses Platform, UI, Raster, and worker threads. The UI thread builds,
reconciles, and lays out. It submits an immutable LayerTree without waiting for
the raster thread. The Platform thread handles windows and native input. Image
decoding and application work use workers.

Multiple NativeViews and RenderView roots are supported structurally from the
first renderer milestone.

## Input and semantics

Pointer routing, hit testing, gesture arbitration, focus, keyboard shortcuts,
IME, text editing, drag and drop, and semantics are framework-level protocols.
Semantics is a render pipeline phase rather than an optional widget-library
feature, so platform accessibility can be added without changing RenderObject
contracts.

A platform text-input adapter connects a real host window to an operating
system text service; a synthetic or terminal-only backend does not satisfy this
requirement. One conditionally built desktop implementation is sufficient for
M2, and operating-system SDK headers and system libraries are allowed. The
adapter must support committed Unicode input, active composition updates,
composition commit and cancellation, selection replacement, action dispatch,
and candidate/composition positioning. Each adapter is bound to one native
view and has one active, generation-addressed session. Stale session operations
are ignored. Editable rectangles are in logical client coordinates and are
converted using the native view's device-pixel ratio. Adapter operations and
client callbacks are confined to the window-owning thread, and exceptions must
not cross the native event-procedure boundary.

## Reflection

C++26 static reflection is an optional metadata facility. It can generate
component inspection, property schemas, debug descriptions, simple opt-in
property comparison, serialization adapters, and test snapshots. It does not
implement build syntax because C++26 reflection cannot rewrite function bodies.

The core remains usable on a C++23 compiler through explicit traits. Reflected
compiler names are not stable IDs. Persistence, plugins, and hot reload require
explicit versioned IDs.

## Hot development

The first implementation supports hot restart with serializable state restore.
Native hot reload requires dynamic component modules with a stable C ABI,
module generations, callback quiescence, task cancellation, and state migration.
It is a later milestone. LLVM ORC or compiler-specific JIT integration cannot
be a prerequisite for the runtime architecture.

## Error handling

Exceptions cannot escape the frame loop or cross plugin/backend C boundaries.
Component error boundaries convert build failures into fallback Views. Render
and destructor paths are `noexcept` where possible. Builds with exceptions
disabled will use the same boundaries with explicit error values.

## Milestones

### M0: Headless architecture

- View concepts and static composition
- persistent Element tree
- positional and keyed reconciliation
- Optional, Choice, and ForEach
- named state slots
- observable invalidation
- depth-ordered dirty rebuild
- tree inspection and deterministic tests

### M1: Box rendering

- Backend, NativeView, and Renderer interfaces
- RenderBox constraints and parent data
- Row, Column, Stack, Padding, Text, and Image
- hit testing and pointer routing
- software/test display list

M1 is accepted when headless tests demonstrate:

- invalid constraints and padding are rejected before corrupting layout state;
- Row, Column, Stack, Padding, Text, Image, and background boxes produce
  deterministic sizes, offsets, and display commands;
- equivalent View updates preserve RenderObjects and perform no layout or paint;
- geometry changes propagate layout dirtiness while paint-only changes avoid
  layout;
- transparent Elements flatten into the render tree and unmount without stale
  pointers;
- overlapping children hit-test in reverse paint order and produce an ancestor
  path;
- pointer activation is suppressed when its down target is removed;
- NativeView and Renderer contracts can submit a complete DisplayList;
- malformed render trees and either owner/object destruction order are safe.

### M2: Interaction

- gestures, focus, keyboard, IME, and text editing
- environment values and inherited dependency tracking
- coroutine resources and cancellation
- animation controller and ticker

M2 foundation acceptance requires headless tests demonstrating:

- typed Environment lookup selects the nearest provider and only invalidates
  subscribed equal components;
- equal components retain the latest descriptor even when build is skipped;
- state writes during build remain queued for a follow-up build;
- root render/flush reentrancy is rejected before an executing component
  descriptor can be replaced;
- GestureArena enrollment remains open until close, resolves one winner, and
  rejects same-pointer recreation during resolution callbacks;
- focus routes keys from leaf to ancestor, safely snapshots mutable handlers,
  reparents around removed nodes, and rejects cross-manager relationships;
- IME sessions retain clients weakly and validate UTF-8 byte range boundaries;
- named non-copyable resources survive rebuild and are destroyed on unmount;
- Element unmount requests cancellation for scoped asynchronous work before
  destroying any sibling resource;
- deterministic tickers and animation controllers stop at their bounds.

M2 completion additionally requires the public GestureDetector/View bridge, a
coroutine Task/executor abstraction, focus ownership by Elements, and at least
one platform text-input adapter. Completion tests must demonstrate that the
declarative tap recognizer enrolls from pointer down until up/cancel, preserves
nested leaf-to-root activation after arena acceptance, suppresses canceled or
removed targets, and remains safe under callback-driven tree replacement. The
platform adapter must be compiled and exercised on its target OS with committed
Unicode input, composition commit/cancel, stale sessions, expired clients, and
candidate positioning.

### M3: Production renderer

- protocol-neutral `RenderObject`, with constraints, geometry, parent data,
  rectangular hit testing, and child layout owned by `RenderBox` rather than
  the universal base;
- explicit parent/child render-protocol compatibility, rejected before render
  tree mutation;
- raster thread and immutable LayerTree submission;
- Skia or Impeller backend;
- repaint boundaries and retained layers;
- Sliver protocol and virtualized scrolling;
- semantics tree and platform accessibility adapters.

M3 begins with a protocol-separation slice. Acceptance requires all existing
box layout, painting, hit testing, and pointer behavior to remain unchanged;
bare RenderObjects must expose no box constraints, size, offset, or box parent
data; and a RenderBox must transactionally reject a child from an incompatible
render protocol. The frame root remains a RenderView/RenderBox. A later explicit
viewport adapter is the box-to-sliver boundary.

The retained-rendering slice then introduces immutable LayerTree snapshots,
ordered layer composition, declarative repaint boundaries, paint invalidation
stopping at the nearest boundary, composition-only ancestor updates, and
DisplayList flattening for compatibility. Old submitted snapshots must remain
valid after RenderObject unmount, clean boundaries must retain layer identity,
and changing one boundary must not repaint clean ancestors or siblings.

The initial Sliver slice introduces validated `SliverConstraints` and
`SliverGeometry`, protocol-specific parent data and layout caching, and a
vertical `RenderViewport` as the explicit box-to-Sliver adapter. A
`RenderSliverToBoxAdapter` bridges back to one box child. Viewport clipping must
remain represented in immutable LayerTrees and compatibility DisplayLists;
paint and hit testing must exclude fully offscreen Slivers and preserve local
coordinates for partially visible content. Cross-protocol child adoption and a
multi-box update to the single-box adapter must fail before tree mutation.
Declarative updates must retain Viewport and Sliver RenderObject identity, and
scrolling a repaint-boundary box must recompose its offset without repainting
its boundary-local content. This slice establishes the protocol but does not
complete virtualized scrolling: M3 still requires a lazy child manager that
constructs and retains only the visible/cache range rather than eagerly
materializing every Element through `ForEach`.

The next virtualization slice adds a fixed-extent Sliver list. Given a positive
finite item extent, it must compute the first and trailing visible indices
without laying out preceding children; only the visible contiguous range may be
laid out, traversed for paint, or considered for hit testing. Exact item and
viewport boundaries use half-open intervals, overscroll produces an empty
visible range, total scroll-extent overflow is rejected, and changing the item
extent invalidates layout without replacing the RenderSliver. This is
layout/paint virtualization only: eager declarative child reconciliation remains
explicitly outside the completion claim until a lazy child manager exists.

The first lazy child-manager slice uses an explicit `lazy_for_each` source under
`SliverFixedExtentList`; ordinary `ForEach` retains eager semantics. The lazy
source takes an owning vector snapshot before `render()` returns, validates all
logical keys before committing a model revision, and invokes no item builder
until layout requests a logical range. Each item must produce exactly one
top-level box RenderObject so logical and mounted child indices remain defined.
Render layout publishes a passive range request, BuildOwner realizes it only
after the layout stack unwinds, then repeats synchronization and layout before a
single final paint. A bounded stabilization loop must reject reentrant or
non-converging realization deterministically. Initial acceptance requires deep
first-frame scrolling to avoid building index zero, visible-range-only Element
mounting, keyed identity retention across overlapping ranges, immediate
disposal outside the requested range, temporary-source lifetime safety, and
offscreen duplicate-key rejection before any item builder invocation. This
slice uses zero extra cache; configurable cache extent and keep-alive lifecycle
remain required before declaring cached virtual scrolling complete.

The cache-range slice adds an explicit `cache_extent` option to
`SliverFixedExtentList`. It is a finite, non-negative count of logical pixels
applied symmetrically before and after the visible scroll interval, clamped to
the list's scroll extent. The requested half-open child range contains every
item whose interval intersects that cache window; the default remains zero.
Cached Elements and RenderObjects stay mounted and retain keyed identity, but
only the visible subrange may be laid out, painted, or hit tested. Scrolling or
changing the cache extent must reconcile the entire requested range before the
frame's only paint and synchronously unmount every item outside it. Invalid
cache extents must fail before mutating the retained tree. Acceptance covers
leading and trailing clamping, exact item boundaries, cached-item promotion to
visible without a new mount, symmetric one-item range shifts, cache shrink to
zero, empty lists, overscroll, and unchanged eager-list behavior. This is a
bounded viewport cache rather than an indefinite keep-alive bucket; policy-based
retention outside the cache window remains a separate future extension.

The policy keep-alive slice adds `keep_alive_when(predicate)` to an immutable
owning lazy source. Source construction materializes and validates keys before
the predicate is evaluated into a retained-key snapshot; both steps finish
before the source can be passed to retained-tree reconciliation.
Only an item that has already been realized may enter the keep-alive bucket.
When it leaves the cache range and its current model item satisfies the policy,
its keyed Element subtree remains mounted in BuildOwner but is detached from
the Sliver's active child sequence. Dormant subtrees preserve state, resources,
dependencies, and focus identity, while render-tree synchronization, lazy range
discovery, layout, paint, and hit testing must not traverse them. If the key
re-enters the cache range with the same child type, normal reconciliation moves
the exact Element and RenderObject subtree back to the active sequence before
paint. A key removed from the model, rejected by a changed policy, or replaced
through a different lazy source type must be synchronously unmounted, including
resource cancellation and stale StateHandle invalidation. The default policy
retains nothing, preserving the bounded-cache behavior. Acceptance covers state
and RenderObject identity through eviction/restoration, dormant paint and hit
test exclusion, policy cancellation, model deletion, never-realized items,
owner destruction, duplicate-key transactionality, and no behavior change for
ordinary eager `ForEach`.

Raster submission uses one owned Renderer on one worker thread and queues only
immutable LayerTree snapshots. Submission is thread-safe and FIFO. Stop is
non-blocking, rejects future submissions, cancels queued frames, and permits the
active render to finish; destruction is externally serialized and joins the
worker before returning. A renderer failure is terminal, cancels pending work,
and is reported to the owner thread. Tests must cover ordering, thread affinity,
immutable snapshot lifetime, callback-requested stop, asynchronous failure,
quiescent destruction, and thread-race instrumentation where available.

A raster-facing surface is separate from NativeView. Acquisition captures
logical size, integer physical extent, DPR, and a resize generation, and returns
ready, temporarily unavailable, out-of-date, or lost. A ready RGBA8888 sRGB
premultiplied frame maps one validated pixel span and is an exactly-once
transaction: it is either presented or abandoned. Presentation is synchronous
with respect to mapped CPU memory; mapped spans must not be retained after the
transaction completes. Invalid requests and request/frame generation or extent
mismatches are rejected before rendering. This CPU surface seam is preparation
for a real Skia SkSurface consumer and is not itself a production renderer.

Each accepted raster submission returns a copyable completion ticket. Repeated
or concurrent waiters observe the same presented, unavailable, out-of-date, or
canceled outcome; terminal acquisition, rendering, and present failures are
propagated through the ticket. The raster worker, not the owner thread, creates,
uses, and destroys the backend-specific SurfaceRenderer. Temporarily unavailable
and out-of-date frames are observable non-terminal results so the frame
coordinator can schedule retry with current metrics.

### M4: Tooling and platforms

- inspector and timeline
- hot restart and state restoration
- optional reflection metadata
- stable plugin ABI experiments
- Android, iOS, Windows, macOS, and Linux hosts

## Prototype acceptance criteria

M0 is accepted when headless tests demonstrate:

- a component preserves named state across rebuilds;
- replacing a View type replaces its Element;
- keyed list reorder preserves item Element identity;
- removing an optional child unmounts it;
- switching a Choice replaces only the active branch;
- an observed value invalidates only subscribed components;
- multiple writes coalesce before a flush;
- tree inspection is deterministic.

## Consequences

The design accepts a small runtime Element abstraction and explicit control-flow
combinators in exchange for predictable identity, normal C++ compilation, and
no mandatory code generator. Static descriptions reduce transient allocation,
while selective erasure prevents template expansion from becoming an ABI and
build-time liability. Renderer independence costs an additional interface but
avoids coupling the framework's future to Flutter Engine internals.
