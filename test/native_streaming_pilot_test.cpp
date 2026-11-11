// Phase 2G host-only mode-switch regression. Engine callbacks are fakes;
// the real private-image bridge and ER-301 graph require a Rack integration test.
#include "../src/er301_native_streaming_pilot.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void check(bool b, const char *why)
{
  if (!b) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

struct FakeEngine
{
  int F = 8;
  int enables = 0;
  int disables = 0;
  int blocks = 0;
  int slices = 0;
  int failAt = -1;
  bool eligible = true;
  bool wasEnabled = false;

  bool enable() { ++enables; wasEnabled = eligible; return wasEnabled; }
  void disable() { ++disables; wasEnabled = false; }
  bool range(const float *in, float *out, int idx)
  {
    if (idx == failAt) return false;
    ++slices;
    check(wasEnabled, "range ran while not enabled");
    out[0] = in[8] * 2.f;
    out[1] = -in[8];
    return true;
  }
  void block(const float *in, float *out)
  {
    ++blocks;
    for (int i=0; i<F; ++i)
    {
      out[i*er301audio::NUM_OUTPUTS] = in[i*er301audio::NUM_INPUTS+8]*2.f;
      out[i*er301audio::NUM_OUTPUTS+1] = -in[i*er301audio::NUM_INPUTS+8];
    }
  }
};

static void process(er301audio::NativeStreamingPilot &pilot, FakeEngine &e,
                    float sample, bool request, float* out)
{
  float input[er301audio::NUM_INPUTS] = {};
  float output[er301audio::NUM_OUTPUTS] = {};
  input[8]=sample;
  pilot.processSample(input, output, e.F, request,
    [&](){return e.enable();},
    [&](){e.disable();},
    [&](const float *in,float*out,int idx){return e.range(in,out,idx);},
    [&](const float *in,float*out){e.block(in,out);});
  *out=output[0];
}

static void baseline(int F)
{
  er301audio::NativeStreamingPilot pilot;
  FakeEngine e; e.F=F;
  float v=0;
  for (int i=0; i<F*3; ++i)
  {
    process(pilot,e,(float)i+1.f,false,&v);
    float expected = i<F ? 0.f : ((float)(i-F)+1.f)*2.f;
    check(std::fabs(v-expected)<0.0001f,"legacy parity for full frames");
  }
  check(e.blocks==3 && e.slices==0 && e.enables==0, "legacy callbacks");
}

static void streamingAndBoundarySwitch()
{
  er301audio::NativeStreamingPilot pilot;
  FakeEngine e;
  float v;
  for (int i=0;i<8;++i)
  {
    // UI switch off after sample 4 must wait until next frame boundary.
    process(pilot,e,(float)i+1.f,i<4,&v);
    check(v==2.f*(i+1), "streaming same-sample output");
  }
  check(e.enables==1 && e.slices==8 && e.blocks==0 && pilot.active(),
        "streaming while toggle pending");
  for(int i=0;i<8;++i)
  {
    process(pilot,e,100.f+i,false,&v);
    check(v==0.f,"first legacy frame flushed after switch");
  }
  check(!pilot.active() && e.disables==1 && e.blocks==1,
        "legacy entered only on frame boundary");
  for(int i=0;i<8;++i)
  {
    process(pilot,e,200.f+i,false,&v);
    check(v==2.f*(100.f+i), "legacy output returned on following frame");
  }
  check(e.blocks==2,"legacy frame progress");
}

static void unsupportedGraph()
{
  er301audio::NativeStreamingPilot pilot;
  FakeEngine e; e.eligible=false;
  float v;
  for (int i=0;i<16;++i)
    process(pilot,e,1.f,true,&v);
  check(!pilot.active() && !pilot.failed(), "unsupported graph stays legacy");
  check(e.blocks==2 && e.slices==0, "unsupported graph still runs DSP");
}

static void graphEditRecoversAfterLegacyFrame()
{
  er301audio::NativeStreamingPilot pilot;
  FakeEngine e; e.failAt=3;
  float v;
  for (int i=0; i<e.F; ++i)
  {
    process(pilot,e,1.f,true,&v);
    if (i>=3) check(v==0.f, "never replay or output failed slice");
  }
  check(!pilot.failed() && !pilot.active() && pilot.recovering(),
        "first graph invalidation is recoverable");
  check(e.blocks==0 && e.slices==3 && e.disables==1,
        "no legacy double-process in failed frame");
  e.failAt=-1;
  for (int i=0; i<e.F; ++i)
  {
    process(pilot,e,10.f,true,&v);
    check(v==0.f, "first recovery block outputs silence");
  }
  check(e.blocks==1 && e.enables==1 && !pilot.recovering(),
        "one full legacy frame restores scheduling cadence");
  for (int i=0; i<e.F; ++i)
  {
    process(pilot,e,20.f+i,true,&v);
    check(v==(20.f+i)*2.f, "streaming resumes with real sample outputs");
  }
  check(pilot.active() && !pilot.failed() && e.enables==2 && e.slices==11,
        "recoverable edit returns to streaming after eligibility check");
}

static void incompatibleGraphUsesLegacyUntilReady()
{
  er301audio::NativeStreamingPilot pilot;
  FakeEngine e; e.failAt=2;
  float v;
  for (int i=0; i<e.F; ++i)
    process(pilot,e,1.f,true,&v);
  e.failAt=-1;
  e.eligible=false;
  for (int i=0; i<e.F*3; ++i)
    process(pilot,e,10.f,true,&v);
  check(!pilot.failed() && !pilot.active() && e.blocks==3,
        "unsupported graph stays on original full-frame path");
  e.eligible=true;
  for (int i=0; i<e.F; ++i)
  {
    process(pilot,e,20.f+i,true,&v);
    check(v==(20.f+i)*2.f, "resumes when graph becomes eligible");
  }
  check(pilot.active() && !pilot.failed(), "eligibility restored");
}

static void repeatedRuntimeFailureLatches()
{
  er301audio::NativeStreamingPilot pilot;
  FakeEngine e; e.failAt=3;
  float v;
  for (int attempt=0; attempt<3; ++attempt)
  {
    for (int i=0; i<e.F; ++i)
      process(pilot,e,1.f,true,&v);
    if (attempt<2)
    {
      check(!pilot.failed() && pilot.recovering(), "retry budget remains");
      for (int i=0; i<e.F; ++i)
        process(pilot,e,2.f,true,&v);
    }
  }
  check(pilot.failed() && !pilot.active(), "three consecutive failures latch");
  check(e.slices==9 && e.blocks==2 && e.enables==3 && e.disables==3,
        "no repeated partial-frame processing");
  for (int i=0; i<e.F*2; ++i)
    process(pilot,e,3.f,true,&v);
  check(e.enables==3 && e.blocks==4, "latched fault remains legacy");
}

static void successfulFrameResetsFailureStreak()
{
  er301audio::NativeStreamingPilot pilot;
  FakeEngine e; e.failAt=3;
  float v;
  for (int i=0; i<e.F; ++i) process(pilot,e,1.f,true,&v);
  for (int i=0; i<e.F; ++i) process(pilot,e,2.f,true,&v);
  e.failAt=-1;
  for (int i=0; i<e.F; ++i) process(pilot,e,3.f,true,&v);
  check(pilot.active() && !pilot.failed(), "streaming recovered");
  e.failAt=3;
  for (int i=0; i<e.F; ++i) process(pilot,e,4.f,true,&v);
  check(!pilot.failed() && pilot.recovering(), "successful frame clears count");
}

int main()
{
  for(int f : {4,8,16,32,64,128}) baseline(f);
  streamingAndBoundarySwitch();
  unsupportedGraph();
  graphEditRecoversAfterLegacyFrame();
  incompatibleGraphUsesLegacyUntilReady();
  repeatedRuntimeFailureLatches();
  successfulFrameResetsFailureStreak();
  std::puts("native_streaming_pilot_test: PASS (legacy, recovery, graph edits, bounded fault latch)");
}
