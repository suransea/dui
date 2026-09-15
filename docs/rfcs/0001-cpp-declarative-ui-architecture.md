# RFC 0001: C++ Declarative UI Architecture

- Status: Accepted for prototype
- Target: DUI 0.1
- Last updated: 2026-09-15

## Executable Examples

Examples are small headless embedding programs, not alternate test suites or
mock native applications. Every example must use only public `dui` headers,
build with `DUI_BUILD_EXAMPLES=ON`, run without platform SDKs or network access,
and emit deterministic text suitable for inspection from a terminal. An example
must demonstrate a complete user-facing concept rather than duplicate a unit-
test fixture.

The initial example set covers four independent paths: local state plus external
Signal and Environment dependencies; semantics activation plus focus/key
routing; visible-range lazy Sliver realization across scrolling; and detached
restoration into a replacement `BuildOwner`. Existing counter and tooling-report
programs remain the minimal state and offline-inspection introductions. Example
executables are smoke-run during verification, while behavioral edge cases stay
in the corresponding test executables.

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
reload, and binary-stable plugins.

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

The first semantics slice exposes an explicit `semantics(child, properties,
on_activate)` box wrapper and produces an immutable, platform-neutral
`SemanticsTree` after a completed frame. A node contains its stable RenderObject
ID, role, label, value, enabled state, supported actions, clipped global bounds,
and ordered semantic children. Unannotated RenderObjects are transparent and
promote semantic descendants to the nearest annotated ancestor. Hidden nodes
exclude their complete subtree. Collection follows each RenderObject's paint
offset, clip, and visible child range, so fully clipped content, offscreen Sliver
children, lazy cache entries, and dormant keep-alive Elements are absent. Bounds
use half-open rectangle intersection and partially visible nodes expose only the
clipped rectangle. Semantic activation resolves the current RenderObject by ID,
revalidates that the node is enabled and supports activation, copies the
callback before invocation, and returns false for stale, hidden, disabled, or
unsupported nodes. Property-only updates retain node identity and require no
layout or paint. Acceptance covers transparent and nested hierarchy, deterministic
ordering, hidden subtree exclusion, viewport clipping, lazy scrolling, stable
updates, enabled/disabled activation, callback-driven mutation, stale IDs, and
queries before the first completed frame. Platform accessibility adapters remain
later slices.

The automatic primitive-semantics slice makes visible non-empty `Text` produce
a text node whose label is its displayed value. `Image` accepts an optional
semantic label and produces an image node only when that label is non-empty;
asset identifiers are never exposed as accessibility text. `GestureDetector`
and `FocusView` produce button-role containers around their semantic
descendants. A gesture container is enabled while it has a callback; a focus
container is enabled only while its FocusNode can accept focus. Disabled
containers expose no activation action. Explicit `semantics(...)` remains an
ordinary semantic container, so inferred descendants preserve deterministic
hierarchy rather than being silently merged or discarded. Semantic activation
reports whether a supported callback was dispatched, independently of the
pointer-routing boolean that controls event propagation. Property-only image
label and callback-presence updates retain RenderObject identity and do not
dirty layout or paint. Acceptance covers primitive hierarchy and roles,
unlabeled-image exclusion, action dispatch, disabled focus, explicit/inferred
nesting, stable property updates, clipping, and lazy visible-range exclusion.

The accessibility-update slice adds a platform-neutral `SemanticsDiffer` between
immutable trees and native adapters. Each flattened `SemanticsEntry` contains a
nonzero stable node ID, optional parent ID, sibling index, role, label, value,
enabled state, clipped bounds, and actions. The first snapshot emits every node
as added in parent-first preorder. Later snapshots emit new nodes parent-first,
then changed or reparented existing nodes in new preorder, then removed nodes in
reverse old preorder so children are removed before parents. A change carries
the complete new entry for add/update and the complete old entry for removal.
Equivalent snapshots emit nothing; callback identity is intentionally absent,
so replacing a callback without changing supported actions is not an
accessibility update. Clearing emits the same deterministic child-first removals.
Zero or duplicate IDs reject an update before replacing the retained flattened
snapshot. Allocation or validation failure likewise preserves the previous
snapshot. This protocol does not call native APIs or retain RenderObjects; UIA,
macOS Accessibility, AT-SPI, and mobile adapters remain separate consumers.
Acceptance covers first publication, property/action changes, sibling reorder,
reparenting into a new parent, subtree add/remove ordering, no-op snapshots,
clear, malformed-tree transactionality, and snapshots produced by BuildOwner.

