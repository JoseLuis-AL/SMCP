#pragma once

// Observer interface between the numerical core and whoever runs it.
//
// The core never touches widgets or the event loop: it reports through a ProgressSink and polls it
// for cancellation. The application supplies a sink that forwards to its dialogs (see
// app/TaskRunner.h); tests and command-line tools use NullProgress.

#include <string>

namespace smcp
{
	class ProgressSink
	{
	public:
		virtual ~ProgressSink() = default;

		/// One line for the operation log.
		virtual void Message(const std::string& text) = 0;
		/// What the operation is doing right now (replaces the previous status).
		virtual void Status(const std::string& text) = 0;
		/// `done` of `total` steps finished.
		virtual void Step(int done, int total) = 0;
		/// True once the user asked to stop; long loops poll it between items.
		virtual bool Canceled() const = 0;
	};

	/// Sink that discards everything and never cancels.
	class NullProgress final : public ProgressSink
	{
	public:
		void Message(const std::string&) override {}
		void Status(const std::string&) override {}
		void Step(int, int) override {}
		bool Canceled() const override { return false; }
	};
} // namespace smcp
