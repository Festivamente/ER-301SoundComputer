#include "plugin.hpp"

extern "C" void fftwf_make_planner_thread_safe(void);

Plugin* pluginInstance;

#if defined(_WIN32)
extern "C" __declspec(dllexport)
#endif
void init(Plugin* p) {
    // Multiple isolated ER-301 images can create/destroy FFT plans from their
    // own interpreter threads. FFTW execution is concurrent-safe, but planner
    // operations are not. Install FFTW's process-wide planner lock once before
    // any module can boot.
    fftwf_make_planner_thread_safe();

    pluginInstance = p;
    p->addModel(modelER301SoundComputer);
}
