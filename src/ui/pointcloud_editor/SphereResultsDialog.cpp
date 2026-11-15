#include "ui/pointcloud_editor/SphereResultsDialog.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include "export/IOExport.h"
#include "export/ExcelExport.h"
#include "core/AsyncTask.h"
#include "common/Settings.h"
#include <QSettings>
#include <QApplication>
#include <stdexcept>
#include <QFileInfo>
#include <QFont>
#include <QHeaderView>
#include <QPainter>
#include <QStyleOptionHeader>
#include <QStyledItemDelegate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTableWidgetItem>
#include <QTextStream>

#include <QRegularExpression>

namespace smcp
{

	namespace
	{
		/// Mean and RMS of the errors gathered for one measurand.
		struct ErrorStats
		{
			double sum = 0.0;
			double sumSquares = 0.0;
			int count = 0;

			void Add(double error)
			{
				sum += error;
				sumSquares += error * error;
				++count;
			}
			double Mean() const { return count > 0 ? sum / count : 0.0; }
			double Rms() const { return count > 0 ? std::sqrt(sumSquares / count) : 0.0; }
		};

		QString Number(double value, int decimals)
		{
			return QString::number(value, 'f', decimals);
		}

		QString DisplayText(QString text)
		{
			return text.replace(QChar(0x00b7), ' ');
		}

		// Two-level header: each column in `groups` and the one after it form a pair (value and
		// error, or mean and RMS) under the title of their comparison, so the pair stays together
		// while the table scrolls or resizes. The table has no vertical lines; the underline of each
		// comparison title, a little shorter than its two columns, is what brackets them.
		class ComparisonHeader : public QHeaderView
		{
		public:
			ComparisonHeader(std::array<int, 2> groups, QWidget* parent) :
				QHeaderView(Qt::Horizontal, parent), _groups(groups)
			{
				setSectionResizeMode(QHeaderView::Stretch);
				setMinimumSectionSize(80);
				setSectionsClickable(false);
				setHighlightSections(false);
			}
			QSize sizeHint() const override { return QSize(QHeaderView::sizeHint().width(), 64); }

		protected:
			void paintSection(QPainter* painter, const QRect& rect, int index) const override
			{
				bool paired = false;
				for (const int first : _groups)
					paired |= index == first || index == first + 1;
				if (!paired)
				{
					QHeaderView::paintSection(painter, rect, index);
					return;
				}
				Draw(painter,
					QRect(rect.x(), rect.y() + rect.height() / 2, rect.width(), rect.height() - rect.height() / 2),
					model()->headerData(index, Qt::Horizontal, Qt::UserRole).toString(),
					Qt::AlignRight | Qt::AlignVCenter);
			}
			void paintEvent(QPaintEvent* event) override
			{
				QHeaderView::paintEvent(event);
				QPainter painter(viewport());
				const QString titles[] = { tr("Original"), tr("Scale corrected") };
				for (std::size_t group = 0; group < _groups.size(); ++group)
				{
					const int column = _groups[group];
					if (column + 1 >= count() || isSectionHidden(column))
						continue;
					Draw(&painter,
						QRect(sectionViewportPosition(column) + Group_Gap, 0,
							sectionSize(column) + sectionSize(column + 1) - Group_Gap, height() / 2),
						titles[group], Qt::AlignCenter);
				}
			}

		private:
			void Draw(QPainter* painter, const QRect& rect, const QString& text, Qt::Alignment alignment) const
			{
				QStyleOptionHeader option;
				initStyleOption(&option);
				option.rect = rect;
				option.text = text;
				option.textAlignment = alignment;
				style()->drawControl(QStyle::CE_Header, &option, painter, this);
			}
			static constexpr int Group_Gap = 12;  ///< Pixels between the underlines of two comparisons.
			std::array<int, 2> _groups;			  ///< First column of Original and of Scale corrected.
		};

		// First column of each comparison (original, scale corrected).
		constexpr std::array<int, 2> Results_Groups = { 3, 5 };
		constexpr std::array<int, 2> Summary_Groups = { 1, 3 };

