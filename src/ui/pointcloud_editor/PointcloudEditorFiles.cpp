// PointcloudEditorDialog, part 4 of 4: files. Adding .xyz clouds from the workspace (with a cache
// of the ones already parsed), exporting layers, and saving and opening editor projects (.smcppce).
// Reading and writing happen on a worker thread; exports never overwrite an existing file.

#include "ui/pointcloud_editor/PointcloudEditorDialog.h"

#include <algorithm>
#include <iterator>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLayout>
#include <QDialogButtonBox>
#include <QToolButton>
#include <QShortcut>
#include <QFileInfo>
#include <QLocale>
#include <QMatrix4x4>
#include <QMessageBox>
#include <QtMath>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTimer>

#include "core/Compensation.h"
#include "export/IOExport.h"
#include "ui/pointcloud_editor/AiInferenceProgressDialog.h"
#include "ui/pointcloud_editor/PointcloudProject.h"
#include "ui/pointcloud_editor/AiModelConfigDialog.h"
#include "ui/pointcloud_editor/ExportPointcloudDialog.h"
#include "ui/pointcloud_editor/IcpDialog.h"
#include "ui/pointcloud_editor/PlaneFitDialog.h"
#include "ui/pointcloud_editor/RemoveOutliersDialog.h"
#include "ui/pointcloud_editor/SphereFitDialog.h"
#include "ui/pointcloud_editor/SphereResultsDialog.h"
#include "ui/widgets/EditorDialogStyle.h"

namespace smcp
{
	namespace
	{
		/// Memory allowed for parsed clouds kept in the cache (~30 million points).
		constexpr std::size_t Cloud_Cache_Budget_Bytes = 512 * 1024 * 1024;

		QString ExportFilename(QString cloudName, int index, QSet<QString>& usedNames)
		{
			cloudName = cloudName.trimmed();
			if (cloudName.endsWith(".xyz", Qt::CaseInsensitive))
			{
				cloudName.chop(4);
			}
			cloudName.replace(QRegularExpression("[<>:\"/\\\\|?*]"), "_");
			while (cloudName.endsWith('.') || cloudName.endsWith(' '))
			{
				cloudName.chop(1);
			}
			if (cloudName.isEmpty())
			{
				cloudName = QString("Cloud %1").arg(index + 1);
			}

			QString filename = cloudName + ".xyz";
			int duplicate = 2;
			while (usedNames.contains(filename.toCaseFolded()))
			{
				filename = QString("%1_%2.xyz").arg(cloudName).arg(duplicate++);
			}
			usedNames.insert(filename.toCaseFolded());
			return filename;
		}

		bool CanExport(QWidget* parent, const QStringList& paths)
		{
			QString error;
			if (IOExport::ValidateNewFiles(paths, error))
				return true;
			QMessageBox::warning(parent, QObject::tr("Cannot export"), error);
			return false;
		}

		QString EnsureXyzExtension(QString filename)
		{
			if (!filename.endsWith(".xyz", Qt::CaseInsensitive))
			{
				filename += ".xyz";
			}
			return filename;
		}
	} // namespace

	void PointcloudEditorDialog::on_load_pointcloud_button_clicked(bool)
	{
		const QStringList names = QDir(_rootDir).entryList({ "*.xyz" }, QDir::Files, QDir::Name);
		if (names.isEmpty())
		{
			SetStatus(tr("No XYZ point clouds in the workspace."));
			return;
		}
		QInputDialog chooser(this);
		chooser.setWindowTitle(tr("Add point cloud"));
		chooser.setLabelText(tr("Workspace point cloud:"));
		chooser.setComboBoxItems(names);
		chooser.setComboBoxEditable(false);
		chooser.setOkButtonText(tr("Load"));
		chooser.layout()->setSizeConstraint(QLayout::SetMinimumSize);
		chooser.setMinimumWidth(600);
		chooser.resize(640, chooser.sizeHint().height());
		if (auto* buttons = chooser.findChild<QDialogButtonBox*>())
			buttons->setCenterButtons(true);
		if (chooser.exec() != QDialog::Accepted)
			return;
		const QString name = chooser.textValue();
		if (name.isEmpty())
			return;
		const QString path = QDir(_rootDir).absoluteFilePath(name);
		const QFileInfo info(path);
		const qint64 fileSize = info.size();
		const QDateTime lastModified = info.lastModified();
		const Pointcloud::ColorCloudPtr cached = FindCachedPointcloud(path, fileSize, lastModified);

		// The file is read on the worker thread; the viewer builds the GPU buffer once, when it publishes
		// the command. The result travels in a shared_ptr so leaving the QFuture copies no point data.
		RunAsync<std::shared_ptr<LoadedPointcloud>>(
			tr("Loading point cloud..."),
			[filepath = path.toStdString(), cached]()
			{
				auto result = std::make_shared<LoadedPointcloud>();
				result->cloud = cached;
				if (!result->cloud)
				{
					auto cloud = std::make_shared<Pointcloud::ColorCloud>();
					if (!IOExport::ReadXyz(filepath, *cloud))
					{
						return result;
					}
					result->cloud = std::move(cloud);
				}
				result->hasFinitePoints = std::any_of(result->cloud->begin(), result->cloud->end(),
					[](const Pointcloud::ColorPoint& point) { return Pointcloud::IsFinite(point); });
				return result;
			},
			[this, name, path, fileSize, lastModified, cached](const std::shared_ptr<LoadedPointcloud>& result)
			{
				if (!result->cloud)
				{
					SetStatus(tr("Error: could not open %1.").arg(name));
					return;
				}
				if (!cached)
				{
					StoreCachedPointcloud(path, fileSize, lastModified, result->cloud);
				}
				const int addedId = result->hasFinitePoints
					? preview_widget->AddPointcloud(result->cloud, QColor(), name,
						  tr("Loaded from %1").arg(QDir::toNativeSeparators(path)))
					: -1;
				if (addedId < 0)
				{
					SetStatus(tr("The file has no points."));
					QMessageBox::information(
						this, tr("No points"), tr("The file %1 contains no points to display.").arg(name));
					return;
				}
				layers_panel->SelectLayer(addedId);
				SetStatus(tr("Point cloud added (%1 points).").arg(result->cloud->size()));
			});
	}

