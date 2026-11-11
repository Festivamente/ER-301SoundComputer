#pragma once

// Phase 2C: allocation-free, frame-range I/O primitives for the existing
// ER-301 interleaved host buffers. No interpolation or sample buffering.
#include <hal/constants.h>

namespace od { namespace streaming {

inline bool validFrameRange(int start, int count, int frameLength)
{
  return frameLength > 0 && start >= 0 && count > 0 &&
         start < frameLength && count <= frameLength - start;
}

inline void copyInputChannel(float *dst, const float *src,
                             int channel, int start, int count)
{
  for (int i = start; i < start + count; ++i)
    dst[i] = src[i * NUM_INPUT_CHANNELS + channel];
}

inline void copyOutputChannel(float *dst, const float *src,
                              int channel, int start, int count)
{
  for (int i = start; i < start + count; ++i)
    dst[i * NUM_OUTPUT_CHANNELS + channel] = src[i];
}

inline void clearOutputChannel(float *dst, int channel,
                               int start, int count)
{
  for (int i = start; i < start + count; ++i)
    dst[i * NUM_OUTPUT_CHANNELS + channel] = 0.0f;
}

}} // namespace od::streaming
