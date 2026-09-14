#pragma once

#include "er301_audio_contract.hpp"

#include <dsp/resampler.hpp>

#include <algorithm>
#include <cstring>

// Rack-rate adapter for the fixed-frame ER-301 engine.
//
// Native 48 kHz and 96 kHz paths bypass this class entirely. At every other
// Rack rate, Rack's built-in Speex resampler converts the 20 inputs to the
// selected ER-301 firmware rate and converts the four engine outputs back to
// Rack's rate. All storage is fixed-size and processSample() performs no
// allocation, file access, logging, locking, or system calls.
namespace er301audio {

class SampleRateAdapter
{
public:
  SampleRateAdapter()
  {
    // Speex quality 10 is the highest-quality mode exposed by Rack's
    // SampleRateConverter. At equal rates no Speex state is allocated.
    mInputConverter.setQuality(10);
    mOutputConverter.setQuality(10);
  }

  void configure(int rackSampleRate, int engineSampleRate, int frameLength)
  {
    mRackSampleRate = sanitizeRate(rackSampleRate);
    mEngineSampleRate = sanitizeEngineRate(engineSampleRate);
    mFrameLength = validFrameLength(frameLength) ? frameLength : 0;

    mInputConverter.setRates(mRackSampleRate, mEngineSampleRate);
    mOutputConverter.setRates(mEngineSampleRate, mRackSampleRate);
    reset();
  }

  void reset()
  {
    mRackInputCount = 0;
    mEngineInputStart = 0;
    mEngineInputCount = 0;
    mRackOutputStart = 0;
    mRackOutputCount = 0;
    mDroppedOutputFrames = 0;

    std::memset(mRackInputBatch, 0, sizeof(mRackInputBatch));
    std::memset(mConvertedInput, 0, sizeof(mConvertedInput));
    std::memset(mEngineInputQueue, 0, sizeof(mEngineInputQueue));
    std::memset(mEngineInputBlock, 0, sizeof(mEngineInputBlock));
    std::memset(mEngineOutputBlock, 0, sizeof(mEngineOutputBlock));
    std::memset(mConvertedOutput, 0, sizeof(mConvertedOutput));
    std::memset(mRackOutputQueue, 0, sizeof(mRackOutputQueue));

    // reset_mem() clears filter history but performs no allocation. Rack keeps
    // the Speex state public through SampleRateConverter::st.
    if (mInputConverter.st)
      speex_resampler_reset_mem(mInputConverter.st);
    if (mOutputConverter.st)
      speex_resampler_reset_mem(mOutputConverter.st);
  }

  int rackSampleRate() const { return mRackSampleRate; }
  int engineSampleRate() const { return mEngineSampleRate; }
  int frameLength() const { return mFrameLength; }
  unsigned droppedOutputFrames() const { return mDroppedOutputFrames; }

  template <typename RenderFrame>
  void processSample(const float *rackInputs,
                     float *rackOutputs,
                     RenderFrame renderFrame)
  {
    silence(rackOutputs);
    if (!rackInputs || !validFrameLength(mFrameLength))
      return;

    InputFrame &destination = mRackInputBatch[mRackInputCount++];
    for (int channel = 0; channel < NUM_INPUTS; ++channel)
      destination.samples[channel] = finiteOrZero(rackInputs[channel]);

    if (mRackInputCount >= INPUT_BATCH_FRAMES)
      convertInputBatch();

    while (mEngineInputCount >= mFrameLength)
    {
      popEngineInputBlock();
      renderFrame(reinterpret_cast<const float *>(mEngineInputBlock),
                  reinterpret_cast<float *>(mEngineOutputBlock));
      convertOutputBlock();
    }

    if (mRackOutputCount > 0)
    {
      const OutputFrame &source =
          mRackOutputQueue[mRackOutputStart % RACK_OUTPUT_QUEUE_FRAMES];
      for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
        rackOutputs[channel] = finiteOrZero(source.samples[channel]);
      mRackOutputStart = (mRackOutputStart + 1) % RACK_OUTPUT_QUEUE_FRAMES;
      --mRackOutputCount;
    }
  }

private:
  using InputFrame = rack::dsp::Frame<NUM_INPUTS>;
  using OutputFrame = rack::dsp::Frame<NUM_OUTPUTS>;

  // A small input batch keeps per-sample Speex overhead low while adding less
  // than 0.75 ms at 44.1 kHz. The existing ER-301 frame remains the dominant
  // deterministic latency.
  static constexpr int INPUT_BATCH_FRAMES = 32;
  static constexpr int CONVERTED_INPUT_FRAMES = 512;
  static constexpr int ENGINE_INPUT_QUEUE_FRAMES = 2048;
  static constexpr int CONVERTED_OUTPUT_FRAMES = 4096;
  static constexpr int RACK_OUTPUT_QUEUE_FRAMES = 8192;

  static int sanitizeRate(int rate)
  {
    // Rack normally offers a much narrower range. These limits merely prevent
    // nonsensical or corrupted values from reaching Speex.
    return std::max(8000, std::min(rate, 768000));
  }

