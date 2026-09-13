# Implementation Status

## M0: Headless Architecture

Status: implemented and verified.

The first vertical slice contains:

- C++23 View values: `Text`, `VStack`, `Fragment`, `Optional`, `Choice`, and
  keyed `ForEach`;
- explicit component `build(BuildContext&)` functions and a public `View`
  concept;
- persistent Elements with type-and-key reconciliation;
- named typed state slots using fixed-string non-type template parameters;
- lifetime-checked state handles;
- automatic Signal read tracking during component builds;
- dependency replacement after conditional reads;
- coalesced, depth-ordered dirty rebuilds;
- deterministic headless tree inspection;
- duplicate-key validation that occurs before list mutation.

The test executable covers every M0 acceptance criterion from RFC 0001,
including same-type Choice alternatives and stateful keyed-list reorder.

## M1: Box Rendering

Status: implemented and verified as a headless framework slice.

The rendering slice contains:

- platform-neutral `Offset`, `Size`, `Rect`, `Insets`, and normalized
  `BoxConstraints`;
- persistent `RenderObject`, `RenderBox`, `BoxParentData`, and `RenderView`;
- owner-managed layout and paint dirty queues;
- strongly validated render-tree attachment with cycle, duplicate, null,
  cross-owner, and multi-parent rejection;
- `RenderText`, `RenderImage`, `RenderVStack`, `RenderHStack`, `RenderStack`,
  `RenderPadding`, and `RenderColoredBox`;
- deterministic text, image, and rectangle DisplayList commands;
- cached DisplayLists when no paint is dirty;
- reverse-paint-order hit testing with an ancestor path;
- pointer down/up activation with generation-safe RenderObject identity;
- `Backend`, `NativeView`, and `Renderer` platform boundaries;
- transparent Element flattening into the persistent render tree.

Tests cover every M1 acceptance criterion added to RFC 0001. The ownership
tests also exercise RenderObjects that outlive their RenderOwner.

## M2: Interaction

Status: implementation complete; platform-native verification remains in
progress.

Implemented foundation:

- reusable Element `DependencySource` subscriptions shared by Signal and
  inherited values;
- typed `EnvironmentScope` values with nearest-ancestor lookup;
- equal-component build skipping that still stores the newest descriptor;
- preservation of state writes scheduled during an active build;
- BuildOwner reconciliation reentrancy rejection;
- GestureArena close/eager-winner/reject/sweep/cancel semantics;
- callback reentrancy protection for same-pointer arena recreation;
- lifetime-safe FocusNode/FocusManager relationships and key bubbling;
- focus route snapshots and safe handler self-replacement;
- weak-client, generation-addressed text input session contracts;
- UTF-8 byte-offset selection and composing-range validation;
- named, move-only RAII resources owned by Elements;
- an explicit Element cancellation phase before any resource destruction;
- deterministic TickerScheduler and forward/reverse AnimationController;
- lazy `Task<T>`/`Task<void>` coroutines and a deterministic `ManualExecutor`,
  including safe invalidation of queued and currently resumed Element work;
- declarative `GestureDetector`/`on_tap` activation with leaf-to-root bubbling;
- declarative `FocusView`/`focusable`, Element-owned FocusNodes, mount-only
  autofocus, click-to-focus, and key dispatch through BuildOwner;
- down-to-up/cancel enrollment of declarative tap routes in GestureArena, with
  generation-safe intersection of actionable down/up hit paths;
- strict UTF-8 scalar validation for editing state and ranges;
- a conditional Win32 IMM32 text-input adapter supporting Unicode character
  editing, composition state/commit/cancel, actions, session replacement, weak
  clients, and DPR-scaled candidate/composition positioning;
- a conditional Win32 UI Automation adapter with a synthetic fragment root,
  live stable-ID providers, transactional incremental updates, role/property and
  tree navigation exposure, keyboard-focus state, DPR-scaled screen bounds,
  point lookup, `WM_GETOBJECT` integration, and asynchronous Invoke/focus action
  routing.