The adapter-delivery slice adds caller-owned `AccessibilityAdapter` and stateful
`AccessibilityBridge` contracts without selecting a native API. `publish(tree,
adapter)` computes changes against a copy of the acknowledged snapshot, skips
the adapter for a no-op, and commits the new snapshot only after `apply()`
returns successfully. `clear(adapter)` follows the same rule for child-first
removals. If an adapter throws, the exception returns to the host boundary and
the bridge retains its previous acknowledged snapshot so the same complete
delta can be retried. The change span passed to `apply()` is callback-scoped and
must not be retained. The adapter and input tree remain caller-owned; the bridge
stores no RenderObject, BuildOwner, callback, or native provider pointer.
Publishing or clearing the same bridge from inside its adapter callback is
rejected before diffing, preventing nested acknowledgment races. A bridge is
noncopyable, nonmovable, and UI-thread confined. Adapter code may destroy the
delivering bridge; a method-local lifetime token completes success or exception
handling without accessing the destroyed object. Action routing
continues through `BuildOwner::perform_semantics_action`; native event-procedure
exception translation remains the platform adapter's responsibility.
Acceptance covers first delivery, no-op suppression, property delivery,
failed-delivery retry with identical changes, failed and successful clear,
reentrant publish/clear rejection, callback-driven bridge destruction, malformed
tree rejection before adapter invocation, and BuildOwner snapshot delivery.

This synchronous bridge requires adapter application on its UI thread. An RFC
0002 host with separate platform and UI executors adds an asynchronous publisher:
it copies an owned delta, posts native application to the platform executor, and
posts a generation-tagged success or failure acknowledgment back to the UI
executor. Only that acknowledgment advances the bridge snapshot; one publication
per window is in flight, failure retries from the last acknowledged tree, and
window/delegate generation changes discard stale completions. Synchronous native
provider queries continue to read the adapter's retained platform-side snapshot
without entering the bridge or `BuildOwner`.

The first native accessibility consumer is a conditional Win32 UI Automation
adapter bound to one `HWND`. Because a semantics snapshot may contain multiple
roots, it presents one synthetic fragment root without consuming a semantic ID
and attaches every semantic root beneath it. Stable 64-bit semantic IDs form
stable UIA runtime and automation IDs. Providers resolve entries from the live
adapter model on every call, so retained COM providers report element-not-
available after removal rather than exposing stale properties. Roles map to UIA
group, text, image, and button control types; labels, read-only values, enabled
state, logical client bounds, parent/sibling/child navigation, deterministic
reverse-order point lookup, and the Invoke pattern are exposed. Invoke posts a
private window message and returns immediately; the host callback then posts a
generation-checked `BuildOwner::perform_semantics_action` request to the UI
executor. It may run directly only when the platform and UI executors are
explicitly co-located and the native provider callback has unwound. Semantic
focus is not fabricated before the platform-neutral tree exposes focus state.
Delivered batches validate and replace the native model transactionally, then
invalidate the root's child structure.
The host forwards `WM_GETOBJECT` to the adapter before `DefWindowProcW`; all COM
and window-procedure entry points translate C++ exceptions. UIA/COM provider
queries may run on non-window threads and read the synchronized retained model.
The RFC 0002 host must marshal HWND mutation, adapter publication, and native
action capture to the window executor; framework action execution is UI-
executor-affine. The current H1 adapter's synthetic-root `SetFocus()` still
calls the HWND directly and must be marshaled before Windows H2 verification.
Bounds
are supplied in logical client coordinates and converted to physical screen
coordinates with a device-pixel ratio that the host updates after DPI changes.
Provider state is synchronized for UIA calls, and teardown disconnects the root
provider and drops pending actions and host callback captures. Acceptance
requires warning-free Win32 compilation, transactional malformed-update tests,
message filtering tests, and target-OS verification of provider navigation,
properties, Invoke routing, bounds, and UIA event observation.

