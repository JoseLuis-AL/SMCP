#include "ui/pointcloud_editor/PointcloudEditorDialog.h"

#include <iterator>

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QTemporaryDir>

#include "export/IOExport.h"
#include "ui/pointcloud_editor/AiInferenceProgressDialog.h"
#include "ui/pointcloud_editor/AiModelConfigDialog.h"
#include "ui/pointcloud_editor/ExportPointcloudDialog.h"
#include "ui/pointcloud_editor/PlaneFitDialog.h"
#include "ui/pointcloud_editor/RemoveOutliersDialog.h"
#include "ui/pointcloud_editor/SphereFitDialog.h"

namespace smcp
{

namespace
{
	/// Memory allowed for parsed clouds kept in the cache (~30 million points).
	constexpr std::size_t Cloud_Cache_Budget_Bytes = 512 * 1024 * 1024;

	/// Colors used to tell the detected planes apart.
	const QColor Plane_Colors[] = {
		QColor(4, 92, 195), QColor(237, 167, 59), QColor(62, 137, 62), QColor(60, 151, 139),
		QColor(228, 59, 68), QColor(26, 188, 156), QColor(230, 126, 34), QColor(149, 165, 166)
	};

	/// Colors used to tell the detected spheres apart.
	const QColor Sphere_Colors[] = {
		QColor(231, 76, 60), QColor(52, 152, 219), QColor(46, 204, 113), QColor(241, 196, 15),
		QColor(155, 89, 182), QColor(26, 188, 156), QColor(230, 126, 34), QColor(149, 165, 166)
	};

	template <std::size_t N>
	const QColor& ColorAt(const QColor (&colors)[N], std::size_t i) { return colors[i % N]; }

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