The input and lifecycle test executables cover the M2 foundation acceptance
criteria in RFC 0001. Rendering tests additionally cover declarative gesture
and focus integration, tree replacement during callbacks, pointer reentrancy,
focus identity across reconciliation, cancellation, and movement between child
hit regions of one detector. The Win32 adapters and their native test
executables cross-compile and link warning-free with Zig's
`x86_64-windows-gnu` target, but have not yet been executed against a real
Windows HWND, IME, UIA client, or screen reader in this environment.

## M3: Production Rendering

Status: protocol-separation, retained-rendering, raster-submission, surface,
Sliver/Viewport, lazy scrolling, semantics, and initial Win32 accessibility
slices implemented and verified; production GPU consumption and native runtime
validation remain in progress.

The first M3 slice moves `BoxConstraints`, `Size`, `BoxParentData`, child box
layout, and rectangular hit testing out of protocol-neutral `RenderObject` and
into `RenderBox`. RenderBox protocol validation is non-overridable and rejects
incompatible children before any parent/child mutation. The RenderView root
remains box-specific, providing the explicit seam needed before adding a future
box-to-sliver viewport.

Normal and ASan/UBSan runs cover unchanged box layout, paint, hit testing,
pointer interaction, owner lifetime, and transactional rejection of a non-box
child. Compile-time assertions verify that a non-box RenderObject exposes no box
geometry API.

The retained-rendering slice adds immutable `LayerTree`, `ContainerLayer`,
`OffsetLayer`, and `DisplayListLayer` snapshots. `BuildOwner::layer_frame()` is
the primary frame API; the existing `frame()` and `DisplayListRenderer` flatten
layers for compatibility. Declarative `RepaintBoundary` caches boundary-local
content, stops paint invalidation, and propagates composition-only dirtiness to
ancestor boundaries. Ordered paint chunks preserve interleaving around nested
boundaries, while stable RenderObject IDs are used only by the UI-thread cache
recipe and never escape into immutable submitted layers.

Tests verify clean root-layer identity reuse, old snapshot validity after
updates and unmount, ordered/translated flattening, nested composition without
ancestor or sibling repaint, moved-boundary local-layer reuse, accumulated
non-boundary offsets, transactional layout retry, and drained paint/composition
queues. Paint-time invalidation is explicitly rejected until generation-based
paint reentrancy is designed.

The raster-submission slice adds an owned `RasterThread` worker. Concurrent
submit/wait/status calls synchronize through a FIFO queue of immutable
LayerTrees. Callback-safe `request_stop()` cancels queued frames while allowing
the active render to finish; externally serialized destruction joins the worker.
Renderer failure is terminal, cancels pending work, and is rethrown to the owner
thread. Backend tests verify FIFO submission, raster-thread affinity, immutable
snapshots, callback-requested stop, quiescent shutdown, and failure propagation
under normal, ASan/UBSan, and ThreadSanitizer runs.

The next backend-preparation slice defines `RasterSurface` acquisition and an
exactly-once `SurfaceFrame` transaction. Requests carry logical/physical size,
DPR, and surface generation; acquisitions distinguish unavailable, out-of-date,
and lost surfaces. Ready frames validate RGBA8888 sRGB premultiplied format,
stride, extent, generation, and one stable CPU pixel mapping. These contracts
are tested fixtures and are not described as a Skia backend.

RasterThread now consumes this surface contract directly. Each queued item
captures its LayerTree and SurfaceRequest, acquires and validates a frame on the
raster worker, invokes a worker-constructed SurfaceRenderer, and presents or
abandons exactly once. Copyable FrameTickets report presented, unavailable,
out-of-date, or canceled completion to repeated/concurrent waiters; surface loss
and renderer failure remain terminal. SurfaceRenderer construction and
destruction are raster-thread-affine, while RasterSurface is explicitly a
thread-safe platform bridge.

