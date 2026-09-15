# Linux Wayland P1b.4b Keyboard Repeat H0/H1 Evidence

Date: 2026-09-15

GitHub Actions run:
https://github.com/suransea/dui/actions/runs/34932382987

Verified commit: `818248c957e33176012c4017d059ae180ab773db`

The Ubuntu 24.04 runner built and linked the native Wayland target against the
distribution Wayland and xkbcommon SDKs with AddressSanitizer and
UndefinedBehaviorSanitizer. This establishes H1 for monotonic `timerfd`
creation/arming/draining, integration into prepared-read polling,
`xkb_keymap_key_repeats`, and final native linkage.

The executable state test establishes H0 for:

- negative timing and sub-nanosecond rate rejection;
- exact one-nanosecond rate and zero-delay scheduling;
- delay measured from the original monotonic key-down time;
- non-repeatable key and non-candidate release behavior;
- repeat-info disable/re-enable and timing replacement;
- active-candidate replacement and stale-generation rejection;
- bounded coalesced expiration delivery;
- release and focus/keymap-style cancellation.

The headless Weston regression remained sanitizer-clean, but it did not inject
keyboard focus, keymap, repeat-info, down, elapsed repeat, or up events.
Therefore this record does not claim keyboard-repeat H2. That requires a
compositor-side input injector driving the complete timed native sequence.