		// The theme draws the cells (padding and the hairline under each row), and a styled cell
		// ignores the background of its item. This delegate paints that background first, so the
		// title row of each cloud keeps its tint.
		class RowTintDelegate : public QStyledItemDelegate
		{
		public:
			using QStyledItemDelegate::QStyledItemDelegate;
			void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
			{
				const QVariant background = index.data(Qt::BackgroundRole);
				if (background.canConvert<QBrush>())
					painter->fillRect(option.rect, background.value<QBrush>());
				QStyledItemDelegate::paint(painter, option, index);
			}
		};

		/// The sphere carrying `label` in a labeled scan, or the `label`-th one of an unlabeled scan.
		const Pointcloud::MeasuredSphere* SphereAt(const SphereScanResult& scan, int label)
		{
			if (scan.labeled)
			{
				for (const auto& sphere : scan.spheres)
				{
					if (sphere.label == label)
						return &sphere;
				}
				return nullptr;
			}
			return label < static_cast<int>(scan.spheres.size()) ? &scan.spheres[label] : nullptr;
		}

		QString SafeFileName(QString name)
		{
			if (name.endsWith(".xyz", Qt::CaseInsensitive))
				name.chop(4);
			name.replace(QRegularExpression("[<>:\"/\\\\|?*]"), "_");
			return name.isEmpty() ? QString("scan") : name;
		}
	} // namespace

	SphereResultsDialog::SphereResultsDialog(std::vector<SphereScanResult> results,
		Pointcloud::ArtifactReference reference, QString defaultDirectory, QString note, QWidget* parent) :
		QDialog(parent, Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint),
		_results(std::move(results)),
		_reference(std::move(reference)),
		_defaultDirectory(std::move(defaultDirectory)),
		_note(std::move(note))
	{
		setupUi(this);
		results_table->setHorizontalHeader(new ComparisonHeader(Results_Groups, results_table));
		summary_table->setHorizontalHeader(new ComparisonHeader(Summary_Groups, summary_table));
		results_table->setItemDelegate(new RowTintDelegate(results_table));
		header_layout->setSpacing(16);
		for (auto* table : { results_table, summary_table })
		{
			// Fluent data table (docs/STYLE.md): rows divided by hairlines from the theme, no grid and
			// no zebra; the cells cannot be edited, so none of them shows a focus rectangle.
			table->verticalHeader()->hide();
			table->verticalHeader()->setDefaultSectionSize(32);
			table->setWordWrap(false);
			table->setShowGrid(false);
			table->setAlternatingRowColors(false);
			table->setFocusPolicy(Qt::NoFocus);
		}
		summary_table->setFixedHeight(64 + 3 * 32 + 2);  // header, three rows and the frame
		header_layout->setStretch(0, 1);
		verticalLayout->setStretch(1, 1);
		analysis_layout->setStretch(0, 1);
		analysis_layout->setStretch(1, 1);
		note_label->setText(DisplayText(_note));
		note_label->setVisible(!_note.isEmpty());
		status_label->hide();
		AnalyzeCampaign();
		FillTable();
		// The summary names its rows in the first column, which must never be cut short (the columns
		// exist only after the first fill).
		summary_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
		after_scale_check->setEnabled(_hasScale);
		on_after_scale_check_toggled(after_scale_check->isChecked());
	}

	SphereResultsDialog::~SphereResultsDialog() = default;

	QStringList SphereResultsDialog::SphereNames() const
	{
		QStringList names;
		if (HasReference())
		{
			for (const auto& name : _reference.names)
				names << QString::fromStdString(name);
			return names;
		}
		std::size_t count = 0;
		for (const auto& scan : _results)
			count = std::max(count, scan.spheres.size());
		for (std::size_t i = 0; i < count; ++i)
			names << QString("S%1").arg(i + 1);
		return names;
	}