The initial Sliver slice adds finite, non-negative `SliverConstraints`,
normalized `SliverGeometry`, `SliverParentData`, and a cached `RenderSliver`
layout protocol without adding box geometry to protocol-neutral
`RenderObject`. `RenderViewport` is a vertical RenderBox externally and accepts
only Sliver children internally; `RenderSliverToBoxAdapter` accepts at most one
box child. All protocol and cardinality checks happen before tree mutation.

Declarative `Viewport` and `SliverToBoxAdapter` Views retain their RenderObjects
across scroll updates. Multiple sequential Slivers compute local scroll,
remaining paint, parent paint offsets, and maximum scroll extent. A retained
`ClipRectLayer` and matching DisplayList clip commands preserve viewport clips
through immutable snapshots and compatibility flattening. Protocol-neutral
paint recording now dispatches offsets and clips without unchecked box casts.
Tests cover validation, compile-time geometry separation, transactional protocol
rejection, partial and fully offscreen painting, clipping, translated hit-test
paths, immutable old snapshots, equal-update layout reuse, and composition-only
movement of repaint-boundary content.

The fixed-extent Sliver slice adds declarative and render-level
`SliverFixedExtentList`. It validates a positive finite item extent, computes a
contiguous visible range directly from scroll offset and viewport paint extent,
and lays out only that range with tight cross-axis and item-axis constraints.
The retained recorder consumes protocol-provided child index ranges, so paint
recording and render-tree hit testing also avoid visiting offscreen children.
Tests cover initial and newly visible layout counts, exact item boundaries,
scroll translation, paint culling, hit testing, overscroll, maximum extent, and
transactional rejection of invalid item extents. With ordinary static children
or `ForEach`, Element/View construction remains eager and render-tree
synchronization visits every Element; callers opt into Element-level
virtualization with the lazy source described below.

The first lazy child-manager slice adds explicit `lazy_for_each` data sources.
They copy or move input items into an owned vector snapshot while retaining
ordinary `ForEach` as the eager compatibility path. Model updates scan and
validate every key without invoking item builders, then commit a revision to the
RenderSliver. Lazy item types are compile-time constrained to exactly one
top-level box RenderObject, including through eligible Components and
single-child transparent wrappers.

Frame production now separates RenderOwner layout from composition. A Sliver
with missing children publishes its logical range request without painting;
after layout unwinds, BuildOwner realizes keyed Elements, synchronizes their
RenderObjects, and repeats layout until stable before painting once. Tests cover
deep initial scroll, visible-only builder and Element counts, logical-to-mounted
index mapping, overlapping keyed identity, exact eviction counts, duplicate
keys outside the visible range, temporary initializer-list ownership, probe
passes without intermediate paint, and unchanged eager `ForEach` behavior.

The cache-range slice adds a finite non-negative `cache_extent(...)` option with
a zero default. Relative range arithmetic avoids overflowing absolute scroll
positions or losing a small viewport at large coordinates. Cached keyed
Elements and RenderObjects remain mounted on both sides of the viewport while
layout, paint, and hit testing continue to use only the visible subrange.
Scrolling reconciles overlap before paint and immediately disposes Elements
outside the new bounded range. Tests cover exact boundaries, leading/trailing
clamping, cached-to-visible identity and hit testing, one-item shifts, shrink to
zero, invalid input transactionality, empty and overscrolled models, and
large-coordinate/overflow behavior. Stabilization counts productive realization
rounds separately from the final layout probe; a dedicated test accepts the
sixteenth round and rejects a seventeenth deterministically.