The semantic-focus slice adds `focusable` and `focused` state independently of
generic enabled state, plus a `focus` semantics action. A node exposes that
action only while it is focusable, enabled, and backed by a live RenderObject
focus handler; explicit properties alone cannot manufacture an actionable focus
target. `FocusView` reports whether its Element-owned `FocusNode` is the current
owner, and focus changes are observable in a new semantics snapshot without
dirtying layout or paint. Action dispatch revalidates the current tree before
requesting focus, preserving the same stale, hidden, clipped, lazy-cache, and
dormant exclusions as activation. The Win32 adapter maps the state to
`IsKeyboardFocusable` and `HasKeyboardFocus`, resolves fragment-root `GetFocus`,
and marshals provider `SetFocus` through its private HWND action message before
requesting native window focus and posting framework focus to the UI executor.
`HasKeyboardFocus` and `GetFocus`
also require actual keyboard focus within the active HWND. Focus events are
deferred until synchronous `WM_SETFOCUS` handling and framework publication
settle, use an adapter-lifetime cookie to reject stale posted messages, and call
UIA outside the model lock. Acceptance covers focus action authorization, focus
transfer and clearing, differ updates that retain stable IDs, callback-driven
tree mutation, and warning-clean Win32 compilation. Native UIA focus-event
interoperability remains target-OS acceptance work.

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
retained native editing snapshots are confined to the window-owning thread.
Generation-checked editing updates and actions are posted to the UI executor
unless both executors are explicitly co-located; exceptions must not cross the
native event-procedure boundary.

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

The budgeted keep-alive slice adds an explicit `keep_alive_limit(count)` to an
owning lazy source. Omitting the limit preserves the policy-selected unbounded
dormant bucket; a limit of zero disables dormant retention without changing the
policy snapshot. The budget counts only detached dormant Elements, never active
or cache-range children. Dormant ownership is ordered from least to most
recently evicted. Restoring a key removes it from that queue, and if it later
leaves the active/cache range it becomes most recent. When an insertion exceeds
the limit, the oldest dormant subtree is synchronously unmounted before frame
completion. Model deletion and policy rejection run before budget enforcement;
shrinking a limit during `render()` immediately unmounts oldest excess entries,
including resource cancellation, dependency removal, focus fallback, and stale
StateHandle invalidation. Increasing a limit never resurrects an evicted
subtree. Key validation, policy evaluation, and lazy item building must still
finish before any ownership mutation, so their failures preserve the prior LRU
queue. Acceptance covers deterministic multi-item eviction, restoration
recency, zero/shrinking/increasing limits, unlimited compatibility, immediate
lifecycle cleanup, retry after builder failure, and unchanged eager `ForEach`.

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

Native host work follows [RFC 0002](0002-cross-platform-hosts.md): one shared
host contract, explicit per-platform adapters, verification tiers that separate
headless tests, SDK builds, automated native execution, and physical/manual
interoperability, and a Linux reference host before further Windows-only
expansion. Android, macOS, iOS, and Windows remain first-class planned hosts and
may advance in parallel whenever suitable SDK runners are available.