  static int sanitizeEngineRate(int rate)
  {
    return rate == 96000 ? 96000 : 48000;
  }

  static bool validFrameLength(int frameLength)
  {
    return frameLength > 0 && frameLength <= MAX_FRAME;
  }

  static void silence(float *rackOutputs)
  {
    if (!rackOutputs)
      return;
    for (int channel = 0; channel < NUM_OUTPUTS; ++channel)
      rackOutputs[channel] = 0.0f;
  }

  void convertInputBatch()
  {
    int inputFrames = mRackInputCount;
    int outputFrames = CONVERTED_INPUT_FRAMES;
    mInputConverter.process(mRackInputBatch, &inputFrames,
                            mConvertedInput, &outputFrames);

    for (int i = 0; i < outputFrames; ++i)
      pushEngineInput(mConvertedInput[i]);

    int remaining = mRackInputCount - inputFrames;
    if (remaining > 0)
    {
      // Speex consumes the complete batch with the large output capacity used
      // here. Keep this defensive path bounded nevertheless, so a backend
      // error can never leave the next audio callback writing past the batch.
      const int keep = std::min(remaining, INPUT_BATCH_FRAMES - 1);
      const int firstKept = mRackInputCount - keep;
      std::memmove(mRackInputBatch,
                   mRackInputBatch + firstKept,
                   sizeof(InputFrame) * keep);
      remaining = keep;
    }
    mRackInputCount = remaining;
  }

  void pushEngineInput(const InputFrame &frame)
  {
    if (mEngineInputCount >= ENGINE_INPUT_QUEUE_FRAMES)
    {
      // This should be unreachable for Rack's supported sample rates. Keep the
      // newest input rather than writing beyond fixed storage.
      mEngineInputStart =
          (mEngineInputStart + 1) % ENGINE_INPUT_QUEUE_FRAMES;
      --mEngineInputCount;
    }
    const int end = (mEngineInputStart + mEngineInputCount) %
                    ENGINE_INPUT_QUEUE_FRAMES;
    mEngineInputQueue[end] = frame;
    ++mEngineInputCount;
  }

  void popEngineInputBlock()
  {
    for (int i = 0; i < mFrameLength; ++i)
    {
      mEngineInputBlock[i] =
          mEngineInputQueue[mEngineInputStart % ENGINE_INPUT_QUEUE_FRAMES];
      mEngineInputStart =
          (mEngineInputStart + 1) % ENGINE_INPUT_QUEUE_FRAMES;
    }
    mEngineInputCount -= mFrameLength;
  }

  void convertOutputBlock()
  {
    int inputFrames = mFrameLength;
    int outputFrames = CONVERTED_OUTPUT_FRAMES;
    mOutputConverter.process(mEngineOutputBlock, &inputFrames,
                             mConvertedOutput, &outputFrames);

    // CONVERTED_OUTPUT_FRAMES is deliberately large enough for a 48 kHz
    // engine feeding Rack at 768 kHz. A partial input consumption would imply
    // a rate outside the supported contract, so the unconsumed tail is safely
    // discarded instead of retaining stale engine audio.
    for (int i = 0; i < outputFrames; ++i)
      pushRackOutput(mConvertedOutput[i]);
  }

  void pushRackOutput(const OutputFrame &frame)
  {
    if (mRackOutputCount >= RACK_OUTPUT_QUEUE_FRAMES)
    {
      mRackOutputStart =
          (mRackOutputStart + 1) % RACK_OUTPUT_QUEUE_FRAMES;
      --mRackOutputCount;
      ++mDroppedOutputFrames;
    }
    const int end = (mRackOutputStart + mRackOutputCount) %
                    RACK_OUTPUT_QUEUE_FRAMES;
    mRackOutputQueue[end] = frame;
    ++mRackOutputCount;
  }

  rack::dsp::SampleRateConverter<NUM_INPUTS> mInputConverter;
  rack::dsp::SampleRateConverter<NUM_OUTPUTS> mOutputConverter;

  InputFrame mRackInputBatch[INPUT_BATCH_FRAMES];
  InputFrame mConvertedInput[CONVERTED_INPUT_FRAMES];
  InputFrame mEngineInputQueue[ENGINE_INPUT_QUEUE_FRAMES];
  InputFrame mEngineInputBlock[MAX_FRAME];
  OutputFrame mEngineOutputBlock[MAX_FRAME];
  OutputFrame mConvertedOutput[CONVERTED_OUTPUT_FRAMES];
  OutputFrame mRackOutputQueue[RACK_OUTPUT_QUEUE_FRAMES];

  int mRackSampleRate = 48000;
  int mEngineSampleRate = 48000;
  int mFrameLength = 0;
  int mRackInputCount = 0;
  int mEngineInputStart = 0;
  int mEngineInputCount = 0;
  int mRackOutputStart = 0;
  int mRackOutputCount = 0;
  unsigned mDroppedOutputFrames = 0;
};

} // namespace er301audio