	void SphereResultsDialog::FillTable()
	{
		const bool millimeters = show_mm_check->isChecked();
		const QString unit = millimeters ? "mm" : QString::fromUtf8("µm");
		const double multiplier = millimeters ? 1.0 : 1000.0;
		const auto length = [=](double value) { return Number(value * multiplier, millimeters ? 4 : 1); };
		// `textColumns` leading columns hold text; the rest hold numbers. The two columns of each
		// comparison in `groups` get the short caption drawn under the comparison title.
		const auto headers = [&](QTableWidget* table, QStringList labels, int textColumns,
								 const std::array<int, 2>& groups, const QStringList& measures)
		{
			table->setColumnCount(labels.size());
			table->setHorizontalHeaderLabels(labels);
			// A header is aligned like the cells below it: text to the left, numbers to the right.
			for (int column = 0; column < labels.size(); ++column)
				table->horizontalHeaderItem(column)->setTextAlignment(
					(column < textColumns ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter);
			for (const int first : groups)
				for (int i = 0; i < 2; ++i)
					table->horizontalHeaderItem(first + i)->setData(Qt::UserRole, measures[i] + " (" + unit + ")");
		};
		headers(results_table,
			{ tr("Object"), tr("Measurement"), tr("Reference\n(%1)").arg(unit), tr("Original value (%1)").arg(unit),
				tr("Original error (%1)").arg(unit), tr("Corrected value (%1)").arg(unit),
				tr("Corrected error (%1)").arg(unit), tr("Form RMS\n(%1)").arg(unit) },
			2, Results_Groups, { tr("Value"), tr("Error") });
		headers(summary_table,
			{ tr("Measurement"), tr("Original mean (%1)").arg(unit), tr("Original RMS (%1)").arg(unit),
				tr("Corrected mean (%1)").arg(unit), tr("Corrected RMS (%1)").arg(unit) },
			1, Summary_Groups, { tr("Mean"), tr("RMS") });
		results_table->clearSpans();
		results_table->setRowCount(0);
		summary_table->setRowCount(3);
		const auto set = [](QTableWidget* table, int row, int column, const QString& text, bool numeric)
		{
			auto* item = new QTableWidgetItem(DisplayText(text));
			item->setTextAlignment((numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter);
			item->setToolTip(item->text());
			table->setItem(row, column, item);
		};
		ErrorStats before[3], after[3];
		std::size_t found = 0;
		for (int resultIndex = 0; resultIndex < static_cast<int>(_results.size()); ++resultIndex)
		{
			const auto& scan = _results[resultIndex];
			found += scan.spheres.size();
			const int group = results_table->rowCount();
			results_table->insertRow(group);
			results_table->setSpan(group, 0, 1, results_table->columnCount());
			const QString detection = HasReference() ? tr("%1 / %2 spheres found%3")
														   .arg(scan.spheres.size())
														   .arg(_reference.Count())
														   .arg(scan.labeled ? QString() : tr("; unidentified"))
													 : tr("%1 spheres found; no reference").arg(scan.spheres.size());
			set(results_table, group, 0, scan.name + " | " + detection, false);
			// The row of each cloud is told apart by its background; the theme uses a single font weight.
			auto* groupItem = results_table->item(group, 0);
			groupItem->setBackground(QColor(0xf3, 0xf3, 0xf3));
			groupItem->setFlags(Qt::ItemIsEnabled);  // a title, not a measurement: it cannot be selected
			groupItem->setToolTip(DisplayText(scan.source));
			const auto append = [&](const QString& object, bool diameter, std::optional<double> measured,
									double nominal, std::optional<double> form)
			{
				const int row = results_table->rowCount();
				results_table->insertRow(row);
				set(results_table, row, 0, object, false);
				set(results_table, row, 1, diameter ? tr("Diameter") : tr("Center distance"), false);
				set(results_table, row, 2, nominal > 0.0 ? length(nominal) : tr("No reference"), true);
				set(results_table, row, 3, measured ? length(*measured) : tr("Not detected"), true);
				set(results_table, row, 4, measured && nominal > 0.0 ? length(*measured - nominal) : "-", true);
				set(results_table, row, 5,
					!_hasScale	   ? tr("Unavailable")
						: measured ? length(*measured * _fixedScale)
								   : "-",
					true);
				set(results_table, row, 6,
					measured && nominal > 0.0 && _hasScale ? length(*measured * _fixedScale - nominal) : "-", true);
				set(results_table, row, 7, form ? length(*form) : "-", true);
				if (measured && nominal > 0.0)
				{
					for (const int category : { diameter ? 0 : 1, 2 })
					{
						before[category].Add(*measured - nominal);
						if (_hasScale)
							after[category].Add(*measured * _fixedScale - nominal);
					}
				}
			};
			for (const auto& row : Pointcloud::ScanMeasurands(scan, _reference))
				append(row.target, row.diameter, row.measured, row.reference, row.formRms);
			if (scan.spheres.empty())
				groupItem->setText(groupItem->text() + tr("; no spheres detected"));
		}
		const QStringList categories = { tr("Diameters"), tr("Center distances"), tr("All measurements") };
		for (int row = 0; row < 3; ++row)
		{
			set(summary_table, row, 0, categories[row], false);
			for (int group = 0; group < 2; ++group)
			{
				const auto& stats = group == 0 ? before[row] : after[row];
				set(summary_table, row, 1 + group * 2, stats.count ? length(stats.Mean()) : "-", true);
				set(summary_table, row, 2 + group * 2, stats.count ? length(stats.Rms()) : "-", true);
				for (int column = 1 + group * 2; column <= 2 + group * 2; ++column)
					summary_table->item(row, column)->setToolTip(tr("%1 reference comparisons").arg(stats.count));
			}
		}
		summary_label->setText(tr("%1 point cloud(s) | Spheres: %2 | Scale k: %3")
				.arg(_results.size())
				.arg(HasReference() ? QString("%1 / %2").arg(found).arg(_results.size() * _reference.Count())
									: QString::number(found))
				.arg(_hasScale ? Number(_fixedScale, 6) : tr("unavailable")));
		validation_label->setText(!_hasScale ? tr("Scale: no identified measurements with a reference.")
				: _validationRms ? tr("Validation RMS: %1 %2 (each cloud corrected using the others).")
									   .arg(length(*_validationRms), unit)
								 : tr("Scale fitted to these results. At least two identified clouds are needed for "
									  "independent validation."));
		const auto range = _coverage.Range();
		QStringList warnings;
		const char* axes[] = { "X", "Y", "Z" };
		for (int axis = 0; axis < 3; ++axis)
			if (_coverageScans >= 2 && range[axis] < 2.0 * _meanDiameter)
				warnings << tr("Limited %1 variation").arg(axes[axis]);
		if (std::abs(_coverage.correlationXY) >= 0.9)
			warnings << tr("X/Y correlated");
		if (std::abs(_coverage.correlationXZ) >= 0.9)
			warnings << tr("X/Z correlated");
		if (std::abs(_coverage.correlationYZ) >= 0.9)
			warnings << tr("Y/Z correlated");
		coverage_label->setText(tr("Coverage (%1): X %2 | Y %3 | Z %4\n%5")
				.arg(unit, length(range[0]), length(range[1]), length(range[2]))
				.arg(_coverageScans < 2 ? tr("Measure more positions to evaluate volume coverage.")
						: warnings.isEmpty()
						? tr("%1 centers across %2 clouds.").arg(_coverage.count).arg(_coverageScans)
						: warnings.join("; ")));
		coverage_label->setToolTip(tr(
			"Camera frame; %1 centers in %2 clouds. Correlations: XY %3, XZ %4, YZ %5. Vary each axis independently.")
				.arg(_coverage.count)
				.arg(_coverageScans)
				.arg(Number(_coverage.correlationXY, 3), Number(_coverage.correlationXZ, 3),
					Number(_coverage.correlationYZ, 3)));
	}

	void SphereResultsDialog::on_export_csv_button_clicked(bool)
	{
		QString path = QFileDialog::getSaveFileName(this, tr("Export sphere measurements"),
			QDir(_defaultDirectory).filePath("spheres.csv"), tr("CSV (*.csv)"), nullptr,
			QFileDialog::DontConfirmOverwrite | QFileDialog::DontUseNativeDialog);
		if (path.isEmpty())
		{
			return;
		}
		if (!path.endsWith(".csv", Qt::CaseInsensitive))
			path += ".csv";

		QString error;
		if (!IOExport::ValidateNewFiles({ path }, error))
		{
			QMessageBox::warning(this, tr("Cannot export"), error);
			return;
		}
		const auto results = _results;
		const auto artifact = _reference;
		const auto names = SphereNames();
		const bool millimeters = show_mm_check->isChecked();
		RunExport(
			[results, artifact, names, path, millimeters]() -> QString
			{
				QFile file(path);
				if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text))
				{
					throw std::runtime_error((tr("Cannot create %1. Existing files will not be overwritten. Check the "
												 "destination and permissions.")
												  .arg(QDir::toNativeSeparators(path)))
							.toStdString());
				}
				QTextStream out(&file);
				out.setCodec("UTF-8");

				const QString unit = millimeters ? "mm" : "um";
				const auto length = [=](double value)
				{ return Number(value * (millimeters ? 1.0 : 1000.0), millimeters ? 6 : 3); };
				const auto quote = [](QString value) { return "\"" + DisplayText(value).replace("\"", "\"\"") + "\""; };
				const bool reference = artifact.Count() > 0;
				QStringList header = { "scan", "source", "spheres_found", "labeled" };
				for (const QString& n : names)
				{
					header << n + "_x_" + unit << n + "_y_" + unit << n + "_z_" + unit << n + "_diameter_" + unit;
					if (reference)
						header << n + "_diameter_error_" + unit;
					header << n + "_form_rms_" + unit << n + "_form_range_" + unit << n + "_points";
				}
				for (int i = 0; i < names.size(); ++i)
				{
					for (int j = i + 1; j < names.size(); ++j)
					{
						header << names[i] + "_" + names[j] + "_distance_" + unit;
						if (reference)
							header << names[i] + "_" + names[j] + "_distance_error_" + unit;
					}
				}
				for (auto& title : header)
					title = quote(title);
				out << header.join(",") << "\n";

				for (std::size_t scanIndex = 0; scanIndex < results.size(); ++scanIndex)
				{
					const SphereScanResult& scan = results[scanIndex];
					const bool usable = !reference || scan.labeled;
					QStringList row = { quote(scan.name), quote(QDir::toNativeSeparators(scan.source)),
						QString::number(scan.spheres.size()), scan.labeled ? "1" : "0" };
					for (int k = 0; k < names.size(); ++k)
					{
						const auto* s = usable ? SphereAt(scan, k) : nullptr;
						const double nominal = reference ? artifact.diameters[k] : 0.0;
						row << (s ? length(s->center[0]) : "") << (s ? length(s->center[1]) : "")
							<< (s ? length(s->center[2]) : "") << (s ? length(s->Diameter()) : "");
						if (reference)
							row << (s && nominal > 0.0 ? length(s->Diameter() - nominal) : "");
						row << (s ? length(s->formRms) : "") << (s ? length(s->formRange) : "")
							<< (s ? QString::number(s->points->size()) : "");
					}
					for (int i = 0; i < names.size(); ++i)
					{
						for (int j = i + 1; j < names.size(); ++j)
						{
							const auto* a = usable ? SphereAt(scan, i) : nullptr;
							const auto* b = usable ? SphereAt(scan, j) : nullptr;
							const double distance = a && b ? cv::norm(a->center - b->center) : 0.0;
							row << (a && b ? length(distance) : "");
							if (reference)
							{
								const double nominal = artifact.distances[i][j];
								row << (a && b && nominal > 0.0 ? length(distance - nominal) : "");
							}
						}
					}
					out << row.join(",") << "\n";
				}
				out.flush();
				if (out.status() != QTextStream::Ok || !file.flush())
				{
					file.close();
					file.remove();
					throw std::runtime_error((tr("The CSV could not be written completely.")).toStdString());
				}
				return tr("Exported %1 scan(s) to %2.").arg(results.size()).arg(QDir::toNativeSeparators(path));
			});
	}

