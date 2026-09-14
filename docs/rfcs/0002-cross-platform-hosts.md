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

Define the platform-neutral interfaces and a deterministic fake host. Acceptance
requires frame-request coalescing, resize/scale ordering, focus loss, pointer and
key forwarding, generation-safe text sessions, semantics acknowledgement and
retry, temporary/out-of-date/lost surface handling, exception containment,
shutdown ordering, and old delegate/window handle invalidation. This slice is H0
for the shared contract in each environment where those tests are recorded; it
does not assign a native tier to untested platforms.

### P1: Linux Reference Host Integration

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
