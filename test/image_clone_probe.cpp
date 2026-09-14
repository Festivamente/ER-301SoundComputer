#include <atomic>
#include <cstdint>

#if defined(__GNUC__)
#define PROBE_EXPORT __attribute__((visibility("default")))
#else
#define PROBE_EXPORT
#endif

namespace {
std::atomic<bool> gClaimed{false};
}

// Deliberately exported rather than hidden. The real ER-301 Sound Computer image contains
// thousands of globally visible Lua/engine symbols. Loading a primary image
// globally must not cause a copied RTLD_LOCAL image's internal calls or data
// references to resolve back into the primary image.
extern "C" {
PROBE_EXPORT int soundcomputer_probe_exported_value = 0;
PROBE_EXPORT thread_local int soundcomputer_probe_exported_tls = 0;

PROBE_EXPORT void soundcomputer_probe_impl_set(int value) {
    soundcomputer_probe_exported_value = value;
}

PROBE_EXPORT int soundcomputer_probe_impl_get() {
    return soundcomputer_probe_exported_value;
}

PROBE_EXPORT void soundcomputer_probe_impl_set_tls(int value) {
    soundcomputer_probe_exported_tls = value;
}

PROBE_EXPORT int soundcomputer_probe_impl_get_tls() {
    return soundcomputer_probe_exported_tls;
}

PROBE_EXPORT const void* soundcomputer_probe_cookie() {
    return static_cast<const void*>(&soundcomputer_probe_exported_value);
}

PROBE_EXPORT bool soundcomputer_probe_claim() {
    bool expected = false;
    return gClaimed.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel);
}

PROBE_EXPORT void soundcomputer_probe_release() {
    soundcomputer_probe_impl_set(0);
    gClaimed.store(false, std::memory_order_release);
}

PROBE_EXPORT void soundcomputer_probe_set(int value) {
    // Call another exported function on purpose. This catches symbol
    // preemption in addition to merely checking that hidden/static storage was
    // duplicated by the loader.
    soundcomputer_probe_impl_set(value);
}

PROBE_EXPORT int soundcomputer_probe_get() {
    return soundcomputer_probe_impl_get();
}

PROBE_EXPORT void soundcomputer_probe_set_tls(int value) {
    soundcomputer_probe_impl_set_tls(value);
}

PROBE_EXPORT int soundcomputer_probe_get_tls() {
    return soundcomputer_probe_impl_get_tls();
}
}
