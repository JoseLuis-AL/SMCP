#include "ui/PointcloudEditorDialog.h"

#include <iterator>

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSignalBlocker>

#include "export/IOExport.h"

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
} // namespace

/* Construction ============================================================================ */

PointcloudEditorDialog::PointcloudEditorDialog(const QString& rootDir, QWidget* parent, Qt::WindowFlags flags)
	: QDialog(parent, flags), _rootDir(rootDir)
{
	setupUi(this);
	AddFitModels();
	UpdatePointcloudCombo();
	SetStatus(QString());
	showMaximized();
}

PointcloudEditorDialog::~PointcloudEditorDialog() = default;

/// Registers the fittable models in the combo box. To add one: a new value in
/// FitModel, an entry here and its branch in on_fit_button_clicked().
void PointcloudEditorDialog::AddFitModels()
{
	fit_model_combo->clear();
	fit_model_combo->addItem(tr("Plane"), static_cast<int>(FitModel::Plane));
	fit_model_combo->addItem(tr("Sphere"), static_cast<int>(FitModel::Sphere));
}

void PointcloudEditorDialog::SetStatus(const QString& text)
{
	status_label->setText(text);
}

/// Disables the command bar and shows the wait cursor while a command runs in the
/// background.
void PointcloudEditorDialog::SetBusy(bool busy)
{
	if (_busy == busy)
	{
		return;
	}
	_busy = busy;

	for (QWidget* w : { static_cast<QWidget*>(pointcloud_combo), static_cast<QWidget*>(refresh_button),
			 static_cast<QWidget*>(remove_outliers_button), static_cast<QWidget*>(fit_model_combo),
			 static_cast<QWidget*>(fit_button), static_cast<QWidget*>(save_button), static_cast<QWidget*>(close_button) })
	{
		w->setEnabled(!busy);
	}

	if (busy)
	{
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	}
	else
	{
		QApplication::restoreOverrideCursor();
	}
}

void PointcloudEditorDialog::ResetResult()
{
	_currentCloud.reset();
	_originalCloud.reset();
	_planes.clear();
	_lastCommand = LastCommand::None;
	preview_widget->ClearPointclouds();
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

	// Populate with signals blocked so clearing and each insertion do not reset or load anything;
	// the selected file is loaded once at the end.
	{
		const QSignalBlocker blocker(pointcloud_combo);
		pointcloud_combo->clear();
		pointcloud_combo->addItems(names);
	}

	for (auto it = _cloudCache.begin(); it != _cloudCache.end();)
	{
		it = names.contains(QFileInfo(it->first).fileName()) ? std::next(it) : _cloudCache.erase(it);
	}

	on_pointcloud_combo_currentIndexChanged(pointcloud_combo->currentIndex());
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

void PointcloudEditorDialog::on_refresh_button_clicked(bool)
{
	UpdatePointcloudCombo();
}

void PointcloudEditorDialog::on_pointcloud_combo_currentIndexChanged(int index)
{
	ResetResult();
	if (index < 0 || pointcloud_combo->currentText().isEmpty())
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
			_currentCloud = result->cloud;
			_originalCloud = result->cloud;
			preview_widget->AddPreparedPointcloud(std::move(result->prepared));
			SetStatus(tr("Point cloud loaded (%1 points).").arg(_currentCloud->size()));
		});
}

/* Commands ================================================================================ */

void PointcloudEditorDialog::on_remove_outliers_button_clicked(bool)
{
	if (!_currentCloud || _currentCloud->empty())
	{
		SetStatus(tr("No point cloud loaded."));
		return;
	}

	const Pointcloud::ColorCloudPtr input = _currentCloud;
	RunAsync<Pointcloud::ColorCloudPtr>(
		tr("Removing outliers..."),
		[input]() {
			Pointcloud::OutlierRemovalParams params;  // k = 50, 1.0 standard deviations
			return Pointcloud::RemoveStatisticalOutliers(*input, params);
		},
		[this, input](const Pointcloud::ColorCloudPtr& filtered) {
			const auto removed = input->size() - filtered->size();
			_currentCloud = filtered;
			_planes.clear();
			_lastCommand = LastCommand::Other;

			preview_widget->ClearPointclouds();
			preview_widget->AddPointcloud(_currentCloud, QColor(), tr("Filtered"));
			preview_widget->AddPointcloud(_originalCloud, QColor(Qt::red), tr("Original"));

			SetStatus(tr("Outliers removed (%1 points discarded, %2 remaining).").arg(removed).arg(_currentCloud->size()));
		});
}

void PointcloudEditorDialog::on_fit_button_clicked(bool)
{
	if (!_currentCloud || _currentCloud->empty())
	{
		SetStatus(tr("No point cloud loaded to fit a model to."));
		return;
	}

	switch (static_cast<FitModel>(fit_model_combo->currentData().toInt()))
	{
	case FitModel::Plane:
		FitPlanes();
		break;
	case FitModel::Sphere:
		FitSpheres();
		break;
	}
}