	QString EnsureXyzExtension(QString filename)
	{
		if (!filename.endsWith(".xyz", Qt::CaseInsensitive))
		{
			filename += ".xyz";
		}
		return filename;
	}
} // namespace

/* Construction ============================================================================ */

int PointcloudEditorDialog::Execute(const QString& rootDir, QWidget* parent)
{
	PointcloudEditorDialog dialog(rootDir, parent);
	return dialog.exec();
}

PointcloudEditorDialog::PointcloudEditorDialog(const QString& rootDir, QWidget* parent, Qt::WindowFlags flags)
	: QDialog(parent, flags), _rootDir(rootDir)
{
	setupUi(this);
	_aiModelService = new WslModelService(this);
	connect(_aiModelService, &WslModelService::OutputReceived, this, [this](const QString& text) {
		const QStringList lines = text.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
		if (lines.isEmpty()) return;
		SetStatus(lines.back());
		if (_aiProgressDialog) _aiProgressDialog->SetOutput(lines.back());
	});
	AddFitModels();
	StartAiModelDiscovery();
	UpdatePointcloudCombo();
	SetStatus(QString());
	showMaximized();
}

PointcloudEditorDialog::~PointcloudEditorDialog() = default;

/// Registers the fittable models in the combo box. To add one: a new value in
/// FitModel, an entry here and its branch in on_fit_model_combo_currentIndexChanged().
void PointcloudEditorDialog::AddFitModels()
{
	fit_model_combo->clear();
	fit_model_combo->addItem(tr("None"), static_cast<int>(FitModel::None));
	fit_model_combo->addItem(tr("Plane"), static_cast<int>(FitModel::Plane));
	fit_model_combo->addItem(tr("Sphere"), static_cast<int>(FitModel::Sphere));
}

/// Queries WSL for the registered models as soon as the editor opens. The selector stays
/// disabled while searching and remains disabled when no model is available.
void PointcloudEditorDialog::StartAiModelDiscovery()
{
	{
		const QSignalBlocker blocker(ai_model_combo);
		ai_model_combo->clear();
		ai_model_combo->addItem(tr("Searching..."));
	}
	ai_model_combo->setEnabled(false);
	ai_model_combo->setToolTip(tr("Searching for AI models in WSL..."));
	_aiModelService->ListModels([this](const QVector<AiModelInfo>& models, const QString& error) {
		SetAvailableAiModels(models, error);
	});
}

void PointcloudEditorDialog::SetAvailableAiModels(const QVector<AiModelInfo>& models, const QString& error)
{
	{
		const QSignalBlocker blocker(ai_model_combo);
		_aiModels.clear();
		ai_model_combo->clear();
		ai_model_combo->addItem(tr("None"));
		for (const AiModelInfo& model : models)
		{
			_aiModels.insert(model.id, model);
			ai_model_combo->addItem(model.displayName, model.id);
		}
		ai_model_combo->setCurrentIndex(0);
	}
	UpdateAiModelControls();

	if (models.isEmpty())
	{
		ai_model_combo->setToolTip(error.isEmpty()
			? tr("No AI models were found in ~/smcp-models inside WSL.")
			: tr("AI models are unavailable: %1").arg(error));
	}
	else
	{
		ai_model_combo->setToolTip(error.isEmpty()
			? tr("Select an AI model to configure and run it on the first point cloud.")
			: error);
	}
}

void PointcloudEditorDialog::UpdateAiModelControls()
{
	ai_model_combo->setEnabled(!_busy && !_aiConfigurationLoading && !_aiModels.isEmpty());
}

void PointcloudEditorDialog::ResetAiModelSelection()
{
	const QSignalBlocker blocker(ai_model_combo);
	ai_model_combo->setCurrentIndex(0);
}

void PointcloudEditorDialog::on_ai_model_combo_currentIndexChanged(int index)
{
	const QString modelId = index > 0 ? ai_model_combo->itemData(index).toString() : QString();
	if (!modelId.isEmpty())
	{
		ConfigureAiModel(modelId);
	}
}

void PointcloudEditorDialog::ConfigureAiModel(const QString& modelId)
{
	if (_busy || _aiConfigurationLoading || !_aiModels.contains(modelId))
	{
		ResetAiModelSelection();
		return;
	}
	if (!FirstPointcloud())
	{
		SetStatus(tr("Load at least one point cloud before running an AI model."));
		ResetAiModelSelection();
		return;
	}

	_aiConfigurationLoading = true;
	UpdateAiModelControls();
	SetStatus(tr("Loading %1 configuration from WSL...").arg(_aiModels.value(modelId).displayName));
	_aiModelService->DescribeModel(modelId, [this, modelId](const AiModelInfo& described, const QString& error) {
		_aiConfigurationLoading = false;
		UpdateAiModelControls();
		if (!error.isEmpty())
		{
			SetStatus(tr("Could not load AI model configuration: %1").arg(error));
			ResetAiModelSelection();
			return;
		}
		if (described.id != modelId)
		{
			SetStatus(tr("WSL returned configuration for an unexpected AI model."));
			ResetAiModelSelection();
			return;
		}

		_aiModels.insert(modelId, described);
		const QJsonObject configuration = _aiConfigurations.contains(modelId)
			? _aiConfigurations.value(modelId) : described.configuration;
		AiModelConfigDialog dialog(described.displayName, configuration, described.configurationSchema, this);
		if (dialog.exec() != QDialog::Accepted)
		{
			SetStatus(tr("AI inference was not started."));
			ResetAiModelSelection();
			return;
		}
		_aiConfigurations.insert(modelId, dialog.Configuration());
		StartAiInference(described, dialog.Configuration());
		if (!_aiInferenceRunning)
		{
			// Preparing the temporary files failed; once running, CloseAiProgressDialog() resets it.
			ResetAiModelSelection();
		}
	});
}

void PointcloudEditorDialog::StartAiInference(const AiModelInfo& model, const QJsonObject& configuration)
{
	const auto pointclouds = preview_widget->Pointclouds();
	if (pointclouds.empty() || !pointclouds.front().cloud || pointclouds.front().cloud->empty())
	{
		SetStatus(tr("The first point cloud is empty."));
		return;
	}

	_aiTemporaryDirectory = std::make_unique<QTemporaryDir>(
		QDir::tempPath() + QDir::separator() + "smcp-ai-XXXXXX");
	if (!_aiTemporaryDirectory->isValid())
	{
		_aiTemporaryDirectory.reset();
		SetStatus(tr("Could not create temporary files for AI inference."));
		return;
	}

	const QString inputPath = QDir(_aiTemporaryDirectory->path()).filePath("input.xyz");
	_aiResultName = WslModelService::ResultFilename(pointclouds.front().name, model.outputSuffix);
	_aiResultPath = QDir(_aiTemporaryDirectory->path()).filePath(_aiResultName);
	const QString configurationPath = QDir(_aiTemporaryDirectory->path()).filePath("configuration.json");

	if (!IOExport::WriteXyz(inputPath.toStdString(), *pointclouds.front().cloud))
	{
		_aiTemporaryDirectory.reset();
		SetStatus(tr("Could not export the first point cloud for AI inference."));
		return;
	}
	QFile configurationFile(configurationPath);
	if (!configurationFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| configurationFile.write(QJsonDocument(configuration).toJson(QJsonDocument::Indented)) < 0)
	{
		_aiTemporaryDirectory.reset();
		SetStatus(tr("Could not write the temporary AI configuration."));
		return;
	}
	configurationFile.close();

	_aiInferenceRunning = true;
	_aiCancelRequested = false;
	SetBusy(true, false);
	_aiProgressDialog = new AiInferenceProgressDialog(model.displayName, this);
	connect(_aiProgressDialog, &AiInferenceProgressDialog::CancelRequested, this, [this]() {
		if (!_aiInferenceRunning) return;
		_aiCancelRequested = true;
		_aiProgressDialog->SetCancelling();
		SetStatus(tr("Cancelling AI inference..."));
		_aiModelService->Cancel();
	});
	// Window-modal and non-blocking: the inference callbacks keep arriving through the event loop.
	_aiProgressDialog->open();
	SetStatus(tr("Running %1 on the first point cloud...").arg(model.displayName));
	_aiModelService->RunInference(model.id, inputPath, _aiResultPath, configurationPath,
		[this](bool success, const QString& diagnostics) { FinishAiInference(success, diagnostics); });
}

void PointcloudEditorDialog::FinishAiInference(bool success, const QString& diagnostics)
{
	_aiInferenceRunning = false;
	if (!success)
	{
		_aiTemporaryDirectory.reset();
		CloseAiProgressDialog();
		SetBusy(false);
		SetStatus(_aiCancelRequested ? tr("AI inference was cancelled.") : tr("AI inference failed: %1").arg(diagnostics));
		return;
	}
	if (!QFileInfo::exists(_aiResultPath))
	{
		_aiTemporaryDirectory.reset();
		CloseAiProgressDialog();
		SetBusy(false);
		SetStatus(tr("AI inference finished, but the expected output file was not created."));
		return;
	}
	if (_aiProgressDialog) _aiProgressDialog->SetLoadingResult();
	LoadAiResult();
}

void PointcloudEditorDialog::LoadAiResult()
{
	const QString path = _aiResultPath;
	const QString name = _aiResultName;
	RunAsync<std::shared_ptr<LoadedPointcloud>>(
		tr("Loading AI inference result..."),
		[path, name]() {
			auto result = std::make_shared<LoadedPointcloud>();
			result->cloud = std::make_shared<Pointcloud::ColorCloud>();
			if (!IOExport::ReadXyz(path.toStdString(), *result->cloud) || result->cloud->empty())
			{
				result->cloud.reset();
				return result;
			}
			result->prepared = PointcloudPreviewWidget::PreparePointcloud(*result->cloud, QColor(), name);
			result->prepared.sourceCloud = result->cloud;
			return result;
		},
		[this, name](const std::shared_ptr<LoadedPointcloud>& result) {
			_aiTemporaryDirectory.reset();
			CloseAiProgressDialog();
			if (!result->cloud)
			{
				SetStatus(tr("The AI result could not be read as an XYZ point cloud."));
				return;
			}
			preview_widget->AddPreparedPointcloud(std::move(result->prepared));
			SetStatus(tr("AI result %1 added (%2 points).").arg(name).arg(result->cloud->size()));
		});
}

void PointcloudEditorDialog::CloseAiProgressDialog()
{
	ResetAiModelSelection();
	if (!_aiProgressDialog)
	{
		return;
	}
	_aiProgressDialog->done(QDialog::Accepted);
	_aiProgressDialog->deleteLater();
	_aiProgressDialog = nullptr;
}

void PointcloudEditorDialog::SetStatus(const QString& text)
{
	status_label->setText(text);
}

/// Disables the command bar and, unless `waitCursor` is false, shows the wait cursor while a
/// command runs in the background.
void PointcloudEditorDialog::SetBusy(bool busy, bool waitCursor)
{
	if (_busy == busy)
	{
		return;
	}
	_busy = busy;

	for (QWidget* w : { static_cast<QWidget*>(pointcloud_combo), static_cast<QWidget*>(load_pointcloud_button),
			 static_cast<QWidget*>(remove_outliers_button),
			 static_cast<QWidget*>(fit_model_combo), static_cast<QWidget*>(save_button) })
	{
		w->setEnabled(!busy);
	}
	UpdateAiModelControls();

	if (busy && waitCursor)
	{
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
		_waitCursor = true;
	}
	else if (!busy && _waitCursor)
	{
		QApplication::restoreOverrideCursor();
		_waitCursor = false;
	}
}

/// The window close button does not interrupt a command already in progress.
void PointcloudEditorDialog::reject()
{
	if (_busy)
	{
		return;
	}
	QDialog::reject();
}

/* Point cloud ============================================================================= */

void PointcloudEditorDialog::UpdatePointcloudCombo()
{
	const QDir dir(_rootDir);
	const QStringList names = dir.entryList({ "*.xyz" }, QDir::Files, QDir::Name);
	const QString selectedName = pointcloud_combo->currentText();

	// Selecting a filename never loads it. Loading is explicit through load_pointcloud_button.
	{
		const QSignalBlocker blocker(pointcloud_combo);
		pointcloud_combo->clear();
		pointcloud_combo->addItems(names);
		const int previousIndex = pointcloud_combo->findText(selectedName);
		if (previousIndex >= 0)
		{
			pointcloud_combo->setCurrentIndex(previousIndex);
		}
	}

	for (auto it = _cloudCache.begin(); it != _cloudCache.end();)
	{
		it = names.contains(QFileInfo(it->first).fileName()) ? std::next(it) : _cloudCache.erase(it);
	}

}

Pointcloud::ColorCloudPtr PointcloudEditorDialog::FindCachedPointcloud(const QString& path, qint64 fileSize,
	const QDateTime& lastModified)
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

void PointcloudEditorDialog::StoreCachedPointcloud(const QString& path, qint64 fileSize, const QDateTime& lastModified,
	const Pointcloud::ColorCloudPtr& cloud)
{
	_cloudCache[path] = CachedPointcloud{ cloud, fileSize, lastModified, ++_cacheUseCounter };

	// Evict least recently used clouds beyond the memory budget, always keeping the one just stored.
	const auto bytesOf = [](const CachedPointcloud& entry) { return entry.cloud->size() * sizeof(Pointcloud::ColorPoint); };
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

void PointcloudEditorDialog::on_load_pointcloud_button_clicked(bool)
{
	if (pointcloud_combo->currentIndex() < 0 || pointcloud_combo->currentText().isEmpty())
	{
		return;
	}

	const QString name = pointcloud_combo->currentText();
	const QString path = QDir(_rootDir).absoluteFilePath(name);
	const QFileInfo info(path);
	const qint64 fileSize = info.size();
	const QDateTime lastModified = info.lastModified();
	const Pointcloud::ColorCloudPtr cached = FindCachedPointcloud(path, fileSize, lastModified);

	// Reading and building the GPU buffer both happen on the worker thread; the result travels in a
	// shared_ptr so leaving the QFuture copies no point data.
	RunAsync<std::shared_ptr<LoadedPointcloud>>(
		tr("Loading point cloud..."),
		[filepath = path.toStdString(), cached, name]() {
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
			result->prepared = PointcloudPreviewWidget::PreparePointcloud(*result->cloud, QColor(), name);
			result->prepared.sourceCloud = result->cloud;
			return result;
		},
		[this, name, path, fileSize, lastModified, cached](const std::shared_ptr<LoadedPointcloud>& result) {
			if (!result->cloud)
			{
				SetStatus(tr("Error: could not open %1.").arg(name));
				return;
			}
			if (!cached)
			{
				StoreCachedPointcloud(path, fileSize, lastModified, result->cloud);
			}
			preview_widget->AddPreparedPointcloud(std::move(result->prepared));
			SetStatus(tr("Point cloud added (%1 points).").arg(result->cloud->size()));
		});
}

Pointcloud::ColorCloudPtr PointcloudEditorDialog::FirstPointcloud() const
{
	const auto pointclouds = preview_widget->Pointclouds();
	return pointclouds.empty() ? nullptr : pointclouds.front().cloud;
}

/* Commands ================================================================================ */

void PointcloudEditorDialog::on_remove_outliers_button_clicked(bool)
{
	const Pointcloud::ColorCloudPtr input = FirstPointcloud();
	if (!input || input->empty())
	{
		SetStatus(tr("No point cloud loaded."));
		return;
	}

	RemoveOutliersDialog dialog(_outlierRemovalParams, this);
	if (dialog.exec() != QDialog::Accepted)
	{
		return;
	}
	_outlierRemovalParams = dialog.Params();

	const Pointcloud::OutlierRemovalParams params = _outlierRemovalParams;
	RunAsync<Pointcloud::ColorCloudPtr>(
		tr("Removing outliers..."),
		[input, params]() {
			return Pointcloud::RemoveStatisticalOutliers(*input, params);
		},
		[this, input](const Pointcloud::ColorCloudPtr& filtered) {
			const auto removed = input->size() - filtered->size();
			preview_widget->SetPointcloudColor(input, QColor("#e43b44"));
			preview_widget->PrependPointcloud(filtered, QColor(), tr("Filtered"));

			SetStatus(tr("Outliers removed from the first cloud (%1 points discarded, %2 remaining).")
				.arg(removed).arg(filtered->size()));
		});
}

void PointcloudEditorDialog::ResetFitModelSelection()
{
	const int noneIndex = fit_model_combo->findData(static_cast<int>(FitModel::None));
	fit_model_combo->setCurrentIndex(noneIndex);
}

void PointcloudEditorDialog::on_fit_model_combo_currentIndexChanged(int index)
{
	if (index < 0 || static_cast<FitModel>(fit_model_combo->itemData(index).toInt()) == FitModel::None)
	{
		return;
	}

	const Pointcloud::ColorCloudPtr first = FirstPointcloud();
	if (!first || first->empty())
	{
		SetStatus(tr("No point cloud loaded to fit a model to."));
		ResetFitModelSelection();
		return;
	}

	switch (static_cast<FitModel>(fit_model_combo->itemData(index).toInt()))
	{
	case FitModel::None:
		break;
	case FitModel::Plane:
	{
		PlaneFitDialog dialog(_planeFitParams, this);
		if (dialog.exec() != QDialog::Accepted)
		{
			ResetFitModelSelection();
			return;
		}
		_planeFitParams = dialog.Params();
		FitPlanes();
		break;
	}
	case FitModel::Sphere:
	{
		SphereFitDialog dialog(_sphereFitParams, this);
		if (dialog.exec() != QDialog::Accepted)
		{
			ResetFitModelSelection();
			return;
		}
		_sphereFitParams = dialog.Params();
		FitSpheres();
		break;
	}
	}
}

/// Extracts the dominant planes with RANSAC and displays them color-coded.
void PointcloudEditorDialog::FitPlanes()
{
	const Pointcloud::ColorCloudPtr input = FirstPointcloud();
	if (!input || input->empty())
	{
		SetStatus(tr("No point cloud loaded to fit a model to."));
		ResetFitModelSelection();
		return;
	}
	const Pointcloud::PlaneFitParams params = _planeFitParams;
	RunAsync<std::vector<Pointcloud::PlaneFit>>(
		tr("Detecting planes..."),
		[input, params]() {
			return Pointcloud::FitPlanes(*input, params);
		},
		[this](const std::vector<Pointcloud::PlaneFit>& planes) {
			if (planes.empty())
			{
				SetStatus(tr("No planes could be estimated for the point cloud."));
				ResetFitModelSelection();
				return;
			}

			preview_widget->SetAllPointcloudsVisible(false);
			std::size_t pointCount = 0;
			for (std::size_t i = 0; i < planes.size(); ++i)
			{
				pointCount += planes[i].points->size();
				preview_widget->AddPointcloud(planes[i].points, ColorAt(Plane_Colors, i), tr("Plane %1").arg(i + 1));
			}
			SetStatus(tr("Detected %1 plane(s) in the first cloud with %2 points in total.")
				.arg(planes.size()).arg(pointCount));
			ResetFitModelSelection();
		});
}

/// Removes the dominant planes first (RANSAC with 3 points converges easily) and fits
/// spheres on what is left: this way the sphere inlier fraction is high enough for
/// RANSAC with 4 points to converge within a few thousand iterations.
void PointcloudEditorDialog::FitSpheres()
{
	const Pointcloud::ColorCloudPtr input = FirstPointcloud();
	if (!input || input->empty())
	{
		SetStatus(tr("No point cloud loaded to fit a model to."));
		ResetFitModelSelection();
		return;
	}
	const Pointcloud::SphereFitParams selectedParams = _sphereFitParams;
	RunAsync<std::vector<Pointcloud::SphereFit>>(
		tr("Detecting spheres..."),
		[input, selectedParams]() {
			Pointcloud::ColorCloudPtr remaining;
			Pointcloud::FitPlanes(*input, Pointcloud::PlaneFitParams(), &remaining);
			if (!remaining)
			{
				remaining = std::make_shared<Pointcloud::ColorCloud>();
			}

			Pointcloud::SphereFitParams params = selectedParams;
			if (params.minInliers == 0)
			{
				params.minInliers = std::max<std::size_t>(50u, input->size() / 100u);
			}
			return Pointcloud::FitSpheres(*remaining, params);
		},
		[this](const std::vector<Pointcloud::SphereFit>& spheres) {
			if (spheres.empty())
			{
				SetStatus(tr("No spheres could be estimated for the point cloud."));
				ResetFitModelSelection();
				return;
			}

			preview_widget->SetAllPointcloudsVisible(false);
			std::size_t pointCount = 0;
			QStringList details;
			for (std::size_t i = 0; i < spheres.size(); ++i)
			{
				const auto& sphere = spheres[i];
				pointCount += sphere.points->size();
				preview_widget->AddPointcloud(sphere.points, ColorAt(Sphere_Colors, i), tr("Sphere %1").arg(i + 1));
				details << tr("sphere %1: center=(%2, %3, %4) radius=%5 (%6 points)")
					.arg(i + 1)
					.arg(sphere.model.center.x, 0, 'f', 3)
					.arg(sphere.model.center.y, 0, 'f', 3)
					.arg(sphere.model.center.z, 0, 'f', 3)
					.arg(sphere.model.radius, 0, 'f', 3)
					.arg(sphere.points->size());
			}
			SetStatus(tr("Detected %1 sphere(s) in the first cloud with %2 points in total: %3")
				.arg(spheres.size())
				.arg(pointCount)
				.arg(details.join("; ")));
			ResetFitModelSelection();
		});
}

void PointcloudEditorDialog::on_save_button_clicked(bool)
{
	const auto pointclouds = preview_widget->Pointclouds();
	if (pointclouds.empty())
	{
		SetStatus(tr("No point clouds to export."));
		return;
	}

	ExportPointcloudDialog dialog(this);
	if (dialog.exec() != QDialog::Accepted)
	{
		return;
	}

	switch (dialog.SelectedMode())
	{
	case ExportPointcloudDialog::Mode::First:
	{
		QSet<QString> usedNames;
		const QString suggested = QDir(_rootDir).filePath(ExportFilename(pointclouds.front().name, 0, usedNames));
		QString filename = QFileDialog::getSaveFileName(this, tr("Export first point cloud"), suggested,
			tr("XYZ point cloud (*.xyz)"));
		if (filename.isEmpty())
		{
			return;
		}
		filename = EnsureXyzExtension(filename);
		SetStatus(tr("Exporting first point cloud..."));
		if (!IOExport::WriteXyz(filename.toStdString(), *pointclouds.front().cloud))
		{
			SetStatus(tr("Error: the first point cloud could not be exported."));
			return;
		}
		SetStatus(tr("Exported the first point cloud to %1.").arg(QDir::toNativeSeparators(filename)));
		break;
	}
	case ExportPointcloudDialog::Mode::Multiple:
	{
		const QString directory = QFileDialog::getExistingDirectory(this, tr("Export point clouds"), _rootDir);
		if (directory.isEmpty())
		{
			return;
		}

		SetStatus(tr("Exporting point clouds..."));
		QSet<QString> usedNames;
		for (int i = 0; i < static_cast<int>(pointclouds.size()); ++i)
		{
			const QString filename = ExportFilename(pointclouds[i].name, i, usedNames);
			if (!IOExport::WriteXyz(QDir(directory).filePath(filename).toStdString(), *pointclouds[i].cloud))
			{
				SetStatus(tr("Error: not all point clouds could be exported."));
				return;
			}
		}
		SetStatus(tr("Exported %1 point cloud(s) to %2.")
			.arg(pointclouds.size()).arg(QDir::toNativeSeparators(directory)));
		break;
	}
	case ExportPointcloudDialog::Mode::Combine:
	{
		auto combined = std::make_shared<Pointcloud::ColorCloud>();
		int visibleCount = 0;
		for (const auto& pointcloud : pointclouds)
		{
			if (!pointcloud.visible)
			{
				continue;
			}
			combined->insert(combined->end(), pointcloud.cloud->begin(), pointcloud.cloud->end());
			++visibleCount;
		}
		if (visibleCount == 0)
		{
			SetStatus(tr("No visible point clouds to combine."));
			return;
		}

		QString filename = QFileDialog::getSaveFileName(this, tr("Export combined point cloud"),
			QDir(_rootDir).filePath("combined.xyz"), tr("XYZ point cloud (*.xyz)"));
		if (filename.isEmpty())
		{
			return;
		}
		filename = EnsureXyzExtension(filename);
		SetStatus(tr("Combining and exporting visible point clouds..."));
		if (!IOExport::WriteXyz(filename.toStdString(), *combined))
		{
			SetStatus(tr("Error: the combined point cloud could not be exported."));
			return;
		}
		SetStatus(tr("Combined %1 visible cloud(s) and exported %2 points to %3.")
			.arg(visibleCount).arg(combined->size()).arg(QDir::toNativeSeparators(filename)));
		break;
	}
	}
}

} // namespace smcp
