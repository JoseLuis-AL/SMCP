#pragma once

// Bridge between the numerical core and the user interface.
//
// The core reports through smcp::ProgressSink and knows nothing about Qt widgets. QueuedProgress
// is the sink the application hands to it: it can be called from any thread and re-emits every
// report as a Qt signal, which Qt delivers on the GUI thread. RunBlocking() runs a job on a worker
// thread while the caller keeps processing events, so dialogs repaint and Cancel works without
// the job ever calling processEvents().

#include <atomic>
#include <exception>
#include <functional>

#include <QEventLoop>
#include <QFutureWatcher>
#include <QCloseEvent>
#include <QObject>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QPushButton>
#include <QString>
#include <QtConcurrent/QtConcurrentRun>

#include "core/Progress.h"

namespace smcp
{
	class QueuedProgress : public QObject, public ProgressSink
	{
		Q_OBJECT

	public:
		explicit QueuedProgress(QObject* parent = nullptr) : QObject(parent) {}

		void Message(const std::string& text) override { emit MessageReported(QString::fromStdString(text)); }
		void Status(const std::string& text) override { emit StatusReported(QString::fromStdString(text)); }
		void Step(int done, int total) override { emit StepReported(done, total); }
		bool Canceled() const override { return _canceled.load(std::memory_order_relaxed); }

	public slots:
		void Cancel() { _canceled.store(true, std::memory_order_relaxed); }

	signals:
		void MessageReported(const QString& text);
		void StatusReported(const QString& text);
		void StepReported(int done, int total);

	private:
		std::atomic<bool> _canceled{ false };
	};

	/// Small progress dialog for a job without a log (decoding one set, reconstructing): a status
	/// line, an indeterminate bar and a centered Cancel in the danger color. It stays on screen, and
	/// modal, until the owner calls Finish(): Cancel, Esc and the close button only request the
	/// cancellation, they never hide the dialog while the worker still runs.
	class BusyProgressDialog : public QDialog
	{
		Q_OBJECT

	public:
		BusyProgressDialog(const QString& label, QWidget* parent) :
			QDialog(parent, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint)
		{
			setWindowModality(Qt::WindowModal);
			setWindowTitle(QStringLiteral("Processing"));
			setMinimumWidth(400);

			auto* layout = new QVBoxLayout(this);
			layout->setContentsMargins(9, 9, 9, 9);
			layout->setSpacing(9);
			_label = new QLabel(label, this);
			_label->setWordWrap(true);
			layout->addWidget(_label);
			auto* bar = new QProgressBar(this);
			bar->setRange(0, 0);  // the duration is not known in advance
			bar->setTextVisible(false);
			layout->addWidget(bar);
			_cancel = new QPushButton(QStringLiteral("Cancel"), this);
			_cancel->setProperty("danger", true);
			_cancel->setMinimumWidth(88);
			layout->addWidget(_cancel, 0, Qt::AlignHCenter);
			connect(_cancel, &QPushButton::clicked, this, &BusyProgressDialog::RequestCancel);
		}

		void setLabelText(const QString& text) { _label->setText(text); }
		void reject() override { RequestCancel(); }
		void Finish()
		{
			_finished = true;
			close();
		}

	signals:
		void canceled();

	protected:
		void closeEvent(QCloseEvent* event) override
		{
			if (_finished)
			{
				event->accept();
				return;
			}
			RequestCancel();
			event->ignore();
		}

	private:
		void RequestCancel()
		{
			if (!_cancel->isEnabled())
				return;
			_cancel->setEnabled(false);
			_label->setText(QStringLiteral("Canceling..."));
			emit canceled();
		}

		QLabel* _label = nullptr;
		QPushButton* _cancel = nullptr;
		bool _finished = false;
	};

	/// Runs `work` on a worker thread and returns when it has finished; meanwhile the calling (GUI)
	/// thread keeps dispatching events. The caller must show a modal dialog first so the user cannot
	/// start a second job. An exception thrown by `work` is returned instead of propagating through Qt.
	inline std::exception_ptr RunBlocking(const std::function<void()>& work)
	{
		std::exception_ptr error;
		QEventLoop loop;
		QFutureWatcher<void> watcher;
		QObject::connect(&watcher, &QFutureWatcher<void>::finished, &loop, &QEventLoop::quit);
		watcher.setFuture(QtConcurrent::run([&work, &error]() {
			try
			{
				work();
			}
			catch (...)
			{
				error = std::current_exception();
			}
		}));
		if (!watcher.isFinished())
		{
			loop.exec();
		}
		watcher.waitForFinished();
		return error;
	}

	/// Text of an exception captured by RunBlocking, for the operation log.
	inline QString DescribeError(const std::exception_ptr& error)
	{
		try
		{
			std::rethrow_exception(error);
		}
		catch (const std::bad_alloc&)
		{
			return QStringLiteral("not enough memory");
		}
		catch (const std::exception& e)
		{
			return QString::fromUtf8(e.what());
		}
		catch (...)
		{
			return QStringLiteral("unknown error");
		}
	}
} // namespace smcp
