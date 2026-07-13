// Stub translation unit for the consumer-side JsiAbiRuntime — the
// facebook::jsi::Runtime implementation backed by the JSI ABI C interface. Like
// jsi_impl.cpp, it #includes the real source from //../../src (resolved via the
// include path) so this in-root TU is what GN compiles. Mirrors the NuGet
// consumer story: JsiAbiRuntime.cpp ships as a consumer-compiled source.
#include "jsi_abi/JsiAbiRuntime.cpp"
