# Description

`Microsoft.JavaScript.V8` is a JavaScript Interface (JSI) implementation backed by Google V8.

It ships an ABI-safe C runtime surface (`jsi_runtime_vtable`) plus consumer-side C++ adapters (`JsiAbiRuntime`, `makeV8Runtime`) so a host application can embed V8 with a stable, CRT-independent boundary.

See <https://github.com/microsoft/v8-jsi>.