	Pointcloud::ColorCloudPtr PointcloudEditorDialog::FindCachedPointcloud(
		const QString& path, qint64 fileSize, const QDateTime& lastModified)
	{
		const auto it = _cloudCache.find(path);
		if (it == _cloudCache.end())
		{
			return nullptr;
		}
		if (it->second.fileSize != fileSize || it->second.lastModified != lastModified)
		{
			_cloudCache.erase(it);
			return nullptr;
		}
		it->second.lastUse = ++_cacheUseCounter;
		return it->second.cloud;
	}

	void PointcloudEditorDialog::StoreCachedPointcloud(
		const QString& path, qint64 fileSize, const QDateTime& lastModified, const Pointcloud::ColorCloudPtr& cloud)
	{
		_cloudCache[path] = CachedPointcloud{ cloud, fileSize, lastModified, ++_cacheUseCounter };

		// Evict least recently used clouds beyond the memory budget, always keeping the one just stored.
		const auto bytesOf = [](const CachedPointcloud& entry)
		{ return entry.cloud->size() * sizeof(Pointcloud::ColorPoint); };
		std::size_t totalBytes = 0;
		for (const auto& entry : _cloudCache)
		{
			totalBytes += bytesOf(entry.second);
		}
		while (totalBytes > Cloud_Cache_Budget_Bytes && _cloudCache.size() > 1)
		{
			auto oldest = _cloudCache.end();
			for (auto it = _cloudCache.begin(); it != _cloudCache.end(); ++it)
			{
				if (it->first != path && (oldest == _cloudCache.end() || it->second.lastUse < oldest->second.lastUse))
				{
					oldest = it;
				}
			}
			totalBytes -= bytesOf(oldest->second);
			_cloudCache.erase(oldest);
		}
	}

