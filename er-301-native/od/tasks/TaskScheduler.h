#pragma once

#include <hal/concurrency/Mutex.h>
#include <vector>

namespace od
{

	class Task;
	class TaskScheduler
	{
	public:
		TaskScheduler();
		virtual ~TaskScheduler();

		void process(float *inputs, float *outputs);

        // Experimental API, NOT called by Pump_callback. Default disabled.
        void setRangeProcessingEnabled(bool enabled);
        // Atomically preflight the entire graph and opt in at a frame boundary.
        // Unlike the legacy test toggle above, this refuses unsupported tasks.
        bool tryEnableRangeProcessing();
        bool tryProcessRange(float *inputs, float *outputs,
                             int startSample, int sampleCount, int frameLength);
        bool rangeProcessingIsImmediate();
		void add(Task *task);
		void remove(Task *task);
		void beginTransaction();
		void endTransaction();
		int size();

	private:
		std::vector<Task *> mTasks;
		std::vector<std::pair<int, Task *>> mTransactions;
		Mutex mMutex;
		int mTransactionDepth = 0;
        bool mRangeProcessingEnabled = false;
        int mNextRangeSample = 0;
        int mRangeFrameLength = 0;
        int mRangePrefixCount = 0;
        bool mRangeImmediate = false;
        bool analyzeRangeScheduleWithoutLock(int &prefixCount,
                                             bool &immediate) const;
	};

} /* namespace od */