The policy keep-alive slice makes every `LazyForEach` an immutable, prevalidated
snapshot. `lazy_for_each` materializes its owning item vector and rejects
duplicate keys before `BuildOwner::render`; `keep_alive_when` evaluates its
predicate into a key set at the same pre-reconciliation boundary. Selected
realized items leaving the cache move into a dormant Element bucket that is
excluded from RenderObject synchronization and lazy range discovery. Keyed
restoration reuses the exact Element and RenderObject subtree. Policy
cancellation and model deletion synchronously unmount dormant entries during
render, invalidating StateHandles, destroying resources, unsubscribing
dependencies, and applying focus fallback without waiting for a frame.
Hash-indexed active/dormant extraction keeps range reconciliation linear in the
managed set rather than quadratic. Tests cover state, resource, RenderObject,
focus, and key-dispatch identity; dirty dormant rebuild exclusion; never-realized
items; same- and different-source duplicate transactionality; throwing policy;
immediate policy/deletion cleanup; owner destruction; and builder-failure retry.

The budgeted keep-alive slice adds an optional `keep_alive_limit(count)` to
`LazyForEach`. Dormant ownership is maintained as an oldest-to-newest queue;
restoration removes an entry, and a later eviction appends it as most recent.
Only dormant Elements consume the limit. Overflow and limit shrink synchronously
unmount oldest subtrees, while zero disables dormant retention and increasing a
limit never resurrects evicted state. The unlimited default preserves the prior
policy behavior. Tests cover deterministic LRU order, restored identity and
recency, immediate StateHandle invalidation, zero/shrink/growth updates, and
unlimited compatibility.

The first semantics slice adds explicit `semantics(...)` box wrappers,
`SemanticsProperties`, immutable `SemanticsTree` snapshots, and activation by
stable RenderObject ID. Transparent RenderObjects promote semantic descendants;
annotated nodes preserve hierarchy and deterministic paint order. Collection
uses checked global offsets, accumulated half-open clips, and visible child
ranges, excluding hidden subtrees, fully clipped content, lazy cache entries,
and dormant keep-alive Elements. Actions are authorized against the current
tree before resolving their RenderObject, so stale, disabled, hidden, cached,
and dormant IDs cannot invoke callbacks. Property-only updates retain identity
without layout or paint. Tests cover nested hierarchy, hidden ancestors,
partial viewport clipping, lazy scrolling, action reentrancy and stale IDs,
pre-frame and dirty-layout rejection, and callback-driven tree replacement.

The automatic primitive-semantics slice adds text nodes for visible non-empty
`Text`, opt-in image nodes through `Image::semantics_label`, and button-role
containers for `GestureDetector` and `FocusView`. Unlabeled images remain
semantically transparent instead of leaking asset identifiers. Focus containers
reflect `can_focus`, and disabled nodes expose no action. Semantic callback
dispatch now reports success independently of pointer propagation, allowing
non-stopping text and gesture handlers to remain valid accessibility actions.
Image label and callback-presence updates retain RenderObject identity without
layout or paint. Tests cover inferred and explicit hierarchy, primitive roles,
unlabeled images, disabled focus, action results, property-only updates, and
automatic lazy-list clipping/cache/keep-alive exclusion.

The accessibility-update slice adds `SemanticsEntry`, `SemanticsChange`, and a
stateful `SemanticsDiffer` for native adapter consumption. Entries flatten
parent ID, sibling index, properties, clipped geometry, and actions without
retaining RenderObjects. Initial additions are parent-first; additions precede
reparent/update records; removals and clear are child-first. Equivalent trees
emit no changes, while zero or duplicate IDs reject transactionally without
replacing the previous snapshot. Tests cover property/action changes, sibling
reorder, moves into new parents, subtree ordering, malformed inputs, clear, and
BuildOwner-produced snapshots. Native API calls remain outside this layer.

The adapter-delivery slice adds caller-owned `AccessibilityAdapter` and
`AccessibilityBridge` contracts. Bridge publication diffs against the last
acknowledged snapshot, suppresses no-op adapter calls, and commits state only
after successful delivery. Adapter exceptions preserve the prior snapshot so
publish and clear retry the same complete delta. Callback-scoped change spans
must not be retained, and reentrant publish or clear on the delivering bridge is
rejected before diffing. Tests cover successful acknowledgment, identical retry
records after failed publish/clear, no-op suppression, reentrancy, malformed
trees, callback-driven bridge destruction on success and failure, and BuildOwner
snapshots. Bridges are noncopyable, nonmovable, and UI-thread confined.