M4 begins with a platform-neutral structured inspector. `BuildOwner::inspect()`
returns an owned value snapshot containing Element ID and generation, depth,
name, debug value, key, update and dirty state, state/dependency/environment/
resource counts, focus-node ownership, active versus dormant keep-alive state,
and optional RenderObject identity, protocol, dirty flags, repaint-boundary
status, and layout/paint counters. Active children retain reconciliation order;
dormant lazy children follow in their deterministic oldest-to-newest retention
order, and the dormant marker propagates through their descendants. Arbitrary
`std::any` state, environment values, resources, callbacks, framework pointers,
and native handles are never retained or serialized. Capture is passive: it
does not flush builds, perform layout, paint, or invoke user code, and rejects
capture during reconciliation or framing rather than exposing a partial tree.
The detached snapshot remains valid after updates or owner destruction, supports
tree-wide stable-ID lookup, and has canonical JSON serialization with fixed
field order, locale-independent numbers, complete control-character escaping,
preserved valid UTF-8, and deterministic `U+FFFD` replacement for malformed
bytes. Capture and serialization reject nesting at the documented 512-node
depth bound with `length_error`; lookup uses an iterative traversal. Empty-owner,
active-tree, dirty-tree, keyed, focus, RenderObject, dormant keep-alive,
snapshot-lifetime, lookup, escaping, and deterministic repeated-capture cases
form acceptance. This inspector slice excludes timeline events, live transport,
state-value opt-in, and a GUI frontend; timeline collection begins in the
following slice.

The first timeline slice adds an opt-in, platform-neutral `TimelineRecorder` to
`BuildOwner`. A caller supplies a positive fixed event capacity and may supply a
monotonic `TimelineClock`; the default clock uses steady time. With no recorder
attached, reconciliation and frame production perform no clock reads or event
storage. Each completed event owns its sequence and span IDs, optional parent
span and frame IDs, UI lane, phase, completed or failed outcome, start time,
non-negative duration, pass index, and phase-specific work count. Initial phases
are root reconciliation, dirty build flushing, complete frame production,
render-tree synchronization, each layout stabilization pass, each productive
lazy realization pass, and final retained-layer composition. Frame work counts
productive realization rounds; build records the pending dirty count at entry;
layout and composition record their pending queue counts at entry.

Spans are assigned sequence IDs at entry and snapshots return retained events in
that order even though nested spans complete before their parents. Scope exit
records failure without replacing the operation's original exception, and a
later operation remains recordable. The recorder retains the most recently
completed events in a preallocated ring, increments an exact dropped-event count
on overflow, and allocates no storage while finishing a span. `snapshot()`
returns detached values; `clear()` removes retained events and resets the drop
count without reusing sequence, span, or frame IDs. Recorder attachment changes
are rejected during reconciliation or framing so one operation cannot split
across recorders. Recorder mutation and owner instrumentation are UI-thread
confined in this slice; raster-lane collection, cross-thread snapshotting, live
transport, and a GUI frontend remain later work. Canonical serialization begins
in the following slice.

Timeline acceptance covers deterministic fake-clock timing and nesting, dirty
work counts, successful and failed reconciliation/frame closure, recovery after
failure, retained clean frames, exact lazy stabilization pass ordering, bounded
overflow and clear behavior, detached lifetime, reentrant attachment rejection,
and a disabled path that performs no clock reads. Existing output, identity,
queue, and exception behavior must remain unchanged with recording enabled.

The timeline-serialization slice adds `TimelineSnapshot::to_json()` as DUI's
versioned canonical storage and transport seed. Version 1 emits exactly
`version`, `droppedEventCount`, and `events` at the root. Each event emits, in
fixed order, `sequence`, `spanId`, `parentSpanId`, `frameId`, `lane`, `phase`,
`outcome`, `startNs`, `durationNs`, `pass`, and `workCount`. Integer values use
base-10 JSON numbers without exponent notation, time remains signed integer
nanoseconds, zero remains the absent parent/frame sentinel, and serialization
uses the classic locale with no insignificant whitespace or trailing newline.
Event vector order is preserved rather than inferred from timestamps or sorted
again. Version 1 enum spellings are `ui`; `reconcile`, `build`, `frame`,
`synchronizeRenderTree`, `layout`, `lazyRealization`, and `composite`; and
`completed` or `failed`. Unknown enum representations and negative durations
throw `invalid_argument` without mutating the snapshot.

