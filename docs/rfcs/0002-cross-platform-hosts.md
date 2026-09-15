# RFC 0002: Cross-Platform Host Architecture

Status: accepted for staged implementation.

## Summary

DUI will implement one platform-neutral host contract and five native hosts:
Linux, Android, macOS, iOS, and Windows. Platform work is not considered
complete merely because it cross-compiles. Each capability records the highest
verification tier actually reached, allowing work to proceed on platforms
available in automation without pretending unavailable native interoperability
has been tested.

Linux is the first reference host integration because it can be built and exercised
in the current development environment. Existing Win32 text-input and UI
Automation adapters remain supported, but new Windows-only functionality is not
the default platform milestone until it is integrated through the shared host
contract. Android and Apple hosts follow the same contract and may advance in
parallel when their SDK runners are available.

## Goals

- Keep View, Element, RenderObject, semantics, text editing, timeline, and
  restoration logic platform-neutral.
- Give every host the same window lifecycle, metrics, frame scheduling, input,
  text-input, accessibility, clipboard, cursor, and surface boundaries.
- Make single-window operation complete before generalizing to multiple windows.
- Keep all native handles and SDK types outside the core public headers.
- Verify native behavior at the strongest level available and report lower
  levels honestly.
- Permit platform implementations and GPU backends to evolve independently.

## Non-Goals

- One universal native event-loop implementation.
- Pixel-identical system text, cursor, IME, or accessibility behavior.
- Shipping every host in one milestone or blocking Linux work on unavailable
  Apple, Android, or Windows hardware.
- Treating a simulator, cross-link, or mocked SDK callback as physical-device
  interoperability.
- Embedding platform SDK objects in restoration snapshots or plugin ABI values.

## Shared Host Contract

The first platform slice defines `HostApplication`, `HostWindow`,
`HostWindowDelegate`, `WindowId`, `WindowMetrics`, and `FrameRequest` in a new
platform-neutral host header. Exact names may change during implementation, but
the ownership and event direction below are normative.

The embedding application owns `HostApplication`; the host either drives a
native loop where the OS permits that model or integrates with a system-owned
loop on Android and Apple platforms. `HostApplication` owns its `HostWindow`
objects. Framework embedding code owns each `HostWindowDelegate` and its
corresponding `BuildOwner`; a window retains only a generation-checked,
non-owning delegate registration. Detaching or destroying the delegate
invalidates queued delivery before framework state is released.
The embedding also owns the platform and UI task runners and keeps them alive
until window shutdown has drained. `TaskRunner::post` only enqueues and never
invokes the task inline. A throwing `post` has the strong exception guarantee:
the supplied task was not enqueued.

A `HostWindow` owns one native window/view, its device-pixel ratio, logical
extent, visibility, focus state, platform-side text-input and accessibility
snapshots, and thread-safe `RasterSurface`. The platform executor owns SDK
objects and callback entry. The UI executor owns `BuildOwner`; the two executors
may be the same thread but the contract does not require that. Asynchronous
native callbacks translate SDK values and post generation-checked events to the
UI executor. Synchronous native text or accessibility queries read retained,
thread-safe platform-side snapshots and never enter `BuildOwner` synchronously.
Accessibility publication across separate executors uses the owned-delta,
generation-tagged asynchronous acknowledgment transaction specified in RFC
0001; the existing synchronous bridge is used only when both executors are
co-located.

Native-to-framework events include window creation and close requests, logical
size and scale changes, visibility and focus changes, monotonic frame pulses,
pointer events, key events, text-editing updates/actions, accessibility actions,
clipboard completion, and recoverable or terminal surface changes.
Framework-to-native requests include scheduling one coalesced frame, updating
the cursor, starting/updating/stopping a generation-addressed text-input
session, publishing an acknowledged semantics snapshot, reading/writing the
clipboard, invalidating current surface metrics, and requesting orderly close.