The Win32 accessibility slice implements the first native consumer. It exposes
one synthetic UIA fragment root for all semantic roots and resolves retained COM
providers against the current stable-ID model. UIA properties cover role, name,
value, automation ID, enabled/content/control state, and DPR-scaled screen
bounds; fragment navigation, reverse-order point lookup, and button Invoke route
back through a private window message to the host's semantic-action callback.
The adapter does not fabricate semantic focus before the neutral tree represents
it. Updates reject malformed IDs, parents, cycles, sibling indexes, geometry,
and change sequences before swapping the native model. `WM_GETOBJECT` and COM
entry points contain exceptions. The conditional test covers transactional
malformed delivery and message filtering; the executable cross-compiles and
links warning-free, while real UIA client and screen-reader verification remains
pending.

The semantic-focus slice adds platform-neutral `focusable` and `focused` state
plus `SemanticsAction::focus`. `FocusView` resolves that state from its live
Element-owned FocusNode, and only a focusable, enabled RenderObject with a real
focus handler exposes the action. Focus requests revalidate the current visible
semantics tree, while state changes remain layout- and paint-neutral and flow
through `SemanticsDiffer` as stable-ID updates. Win32 maps the state to
`IsKeyboardFocusable`, `HasKeyboardFocus`, fragment-root `GetFocus`, and an
asynchronous `SetFocus` request dispatched on the HWND thread. Reported focus is
gated by the HWND thread's actual keyboard focus, and cookie-scoped deferred
notifications avoid stale adapter messages and publish focus-change events only
after native and framework focus settle. Tests cover eligibility, autofocus,
transfer, clearing, stale/disabled action rejection, focus diff records, and
malformed multiple-focus transactionality. Native UIA focus-event and assistive-
technology observation remain target-OS work.

## M4: Tooling and Platforms

Status: structured-inspector, concurrent UI/raster timeline collection, and
canonical timeline serialization slices implemented and verified.

`BuildOwner::inspect()` now captures an owned, passive Element-tree snapshot.
Each node records stable identity/generation, depth, name/value/key, active or
dormant keep-alive state, update/dirty state, safe state/dependency/environment/
resource counts, focus ownership, and optional RenderObject protocol, dirty,
boundary, and layout/paint-counter metadata. Active reconciliation order and
dormant oldest-to-newest order are preserved, with dormant state propagated
through retained descendants. Snapshots retain no Elements, RenderObjects,
arbitrary `std::any` values, callbacks, resources, or native handles; they remain
valid after updates and owner destruction. Tree-wide ID lookup and canonical
locale-independent JSON serialization are included; valid UTF-8 is preserved,
malformed bytes become deterministic `U+FFFD` escapes, and capture/serialization
enforce a 512-node depth bound while lookup remains iterative. Existing
`dump_tree()` output remains compatible. Tests cover empty, dirty, framed,
keyed, focused, escaped/malformed UTF-8, bounded deep, deterministic, owner-
independent, reentrancy-rejected, and dormant Sliver snapshots. Timeline live
transport, opt-in value inspection, and GUI tooling remain pending.

The initial timeline slice adds an opt-in, fixed-capacity `TimelineRecorder`
with a steady default clock and injectable monotonic clocks. `BuildOwner`
records correlated reconciliation, dirty-build, complete-frame, render-tree
synchronization, per-pass layout, productive lazy realization, and final
composition spans. Events retain start-order sequence/span IDs, parent and frame
IDs, outcome, non-negative timing, pass index, and work count. Successful and
failed spans close without changing the original operation result or exception;
recorder replacement is rejected during active reconciliation or framing.
Completed events enter a preallocated newest-retaining ring, overflow increments
an exact drop count, and snapshots remain valid after recorder and owner
destruction. Tests use fake clocks to cover exact timing and nesting, disabled
clock reads, dirty and clean work counts, failure recovery, bounded overflow,
clear without ID reuse, and all successful sixteen-pass and rejected
seventeen-pass lazy stabilization phases. Live transport and timeline GUI
tooling remain pending.

