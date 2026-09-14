# Linux Wayland H1 Evidence: 2026-09-14

## Scope

- Target: `dui_wayland_native_link_tests`
- Evidence tier: H1 native build and final link only
- Runtime claim: none; no compositor was available

## Environment

- OS/kernel: Linux 6.12.103, NixOS-derived container environment
- Architecture: x86_64
- Compiler: GCC 15.2.0
- CMake: 4.1.6
- Generator: Ninja
- Wayland: upstream 1.24.0 built locally from its release archive
- wayland-protocols: upstream 1.45 built locally from its release archive
- Protocols: stable xdg-shell, stable viewporter, staging fractional-scale-v1

## Result

Configured with `DUI_REQUIRE_WAYLAND_NATIVE=ON`. The official
`wayland-scanner` generated all three protocol bindings, the C and C++ targets
compiled warning-free, and `dui_wayland_native_link_tests` linked against
libwayland-client and passed. The test intentionally did not connect to a
display. P1b.2 must provide xdg-window and `wl_shm` runtime evidence under
Weston before Linux advances to H2.

GitHub Actions run
[`34875281670`](https://github.com/suransea/dui/actions/runs/34875281670) repeated
the required configure, generation, build, final link, and smoke test
successfully on Ubuntu 24.04 using distribution `libwayland-dev` and
`wayland-protocols` packages.