The host does not build Views from a native callback recursively. It queues or
coalesces work at the executor boundary, runs reconciliation and frame
production on the UI executor, and submits immutable `LayerTree` values to the
raster worker. Exceptions are caught before returning through C, Objective-C,
JNI, COM, Wayland, or system callback boundaries. Shutdown invalidates event
generations, stops new callbacks, cancels text input and accessibility actions,
detaches the delegate, drains or cancels raster work, destroys native bridges on
their required executors, and only then releases the window.

The initial contract supports one window while retaining `WindowId` in every
host event and frame request. This prevents an eventual multi-window extension
from relying on process-global mutable window state.

## Rendering Boundary

Each host rendering path has three independently named layers:

- a presentation object, such as Wayland buffers, Android `Surface`,
  `CAMetalLayer`, or a DXGI swap chain;
- a graphics API and device, such as Vulkan, OpenGL ES through EGL, Metal, or
  Direct3D;
- a `LayerTree`/DisplayList consumer that performs clipping, transforms, image
  decoding, text shaping, font resolution, glyph rasterization, blending, and
  writes the acquired frame.

A presentation object or graphics context is not a renderer. The current
repository has an abstract `RasterSurface`, test fixtures, and a deterministic
DisplayList flattener, but no production CPU or GPU consumer. Native host
lifecycle may be exercised with a diagnostic test-pattern renderer; that does
not count as framework rendering completion. A production renderer slice must
select and verify a concrete consumer, with HarfBuzz/platform shaping and font
rasterization or an equivalent integrated library, before any host is described
as rendering complete.

## Platform Matrix

| Platform | Window and loop | Presentation and graphics | Text input | Accessibility | Automated runtime target |
| --- | --- | --- | --- | --- | --- |
| Linux | Wayland display, seat, `xdg_surface`, and `xdg_toplevel`; X11 is a later compatibility backend | `wl_shm` diagnostic buffers first; Vulkan WSI or EGL plus OpenGL ES later; renderer tracked separately | `text-input-v3` when advertised, with explicit unavailable fallback; xkbcommon key translation | AT-SPI2 over the accessibility bus, fed by `AccessibilityBridge` diffs | Headless Weston plus isolated D-Bus session |
| Android | Thin Kotlin/Java `Activity` and custom `View`, with lifecycle and events crossing a narrow JNI layer | `Surface`/`ANativeWindow` with Vulkan or EGL plus OpenGL ES; renderer tracked separately | `InputConnection`, editor info, composing/selection updates, and IME actions after the neutral editing prerequisite | `AccessibilityNodeProvider` subset matching the neutral semantics model | Android emulator with instrumentation tests |
| macOS | `NSApplication`, `NSWindow`, and `NSView` on the main thread | `CAMetalLayer` and Metal; renderer tracked separately | `NSTextInputClient` subset after the neutral editing prerequisite | `NSAccessibility` subset matching the neutral semantics model | macOS CI runner with XCTest; interactive assistive-technology checks remain manual |
| iOS | `UIApplication`/scene lifecycle, `UIWindow`, and `UIView` on the main thread | `CAMetalLayer` and Metal; renderer tracked separately | `UIKeyInput`/`UITextInput` subset after the neutral editing prerequisite | `UIAccessibility` subset matching the neutral semantics model | iOS Simulator XCTest plus later physical-device checks |
| Windows | Win32 message loop and per-window `HWND` host | DXGI swap chain with Direct3D; renderer tracked separately | Existing IMM32 adapter integrated first; TSF is the production follow-up | Existing UI Automation subset integrated through shared semantics publication | Windows CI runner with message-pumped tests; Narrator and third-party screen readers remain manual |

Platform code may use Objective-C++, Kotlin/Java, JNI, COM, Wayland protocols,
or operating-system SDK libraries internally. Those languages and types stop at
the host boundary. The C++ core remains C++23 and does not acquire conditional
SDK includes.

## Shared Model Prerequisites

The existing text-input contract transfers complete UTF-8 editing values and one
editable rectangle. Before claiming broad `NSTextInputClient`, `UITextInput`, or
`InputConnection` support, a platform-neutral editable View/RenderObject slice
must add range-specific caret and character geometry, selection commands,
editing traits, scroll-to-visible requests, and transactional synchronization.
Until then, native adapters report the exact subset of callbacks they implement.