This native JSON is intentionally not RFC 8785/JCS or Chrome Trace Event JSON.
It preserves the complete current model without converting nanoseconds to
floating-point microseconds, inventing process/thread IDs, or moving DUI fields
into tool-specific argument bags. Consumers that parse JSON numbers through
IEEE-754 must use a lossless integer path for IDs and counters above 2^53. A
later Chrome/Perfetto exporter may perform an explicit lossy mapping without
changing this schema. Acceptance covers empty snapshots, every enum spelling,
all fields and fixed ordering, zero and correlated IDs, signed time and integer
limits, nonzero drop counts, vector-order preservation, repeated byte identity,
locale independence, recorder-produced overflow snapshots, and malformed enum
or negative-duration rejection.

The raster-timeline slice makes `TimelineRecorder` safe for concurrent UI and
raster recording plus concurrent `snapshot()` and `clear()`. Identifier and ring
state are mutex-protected; calls to the injected clock are separately serialized
and occur without the recorder-state mutex. The clock remains `noexcept`, passive,
and non-reentrant with respect to the same recorder. Ring retention linearizes at
event completion. A concurrent clear removes events completed before clear takes
the state lock; a span already in progress may finish into the cleared ring.
Snapshots copy under lock and sort their detached copy by entry sequence after
unlocking.

`RasterThread` accepts an optional recorder fixed for its lifetime. A raster root
span starts on the raster worker only after a validated submission is dequeued
and renderer construction has succeeded, so it excludes queue latency. Its
`frameId` equals the `FrameTicket` ID and is interpreted within the raster lane;
this slice does not fabricate correlation to the UI frame that produced the
`LayerTree`. Child spans cover validated surface acquisition including
`take_frame()`, `SurfaceRenderer::render()`, and `SurfaceFrame::present()`.
Their version-2 spellings are `raster`, `rasterFrame`, `surfaceAcquire`,
`rasterize`, and `surfacePresent`. Every raster span has work count one and pass
zero. Successful work is `completed`; transient acquisition or presentation is
`unavailable` or `outOfDate`; observed surface loss is `lost`; and other
exceptions are `failed`. Children share the ticket frame ID and name the raster
root as parent. The root adopts the terminal child outcome. Existing ticket
values and exceptions remain authoritative and unchanged.

Submissions canceled before dequeue and submissions pending behind renderer
factory or terminal worker failure emit no raster event because no raster work
began. Invalid or rejected submissions likewise emit nothing. A null recorder
performs no clock reads. Version 1 JSON bytes remain unchanged for empty and
UI-only snapshots; snapshots containing any raster lane, raster phase, or new
outcome emit version 2 with the otherwise unchanged schema and field order.
Acceptance covers ready/presented, unavailable, out-of-date, acquire-loss,
present-loss, and renderer-failure paths; exact phase omission after an early
exit; ticket/frame and parent correlation; concurrent UI/raster collection,
snapshot, and clear; disabled behavior; canonical version selection; and
ThreadSanitizer execution without races or deadlock.

The cross-lane correlation slice adds a recorder-local nonzero `flowId` to
timeline events. A recorded `BuildOwner::layer_frame()` allocates one flow,
applies it to the UI frame and every nested frame phase, and returns a new
`LayerTree` value carrying that flow plus weak opaque recorder provenance.
`RenderOwner` continues to cache an untagged rendering snapshot: clean frames
reuse the same immutable root layer but receive distinct flows, and older trees
retain their original scalar flow. Reconciliation and standalone flush events
remain uncorrelated with flow zero.

`RasterThread` propagates a submitted tree's flow to its raster root and children
only when the tree's weak provenance token matches the recorder fixed to that
raster thread. A different recorder, an expired token, or an ordinary manually
constructed tree produces raster events with flow zero. The weak token is never
serialized or publicly exposed and does not retain the source recorder. Flow IDs
are meaningful only inside snapshots from the matching recorder; `frameId`
continues to identify the lane-local UI frame or raster ticket, while
`parentSpanId` continues to express nesting rather than asynchronous causality.

