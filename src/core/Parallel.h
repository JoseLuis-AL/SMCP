#pragma once

// Deterministic data parallelism for the numerical core.
//
// ParallelFor splits [0, count) into contiguous ranges, one per thread. Work items must be
// independent and write only data they own, so the result never depends on the thread count or on
// scheduling: every algorithm built on it returns bit-identical results to its serial form.

#include <algorithm>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace smcp::Parallel
{
	/// Hardware threads available to the process (at least 1).
	inline unsigned HardwareThreads()
	{
		return std::max(1u, std::thread::hardware_concurrency());
	}

	/// Calls `work(first, last)` over contiguous ranges that cover [0, count) exactly once.
	/// `minPerThread` bounds how small a range may be, so tiny inputs stay on the calling thread.
	/// The first exception thrown by any range is rethrown on the calling thread after all join.
	template <typename Work>
	void ParallelFor(long long count, long long minPerThread, Work&& work)
	{
		if (count <= 0)
		{
			return;
		}
		const long long maxThreads = std::max<long long>(1, count / std::max<long long>(1, minPerThread));
		const int threadCount = static_cast<int>(std::min<long long>(HardwareThreads(), maxThreads));
		if (threadCount <= 1)
		{
			work(static_cast<long long>(0), count);
			return;
		}

		std::exception_ptr error;
		std::mutex errorMutex;
		const auto guarded = [&](long long first, long long last)
		{
			try
			{
				work(first, last);
			}
			catch (...)
			{
				const std::lock_guard<std::mutex> lock(errorMutex);
				if (!error)
				{
					error = std::current_exception();
				}
			}
		};

		std::vector<std::thread> workers;
		workers.reserve(threadCount - 1);
		for (int t = 1; t < threadCount; ++t)
		{
			workers.emplace_back(guarded, count * t / threadCount, count * (t + 1) / threadCount);
		}
		guarded(0, count / threadCount);
		for (auto& worker : workers)
		{
			worker.join();
		}
		if (error)
		{
			std::rethrow_exception(error);
		}
	}
} // namespace smcp::Parallel
