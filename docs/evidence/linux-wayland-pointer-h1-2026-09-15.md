# Linux Wayland P1b.3a Pointer H0/H1 Evidence

Date: 2026-09-15

GitHub Actions run:
https://github.com/suransea/dui/actions/runs/34925805062

Verified commit: `492b1fae4ef2430b9c74842cf0aaeeeb6ebb42d2`

The Ubuntu 24.04 runner built the native Wayland target against the distribution
SDK with AddressSanitizer and UndefinedBehaviorSanitizer. This establishes H1
for the seat capability listener, version-gated seat/pointer release requests,
`wl_pointer` listener ABI, primary-button translation adapter, and final link.

The executable state test establishes H0 for:

- finite surface-local coordinates and ignored hover motion;
- focused unmatched-release recovery;
- primary down, pressed motion, and up;
- duplicate-down rejection;
- exactly one cancellation on surface leave or capability loss.

The headless Weston window test remained sanitizer-clean and checked that native
pointer-object ownership matched the seat capability advertised by that
compositor. It did not inject enter, button, motion, or leave events. Therefore
this record does not claim pointer-event H2. That requires a compositor-side
input injector driving the complete native sequence.
