# Linux Wayland P1b.4a Wakeable Dispatch H1/H2 Evidence

Date: 2026-09-15

GitHub Actions run:
https://github.com/suransea/dui/actions/runs/34930791065

Verified commit: `311897419da668cc0ba83260814d7e8342c20f3b`

The Ubuntu 24.04 runner built and linked the native Wayland target against the
distribution SDK with AddressSanitizer and UndefinedBehaviorSanitizer. This
establishes H1 for Linux `eventfd`, `poll`, the libwayland prepared-read API, and
the final native ABI.

The native test then ran against headless Weston and verified that:

- a worker-thread post woke connection dispatch after the compositor had
  reached its configured and framed steady state;
- the posted task executed on the connection owner thread;
- existing configure, scaling, frame callback, input-capability, and reentrant
  window-shutdown checks remained sanitizer-clean;
- a retained task-runner handle rejected posts after connection shutdown.

This establishes H2 for the wakeable event-loop subset. It does not establish
keyboard-repeat H2 or framework `RasterSurface` submission/recovery; those are
separate P1b.4b and P1b.4c slices.