`flowId` is appended to the C++ event value to preserve positional source
compatibility. Canonical JSON version 1 and version 2 bytes remain unchanged when
every retained flow is zero. A snapshot containing any nonzero flow emits version
3 and adds `flowId` after `frameId` to every event, including zero for unrelated
events in the same snapshot. Acceptance covers matching and mismatched recorder
paths, unrecorded trees, all raster outcomes, clean-root reuse with fresh flows,
old-tree immutability, recorder non-retention, standalone zero-flow events,
mixed-flow version-3 field order, and concurrent propagation without torn IDs.

The trace-export slice adds `TimelineSnapshot::to_chrome_trace_json()` as a
tooling adapter without changing the canonical DUI schema. It emits a Chrome
Trace Event JSON object with a `traceEvents` array, fixed process ID 1, UI thread
ID 1, raster thread ID 2, and metadata naming both threads. Every retained span
becomes a complete (`X`) event in snapshot order. Its phase name is the event
name, its lane selects `dui.ui` or `dui.raster`, and its argument object carries
the outcome, sequence/span/parent/frame identifiers, pass, work count, and flow
ID. Identifier arguments are decimal strings so browser tooling cannot silently
truncate them, while pass and work count remain numeric. The root also reports
DUI's dropped-event count so truncation remains visible to tooling even though
it is not a Trace Event concept.

Trace Event timestamps are microseconds. The exporter shifts starts by the
earliest retained start and reports that signed nanosecond origin separately as
the decimal string `duiTimeOriginNs`. It writes non-negative relative starts and
durations from integer nanoseconds as decimal microseconds with at most three
fractional digits, without floating-point conversion or locale dependence. To
remain safely round-trippable through the binary64 parsing used by trace tools,
relative starts and durations may not exceed `2^50 - 1` nanoseconds; larger
values are rejected instead of silently losing precision. The exporter does not
reorder events, infer missing parents, or mutate the snapshot. Unknown enum
representations and negative durations are rejected under the same rules as
canonical serialization.

For each nonzero-flow UI `frame` root, the exporter emits a flow-start (`s`)
marker at that span's start. For each nonzero-flow raster `rasterFrame` root, it
emits the matching flow-end (`f`) marker at that span's start. Flow IDs use
Perfetto-compatible lowercase hexadecimal strings with a `0x` prefix and scope
`dui`, avoiding JavaScript integer truncation; markers use enclosing-slice
binding and are placed immediately after their corresponding complete event.
Zero-flow and nested events produce no marker.
The adapter intentionally performs no structural repair or deduplication for
manually assembled snapshots. Acceptance covers empty output and metadata,
both lanes and all enum spellings, exact signed origin formatting,
sub-microsecond and maximum-window relative times, out-of-window rejection,
fixed event/argument order, dropped counts, flow marker placement and hexadecimal
string IDs, zero-flow omission, locale independence, repeatable bytes, and
malformed-event rejection while canonical DUI JSON bytes remain unchanged.

The incremental-transport slice adds bounded polling over completed timeline
events without introducing callbacks, sockets, or an executor dependency.
`TimelineRecorder::read_completed(maxEvents, cursor)` returns a detached
`TimelineBatch` containing events, the cursor for the next poll, and an exact
`missedEventCount`. The default cursor starts at the recorder's first completion;
`maxEvents` must be nonzero. Returned cursors are copyable opaque values tied by
weak provenance to one recorder, do not retain it, and are rejected by a
different recorder even if the source recorder has expired.

