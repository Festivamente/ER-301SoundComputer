#pragma once

// Phase 2D: the exact range equivalents of Repeater's existing
// memcpy() and linear fade loops. No new buffers or allocations.
#include <od/tasks/StreamingFrameIO.h>
#include <cstring>

namespace od { namespace streaming {

inline bool copyRepeaterRange(const float *input, float *output,
                              int frameLength, int start, int count)
{
  if (!input || !output || !validFrameRange(start, count, frameLength))
    return false;
  std::memcpy(output + start, input + start, sizeof(float) * count);
  return true;
}

inline bool fadeRepeaterRange(const float *input, float *output,
                              const float *ramp, int frameLength,
                              int start, int count, float slope, float offset)
{
  if (!input || !output || !ramp ||
      !validFrameRange(start, count, frameLength))
    return false;
  for (int i = start; i < start + count; ++i)
    output[i] = input[i] * (slope * ramp[i] + offset);
  return true;
}

}} // namespace od::streaming