The existing semantics model covers a small role set plus activate and focus.
Before broad native accessibility claims, a neutral semantics slice must add the
roles, checked/selected/range/editable state, scrolling and editing actions, and
collection metadata required by that adapter. Platform bridges may ship earlier
for the supported subset, but unsupported properties and actions remain explicit
rather than fabricated.

## Host Completion Checklist

Every first-class host uses the same capability checklist: native lifecycle and
metrics; frame scheduling and coalescing; pointer and keyboard input; diagnostic
surface recovery; production LayerTree rendering; text input; accessibility;
clipboard; cursor or pointer-icon behavior where the OS exposes it; restoration
through native lifecycle replacement; exception containment; and shutdown. Each
row records its own H0-H3 evidence. A host integration is called complete only
when every applicable row has H2 evidence; H3 remains environment-specific
interoperability evidence and is never implied by host completion.

## Verification Tiers

Every platform capability is reported at one of these tiers:

| Tier | Evidence | What may be claimed |
| --- | --- | --- |
| H0: contract | Platform-neutral fake-host tests exercise ordering, coalescing, metrics, input translation, failure, and shutdown | Host contract implemented; no native implementation claim |
| H1: native build | The native target compiles and links warning-free against its real SDK/toolchain | Native adapter builds; no runtime interoperability claim |
| H2: automated native runtime | Tests run in a compositor, emulator, simulator, or target-OS CI runner with a real event loop | Automated native behavior verified for the exercised environment |
| H3: physical/manual interoperability | A named capability is exercised with recorded hardware, OS, client/IME, and conditions | Only that capability and recorded combination are verified; no general production-readiness claim |

A lower tier never substitutes for a higher tier. In particular, Windows cross-
compilation is H1, not native verification; an iOS Simulator is H2, not a device;
and headless Weston verifies Wayland protocol behavior but not every desktop
compositor.

Tiers are recorded per platform and capability, not once for an entire host.
Committed test manifests record date, result and evidence, OS/toolchain versions,
architecture, display backend, GPU or software surface, scale factors, IME, and
accessibility client. Platform-specific failures do not weaken the shared
contract tests and do not grant a broader claim than the evidence.

## Implementation Slices

### P0: Host Contract

P0 is split into two committed sub-slices so native lifecycle and asynchronous
service transactions can be reviewed independently.

P0a adds `dui/host.hpp` with strong `WindowId`, validated generation-bearing
`WindowMetrics`, `TaskRunner`, `HostWindowDelegate`, `HostWindowControl`, copyable
`HostWindow`, platform-facing `HostWindowDriver`, move-only `DelegateBinding`,
`HostApplication`, and a factory returning paired window/driver endpoints. The
window is a shared coordinator, not a native subclass. Framework commands post
to the platform runner; translated native events post to the UI runner. The
driver is weak and its entry points are serialized by the native adapter on the
platform executor; coordinator locking protects lifetime and cross-executor
state, not ordering among concurrent native producers. Every queued delegate
event captures a binding generation, and shutdown makes all copied endpoints
inert.

P0a moves the existing pointer event value to the platform-neutral input header
while retaining temporary `BuildOwner` aliases. Metrics include logical and
physical extent, DPR, and a nonzero generation; invalid or overflowing updates
are contained. Frame demand coalesces to one native request, parks while hidden
or while the surface is unavailable/out-of-date/lost, resumes once when usable,
and captures the metrics generation accepted by a pulse. Visibility or focus
loss emits sorted cancellation for active pointers before lifecycle callbacks.
Replacing or detaching a delegate terminates its active pointer streams so a new
`BuildOwner` cannot inherit a move, up, or cancellation without the corresponding
down. Accepted frame timestamps are non-negative and monotonic. A failed native
frame-control request clears its armed demand so a later framework request may
retry. Failure to enqueue a translated native event stops the coordinator rather
than retaining mutated state without its matching callback. Shutdown never
invokes platform control outside the platform executor when posting fails. No
delegate, runner, control, or error callback executes while coordinator state is
locked. Acceptance covers validation and generation, weak binding/rebinding,
separate FIFO runners, callback ordering, frame re-request, stale pulses,
pointer/key validation, all surface transitions, exception containment,
idempotent ordered shutdown, and inert old handles. This establishes H0 only in
the environments where the fake-runner/control tests execute.