	void SphereResultsDialog::on_export_excel_button_clicked(bool)
	{
		QString path = QFileDialog::getSaveFileName(this, tr("Export sphere measurements to Excel"),
			QDir(_defaultDirectory).filePath("spheres.xlsx"), tr("Excel workbook (*.xlsx)"), nullptr,
			QFileDialog::DontConfirmOverwrite | QFileDialog::DontUseNativeDialog);
		if (path.isEmpty())
			return;
		if (!path.endsWith(".xlsx", Qt::CaseInsensitive))
			path += ".xlsx";
		QString error;
		if (!IOExport::ValidateNewFiles({ path }, error))
		{
			QMessageBox::warning(this, tr("Cannot export"), error);
			return;
		}

		const bool millimeters = show_mm_check->isChecked();
		QVector<IOExport::ExcelRow> rows = { { { summary_label->text() }, true, true },
			{ { tr("Units: %1 | Scale correction (k): %2")
					  .arg(millimeters ? "mm" : QString::fromUtf8("µm"),
						  after_scale_check->isChecked() ? tr("shown") : tr("hidden")) },
				false, true },
			{} };
		const auto appendTable = [&](QTableWidget* table, int numericFirst)
		{
			IOExport::ExcelRow header;
			header.heading = true;
			for (int column = 0; column < table->columnCount(); ++column)
				if (!table->isColumnHidden(column))
					header.values << table->horizontalHeaderItem(column)->text().replace('\n', ' ');
			rows << header;
			for (int row = 0; row < table->rowCount(); ++row)
			{
				IOExport::ExcelRow output;
				output.merged = output.heading = table->columnSpan(row, 0) > 1;
				for (int column = 0; column < table->columnCount(); ++column)
				{
					if (table->isColumnHidden(column) || (output.merged && column > 0))
						continue;
					const auto* item = table->item(row, column);
					const QString text = item ? item->text() : QString();
					bool numeric = false;
					const double value = text.toDouble(&numeric);
					output.values << (column >= numericFirst && numeric ? QVariant(value) : QVariant(text));
				}
				rows << output;
			}
		};
		appendTable(results_table, 2);
		rows << IOExport::ExcelRow{} << IOExport::ExcelRow{ { tr("Error summary") }, true, true };
		appendTable(summary_table, 1);
		rows << IOExport::ExcelRow{} << IOExport::ExcelRow{ { tr("Observations") }, true, true };
		for (const auto* label : { validation_label, coverage_label, note_label })
			if (!label->text().isEmpty())
				rows << IOExport::ExcelRow{ { label->text() }, false, true };
		RunExport(
			[rows = std::move(rows), path, millimeters]()
			{
				IOExport::WriteExcel(path, rows, millimeters);
				return tr("Exported the results to %1.").arg(QDir::toNativeSeparators(path));
			});
	}

