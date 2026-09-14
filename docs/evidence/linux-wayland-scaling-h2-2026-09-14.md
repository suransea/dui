# Linux Wayland P1b.2b Scaling H2 Evidence

Date: 2026-09-14

GitHub Actions run:
https://github.com/suransea/dui/actions/runs/34882487888

Verified commit: `ede1d0a5b9349cfeb9b6a5070396cbcb6550787c`

The Ubuntu 24.04 runner generated the installed Wayland protocol bindings and
built the state, final-link, and native-window targets with AddressSanitizer and
UndefinedBehaviorSanitizer. The P1a state test covered a scale transition while
an older frame callback remained outstanding, followed by exact gate release
and a scaled replacement frame.

The H2 step started Weston with its headless backend and `--scale=2`. Against the
real compositor event loop, the native test verified:

- at least one bound `wl_output` and a surface/output scaling path matching the
  globals advertised by that compositor;
- effective scale 240/120 for a 320 by 200 logical window;
- a 640 by 400 diagnostic shared-memory buffer;
- matching physical dimensions and device-pixel ratio 2.0 in both current host
  metrics and the delivered `FrameRequest`;
- one frame for coalesced demand and sanitizer-clean reentrant shutdown.

The test also requires viewport use whenever both viewporter and
fractional-scale globals are advertised. This run establishes scaling H2 for the
protocol path advertised by the packaged Weston. It does not independently
claim fractional-specific H2 when that global is absent, multi-output migration,
dynamic output removal, production rendering, input, or H3 hardware coverage.