	void PointcloudEditorDialog::on_save_button_clicked(bool)
	{
		if (IsWorking())
			return;
		const auto clouds = SelectedClouds();
		if (clouds.empty())
		{
			SetStatus(tr("Select visible point clouds to export."));
			return;
		}
		ExportPointcloudDialog dialog(this);
		dialog.first_radio->setEnabled(clouds.size() == 1);
		if (clouds.size() > 1)
			dialog.multiple_radio->setChecked(true);
		if (dialog.exec() != QDialog::Accepted)
			return;
		const auto mode = dialog.SelectedMode();
		QStringList targets;
		if (mode == ExportPointcloudDialog::Mode::Multiple)
		{
			const auto directory = QFileDialog::getExistingDirectory(this, tr("Export point clouds"), _rootDir,
				QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
			if (directory.isEmpty())
				return;
			QSet<QString> names;
			for (int i = 0; i < static_cast<int>(clouds.size()); ++i)
				targets << QDir(directory).filePath(ExportFilename(clouds[i].name, i, names));
		}
		else
		{
			const auto path = QFileDialog::getSaveFileName(this, tr("Export point clouds"),
				QDir(_rootDir).filePath(
					mode == ExportPointcloudDialog::Mode::Combine ? "combined.xyz" : clouds.front().name),
				tr("XYZ (*.xyz)"), nullptr, QFileDialog::DontConfirmOverwrite | QFileDialog::DontUseNativeDialog);
			if (path.isEmpty())
				return;
			targets << EnsureXyzExtension(path);
		}
		if (!CanExport(this, targets))
			return;
		RunAsync<QString>(
			tr("Export point clouds"),
			[clouds, targets, mode]()
			{
				std::vector<Pointcloud::ColorCloudPtr> outputs;
				if (mode == ExportPointcloudDialog::Mode::Combine)
				{
					auto merged = std::make_shared<Pointcloud::ColorCloud>();
					std::size_t total = 0;
					for (const auto& c : clouds)
						total += c.cloud->size();
					merged->reserve(total);
					for (const auto& c : clouds)
						merged->insert(merged->end(), c.cloud->begin(), c.cloud->end());
					outputs.push_back(merged);
				}
				else if (mode == ExportPointcloudDialog::Mode::First)
					outputs.push_back(clouds.front().cloud);
				else
					for (const auto& c : clouds)
						outputs.push_back(c.cloud);
				for (int i = 0; i < targets.size(); ++i)
					if (!IOExport::WriteXyz(targets[i].toStdString(), *outputs[i]))
						throw std::runtime_error(QObject::tr("Cannot export %1. Existing files were not overwritten; "
															 "earlier files in the batch may have been created.")
								.arg(targets[i])
								.toStdString());
				return QObject::tr("Exported %1 file(s).").arg(targets.size());
			},
			[this](const QString& message) { SetStatus(message); });
	}

	void PointcloudEditorDialog::on_save_project_action_triggered(bool)
	{
		SaveProject(false, false);
	}

	void PointcloudEditorDialog::on_save_project_as_action_triggered(bool)
	{
		SaveProject(true, false);
	}

	void PointcloudEditorDialog::SaveProject(bool chooseFile, bool closeAfterSave)
	{
		if (IsWorking())
			return;
		QString path = _projectPath;
		if (chooseFile || path.isEmpty())
		{
			QFileDialog picker(this, tr("Save editor project"),
				path.isEmpty() ? QDir(_rootDir).filePath("project.smcppce") : path,
				tr("SMCP Point Cloud Editor (*.smcppce)"));
			picker.setOption(QFileDialog::DontUseNativeDialog);
			picker.setAcceptMode(QFileDialog::AcceptSave);
			picker.setDefaultSuffix("smcppce");
			if (picker.exec() != QDialog::Accepted)
				return;
			path = picker.selectedFiles().first();
			if (!path.endsWith(".smcppce", Qt::CaseInsensitive))
			{
				path += ".smcppce";
				if (QFileInfo::exists(path) &&
					QMessageBox::question(this, tr("Save editor project"),
						tr("Replace the existing project \"%1\"?").arg(QDir::toNativeSeparators(path)),
						QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save)
					return;
			}
		}
		StopTool();
		SetSelectionTool(false);
		const auto project = preview_widget->CaptureProject();
		SetBusy(true);
		SetStatus(tr("Saving editor project..."));
		RunBackground<QString>(
			this,
			[path, project]()
			{
				PointcloudProject::Save(path, project);
				return path;
			},
			[this, closeAfterSave](const QString& savedPath)
			{
				_projectPath = savedPath;
				setWindowTitle(tr("Point Cloud Editor - %1[*]").arg(QFileInfo(savedPath).fileName()));
				SetModified(false);
				SetStatus(tr("Project saved: %1").arg(QDir::toNativeSeparators(savedPath)));
				// Closes once the busy state is cleared, so reject() is not refused as a running command.
				if (closeAfterSave)
					QTimer::singleShot(0, this,
						[this]()
						{
							if (!_modified && !IsWorking())
								QDialog::reject();
						});
			},
			[this](std::exception_ptr error) { FailTask(error); }, [this]() { SetBusy(false); });
	}

	void PointcloudEditorDialog::on_open_project_action_triggered(bool)
	{
		if (IsWorking())
			return;
		const QString path = QFileDialog::getOpenFileName(this, tr("Open editor project"),
			_projectPath.isEmpty() ? _rootDir : _projectPath, tr("SMCP Point Cloud Editor (*.smcppce)"), nullptr,
			QFileDialog::DontUseNativeDialog);
		if (path.isEmpty())
			return;
		if ((!preview_widget->Layers().empty() || preview_widget->HistoryLabels().size() > 1) &&
			QMessageBox::question(this, tr("Open editor project"),
				tr("Replace the current editor session with this project? Unsaved work in this session will be lost."),
				QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
			return;
		SetBusy(true);
		SetStatus(tr("Opening editor project..."));
		RunBackground<PointcloudPreviewWidget::ProjectState>(
			this, [path]() { return PointcloudProject::Load(path); },
			[this, path](PointcloudPreviewWidget::ProjectState project)
			{
				StopTool();
				SetSelectionTool(false);
				_openingProjectPath = path;
				preview_widget->RestoreProject(std::move(project));
			},
			[this](std::exception_ptr error) { FailTask(error); }, [this]() { SetBusy(false); });
	}
} // namespace smcp
