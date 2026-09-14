#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>

namespace {
using CookieFn = const void* (*)();
using ClaimFn = bool (*)();
using ReleaseFn = void (*)();
using SetFn = void (*)(int);
using GetFn = int (*)();

struct Probe {
    void* handle = nullptr;
    CookieFn cookie = nullptr;
    ClaimFn claim = nullptr;
    ReleaseFn release = nullptr;
    SetFn set = nullptr;
    GetFn get = nullptr;
    SetFn setTls = nullptr;
    GetFn getTls = nullptr;
};

bool copyFile(const std::string& source, const std::string& destination) {
    std::ifstream input(source.c_str(), std::ios::binary);
    std::ofstream output(destination.c_str(), std::ios::binary | std::ios::trunc);
    if (!input || !output)
        return false;
    output << input.rdbuf();
    output.flush();
    return output.good();
}

void* symbol(void* handle, const char* name) {
    dlerror();
    void* result = dlsym(handle, name);
    const char* error = dlerror();
    if (error) {
        std::fprintf(stderr, "dlsym(%s): %s\n", name, error);
        return nullptr;
    }
    return result;
}

bool openProbe(const std::string& path, Probe& probe, bool global) {
    int flags = RTLD_NOW | (global ? RTLD_GLOBAL : RTLD_LOCAL);
#if defined(__APPLE__) && defined(RTLD_FIRST)
    flags |= RTLD_FIRST;
#endif
    probe.handle = dlopen(path.c_str(), flags);
    if (!probe.handle) {
        std::fprintf(stderr, "dlopen(%s): %s\n", path.c_str(), dlerror());
        return false;
    }
    probe.cookie = reinterpret_cast<CookieFn>(symbol(probe.handle, "soundcomputer_probe_cookie"));
    probe.claim = reinterpret_cast<ClaimFn>(symbol(probe.handle, "soundcomputer_probe_claim"));
    probe.release = reinterpret_cast<ReleaseFn>(symbol(probe.handle, "soundcomputer_probe_release"));
    probe.set = reinterpret_cast<SetFn>(symbol(probe.handle, "soundcomputer_probe_set"));
    probe.get = reinterpret_cast<GetFn>(symbol(probe.handle, "soundcomputer_probe_get"));
    probe.setTls = reinterpret_cast<SetFn>(symbol(probe.handle, "soundcomputer_probe_set_tls"));
    probe.getTls = reinterpret_cast<GetFn>(symbol(probe.handle, "soundcomputer_probe_get_tls"));
    return probe.cookie && probe.claim && probe.release && probe.set && probe.get &&
           probe.setTls && probe.getTls;
}

int fail(const char* message) {
    std::fprintf(stderr, "image_clone_test: FAIL: %s\n", message);
    return 1;
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s PROBE_LIBRARY WORK_DIRECTORY\n", argv[0]);
        return 2;
    }

    const std::string source(argv[1]);
    const std::string work(argv[2]);
    if (::mkdir(work.c_str(), 0755) != 0 && errno != EEXIST) {
        std::fprintf(stderr, "mkdir(%s): %s\n", work.c_str(), std::strerror(errno));
        return 2;
    }

#if defined(__APPLE__)
    const char* extension = ".dylib";
#else
    const char* extension = ".so";
#endif
    const std::string firstPath = work + "/slot-1" + extension;
    const std::string secondPath = work + "/slot-2" + extension;
    if (!copyFile(source, firstPath) || !copyFile(source, secondPath))
        return fail("could not copy probe images");

    // Model Rack's real load order: its primary plugin image may already be
    // visible when additional byte-identical copies are opened locally.
    Probe primary, first, second;
    if (!openProbe(source, primary, true) ||
        !openProbe(firstPath, first, false) ||
        !openProbe(secondPath, second, false))
        return fail("could not load the primary and both copied images");
    if (primary.cookie() == first.cookie() ||
        primary.cookie() == second.cookie() ||
        first.cookie() == second.cookie())
        return fail("dynamic loader coalesced or preempted copied-image data");
    if (!primary.claim() || !first.claim() || !second.claim())
        return fail("independent images could not all claim ownership");
    if (primary.claim() || first.claim() || second.claim())
        return fail("same image allowed duplicate ownership");

    primary.set(41);
    first.set(113);
    second.set(227);
    if (primary.get() != 41 || first.get() != 113 || second.get() != 227)
        return fail("exported calls or globals were preempted across images");

    primary.setTls(7);
    first.setTls(17);
    second.setTls(31);
    if (primary.getTls() != 7 || first.getTls() != 17 ||
        second.getTls() != 31)
        return fail("copied-image thread-local storage is not isolated");

    first.release();
    if (!first.claim())
        return fail("released image could not be reused");
    if (second.get() != 227)
        return fail("reusing one image changed another image's state");

    first.release();
    second.release();
    primary.release();
    dlclose(second.handle);
    dlclose(first.handle);
    dlclose(primary.handle);
    std::puts("image_clone_test: PASS");
    return 0;
}