The older `Backend`/`NativeView` API remains temporarily available for existing
raster and rendering tests. It is a compatibility seam, not a second lifecycle
contract; native integrations use `HostWindow`, and later rendering work either
adapts or retires the old types once their remaining renderer responsibilities
have moved behind the host/raster boundary.

P0b adds generation-safe text-input marshaling, owned asynchronous semantics
publication with acknowledgment/retry, clipboard requests, and cursor commands.
It adapts the existing `TextInputBackend` and `AccessibilityBridge` rather than
replacing them. It is delivered as three independently reviewed commits:

- P0b.1 extends `AccessibilityBridge` with UI-executor `prepare`, `prepare_clear`,
  `acknowledge`, `reject`, `retry`, and `reset_acknowledged` transactions. A
  prepared `AccessibilityPublication` owns its complete change batch and a
  nonzero generation. Preparation does not advance acknowledged entries; only a
  matching in-flight acknowledgment commits the candidate. Rejection retains
  the exact candidate and batch, while retry copies that batch under a fresh
  generation so a delayed old acknowledgment remains stale. Only one
  transaction may exist, and synchronous and asynchronous delivery cannot be
  mixed while it is pending.
- P0b.2a attaches an optional accessibility service to the host coordinator. At
  most one semantics publication is in flight; platform success/failure posts a
  publication/service-generation result to the UI executor, adapter replacement
  resets acknowledgment to empty, and native semantics actions carry the
  service generation before posting through the current delegate generation.
  An adapter must start with an empty retained model and apply each batch
  transactionally: throwing leaves that model unchanged.
- P0b.2b attaches the optional text-input service. Text updates received before
  native start coalesce; replacement and stop invalidate stale public sessions,
  and native backend sessions remain private to the coordinator. `HostWindow`
  exposes a coordinator-backed implementation of the existing
  `TextInputBackend`; it allocates a nonzero public session immediately and keeps
  only a weak framework client. Each native forwarding client carries the
  public session and text-service generation. Start, latest full editing value,
  latest editable rectangle, replacement, and stop synchronize on the platform
  executor. Backend replacement stops the old native session before starting
  the current public session on the replacement. A stale public session is
  ignored before value validation, and stale native value/action callbacks are
  discarded on the UI executor. Backend exceptions preserve retryable desired
  state; task-post failure stops the host. Shutdown invalidates the public
  session before stopping and releasing native text objects on the platform
  executor.
- P0b.3 adds generation-bearing clipboard requests and coalesced cursor
  commands, then integrates all service invalidation and platform-affine release
  into host shutdown. Clipboard read/write requests use a nonzero
  `(service-generation, sequence)` token. Completions must match one outstanding
  request; duplicates and old-service results are inert, successful reads must
  contain valid UTF-8, and replacement completes outstanding requests as
  canceled on the UI executor. Cursor commands retain desired versus applied
  state, discard queued intermediate values, permit one platform apply at a
  time, and leave failure dirty so setting the same value explicitly retries.
  Backend replacement reapplies the current desired cursor once. Shutdown
  invalidates pending requests/tasks before delegate shutdown and releases both
  backends on the platform executor.

P0b.1 acceptance covers no-op preparation, owned deltas, acknowledgment,
rejection, exact retry under a fresh generation, stale results, clear, reset,
malformed-tree transactionality, generation ordering, and rejection of mixed
synchronous/asynchronous delivery. P0b.2 and P0b.3 add service replacement,
stale callback, failure/retry, executor ordering, and shutdown tests without
emulating an IME or native accessibility client.

### P1: Linux Reference Host Integration