`TimelineSnapshot::to_json()` now emits versioned canonical DUI JSON with fixed
root and event field order, classic-locale base-10 integers, nanosecond timing,
stable enum spellings, and no insignificant whitespace. It preserves the
snapshot's event order and explicit parent/frame relationships rather than
inferring nesting from timestamps. Unknown enum values and negative durations
are rejected. Tests cover empty and recorder-produced overflow snapshots, all
enum names, signed time and integer limits, repeated byte identity, locale
independence, vector-order preservation, and malformed values. A lossy
Chrome/Perfetto trace adapter remains separate future work.

The raster-timeline slice makes recorder IDs, ring state, snapshots, clear, and
injected-clock access safe across UI, raster, and observer threads. An optional
recorder fixed at `RasterThread` construction records one raster root per
dequeued submission plus surface-acquire, rasterize, and present children.
Raster events use their ticket ID as a lane-local frame ID and report completed,
unavailable, out-of-date, lost, or failed outcomes without inventing a link to
the originating UI frame. Work canceled before dequeue emits no raster event.
Canonical JSON remains byte-compatible version 1 for UI-only snapshots and
selects version 2 for the new raster vocabulary. Tests cover exact successful
hierarchy and fake-clock timing, transient acquisition, acquire/present loss,
present out-of-date, renderer failure, queued cancellation, disabled recording,
and concurrent UI/raster recording with snapshot, clear, and serialization.

## Verification

The prototype has been built and tested with:

- GCC 15.2;
- Clang 21.1;
- Zig 0.14.1 bundled Clang in direct C++23 compatibility mode;
- Clang AddressSanitizer;
- Clang UndefinedBehaviorSanitizer;
- GCC ThreadSanitizer.

## Known Prototype Constraints

- Components and state values are currently copy constructible because the
  prototype stores component descriptors in `std::any`.
- A failed user build is not transactional yet. Error boundaries and atomic
  fallback installation are planned before a platform frame loop is added.
- Element allocation uses the standard allocator. PMR arenas and SBO-based
  owning type erasure will follow profiling.
- The current DisplayList is a deterministic test representation rather than a
  GPU command encoding.
- The initial Sliver implementation lays out static Slivers eagerly and supports
  only a vertical axis. Fixed-extent lists virtualize layout, paint traversal,
  and hit testing; `lazy_for_each` additionally virtualizes visible Element
  construction and synchronization with a bounded cache range and optional
  policy-selected dormant keep-alive. Dormant retention can be count-bounded
  with least-recently-used eviction; byte-aware memory budgeting is not yet
  available.
- `DisplayListRenderer` consumes LayerTree on the raster worker but still
  flattens it to the headless DisplayList representation; a GPU layer consumer
  is not implemented yet.
- Pointer routing currently implements arena-backed tap recognition and
  leaf-to-root activation. Drag recognizers, touch slop, capture, and hover
  policies remain future interaction work.
- UI-thread confinement is contractual rather than executor-enforced.
- Environment references are build-scoped and must not be retained by a
  component or callback.
- The only native text-input adapter is currently Win32 IMM32. Other platforms
  still require adapters.
- Windows UI Automation is the only native accessibility adapter. macOS
  Accessibility, Linux AT-SPI, and mobile adapters are not implemented, and the
  Win32 provider still requires target-OS interoperability verification.

## Next Milestone

Run the Win32 text-input and accessibility suites against a real message-pumped
HWND, native IMEs, UIA clients, and a screen reader. M3 continues with a
production GPU layer consumer and accessibility adapters for other platforms.
