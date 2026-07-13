// Stub translation unit so the sandbox GN build (rooted at deps/chromium) can
// compile the consumer-side JSI C++ implementation, which lives OUTSIDE the GN
// root under //../../src. GN rejects sources above its root, but an #include
// resolved via the -I//../../src include path is fine. This mirrors the NuGet
// consumer story: jsi.cpp ships as a consumer-compiled source.
#include "jsi/jsi.cpp"