Transport order is completion order, not `TimelineEvent::sequence` order. Span
sequence is allocated at begin time, so sorting a live batch by sequence could
advance past a lower-sequence span that remains in progress. Each completed span
therefore receives a private recorder-local nonzero completion position while
holding the same state lock that inserts it into the ring. The transport cursor
names the next completion position to inspect; it is not serialized and does not
change `TimelineEvent`, `TimelineSnapshot`, canonical JSON, or Trace Event JSON.
Snapshot capture continues to sort detached events by start sequence.

Ring overwrite advances a private retention floor. `clear()` empties the ring
and advances that floor to the next completion position without resetting
completion allocation. A span in progress during clear is retained if it
finishes afterward because insertion is the completion linearization point. A
stale cursor advances to the floor and reports the exact number of unavailable
completions as `missedEventCount`, including when the retained ring is empty. A
limited read advances only through returned events; an up-to-date read returns
no events, no miss, and the same logical position. If the 64-bit completion
position space is exhausted, incremental reads fail explicitly rather than
wrapping or silently misdelivering, while ordinary bounded recording and
snapshots continue.

Read, finish, overwrite, and clear linearize under the recorder state mutex,
while copying a batch invokes no clock and no user code. Acceptance covers
initial and limited reads, completion-order delivery, overwrite and clear gaps,
an in-flight completion across clear, empty polls, continued reads after gaps,
cursor non-retention and recorder mismatch, zero limits, detached batch lifetime,
concurrent UI/raster production with polling and clear, and unchanged
snapshot/serialization behavior.

The inspector-state-values slice keeps state private by default and adds an
explicit formatting overload, `BuildContext::state<Name>(initial, formatter)`.
The formatter is copyable, receives `const T&`, and returns a string-convertible
value. The ordinary one-argument state declaration removes any formatter from
that named slot, so exposure can be revoked by the current build. Equal-view
reconciliation that skips a build preserves the last mounted declaration.

Each opted-in state slot caches only its formatted string. The cache is refreshed
when the formatter is installed during build and after a successful
`StateHandle::set()` assignment; `update()` inherits the same path. Formatting
does not run from `BuildOwner::inspect()`, snapshot lookup, equality, or JSON
serialization, keeping inspection passive and preventing callbacks under tooling
code. Formatter exceptions, including allocation failure while producing the
string, are caught and represented as an unavailable value without changing the
state assignment, dirty scheduling, or user exception behavior. Registration of
the formatter itself remains ordinary build work and may fail before installation.
Formatter installation, stored-target replacement or revocation, Element
unmount, cache refresh, and their user-defined copy/destruction hooks execute
under an owner guard that
rejects reentrant render, flush, frame production, recorder replacement, and
state declaration or `StateHandle` mutation. A formatter therefore cannot
unmount, rehash the state map, or recursively rewrite the slot whose cache
receives its result. Rejection during formatting is isolated like any other
formatter failure.

`InspectorNode` appends a detached `state_values` vector whose entries contain
the declared state name and an optional formatted value. Capture includes only
opted-in slots and sorts entries by bytewise state name because Element storage
is unordered. Canonical inspector JSON preserves existing bytes when the vector
is empty; otherwise it inserts `stateValues` immediately after `stateSlotCount`,
with each entry encoded as fixed-order `name` and string-or-null `value` fields.
Snapshots never retain the formatter, `std::any`, state object, Element, owner,
or arbitrary captured resources. Acceptance covers default privacy and byte
compatibility, custom formatting and escaping, deterministic multi-slot order,
immediate refresh after set/update, formatter replacement and revocation,
exception-to-null isolation, detached snapshot lifetime, dormant keep-alive
values, and unchanged state identity/rebuild behavior.

The static-tooling-frontend slice adds a detached `ToolingReport` containing an
`InspectorSnapshot` and `TimelineSnapshot`. `to_html()` produces one complete,
deterministic, responsive HTML document that callers may write to disk and open
in a browser. The report is a passive presentation adapter: it does not retain a
`BuildOwner` or recorder, poll live transport, invoke formatters, mutate either
snapshot, or change their JSON schemas. A later live frontend can consume the
same snapshots and completion batches.