P1 is split so protocol ordering can be reviewed independently without treating
a mock compositor as native evidence. P1a adds a platform-neutral Wayland
surface state engine with no generated protocol or SDK types in its public API.
It requires one initial bufferless commit, stages `xdg_toplevel.configure` until
an `xdg_surface.configure` serial snapshots it, coalesces superseded serials,
and produces generation-tagged pre-submission content plans. A plan contains the
configure acknowledgment, logical/buffer extents, integer buffer scale or
fractional viewporter mapping, and whether that commit creates the sole frame
callback. New configure/scale/buffer-loss state invalidates a prepared plan;
discarding it before irreversible surface requests retains desired state for
retry under a fresh generation. Temporary buffer resources created first may be
destroyed with a discarded plan. Submission records acknowledgment and
frame-callback intent immediately before the adapter issues those irreversible
requests. Any later native transport failure is terminal for the host
connection, never a retry of the consumed serial. Configure
commits may proceed while a frame callback is outstanding, but ordinary frame
demand waits for that callback's exact generation. P1a tests establish H0 only.

P1b connects that engine to real `libwayland-client`, generated stable protocol
bindings, xkbcommon, `wl_shm`, and `HostWindowDriver`. Its target is enabled only
when those development dependencies are found. Running P1b under headless
Weston is the first Linux H2 claim; compiling P1a without those dependencies is
not H1 evidence.

P1b.1 establishes the native SDK and connection boundary before creating a
window. CMake discovers `wayland-client`, `wayland-protocols`, and
`wayland-scanner`, generates private xdg-shell, viewporter, and fractional-scale
bindings from installed XML, and exposes a separate `dui::wayland_native`
target. A thread-affine RAII connection performs registry discovery, binds only
supported versions, requires compositor, shared-memory, and xdg-shell globals,
tracks optional seat/scaling globals and removals with known-instance fallback,
and answers xdg-shell ping while its owner thread dispatches the connection.
Missing dependencies omit the target by default; an explicit require
option fails configuration. A final executable link against the real client ABI
is H1 evidence only. P1b.2 creates the xdg window, drives P1a plans, submits
diagnostic `wl_shm` buffers, and integrates `HostWindowDriver`; compositor runtime
tests begin there.

P1b.2a delivers one xdg toplevel per connection using a non-inline owner-thread
task runner for both host executors. The initial bufferless commit triggers
configure. Each accepted P1a plan allocates and paints a checked XRGB8888
`memfd`/`wl_shm` diagnostic buffer before crossing the submission boundary, then
acks configure, sets integer buffer scale, optionally creates the sole frame
callback, attaches, damages, and commits in that order. Buffer release owns
proxy and mapping retirement. Frame timestamps unwrap the protocol's 32-bit
milliseconds and carry the matching host frame generation into
`HostWindowDriver`; xdg close enters the framework delegate. Window destruction
detaches its host control before owner-thread native destruction so queued or
reentrant shutdown work cannot retain a native owner. Headless Weston
verifies initial configure, one diagnostic commit, coalesced frame demand, and
host shutdown notification as H2 for this subset only.

P1b.2b connects native output and fractional scaling without expanding into
input or renderer ownership. The connection binds every `wl_output` through
version 4, validates positive integer scale events, and keeps proxy addresses
stable while a surface references them. `wl_surface.enter` and `leave` maintain
the entered-output set; without both fractional-scale and viewporter globals,
the effective fallback is the maximum scale of entered outputs, or one when the
set is empty. Output scale changes and removal recompute that value. When both
optional protocols exist, each window creates exactly one fractional-scale
object and viewport. `preferred_scale` supersedes output integer scale, and
fractional plans keep buffer scale one while setting viewport destination to the
logical extent. Manager removal is deferred while child objects exist. Every
scale transition invalidates stale P1a plans, repaints a correctly sized buffer,
and publishes matching physical extent and device-pixel ratio through
`HostWindowDriver`. Weston configured at scale two must verify output binding,
an effective 240/120 scale, doubled physical dimensions, frame delivery, and
sanitizer-clean shutdown. It verifies viewport use as well when the compositor
advertises both optional protocols; otherwise the generated and linked
fractional path remains H1 until an H2 compositor advertises it. P1b.4 adds
wakeable cross-thread dispatch, a framework-facing `RasterSurface`, and
recoverable surface recreation.

