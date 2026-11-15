#include "ui/pointcloud_editor/SphereResultsDialog.h"
#include "export/ExcelExport.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QXmlStreamReader>
#include <QtGui/private/qzipreader_p.h>
#include <limits>
#include <cmath>
#include <iostream>

bool SphereResultsChecks()
{
	using namespace smcp;
	bool ok = true;
	const auto check = [&](bool condition, const char* message)
	{
		if (!condition)
			std::cerr << "FAIL: " << message << '\n';
		ok &= condition;
	};
	Pointcloud::MeasuredSphere a, b;
	a.center = cv::Vec3d(1, 2, 3);
	a.radius = 5.05;
	a.label = 0;
	a.formRms = 0.002;
	a.formRange = 0.005;
	a.points = std::make_shared<Pointcloud::ColorCloud>();
	b = a;
	b.center[0] += 100.2;
	b.radius = 6;
	b.label = 1;
	SphereScanResult scan{ "sample\"cloud.xyz", "sample.xyz", { a, b }, true };
	Pointcloud::ArtifactReference reference{ { "E1", "E2" }, { 10, 12 }, { { 0, 100 }, { 100, 0 } } };
	QTemporaryDir directory;
	SphereResultsDialog dialog({ scan }, reference, directory.path());
	dialog.show();
	const auto cell = [&](int row, int column) { return dialog.results_table->item(row, column)->text(); };
	check(!dialog.show_mm_check->isChecked(), "results default to micrometers");
	check(dialog.after_scale_check->isEnabled() && !dialog.after_scale_check->isChecked() &&
			dialog.results_table->isColumnHidden(5) && dialog.results_table->isColumnHidden(6) &&
			dialog.summary_table->isColumnHidden(3) && dialog.summary_table->isColumnHidden(4),
		"scale correction is optional and initially hidden in both tables");
	dialog.after_scale_check->click();
	check(!dialog.results_table->isColumnHidden(5) && !dialog.results_table->isColumnHidden(6) &&
			!dialog.summary_table->isColumnHidden(3) && !dialog.summary_table->isColumnHidden(4),
		"scale checkbox reveals corrected measurements and error statistics");
	check(dialog.results_table->columnCount() == 8 && dialog.results_table->rowCount() == 4,
		"one cloud group, two sphere rows and one center-distance row");
	check(dialog.results_table->columnSpan(0, 0) == 8 && cell(1, 0) == "E1" && cell(3, 0) == "E1 / E2",
		"measurements are grouped by cloud and object");
	check(cell(1, 2) == "10000.0" && cell(1, 3) == "10100.0" && cell(1, 4) == "100.0" && cell(1, 7) == "2.0",
		"reference, diameter, error and form use micrometers");
	check(cell(3, 3) == "100200.0" && cell(3, 4) == "200.0", "center distances and errors use micrometers");
	const double scale = (10.1 * 10 + 12 * 12 + 100.2 * 100) / (10.1 * 10.1 + 12 * 12 + 100.2 * 100.2);
	check(std::abs(cell(1, 5).toDouble() - 10100 * scale) < 0.051,
		"corrected measurements are displayed alongside originals");
	check(dialog.summary_table->rowCount() == 3 && dialog.summary_table->item(2, 2)->text() == "129.1",
		"error summary contains RMS across individual valid comparisons");
	dialog.show_mm_check->setChecked(true);
	check(cell(1, 2) == "10.0000" && cell(1, 3) == "10.1000" && cell(1, 4) == "0.1000" && cell(1, 7) == "0.0020",
		"millimeter checkbox converts every dimensional column");
	check(dialog.summary_table->item(2, 2)->text() == "0.1291" && dialog.coverage_label->text().contains("X 100.2000"),
		"summary statistics and coverage follow the selected unit");
	check(dialog.results_table->horizontalHeaderItem(5)->text().contains("mm"), "headers identify the selected unit");
	check(dialog.after_scale_check->isChecked() && !dialog.results_table->isColumnHidden(5),
		"unit switching preserves the scale-correction checkbox");
	dialog.after_scale_check->click();
	check(dialog.results_table->isColumnHidden(5) && dialog.summary_table->isColumnHidden(3) &&
			cell(1, 3) == "10.1000" && dialog.show_mm_check->isChecked(),
		"disabling correction preserves original measurements and selected units");
	dialog.show_mm_check->setChecked(false);
	check(cell(1, 3) == "10100.0", "unit changes retain the original measurement precision");

	// Drive the real nonnative picker and worker, including the unit captured at export time.
	QString target;
	QTimer chooser;
	QObject::connect(&chooser, &QTimer::timeout,
		[&]()
		{
			if (auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
			{
				picker->selectFile(target);
				picker->findChild<QLineEdit*>("fileNameEdit")->setText(target);
				static_cast<QDialog*>(picker)->accept();
			}
		});
	chooser.start(10);
	for (const bool millimeters : { false, true })
	{
		dialog.show_mm_check->setChecked(millimeters);
		target = directory.filePath(millimeters ? "millimeters.csv" : "micrometers.csv");
		dialog.on_export_csv_button_clicked();
		QElapsedTimer deadline;
		deadline.start();
		while (!dialog.isEnabled() && deadline.elapsed() < 10000)
		{
			QApplication::processEvents();
			QThread::msleep(1);
		}
		check(
			dialog.isEnabled() && QApplication::overrideCursor() == nullptr, "CSV export restores controls and cursor");
		QFile file(target);
		check(file.open(QIODevice::ReadOnly), "CSV export creates the selected file");
		if (!file.isOpen())
			std::cerr << "CSV target: " << target.toStdString()
					  << "; status: " << dialog.status_label->text().toStdString()
					  << "; files: " << QDir(directory.path()).entryList(QDir::Files).join(", ").toStdString() << '\n';
		const QByteArray csv = file.readAll();
		const auto lines = csv.split('\n');
		const auto values = lines.value(1).split(',');
		check(csv.contains(millimeters ? "E1_diameter_mm" : "E1_diameter_um") &&
				!csv.contains(millimeters ? "_um" : "_mm"),
			"all CSV dimensional headers use one selected unit");
		check(std::abs(values.value(7).toDouble() - (millimeters ? 10.1 : 10100.0)) < 0.00001 &&
				std::abs(values.value(8).toDouble() - (millimeters ? 0.1 : 100.0)) < 0.00001 &&
				std::abs(values.value(9).toDouble() - (millimeters ? 0.002 : 2.0)) < 0.00001,
			"CSV converts coordinates, dimensions, errors and form without mixed units");
		check(csv.contains("\"sample\"\"cloud.xyz\""), "CSV escapes quotes in cloud names");
	}
	for (const bool millimeters : { false, true })
	{
		for (const bool corrected : { false, true })
		{
			dialog.show_mm_check->setChecked(millimeters);
			dialog.after_scale_check->setChecked(corrected);
			target = directory.filePath(QString("results_%1_%2.xlsx").arg(millimeters).arg(corrected));
			dialog.export_excel_button->click();
			QElapsedTimer deadline;
			deadline.start();
			while (!dialog.isEnabled() && deadline.elapsed() < 10000)
			{
				QApplication::processEvents();
				QThread::msleep(1);
			}
			check(dialog.isEnabled() && QApplication::overrideCursor() == nullptr,
				"Excel worker restores controls and cursor");
			QZipReader archive(target);
			const QByteArray sheet = archive.fileData("xl/worksheets/sheet1.xml");
			check(archive.status() == QZipReader::NoError && !sheet.isEmpty() &&
					!archive.fileData("[Content_Types].xml").isEmpty() &&
					!archive.fileData("xl/workbook.xml").isEmpty(),
				"Excel button creates an Open XML workbook package");
			QMap<QString, QVariant> cells;
			QString reference, type;
			QXmlStreamReader xml(sheet);
			while (!xml.atEnd())
			{
				xml.readNext();
				if (!xml.isStartElement())
					continue;
				if (xml.name() == "c")
				{
					reference = xml.attributes().value("r").toString();
					type = xml.attributes().value("t").toString();
				}
				else if (xml.name() == "v" || xml.name() == "t")
				{
					const QString text = xml.readElementText();
					cells[reference] = type == "inlineStr" ? QVariant(text) : QVariant(text.toDouble());
				}
			}
			check(!xml.hasError(), "Excel worksheet is well-formed XML");
			check(cells["D6"].type() == QVariant::Double &&
					std::abs(cells["D6"].toDouble() - (millimeters ? 10.1 : 10100.0)) < 0.00001 &&
					std::abs(cells["E6"].toDouble() - (millimeters ? 0.1 : 100.0)) < 0.00001,
				"Excel measurements and errors are numeric cells in the selected unit");
			check(cells["C4"].toString().contains(millimeters ? "mm" : QString::fromUtf8("µm")) &&
					cells["A10"].toString() == "Error summary" && cells["A16"].toString() == "Observations" &&
					cells["A5"].toString().contains(scan.name),
				"Excel keeps cloud grouping, unit headers, summary and observations");
			check(corrected ? cells["F4"].toString().contains("Corrected") && cells.contains("H6")
							: !cells.contains("G6") && cells["F4"].toString().contains("Form RMS"),
				"Excel includes corrected columns only when the scale checkbox is checked");
			const QByteArray styles = archive.fileData("xl/styles.xml");
			check(styles.contains(millimeters ? "formatCode=\"0.0000\"" : "formatCode=\"0.0\""),
				"Excel number formatting matches the selected unit");
			archive.close();
			QFile saved(target);
			check(saved.open(QIODevice::ReadOnly), "Excel result can be read back");
			const auto original = saved.readAll();
			saved.close();
			bool rejected = false;
			try
			{
				IOExport::WriteExcel(target, {}, millimeters);
			}
			catch (const std::exception&)
			{
				rejected = true;
			}
			check(saved.open(QIODevice::ReadOnly) && rejected && saved.readAll() == original,
				"Excel writer never overwrites an existing destination");
		}
	}
	chooser.stop();
	bool invalidRejected = false;
	const QString invalidPath = directory.filePath("invalid.xlsx");
	try
	{
		IOExport::WriteExcel(invalidPath, { { { std::numeric_limits<double>::infinity() } } }, false);
	}
	catch (const std::exception&)
	{
		invalidRejected = true;
	}
	check(invalidRejected && !QFile::exists(invalidPath), "invalid Excel measurements leave no partial file");
	check(dialog.summary_label->text().contains("Spheres: 2 / 2"), "export status preserves the campaign summary");

	SphereScanResult missing = scan;
	missing.spheres.pop_back();
	SphereResultsDialog partial({ missing }, reference, directory.path());
	check(
		partial.results_table->item(2, 3)->text() == "Not detected" && partial.results_table->item(3, 4)->text() == "-",
		"missing objects and distances are explicit and never treated as zero errors");
	check(partial.summary_table->item(1, 2)->text() == "-", "missing comparisons have no RMS");
	scan.labeled = false;
	SphereResultsDialog unidentified({ scan }, reference, directory.path());
	check(unidentified.results_table->item(1, 0)->text() == "S1" &&
			unidentified.results_table->item(1, 3)->text() == "10100.0" &&
			unidentified.results_table->item(1, 2)->text() == "No reference" &&
			unidentified.results_table->item(1, 5)->text() == "Unavailable",
		"unidentified spheres retain measurements without inventing artifact matches");
	SphereResultsDialog noReference({ scan }, {}, directory.path());
	check(!noReference.after_scale_check->isEnabled() && noReference.results_table->isColumnHidden(5) &&
			!unidentified.after_scale_check->isEnabled(),
		"scale correction is disabled when there are no valid reference comparisons");
	check(noReference.results_table->rowCount() == 4 && noReference.summary_table->item(2, 2)->text() == "-",
		"measurements without an artifact remain available without false error statistics");
	SphereResultsDialog empty({}, {}, directory.path());
	check(empty.results_table->rowCount() == 0 && !empty.centers_button->isEnabled(), "empty campaign is safe");

	scan.labeled = true;
	SphereScanResult second = scan;
	for (auto& sphere : second.spheres)
	{
		sphere.radius *= 1.01;
		sphere.center *= 1.01;
	}
	SphereResultsDialog campaign({ scan, second }, reference, directory.path());
	check(campaign.validation_label->text().contains("Validation RMS:"),
		"multiple usable clouds enable held-out validation");
	const QString validationUm = campaign.validation_label->text();
	campaign.show_mm_check->setChecked(true);
	check(validationUm != campaign.validation_label->text() && campaign.validation_label->text().contains(" mm "),
		"validation RMS follows the selected display unit");
	const QString forbidden = QString(QChar(0x00b7));
	SphereResultsDialog sanitized({ scan }, reference, directory.path(), "note" + forbidden + "text");
	check(!sanitized.note_label->text().contains(forbidden), "displayed notes omit the prohibited separator");
	for (auto* label : sanitized.findChildren<QLabel*>())
		check(!label->text().contains(forbidden), "result labels contain no prohibited separator");

	// Rows of a scan, as the table shows them.
	{
		auto named = reference;
		named.name = "Ball bar";
		const auto rows = Pointcloud::ScanMeasurands(scan, named);
		check(rows.size() == 3 && rows[0].target == "E1" && rows[0].diameter && rows[0].formRms &&
				rows[2].target == "E1 / E2" && !rows[2].diameter && !rows[2].formRms && rows[2].reference == 100,
			"rows follow the table: diameters, then named distances");
	}
	return ok;
}
