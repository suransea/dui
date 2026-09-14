# Linux Wayland P1b.2a H2 Evidence

Date: 2026-09-14

GitHub Actions run:
https://github.com/suransea/dui/actions/runs/34879164321

Verified commit: `ab86a1930f5f1c37e87251bf2a99f688a05e0533`

The Ubuntu runner installed distribution `libwayland-dev`,
`wayland-protocols`, and Weston packages, generated the protocol bindings,
compiled and linked the native target with AddressSanitizer and
UndefinedBehaviorSanitizer, and passed the final-link smoke test.

The runtime step started Weston with its headless backend and exercised a real
Wayland event loop. The native test verified:

- initial xdg configure followed by a diagnostic `wl_shm` content commit;
- coalesced host frame demand producing exactly one timestamped frame callback;
- framework delegate creation and shutdown notification;
- reentrant destruction of `WaylandWindow` from `window_shutting_down` without a
  sanitizer finding;
- completion within the workflow's 30-second process timeout.

This is H2 evidence only for the P1b.2a diagnostic-window subset. It does not
claim production rendering, output or fractional scaling, seat input,
cross-thread event-loop wakeup, recoverable surface recreation, or H3 hardware
coverage.