The document uses no script, remote asset, font, image, or external stylesheet.
A restrictive Content Security Policy permits only its inline style. The header
summarizes mount/unmount and pending pipeline counts plus retained/dropped
timeline totals. The Element panel renders the active and dormant hierarchy in
snapshot order with stable IDs, keys, lifecycle/dirty/focus state, safe counts,
optional RenderObject details, and opted-in state strings or unavailable values.
The timeline panel renders retained events in snapshot order as a horizontally
scrollable table containing lane, phase, outcome, nanosecond timing, span,
parent, frame, flow, pass, and work identifiers. Empty tree and timeline states
have explicit text rather than fabricated rows.

All user-derived strings are first encoded with the inspector's canonical JSON
string rules, then HTML-escaped and displayed as literals. This preserves valid
UTF-8, visibly normalizes malformed input, represents control characters, and
prevents markup injection without a second divergent Unicode decoder. Numeric
fields use classic-locale base-10 output. Tree rendering enforces the same
512-node depth bound and rejects unknown Element-state or RenderObject-protocol
enums; timeline rendering rejects unknown enums and negative durations as the
existing serializers do. Acceptance covers empty and mixed
reports, active/dormant nesting, state string/null output, RenderObject metadata,
all timeline lanes/outcomes and correlation IDs, stable snapshot order,
responsive/security markup, hostile and malformed strings, locale independence,
repeatable bytes, detached lifetime, and malformed/deep input rejection.

The initial state-restoration slice supports same-process replacement-owner hot
restart without transplanting a live tree.
`BuildContext::restorable_state<Name>(initial, restorationId)` opts a named slot
into restoration with a caller-owned,
nonempty, globally unique byte-string ID. A formatter overload composes the same
slot with inspector exposure. Supported values are `bool`, signed or unsigned
integrals losslessly representable by `int64` or `uint64`, and `std::string`.
Detached records use a tagged `bool`, `int64`, `uint64`, or string value rather
than object representations, RTTI names, `std::any`, callbacks, pointers, or
native handles.
Custom codecs, migrations, persistent wire serialization, hierarchical ID
scopes, and module loading/unloading are later slices.

`BuildOwner::save_restoration_state()` passively copies cached restoration
values from active and dormant Elements, merges records not yet claimed from an
installed snapshot, and sorts by bytewise ID. It invokes no user code and
rejects capture during reconciliation, framing, or inspector formatting.
Duplicate live IDs are rejected when declared. Calling ordinary `state<Name>`
revokes restoration for that slot; changing a slot's restoration ID is allowed
only when the replacement ID is otherwise unused. State mutation stages the
tagged value before assigning the live value, then refreshes inspection and
schedules the existing coalesced rebuild.

`BuildOwner::restore_state(snapshot)` validates nonempty unique IDs into staged
storage and atomically replaces pending records. It is allowed only before that
owner has ever mounted an Element and invokes no user code. On the first
matching restorable declaration, an exactly matching value category is range-
checked, installed before the build reads its handle, and consumed only after
slot installation succeeds. A type/category/range mismatch throws and leaves
the pending record available. A live slot that newly adopts an ID supersedes
and consumes any pending record rather than overwriting established state.
Unknown records remain pending for future lazy mounts, survive another snapshot,
and can be removed explicitly with `discard_pending_restoration()`.

The old owner remains independent and usable while a replacement owner is
constructed; its Elements, state handles, resources, focus, gestures, tasks,
RenderObjects, and callbacks never cross the boundary. Whole-tree atomicity is
provided by swapping to the replacement owner only after its initialization
succeeds, because failed user builds are not yet transactional. Acceptance
covers first-build restoration, owner and handle independence, private and
revoked state, inspector composition, all supported value categories and range
edges, deterministic order, duplicate/empty IDs, pristine-owner enforcement,
pending lazy-style records and discard, dormant capture, category/range failure
without consumption, repeated replacement, mutation refresh, and formatter
reentrancy rejection during restoration capture.

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
