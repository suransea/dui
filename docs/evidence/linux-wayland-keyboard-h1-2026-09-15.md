# Linux Wayland P1b.3b Keyboard H0/H1 Evidence

Date: 2026-09-15

GitHub Actions run:
https://github.com/suransea/dui/actions/runs/34928955566

Verified commit: `f681a6a06e16c2eb7767783da1df2948635ac1bd`

The Ubuntu 24.04 runner built and linked the native Wayland target against the
distribution Wayland and xkbcommon SDKs with AddressSanitizer and
UndefinedBehaviorSanitizer. This establishes H1 for keyboard capability
ownership, the `wl_keyboard` listener ABI, bounded XKB V1 keymap mapping and
transactional replacement, layout-aware logical-key translation, effective
modifier lookup, focus handling, and final link.

The executable state test establishes H0 for:

- suppression while unfocused and rejection of empty logical keys;
- stable logical identity across down, repeat, and up;
- effective modifier snapshots;
- duplicate-down rejection and ignored unmatched releases;
- held-key clearing on keymap replacement and focus loss.

The headless Weston window test remained sanitizer-clean and checked that native
keyboard-object ownership matched the seat capability advertised by that
compositor. It did not inject focus, keymap, modifier, key, or repeat events.
Therefore this record does not claim keyboard-event H2. That requires a
compositor-side input injector driving the complete native sequence.
