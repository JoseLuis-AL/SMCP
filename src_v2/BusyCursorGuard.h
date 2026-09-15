#pragma once

#include <QApplication>
#include <QCursor>

namespace SMCP
{
	/// <summary>
	/// RAII guard that sets a wait cursor on construction and restores it on destruction. Guarantees cursor restoration even if an exception is thrown.
	/// Usage: simply create an instance at the beginning of the scope that needs a busy cursor.
	/// </summary>
	class BusyCursorGuard final
	{
	public:
		BusyCursorGuard() { QApplication::setOverrideCursor(QCursor(Qt::WaitCursor)); }
		~BusyCursorGuard() { QApplication::restoreOverrideCursor(); }

		// Non-copyable, non-movable.
		BusyCursorGuard(const BusyCursorGuard&) = delete;
		BusyCursorGuard& operator=(const BusyCursorGuard&) = delete;
		BusyCursorGuard(BusyCursorGuard&&) = delete;
		BusyCursorGuard& operator=(BusyCursorGuard&&) = delete;
	};
}