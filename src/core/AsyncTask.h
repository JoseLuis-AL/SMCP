#pragma once

#include <exception>
#include <memory>
#include <QFutureWatcher>
#include <QScopeGuard>
#include <QtConcurrent/QtConcurrentRun>

namespace smcp
{
	template <class Result> struct TaskResult
	{
		std::shared_ptr<Result> value;
		std::exception_ptr error;
	};

	// Capture before QtConcurrent translates exceptions; finish runs on every completion path.
	template <class Result, class Work, class Done, class Failed, class Finished>
	void RunBackground(QObject* owner, Work work, Done done, Failed failed, Finished finished)
	{
		QFutureWatcher<TaskResult<Result>>* watcher = nullptr;
		try
		{
			watcher = new QFutureWatcher<TaskResult<Result>>(owner);
			QObject::connect(watcher, &QFutureWatcherBase::finished, owner,
				[watcher, done, failed, finished]()
				{
					const auto cleanup = qScopeGuard(
						[&]()
						{
							if (watcher)
							{
								watcher->disconnect();
								watcher->deleteLater();
							}
							finished();
						});
					try
					{
						const auto result = watcher->result();
						if (result.error)
							std::rethrow_exception(result.error);
						done(std::move(*result.value));
					}
					catch (...)
					{
						failed(std::current_exception());
					}
				});
			watcher->setFuture(QtConcurrent::run(
				[work]()
				{
					TaskResult<Result> result;
					try
					{
						result.value = std::make_shared<Result>(work());
					}
					catch (...)
					{
						result.error = std::current_exception();
					}
					return result;
				}));
		}
		catch (...)
		{
			const auto cleanup = qScopeGuard(
				[&]()
				{
					if (watcher)
					{
						watcher->disconnect();
						watcher->deleteLater();
					}
					finished();
				});
			failed(std::current_exception());
		}
	}
}
