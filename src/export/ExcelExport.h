#pragma once

#include <QString>
#include <QVariant>
#include <QVector>

namespace smcp::IOExport
{
	struct ExcelRow
	{
		QVector<QVariant> values;
		bool heading = false;
		bool merged = false;
	};

	/// Writes a single-sheet .xlsx report with numeric cells, without overwriting existing files.
	/// Throws on invalid spreadsheet data or incomplete writes.
	void WriteExcel(const QString& path, const QVector<ExcelRow>& rows, bool millimeters);
}