P1b.3 is split by the translation boundary. P1b.3a adds a platform-neutral
primary-pointer state machine and the native seat/pointer lifecycle. The state
machine owns one stable pointer ID, stores finite surface-local coordinates,
emits no hover event, maps the primary button to `down`/`up`, maps motion only
while pressed, and emits `cancel` on surface leave, pointer capability loss, or
shutdown. Duplicate press is invalid; release without an observed press is
ignored so capability acquisition in the middle of a physical stream does not
invent a down event. The native connection listens to seat capabilities and
creates at most one `wl_pointer`; the window accepts events only while the
pointer focuses its own `wl_surface`. Non-primary buttons and axes remain
unrepresented until the framework input value grows those fields. Seat removal
is deferred while a pointer child exists, and every C callback contains
allocation and validation failures before entering `HostWindowDriver`.
Executable state tests establish H0 and real SDK compilation establishes H1.
Headless Weston establishes pointer H2 only when an input injector or advertised
pointer capability actually drives enter, primary down, motion, up, and leave;
mere seat discovery is not pointer H2.

P1b.3b adds a platform-neutral keyboard stream state and a native xkbcommon
adapter. CMake requires `xkbcommon >= 1.0` for the native target. A keyboard
capability creates at most one `wl_keyboard`; capability or seat loss retires it
before its parent. XKB V1 keymap events validate a non-zero bounded size, map the
fd privately, close it on every branch, and build context/keymap/state
replacements completely before retiring the prior state. Unsupported formats or
malformed maps terminate the native host. Enter for this window establishes host
focus but does not synthesize downs for the compositor's unordered pre-held key
array. Leave clears focus and held-key identity without inventing ups because
`KeyEvent` has no cancellation phase. Keycodes add the protocol-required offset
eight. Printable logical keys use layout-resolved UTF-8; named keys normalize at
least Return, Escape, Backspace, Tab, and arrows, with the canonical XKB keysym
name as fallback. Down stores logical identity, duplicate down is invalid, and
up reuses that stored identity so intervening modifier changes cannot rename the
release; unmatched up is ignored. Effective Shift, Control, Mod1, and Mod4 state
populate framework modifiers from `wl_keyboard.modifiers`. Repeat metadata is
validated and retained, but client-timer synthesis moves to P1b.4 with the
wakeable event loop; a future compositor-provided repeated state maps directly
to `KeyPhase::repeat` when a sufficiently new seat version is negotiated.
Executable stream tests establish H0 and the real SDK build establishes H1.
Keyboard H2 requires injected focus, keymap, modifier, down, and up events; seat
or keyboard-object discovery alone is not H2.

P1b.4 is split at its ownership boundaries. P1b.4a makes connection dispatch
wakeable, P1b.4b adds client-generated keyboard repeat, and P1b.4c replaces the
diagnostic-only buffer path with a framework-facing shared-memory
`RasterSurface` and recoverable buffer recreation. Each sub-slice is committed
and verified independently; implementing one does not grant the claims of the
later slices.

P1b.4a replaces the blocking `wl_display_dispatch` call with the canonical
prepared-read sequence around the display fd and one close-on-exec, nonblocking
Linux `eventfd` owned by the task runner. Posting from any thread first secures
one wake token and then enqueues while continuously holding the runner mutex, so
the owner cannot consume the token between those operations. Interrupted writes
retry; a saturated counter means a wake is already pending and is not a failure.
A fatal wake failure leaves the task unqueued, preserving `TaskRunner::post`'s
strong exception guarantee, while a spurious token caused by allocation failure
is harmless. Owner-thread draining consumes pending wake tokens and executes
tasks outside the mutex. The connection atomically closes the runner before its
final drain; later posts throw without enqueueing, and retained runner handles
may safely outlive the connection.

