#include <od/AudioThread.h>
#include <od/extras/Profiler.h>
#include <od/config.h>

#include <cstdlib>
#include <cstring>

ConfigData globalConfig;

namespace od {

float *AudioThread::getFrame()
{
  const std::size_t count = (std::size_t)globalConfig.frameLength;
  float *frame = static_cast<float *>(std::calloc(count, sizeof(float)));
  return frame;
}

void AudioThread::releaseFrame(float *frame)
{
  std::free(frame);
}

ExecutionTimer::ExecutionTimer() {}
ExecutionTimer::~ExecutionTimer() {}

} // namespace od
