#include "export/ExcelExport.h"

#include <QBuffer>
#include <QFile>
#include <QObject>
#include <QXmlStreamWriter>
// shortcut: Qt's ZIP writer is private API; check this file when upgrading the Qt kit.
#include <QtGui/private/qzipwriter_p.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace smcp::IOExport
{
	namespace
	{
		QString ColumnName(int column)
		{
			QString name;
			for (++column; column > 0; column = (column - 1) / 26)
				name.prepend(QChar('A' + (column - 1) % 26));
			return name;
		}
	}

	void WriteExcel(const QString& path, const QVector<ExcelRow>& rows, bool millimeters)
	{
		int columns = 1;
		for (const auto& row : rows)
			columns = std::max(columns, row.values.size());
		if (rows.size() > 1048576 || columns > 16384)
			throw std::runtime_error(QObject::tr("The results exceed Excel's worksheet size limit.").toStdString());

		QByteArray worksheet;
		QXmlStreamWriter xml(&worksheet);
		xml.writeStartDocument();
		xml.writeStartElement("worksheet");
		xml.writeDefaultNamespace("http://schemas.openxmlformats.org/spreadsheetml/2006/main");
		xml.writeStartElement("sheetViews");
		xml.writeStartElement("sheetView");
		xml.writeAttribute("workbookViewId", "0");
		xml.writeEmptyElement("pane");
		xml.writeAttribute("ySplit", "4");
		xml.writeAttribute("topLeftCell", "A5");
		xml.writeAttribute("activePane", "bottomLeft");
		xml.writeAttribute("state", "frozen");
		xml.writeEndElement();
		xml.writeEndElement();
		xml.writeEmptyElement("sheetFormatPr");
		xml.writeAttribute("defaultRowHeight", "24");
		xml.writeStartElement("cols");
		for (int column = 0; column < columns; ++column)
		{
			xml.writeEmptyElement("col");
			xml.writeAttribute("min", QString::number(column + 1));
			xml.writeAttribute("max", QString::number(column + 1));
			xml.writeAttribute("width", column < 2 ? "26" : "23");
			xml.writeAttribute("customWidth", "1");
		}
		xml.writeEndElement();
		xml.writeStartElement("sheetData");
		QStringList merges;
		for (int rowIndex = 0; rowIndex < rows.size(); ++rowIndex)
		{
			const auto& row = rows[rowIndex];
			const QString number = QString::number(rowIndex + 1);
			xml.writeStartElement("row");
			xml.writeAttribute("r", number);
			if (row.merged)
			{
				merges << "A" + number + ":" + ColumnName(columns - 1) + number;
				xml.writeAttribute("ht", row.heading ? "28" : "44");
				xml.writeAttribute("customHeight", "1");
			}
			for (int column = 0; column < row.values.size(); ++column)
			{
				const auto& value = row.values[column];
				const bool numeric = value.type() == QVariant::Double;
				if (numeric && !std::isfinite(value.toDouble()))
					throw std::runtime_error(QObject::tr("A result contains an invalid numeric value.").toStdString());
				const QString text = value.toString();
				if (!numeric && text.size() > 32767)
					throw std::runtime_error(QObject::tr("A result exceeds Excel's cell text limit.").toStdString());
				xml.writeStartElement("c");
				xml.writeAttribute("r", ColumnName(column) + number);
				xml.writeAttribute("s", row.heading ? "1" : numeric ? "2" : "0");
				if (numeric)
					xml.writeTextElement("v", QString::number(value.toDouble(), 'g', 17));
				else
				{
					xml.writeAttribute("t", "inlineStr");
					xml.writeStartElement("is");
					xml.writeStartElement("t");
					xml.writeAttribute("xml:space", "preserve");
					xml.writeCharacters(text);
					xml.writeEndElement();
					xml.writeEndElement();
				}
				xml.writeEndElement();
			}
			xml.writeEndElement();
		}
		xml.writeEndElement();
		if (!merges.isEmpty())
		{
			xml.writeStartElement("mergeCells");
			xml.writeAttribute("count", QString::number(merges.size()));
			for (const auto& range : merges)
			{
				xml.writeEmptyElement("mergeCell");
				xml.writeAttribute("ref", range);
			}
			xml.writeEndElement();
		}
		xml.writeEndDocument();
		if (xml.hasError())
			throw std::runtime_error(
				QObject::tr("The results could not be encoded as an Excel worksheet.").toStdString());

		QBuffer archive;
		archive.open(QIODevice::ReadWrite);
		QZipWriter zip(&archive);
		zip.addFile("[Content_Types].xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
<Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
</Types>)");
		zip.addFile("_rels/.rels", R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>)");
		zip.addFile("xl/workbook.xml", R"(<?xml version="1.0" encoding="UTF-8"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<sheets><sheet name="Sphere Measurements" sheetId="1" r:id="rId1"/></sheets></workbook>)");
		zip.addFile("xl/_rels/workbook.xml.rels", R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>)");
		zip.addFile("xl/styles.xml",
			QString(R"(<?xml version="1.0" encoding="UTF-8"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
<numFmts count="1"><numFmt numFmtId="164" formatCode="%1"/></numFmts>
<fonts count="2"><font><sz val="9"/><name val="Segoe UI"/></font><font><b/><sz val="9"/><name val="Segoe UI"/></font></fonts>
<fills count="3"><fill><patternFill patternType="none"/></fill><fill><patternFill patternType="gray125"/></fill><fill><patternFill patternType="solid"><fgColor rgb="FFC5EAFF"/><bgColor indexed="64"/></patternFill></fill></fills>
<borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>
<cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
<cellXfs count="3">
<xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="0" fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
<xf numFmtId="164" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1" applyAlignment="1"><alignment horizontal="right" vertical="center"/></xf>
</cellXfs><cellStyles count="1"><cellStyle name="Normal" xfId="0" builtinId="0"/></cellStyles>
</styleSheet>)")
				.arg(millimeters ? "0.0000" : "0.0")
				.toUtf8());
		zip.addFile("xl/worksheets/sheet1.xml", worksheet);
		zip.close();
		if (zip.status() != QZipWriter::NoError)
			throw std::runtime_error(QObject::tr("The Excel archive could not be created.").toStdString());

		QFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
			throw std::runtime_error(QObject::tr(
				"Cannot create %1. Existing files will not be overwritten. Check the destination and permissions.")
					.arg(path)
					.toStdString());
		if (file.write(archive.data()) != archive.data().size() || !file.flush())
		{
			file.close();
			file.remove();
			throw std::runtime_error(
				QObject::tr("The Excel file could not be written completely. Check available disk space.")
					.toStdString());
		}
	}
}