Dispatch drains tasks and dispatches already queued Wayland events until
`wl_display_prepare_read` succeeds, then flushes requests and polls the display
and wake fds. A flush that would block adds writable interest and is retried when
signaled. A successful display read is followed immediately by pending-event
dispatch before tasks; every path that does not read first cancels the prepared
read. Wake-only readiness cancels the read before draining tasks. Interrupted
polls retry. Display errors, invalid descriptors, read failures, fatal flush
failures, and callback failures retain the existing terminal transport behavior.
Public methods and proxy ownership remain owner-thread affine.
An H2 Weston test posts through the public runner from a worker while the owner
is blocked in `dispatch()` and verifies owner-thread execution without a
compositor event or polling delay. Real SDK compile/link is H1; the live
cross-thread wake and clean shutdown under Weston are H2 for this event-loop
subset.

P1b.4b uses the same poll set with a monotonic timer source.
`xkb_keymap_key_repeats` is authoritative for eligibility. A repeatable key down
arms the compositor-provided nonnegative delay/rate; rate zero, including a new
repeat-info event, disables repeat. A newer repeatable down replaces the active
key, while a non-repeatable down does not. Checked nanosecond conversion rejects
unrepresentable timing. Timer expirations reuse `WaylandKeyboardState`'s held
logical identity and current effective modifiers, advance the deadline by the
reported expiration count, and bound event delivery per dispatch rather than
entering an unbounded catch-up loop. When display and timer are both ready,
Wayland events are read and dispatched first. Key up, focus or capability loss,
keymap replacement, shutdown, and replacement therefore prevent repeat delivery
after the cancellation event has been dispatched. H0 covers timer-independent
repeat policy and H2 requires injected keyboard events plus elapsed timer
delivery; object discovery alone remains insufficient.

P1b.4c supplies the host coordinator with a thread-safe `RasterSurface` whose
acquisition allocates one checked memfd-backed storage transaction for the
requested physical extent and metrics generation. Raster code writes through
the existing RGBA8888 premultiplied byte contract; presentation explicitly
converts it to advertised Wayland `ARGB8888` native packed-channel storage rather
than treating `XRGB8888` bytes as compatible or discarding alpha.

Present posts ownership to the Wayland thread and synchronously waits for that
thread to accept or reject native submission. It returns `out_of_date` if
metrics or surface generation changed, `lost` after terminal transport failure,
and `presented` only after submission crosses the native commit boundary.
Shutdown first closes acquisition and resolves queued or in-flight presentation
waiters as `lost`; it never joins or waits for raster work while the raster
thread is waiting for owner-thread acknowledgment. Abandon releases unsubmitted
storage without issuing protocol requests. All Wayland proxy creation,
attachment, and destruction remains on the owner thread. Buffer mappings may
cross threads under transaction ownership; unsubmitted storage retires when
raster ownership ends, successfully committed storage waits for
`wl_buffer.release`, and terminal teardown force-retires all remaining storage.
Recoverable native allocation failure occurs during acquisition and returns
`SurfaceAcquireStatus::unavailable`; after a ready frame, presentation reports
only stale `out_of_date` or terminal `lost` failures. A fresh matching
acquisition can restore availability and repaint without reusing a consumed
configure serial. The diagnostic fill remains only a test renderer. H0 covers
abandon, transaction races, stale generations, conversion, and shutdown
ordering. H2 under Weston covers diagnostic `wl_shm` creation and commit,
observed release after replacement or detachment, resize/scale invalidation,
recreation, and sanitizer-clean shutdown; it does not claim production renderer
completion.

Implement the Wayland host first. The client performs an initial bufferless
commit to trigger `xdg_surface.configure`, waits for configure, coalesces
superseded configure events, acknowledges the latest configure it applies, and
only then commits that generation's content buffer. Committed content requests
at most one outstanding `wl_surface.frame` callback;
that callback permits the next frame production/commit but does not bootstrap
initial configuration. Output enter/leave and scale changes update a metrics
generation.
Integer `wl_surface.set_buffer_scale` is the fallback; when advertised,
`wp_fractional_scale_v1` and `wp_viewporter` preserve fractional logical-to-buffer
mapping. Stale configure and frame generations cannot submit old extents.