/// Extracts the dominant planes with RANSAC and displays them color-coded.
void PointcloudEditorDialog::FitPlanes()
{
	const Pointcloud::ColorCloudPtr input = _currentCloud;
	RunAsync<std::vector<Pointcloud::PlaneFit>>(
		tr("Detecting planes..."),
		[input]() {
			Pointcloud::PlaneFitParams params;  // up to 5 planes, 1000 iterations, threshold 0.5
			return Pointcloud::FitPlanes(*input, params);
		},
		[this](const std::vector<Pointcloud::PlaneFit>& planes) {
			preview_widget->ClearPointclouds();
			_planes.clear();

			if (planes.empty())
			{
				_lastCommand = LastCommand::Other;
				SetStatus(tr("No planes could be estimated for the point cloud."));
				return;
			}

			auto merged = std::make_shared<Pointcloud::ColorCloud>();
			for (std::size_t i = 0; i < planes.size(); ++i)
			{
				_planes.push_back(planes[i].points);
				merged->insert(merged->end(), planes[i].points->begin(), planes[i].points->end());
				preview_widget->AddPointcloud(planes[i].points, ColorAt(Plane_Colors, i), tr("Plane %1").arg(i + 1));
			}

			_currentCloud = merged;
			_lastCommand = LastCommand::FitPlanes;
			SetStatus(tr("Detected %1 plane(s) with %2 points in total.").arg(planes.size()).arg(_currentCloud->size()));
		});
}

/// Removes the dominant planes first (RANSAC with 3 points converges easily) and fits
/// spheres on what is left: this way the sphere inlier fraction is high enough for
/// RANSAC with 4 points to converge within a few thousand iterations.
void PointcloudEditorDialog::FitSpheres()
{
	const Pointcloud::ColorCloudPtr input = _currentCloud;
	RunAsync<std::vector<Pointcloud::SphereFit>>(
		tr("Detecting spheres..."),
		[input]() {
			Pointcloud::ColorCloudPtr remaining;
			Pointcloud::FitPlanes(*input, Pointcloud::PlaneFitParams(), &remaining);
			if (!remaining)
			{
				remaining = std::make_shared<Pointcloud::ColorCloud>();
			}

			Pointcloud::SphereFitParams params;  // up to 5 spheres, threshold 0.25, radius in [1, 10000]
			params.minInliers = std::max<std::size_t>(50u, input->size() / 100u);
			return Pointcloud::FitSpheres(*remaining, params);
		},
		[this](const std::vector<Pointcloud::SphereFit>& spheres) {
			preview_widget->ClearPointclouds();
			_planes.clear();

			if (spheres.empty())
			{
				_lastCommand = LastCommand::Other;
				SetStatus(tr("No spheres could be estimated for the point cloud."));
				return;
			}

			auto merged = std::make_shared<Pointcloud::ColorCloud>();
			QStringList details;
			for (std::size_t i = 0; i < spheres.size(); ++i)
			{
				const auto& sphere = spheres[i];
				merged->insert(merged->end(), sphere.points->begin(), sphere.points->end());
				preview_widget->AddPointcloud(sphere.points, ColorAt(Sphere_Colors, i), tr("Sphere %1").arg(i + 1));
				details << tr("sphere %1: center=(%2, %3, %4) radius=%5 (%6 points)")
					.arg(i + 1)
					.arg(sphere.model.center.x, 0, 'f', 3)
					.arg(sphere.model.center.y, 0, 'f', 3)
					.arg(sphere.model.center.z, 0, 'f', 3)
					.arg(sphere.model.radius, 0, 'f', 3)
					.arg(sphere.points->size());
			}

			_currentCloud = merged;
			_lastCommand = LastCommand::FitSpheres;
			SetStatus(tr("Detected %1 sphere(s) with %2 points in total: %3")
				.arg(spheres.size())
				.arg(_currentCloud->size())
				.arg(details.join("; ")));
		});
}

void PointcloudEditorDialog::on_save_button_clicked(bool)
{
	if (!_currentCloud || _currentCloud->empty())
	{
		SetStatus(tr("No point cloud to save."));
		return;
	}

	const QString filepath = QFileDialog::getSaveFileName(this, tr("Save point cloud"), _rootDir,
		tr("Point Cloud (*.xyz);;All files (*)"));
	if (filepath.isEmpty())
	{
		return;
	}

	SetStatus(tr("Saving..."));

	// After "Fit Plane" one file per plane is written: <name>_1.xyz, <name>_2.xyz, ...
	if (_lastCommand == LastCommand::FitPlanes && !_planes.empty())
	{
		const QFileInfo info(filepath);
		const QString suffix = info.suffix();
		for (std::size_t i = 0; i < _planes.size(); ++i)
		{
			const QString name = suffix.isEmpty()
				? QString("%1_%2").arg(info.completeBaseName()).arg(i + 1)
				: QString("%1_%2.%3").arg(info.completeBaseName()).arg(i + 1).arg(suffix);
			if (!IOExport::WriteXyz(QDir(info.absolutePath()).filePath(name).toStdString(), *_planes[i]))
			{
				SetStatus(tr("Error: not all planes could be saved."));
				return;
			}
		}
		SetStatus(tr("Saved %1 plane(s) using the prefix %2.").arg(_planes.size()).arg(info.fileName()));
		return;
	}

	if (!IOExport::WriteXyz(filepath.toStdString(), *_currentCloud))
	{
		SetStatus(tr("Error: could not open the file for writing."));
		return;
	}
	SetStatus(tr("Saved: %1 (%2 points).").arg(QFileInfo(filepath).fileName()).arg(_currentCloud->size()));
}

void PointcloudEditorDialog::on_close_button_clicked(bool)
{
	if (_busy)
	{
		return;
	}
	ResetResult();
	accept();
}

} // namespace smcp
