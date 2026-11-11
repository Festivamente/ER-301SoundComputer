#pragma once

#include <od/extras/ReferenceCounted.h>
#include <od/extras/Profiler.h>
#include <string>


namespace od
{

    class Task : public ReferenceCounted
    {
    public:
        Task(const std::string &name);
        virtual ~Task();

#ifndef SWIGLUA
        virtual void process(float *inputs, float *outputs)
        {
        }

        // Phase 2B: opt-in only. A task must guarantee successful execution
        // for every valid partial range before advertising range support.
        virtual bool supportsRange() const { return false; }
        // Phase 2E: tasks which intentionally run once AFTER all DSP slices
        // can opt in here. The scheduler only permits them as an ordered
        // suffix, so their legacy end-of-frame execution order is retained.
        virtual bool supportsFrameEnd() const { return false; }
        virtual bool processRange(float *inputs, float *outputs,
                                  int startSample, int sampleCount)
        {
            (void)inputs;
            (void)outputs;
            (void)startSample;
            (void)sampleCount;
            return false;
        }

        // Called by TaskScheduler before any scheduled task is released.
        // Implementations should sever cross-task graph connections while
        // every task is still kept alive by the scheduler.
        virtual void prepareForShutdown();

        std::string mName;

        // higher value --> higher priority
        int mPriority = 0;
        bool mActive = false;
#endif

        const std::string &name()
        {
            return mName;
        }

        void setName(const std::string &name)
        {
            mName = name;
        }

    protected:
        ExecutionTimer mExecutionTimer;
    };

} /* namespace od */