Start with configure/close, scale and logical metrics, pointer/keyboard input,
and a concrete `wl_shm` `RasterSurface` plus diagnostic test-pattern
`SurfaceRenderer`; then add a selected production renderer/graphics path,
clipboard/data-device, cursor themes, text-input-v3, and AT-SPI2. CI runs under
headless Weston and isolated D-Bus. H2 acceptance for the initial integration
covers configure ordering, integer and advertised fractional scaling, frame
coalescing after the frame callback, input, clean shutdown, and recoverable buffer
recreation. Rendering, clipboard, cursor, IME, and AT-SPI each receive separate
H2 rows; unavailable protocol detection is tested behavior, not successful
interoperability for that capability. When available, `wp_presentation` evidence
is tracked separately from frame-callback scheduling.

### P2: Android Host

Add the thin managed shell and JNI bridge without moving framework ownership to
the JVM. `Choreographer` provides frame callbacks; requests coalesce, pause while
backgrounded, and carry generations so callbacks from a replaced Activity or
Surface are ignored. Initial H2 emulator coverage includes activity recreation,
pause/resume, scheduler coalescing, stale callback rejection, surface create/
destroy/recreate, density/rotation/inset changes, touch, hardware keys, and
restoration across Activity replacement. After the shared model prerequisites,
separate H2 capability rows exercise production rendering, clipboard, pointer-
icon behavior, the supported `InputConnection` callbacks, and accessibility
virtual-node queries/actions. Physical-device graphics, vendor IMEs, TalkBack,
and process-death testing are independent H3 records.

### P3: Apple Hosts

Share Metal surface and C++ delegate plumbing where practical, while keeping
AppKit and UIKit lifecycle, text, and accessibility adapters separate. A macOS
`CVDisplayLink`, when used, only captures timing and coalesces a main-thread
frame request; it never touches AppKit, configures `CAMetalLayer`, reconciles, or
allocates framework objects on its real-time callback thread. Initial macOS H2
covers window lifecycle, backing-scale changes, pointer/key input, frame dispatch,
and surface recreation. iOS uses `CADisplayLink`; callbacks coalesce and stop
while the scene is inactive, and generation checks reject callbacks from replaced
views or surfaces. Initial iOS H2 covers scene/view/surface lifecycle,
orientation/insets, touch callbacks, frame coalescing, stale callback rejection,
and restoration in Simulator. After shared prerequisites, production rendering,
clipboard, cursor/pointer behavior where supported, `NSTextInputClient`,
`UIKeyInput`/`UITextInput`, `NSAccessibility`, and `UIAccessibility` subsets are
recorded as separate H2 capabilities. VoiceOver, complex IMEs, external displays,
and physical-device suspend/resume remain specific H3 evidence.

### P4: Windows Host Integration

Build a real `HWND` host around the shared contract and connect the existing
IMM32 and UI Automation adapters. Initial H2 covers message-pumped lifecycle,
DPI/resize/focus, frame scheduling, pointer/key routing, diagnostic surface
recreation, and shutdown on Windows CI. IMM32 and the currently supported UIA
subset receive separate H2 capability rows. TSF, production Direct3D rendering,
clipboard, cursor handling, Narrator, third-party screen readers, and broad IME
coverage advance as separate rows. Until an H2 runner exists, the current
adapters remain accurately reported as H1.

## Scheduling Policy

P0 precedes native host integration. P1 is the default next platform slice
because it is directly executable in the current environment. P2, P3, and P4
may proceed in parallel after P0 when suitable runners exist; their numbering is
not a product-priority ranking. Work that can only be cross-compiled should not
displace executable H0/H2 work unless it fixes a correctness or security defect.

Each native round still follows the project rule: update this RFC or an
implementation RFC first, implement one independently testable slice, run all
available shared and platform checks, record the achieved tier, independently
review it, then commit and push that round.

## Consequences

The shared host boundary adds explicit lifecycle and event translation code, but
prevents the C++ framework from becoming a Win32, AppKit, UIKit, Android, or
Wayland abstraction leak. Linux-first execution gives immediate native feedback
while the tier model keeps unsupported claims out of status documents. Some
platform capabilities will remain at different tiers for extended periods; this
is preferable to treating cross-compilation as proof of runtime behavior.
