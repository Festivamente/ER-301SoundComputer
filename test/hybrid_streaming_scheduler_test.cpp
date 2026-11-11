#include <od/tasks/TaskScheduler.h>
#include <od/tasks/Task.h>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char *message) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

struct FakeTask : od::Task {
  FakeTask(const char *name, bool range, bool frameEnd, int priority)
      : od::Task(name), rangeCapable(range), frameEndOnly(frameEnd) {
    mPriority = priority;
  }
  bool rangeCapable;
  bool frameEndOnly;
  int rangeCalls = 0;
  int blockCalls = 0;
  bool supportsRange() const override { return rangeCapable; }
  bool supportsFrameEnd() const override { return frameEndOnly; }
  bool processRange(float *, float *, int, int) override {
    ++rangeCalls;
    return true;
  }
  void process(float *, float *) override { ++blockCalls; }
};

void testHybridPrefix() {
  od::TaskScheduler scheduler;
  auto *input = new FakeTask("input", true, false, 100);
  auto *legacy = new FakeTask("legacy", false, false, 50);
  auto *output = new FakeTask("output", true, false, 0);
  auto *end = new FakeTask("end", false, true, -100);
  scheduler.add(input); scheduler.add(legacy); scheduler.add(output); scheduler.add(end);

  check(scheduler.tryEnableRangeProcessing(), "hybrid schedule enables");
  check(!scheduler.rangeProcessingIsImmediate(), "hybrid output is frame delayed");
  float in[4] = {}, out[4] = {};
  for (int i = 0; i < 4; ++i)
    check(scheduler.tryProcessRange(in, out, i, 1, 4), "hybrid slice succeeds");
  check(input->rangeCalls == 4, "input streams every sample");
  check(input->blockCalls == 0, "input not double processed");
  check(legacy->blockCalls == 1, "legacy task runs once at frame end");
  check(output->rangeCalls == 0 && output->blockCalls == 1,
        "output follows legacy suffix once per frame");
  check(end->blockCalls == 1, "frame-end task runs once");
}

void testImmediateSchedule() {
  od::TaskScheduler scheduler;
  auto *input = new FakeTask("input", true, false, 100);
  auto *unit = new FakeTask("unit", true, false, 50);
  auto *output = new FakeTask("output", true, false, 0);
  auto *end = new FakeTask("end", false, true, -100);
  scheduler.add(input); scheduler.add(unit); scheduler.add(output); scheduler.add(end);

  check(scheduler.tryEnableRangeProcessing(), "immediate schedule enables");
  check(scheduler.rangeProcessingIsImmediate(), "fully range DSP is immediate");
  float in[4] = {}, out[4] = {};
  for (int i = 0; i < 4; ++i)
    check(scheduler.tryProcessRange(in, out, i, 1, 4), "immediate slice succeeds");
  check(input->rangeCalls == 4 && unit->rangeCalls == 4 && output->rangeCalls == 4,
        "all streaming tasks run each sample");
  check(unit->blockCalls == 0 && output->blockCalls == 0,
        "streaming tasks are not block reprocessed");
  check(end->blockCalls == 1, "immediate frame-end suffix runs once");
}
}

int main() {
  testHybridPrefix();
  testImmediateSchedule();
  if (failures) return 1;
  std::puts("hybrid_streaming_scheduler_test: PASS");
  return 0;
}