	/// Writes, per scan, the points kept by each sphere fit with their label (x y z label, in the scan's own
	/// coordinates), plus manifest.json with the reference and the fitted spheres: the dataset a compensation
	/// model is trained on.
	void SphereResultsDialog::on_export_training_button_clicked(bool)
	{
		const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder for the training data"),
			_defaultDirectory, QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
		if (folder.isEmpty())
		{
			return;
		}
		const auto results = _results;
		const auto artifact = _reference;
		const auto names = SphereNames();
		const auto fixedScale = _fixedScale;
		RunExport(
			[results, artifact, names, fixedScale, folder]() -> QString
			{
				const bool reference = artifact.Count() > 0;

				QStringList targets;
				std::vector<const SphereScanResult*> inputs;
				QSet<QString> usedNames;
				for (const auto& scan : results)
				{
					if (reference && !scan.labeled)
						continue;
					QString base = SafeFileName(scan.name);
					for (int n = 2; usedNames.contains(base.toCaseFolded()); ++n)
						base = SafeFileName(scan.name) + QString("_%1").arg(n);
					usedNames.insert(base.toCaseFolded());
					targets << QDir(folder).filePath(base + ".spheres.xyz");
					inputs.push_back(&scan);
				}
				targets << QDir(folder).filePath("manifest.json");
				QString error;
				if (!IOExport::ValidateNewFiles(targets, error))
				{
					throw std::runtime_error((error).toStdString());
				}
				QJsonArray scans;
				int written = 0;
				for (int index = 0; index < static_cast<int>(inputs.size()); ++index)
				{
					const auto& scan = *inputs[index];
					const QString fileName = QFileInfo(targets[index]).fileName();
					QFile file(QDir(folder).filePath(fileName));
					if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text))
					{
						throw std::runtime_error((tr("Cannot create %1. Existing files will not be overwritten; some "
													 "earlier files may have been "
													 "created.")
													  .arg(QDir::toNativeSeparators(file.fileName())))
								.toStdString());
					}
					QTextStream out(&file);
					QJsonArray spheres;
					for (int k = 0; k < static_cast<int>(scan.spheres.size()); ++k)
					{
						const Pointcloud::MeasuredSphere& sphere = scan.spheres[k];
						const QString label = scan.labeled ? names[sphere.label] : names.value(k);
						for (const auto& p : *sphere.points)
						{
							out << Number(p.x, 6) << ' ' << Number(p.y, 6) << ' ' << Number(p.z, 6) << ' ' << label
								<< '\n';
						}
						spheres.append(QJsonObject{
							{ "label", label },
							{ "center", QJsonArray{ sphere.center[0], sphere.center[1], sphere.center[2] } },
							{ "diameter", sphere.Diameter() },
							{ "form_rms", sphere.formRms },
							{ "points", static_cast<int>(sphere.points->size()) },
						});
					}
					out.flush();
					if (out.status() != QTextStream::Ok || !file.flush())
					{
						file.close();
						file.remove();
						throw std::runtime_error(
							(tr("The sphere points could not be written completely. Check available disk space."))
								.toStdString());
					}
					scans.append(
						QJsonObject{ { "name", scan.name }, { "source", QDir::toNativeSeparators(scan.source) },
							{ "points_file", fileName }, { "spheres", spheres } });
					++written;
				}

				QJsonObject manifest{ { "schema_version", 1 }, { "units", "mm" },
					{ "note",
						"Points are in the coordinates of each scan; for compensation they must be raw reconstructions "
						"(camera frame), not clouds moved or aligned in the editor." },
					{ "scans", scans } };
				if (reference)
				{
					// Starting point for a compensation model's global scale (the a0 term of the PINN).
					manifest.insert("fixed_scale", fixedScale);
				}
				if (reference)
				{
					QJsonArray spheres, distances;
					for (std::size_t i = 0; i < artifact.Count(); ++i)
					{
						spheres.append(QJsonObject{
							{ "name", names[static_cast<int>(i)] }, { "diameter", artifact.diameters[i] } });
						for (std::size_t j = i + 1; j < artifact.Count(); ++j)
						{
							if (artifact.distances[i][j] > 0.0)
							{
								distances.append(QJsonObject{ { "from", names[static_cast<int>(i)] },
									{ "to", names[static_cast<int>(j)] }, { "value", artifact.distances[i][j] } });
							}
						}
					}
					manifest.insert("artifact", QJsonObject{ { "spheres", spheres }, { "distances", distances } });
				}
				QFile manifestFile(QDir(folder).filePath("manifest.json"));
				if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
					manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0)
				{
					throw std::runtime_error((tr("Cannot create manifest.json. Existing files will not be overwritten; "
												 "sphere files may already have "
												 "been created."))
							.toStdString());
				}
				return tr("Exported the sphere points of %1 scan(s) and manifest.json to %2.")
					.arg(written)
					.arg(QDir::toNativeSeparators(folder));
			});
	}

	void SphereResultsDialog::reject()
	{
		if (!_exporting)
			QDialog::reject();
	}
	void SphereResultsDialog::RunExport(std::function<QString()> work)
	{
		if (_exporting)
			return;
		_exporting = true;
		setEnabled(false);
		QApplication::setOverrideCursor(Qt::WaitCursor);
		status_label->show();
		status_label->setText(tr("Exporting..."));
		RunBackground<QString>(
			this, std::move(work), [this](const QString& message) { status_label->setText(DisplayText(message)); },
			[this](std::exception_ptr error)
			{
				QString message = tr("Export failed.");
				try
				{
					std::rethrow_exception(error);
				}
				catch (const std::exception& e)
				{
					message = QString::fromUtf8(e.what());
				}
				catch (...)
				{
				}
				status_label->setText(DisplayText(message));
				QMessageBox::warning(this, tr("Cannot export"), message);
			},
			[this]()
			{
				_exporting = false;
				setEnabled(true);
				QApplication::restoreOverrideCursor();
			});
	}

	void SphereResultsDialog::AnalyzeCampaign()
	{
		std::vector<std::vector<Pointcloud::Measurand>> perScan;
		std::vector<Pointcloud::Measurand> all;
		std::vector<cv::Vec3d> centers;
		double diameterSum = 0.0;
		for (const auto& scan : _results)
		{
			if (HasReference() && !scan.labeled)
				continue;
			if (HasReference())
			{
				auto values = Pointcloud::Measurands(scan.spheres, _reference);
				if (!values.empty())
				{
					all.insert(all.end(), values.begin(), values.end());
					perScan.push_back(std::move(values));
				}
			}
			_coverageScans += scan.spheres.empty() ? 0 : 1;
			for (const auto& sphere : scan.spheres)
			{
				centers.push_back(sphere.center);
				diameterSum += sphere.Diameter();
			}
		}
		_hasScale = !all.empty();
		_fixedScale = Pointcloud::FixedScale(all);
		ErrorStats validation;
		for (std::size_t i = 0; perScan.size() >= 2 && i < perScan.size(); ++i)
		{
			std::vector<Pointcloud::Measurand> others;
			for (std::size_t j = 0; j < perScan.size(); ++j)
				if (i != j)
					others.insert(others.end(), perScan[j].begin(), perScan[j].end());
			const double scale = Pointcloud::FixedScale(others);
			for (const auto& value : perScan[i])
				validation.Add(scale * value.measured - value.reference);
		}
		if (validation.count)
			_validationRms = validation.Rms();
		_coverage = Pointcloud::Coverage(centers);
		_meanDiameter = centers.empty() ? 0.0 : diameterSum / static_cast<double>(centers.size());
		centers_button->setEnabled(!centers.empty());
	}

	void SphereResultsDialog::on_show_mm_check_toggled(bool)
	{
		FillTable();
	}

	void SphereResultsDialog::on_after_scale_check_toggled(bool checked)
	{
		for (int column : { 5, 6 })
			results_table->setColumnHidden(column, !checked);
		for (int column : { 3, 4 })
			summary_table->setColumnHidden(column, !checked);
	}

	/// Each center becomes a small sphere of points (half the fitted radius) in its label's color, so the
	/// campaign's positions can be inspected in the viewer together with the scans.
	void SphereResultsDialog::on_centers_button_clicked(bool)
	{
		const QColor colors[] = { QColor(231, 76, 60), QColor(52, 152, 219), QColor(46, 204, 113), QColor(241, 196, 15),
			QColor(155, 89, 182), QColor(26, 188, 156), QColor(230, 126, 34), QColor(149, 165, 166) };
		constexpr int Samples = 200;
		const double golden = M_PI * (3.0 - std::sqrt(5.0));

		auto cloud = std::make_shared<Pointcloud::ColorCloud>();
		for (const SphereScanResult& scan : _results)
		{
			if (HasReference() && !scan.labeled)
				continue;
			for (std::size_t i = 0; i < scan.spheres.size(); ++i)
			{
				const auto& sphere = scan.spheres[i];
				const QColor color = colors[(scan.labeled ? sphere.label : static_cast<int>(i)) % 8];
				const double radius = 0.5 * sphere.radius;
				for (int s = 0; s < Samples; ++s)
				{
					// Fibonacci sphere: evenly spread directions.
					const double z = 1.0 - 2.0 * (s + 0.5) / Samples;
					const double r = std::sqrt(1.0 - z * z);
					const double angle = golden * s;
					Pointcloud::ColorPoint p;
					p.x = static_cast<float>(sphere.center[0] + radius * r * std::cos(angle));
					p.y = static_cast<float>(sphere.center[1] + radius * r * std::sin(angle));
					p.z = static_cast<float>(sphere.center[2] + radius * z);
					p.r = static_cast<std::uint8_t>(color.red());
					p.g = static_cast<std::uint8_t>(color.green());
					p.b = static_cast<std::uint8_t>(color.blue());
					cloud->push_back(p);
				}
			}
		}
		emit CentersLayerRequested(cloud, tr("sphere_centers"));
		status_label->show();
		status_label->setText(tr("Added the sphere centers to the viewer as the layer \"sphere_centers\"."));
	}

	void SphereResultsDialog::on_close_button_clicked(bool)
	{
		accept();
	}

} // namespace smcp
