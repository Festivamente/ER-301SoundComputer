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
