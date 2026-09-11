# DUI

DUI is an experimental C++ declarative UI framework based on Flutter's
Widget/Element/RenderObject architecture. It combines short-lived, statically
typed view descriptions with a persistent runtime element tree.

The current implementation is intentionally headless. It validates build,
identity, reconciliation, state, dependency tracking, box layout, painting,
hit testing, pointer activation, and the NativeView/Renderer boundary before
introducing a production graphics backend.

Frames are represented by immutable retained `LayerTree` snapshots. Declarative
`RepaintBoundary` Views isolate paint work, while the compatibility renderer
flattens retained layers into the deterministic DisplayList used by tests. An
owned raster worker submits immutable snapshots asynchronously and provides
quiescent stop/join semantics. Raster submission captures surface metrics and
returns a shared completion ticket for presentation, retry, cancellation, or
terminal failure.

```cpp
struct Counter {
    auto build(dui::BuildContext& context) const {
        auto count = context.state<"count">(0);

        return dui::VStack{
            dui::Text{"Count: " + std::to_string(count.get())},
            dui::Text{"Increment", [count]() mutable {
                count.update([](int value) { return value + 1; });
            }},
        };
    }
};
```

See [RFC 0001](docs/rfcs/0001-cpp-declarative-ui-architecture.md) for the
architecture and implementation roadmap.

Implemented layout Views include `VStack`, `HStack`, `Stack`, `Padding`, and
`ColoredBox`; primitives include `Text` and `Image`. A deterministic DisplayList
serves as the test renderer.

The interaction foundation includes typed Environment values, Signals, named
RAII resources, cancellation, GestureArena, focus/key routing, IME contracts,
tickers, animation controllers, declarative gesture/focus Views, and lazy
Task coroutines with a deterministic executor. Declarative taps are arbitrated
through GestureArena, and a conditional Win32 IMM32 adapter provides native
text input. Native Windows IME verification and adapters for other platforms
remain in progress.

## Build

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Use the `asan` preset for AddressSanitizer and UndefinedBehaviorSanitizer.

The core requires C++23. C++26 static reflection will be an optional metadata
and tooling enhancement, not a requirement for view composition.
