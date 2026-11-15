// PointcloudEditorDialog, part 2 of 4: the commands of the PointCloud, Fit and Measure menus.
//
// Every command has the same shape: check the selection, ask for parameters with its dialog, run
// the computation on a worker (RunSelected: once per selected cloud; RunAsync: one job) and publish
// the results as layers inside a single undoable viewer command. The long ones pass the flag from
// NewCancelFlag() to the core so the Cancel button can stop them.
// docs/EDITOR_ASYNC_HISTORY.md explains how to add one.

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
		/// Colors used to tell the detected planes apart.
		const QColor Plane_Colors[] = { QColor(4, 92, 195), QColor(237, 167, 59), QColor(62, 137, 62),
			QColor(60, 151, 139), QColor(228, 59, 68), QColor(26, 188, 156), QColor(230, 126, 34),
			QColor(149, 165, 166) };

		/// Colors used to tell the detected spheres apart.
		const QColor Sphere_Colors[] = { QColor(231, 76, 60), QColor(52, 152, 219), QColor(46, 204, 113),
			QColor(241, 196, 15), QColor(155, 89, 182), QColor(26, 188, 156), QColor(230, 126, 34),
			QColor(149, 165, 166) };

		/// Colors used to tell the registered clouds apart (the first selected one is the reference).
		const QColor Registration_Colors[] = { QColor(149, 165, 166), QColor(228, 59, 68), QColor(4, 92, 195),
			QColor(62, 137, 62), QColor(237, 167, 59), QColor(155, 89, 182), QColor(26, 188, 156),
			QColor(230, 126, 34) };

		template <std::size_t N> const QColor& ColorAt(const QColor (&colors)[N], std::size_t i)
		{
			return colors[i % N];
		}
	} // namespace

	void PointcloudEditorDialog::on_align_centroids_action_triggered(bool)
	{
		if (IsWorking())
			return;
		if (SelectedClouds().size() < 2)
		{
			SetStatus(tr("Select at least two visible point clouds to align their centroids."));
			return;
		}
		preview_widget->AlignSelectedCentroids();
		SetStatus(tr("Aligning selected centroids to the first selected point cloud in panel order."));
	}

	void PointcloudEditorDialog::on_remove_outliers_action_triggered(bool)
	{
		const Pointcloud::ColorCloudPtr input = FirstSelectedCloud();
		if (!input || input->empty())
		{
			SetStatus(tr("Select at least one visible point cloud."));
			return;
		}

		RemoveOutliersDialog dialog(_outlierRemovalParams, this);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}
		_outlierRemovalParams = dialog.Params();

		const Pointcloud::OutlierRemovalParams params = _outlierRemovalParams;
		const auto cancel = NewCancelFlag();
		RunSelected<Pointcloud::ColorCloudPtr>(
			tr("Removing outliers..."),
			[params, cancel](const CloudLayer& layer)
			{ return Pointcloud::RemoveStatisticalOutliers(*layer.cloud, params, cancel.get()); },
			[this, params](const CloudLayer& layer, const Pointcloud::ColorCloudPtr& filtered)
			{
				const auto input = layer.cloud;
				const auto inputName = layer.name;
				const auto removed = input->size() - filtered->size();
				// The input turns red so the discarded points stand out against the filtered copy
				// drawn over it. By layer id: layers loaded from the same file share their cloud.
				preview_widget->SetLayerColor(layer.id, QColor("#e43b44"));
				preview_widget->PrependPointcloud(filtered, QColor(), tr("(Filtered) %1").arg(inputName),
					tr("Outlier removal of %1 (k=%2, multiplier %3): %4 points discarded")
						.arg(inputName)
						.arg(params.meanK)
						.arg(params.stddevMult)
						.arg(removed));

				SetStatus(tr("Outliers removed from %1 (%2 points discarded, %3 remaining).")
						.arg(inputName)
						.arg(removed)
						.arg(filtered->size()));
			});
	}

	/// Registers selected clouds onto the first selected reference and publishes labeled copies and their union.
	void PointcloudEditorDialog::on_register_action_triggered(bool)
	{
		if (IsWorking())
			return;
		const auto pointclouds = SelectedClouds();
		if (pointclouds.size() < 2)
		{
			SetStatus(tr("Select at least two visible point clouds for ICP."));
			return;
		}

		std::vector<PointcloudPreviewWidget::ListedPointcloud> sources;
		for (std::size_t i = 1; i < pointclouds.size(); ++i)
		{
			if (pointclouds[i].visible && pointclouds[i].cloud && !pointclouds[i].cloud->empty())
			{
				sources.push_back(pointclouds[i]);
			}
		}
		if (sources.empty())
		{
			SetStatus(
				tr("Show at least one point cloud besides the first selected one to register it onto the first."));
			return;
		}

		IcpDialog dialog(_icpParams, _icpAlignToMerged, this);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}
		_icpParams = dialog.Params();
		_icpAlignToMerged = dialog.AlignToMerged();

		const Pointcloud::ColorCloudPtr reference = pointclouds.front().cloud;
		const QString referenceName = pointclouds.front().name;
		const Pointcloud::IcpParams params = _icpParams;
		const bool alignToMerged = _icpAlignToMerged;
		const auto cancel = NewCancelFlag();
		RunAsync<std::shared_ptr<Registration>>(
			tr("Registering %1 point cloud(s) onto %2 with ICP...").arg(sources.size()).arg(referenceName),
			[reference, sources, params, alignToMerged, cancel]()
			{
				auto result = std::make_shared<Registration>();
				result->merged = std::make_shared<Pointcloud::ColorCloud>(*reference);
				for (const auto& source : sources)
				{
					Registration::Aligned aligned;
					aligned.name = source.name;
					aligned.icp =
						Pointcloud::AlignIcp(*source.cloud, alignToMerged ? *result->merged : *reference, params, cancel.get());
					if (aligned.icp.valid)
					{
						aligned.cloud = Pointcloud::TransformCloud(*source.cloud, aligned.icp.transform);
						result->merged->insert(result->merged->end(), aligned.cloud->begin(), aligned.cloud->end());
					}
					result->aligned.push_back(std::move(aligned));
				}
				return result;
			},
			[this, reference, referenceName](const std::shared_ptr<Registration>& result)
			{
				QStringList details;
				std::size_t registered = 0;
				for (const auto& aligned : result->aligned)
				{
					if (!aligned.cloud)
					{
						details << tr("%1: failed").arg(aligned.name);
						continue;
					}
					++registered;
					details << tr("%1: RMSE=%2, %3 iteration(s)%4")
								   .arg(aligned.name)
								   .arg(aligned.icp.rmse, 0, 'f', 4)
								   .arg(aligned.icp.iterations)
								   .arg(aligned.icp.converged ? QString() : tr(", not converged"));
				}
				if (registered == 0)
				{
					SetStatus(tr("ICP could not register any point cloud onto %1 (no correspondences): %2")
							.arg(referenceName)
							.arg(details.join("; ")));
					return;
				}


				preview_widget->AddPointcloud(reference, ColorAt(Registration_Colors, 0),
					tr("(ICP) (Reference) %1").arg(referenceName),
					tr("ICP reference: %1, unchanged").arg(referenceName));
				std::size_t colorIndex = 1;
				for (const auto& aligned : result->aligned)
				{
					if (aligned.cloud)
					{
						preview_widget->AddPointcloud(aligned.cloud, ColorAt(Registration_Colors, colorIndex++),
							tr("(ICP) (Aligned) %1").arg(aligned.name),
							tr("%1 aligned onto %2 with ICP: RMSE %3, %4 iteration(s)%5")
								.arg(aligned.name)
								.arg(referenceName)
								.arg(aligned.icp.rmse, 0, 'f', 4)
								.arg(aligned.icp.iterations)
								.arg(aligned.icp.converged ? QString() : tr(", not converged")));
					}
				}
				const auto mergedName = tr("(ICP) (Merged) %1").arg(referenceName);
				preview_widget->PrependPointcloud(result->merged, QColor(), mergedName,
					tr("Union of %1 and %2 cloud(s) aligned onto it with ICP").arg(referenceName).arg(registered));

				SetStatus(tr("Registered %1 of %2 cloud(s) onto %3; \"%4\" (%5 points) is now first: %6")
						.arg(registered)
						.arg(result->aligned.size())
						.arg(referenceName)
						.arg(mergedName)
						.arg(result->merged->size())
						.arg(details.join("; ")));
			});
	}

	void PointcloudEditorDialog::on_fit_plane_action_triggered(bool)
	{
		ConfigureFit(FitModel::Plane);
	}

	void PointcloudEditorDialog::on_fit_sphere_action_triggered(bool)
	{
		ConfigureFit(FitModel::Sphere);
	}

	/// Keep the parameter dialogs and fit dispatch shared by the two menu entries.
	void PointcloudEditorDialog::ConfigureFit(FitModel model)
	{
		if (IsWorking())
			return;
		const Pointcloud::ColorCloudPtr first = FirstSelectedCloud();
		if (!first || first->empty())
		{
			SetStatus(tr("No point cloud loaded to fit a model to."));
			return;
		}

		switch (model)
		{
			case FitModel::Plane:
			{
				PlaneFitDialog dialog(_planeFitParams, this);
				if (dialog.exec() != QDialog::Accepted)
				{
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
		const Pointcloud::ColorCloudPtr input = FirstSelectedCloud();
		if (!input || input->empty())
		{
			SetStatus(tr("No point cloud loaded to fit a model to."));
			return;
		}
		const Pointcloud::PlaneFitParams params = _planeFitParams;
		const auto cancel = NewCancelFlag();
		RunSelected<std::vector<Pointcloud::PlaneFit>>(
			tr("Detecting planes..."),
			[params, cancel](const CloudLayer& layer)
			{ return Pointcloud::FitPlanes(*layer.cloud, params, nullptr, cancel.get()); },
			[this](const CloudLayer& layer, const std::vector<Pointcloud::PlaneFit>& planes)
			{
				const auto inputName = layer.name;
				if (planes.empty())
				{
					SetStatus(tr("No planes could be estimated for the point cloud."));
					return;
				}


				std::size_t pointCount = 0;
				for (std::size_t i = 0; i < planes.size(); ++i)
				{
					pointCount += planes[i].points->size();
					const auto& model = planes[i].model;
					preview_widget->PrependPointcloud(planes[i].points, ColorAt(Plane_Colors, i),
						tr("Plane %1").arg(i + 1),
						tr("Plane %1 of %2: normal (%3, %4, %5), d=%6")
							.arg(i + 1)
							.arg(inputName)
							.arg(model.normal[0], 0, 'f', 4)
							.arg(model.normal[1], 0, 'f', 4)
							.arg(model.normal[2], 0, 'f', 4)
							.arg(model.d, 0, 'f', 3));
				}
				SetStatus(tr("Detected %1 plane(s) in the selected cloud with %2 points in total.")
						.arg(planes.size())
						.arg(pointCount));
			});
	}

	/// Removes the dominant planes first (RANSAC with 3 points converges easily) and fits
	/// spheres on what is left: this way the sphere inlier fraction is high enough for
	/// RANSAC with 4 points to converge within a few thousand iterations.
	void PointcloudEditorDialog::FitSpheres()
	{
		const Pointcloud::ColorCloudPtr input = FirstSelectedCloud();
		if (!input || input->empty())
		{
			SetStatus(tr("No point cloud loaded to fit a model to."));
			return;
		}
		const Pointcloud::SphereFitParams selectedParams = _sphereFitParams;
		const auto cancel = NewCancelFlag();
		RunSelected<std::vector<Pointcloud::SphereFit>>(
			tr("Detecting spheres..."),
			[selectedParams, cancel](const CloudLayer& layer)
			{
				const auto input = layer.cloud;
				Pointcloud::ColorCloudPtr remaining;
				Pointcloud::FitPlanes(*input, Pointcloud::PlaneFitParams(), &remaining, cancel.get());
				if (!remaining)
				{
					remaining = std::make_shared<Pointcloud::ColorCloud>();
				}

				Pointcloud::SphereFitParams params = selectedParams;
				if (params.minInliers == 0)
				{
					params.minInliers = std::max<std::size_t>(50u, input->size() / 100u);
				}
				return Pointcloud::FitSpheres(*remaining, params, nullptr, cancel.get());
			},
			[this](const CloudLayer& layer, const std::vector<Pointcloud::SphereFit>& spheres)
			{
				const auto inputName = layer.name;
				if (spheres.empty())
				{
					SetStatus(tr("No spheres could be estimated for the point cloud."));
					return;
				}


				std::size_t pointCount = 0;
				QStringList details;
				for (std::size_t i = 0; i < spheres.size(); ++i)
				{
					const auto& sphere = spheres[i];
					pointCount += sphere.points->size();
					preview_widget->PrependPointcloud(sphere.points, ColorAt(Sphere_Colors, i),
						tr("Sphere %1").arg(i + 1),
						tr("Sphere %1 of %2: center (%3, %4, %5), radius %6")
							.arg(i + 1)
							.arg(inputName)
							.arg(sphere.model.center.x, 0, 'f', 3)
							.arg(sphere.model.center.y, 0, 'f', 3)
							.arg(sphere.model.center.z, 0, 'f', 3)
							.arg(sphere.model.radius, 0, 'f', 3));
					details << tr("sphere %1: center=(%2, %3, %4) radius=%5 (%6 points)")
								   .arg(i + 1)
								   .arg(sphere.model.center.x, 0, 'f', 3)
								   .arg(sphere.model.center.y, 0, 'f', 3)
								   .arg(sphere.model.center.z, 0, 'f', 3)
								   .arg(sphere.model.radius, 0, 'f', 3)
								   .arg(sphere.points->size());
				}
				SetStatus(tr("Detected %1 sphere(s) in the selected cloud with %2 points in total: %3")
						.arg(spheres.size())
						.arg(pointCount)
						.arg(details.join("; ")));
			});
	}

	/// Measures the reference spheres of the selected visible point clouds on the worker thread. Each
	/// fitted sphere is added as a colored layer and the table of results opens.
	void PointcloudEditorDialog::on_measure_spheres_action_triggered(bool)
	{
		if (IsWorking())
			return;
		MeasureSpheres();
	}

	void PointcloudEditorDialog::MeasureSpheres()
	{
		const auto pointclouds = SelectedClouds();
		if (pointclouds.empty())
		{
			SetStatus(tr("Select visible point clouds to measure."));
			return;
		}

		// The dialog can also describe a folder of scans; the editor only measures its layers.
		_sphereMeasurement.useFolder = false;
		SphereMeasurementDialog dialog(_sphereMeasurement, pointclouds.front().name, this);
		dialog.layer_radio->setChecked(true);
		dialog.layer_radio->setText(tr("Selected visible point clouds (%1)").arg(pointclouds.size()));
		dialog.folder_radio->hide();
		dialog.folder_edit->hide();
		dialog.folder_browse_button->hide();
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}
		_sphereMeasurement = dialog.Values();
		_sphereMeasurement.useFolder = false;
		const SphereMeasurementDialog::Settings settings = _sphereMeasurement;
		const Pointcloud::ArtifactReference reference = dialog.Reference();
		const Pointcloud::CompensationModel compensation = dialog.Compensation();

		// Layer history (filters, transforms) travels with the results.
		QStringList details;
		for (const auto& layer : pointclouds)
		{
			PointcloudPreviewWidget::LayerInfo info;
			details << (preview_widget->FindLayer(layer.id, info) ? info.details : QString());
		}

		const auto cancel = NewCancelFlag();
		RunAsync<std::shared_ptr<std::vector<SphereScanResult>>>(
			tr("Measuring spheres..."),
			[settings, reference, compensation, pointclouds, details, cancel]()
			{
				auto results = std::make_shared<std::vector<SphereScanResult>>();
				for (std::size_t i = 0; i < pointclouds.size(); ++i)
				{
					const Pointcloud::ColorCloud& cloud = *pointclouds[i].cloud;
					SphereScanResult scan;
					scan.name = pointclouds[i].name;
					scan.details = details.value(static_cast<int>(i));
					scan.spheres = Pointcloud::MeasureSpheres(
						settings.compensate ? *Pointcloud::Compensate(cloud, compensation) : cloud, settings.params, cancel.get());
					scan.labeled = reference.Count() > 0 && Pointcloud::LabelSpheres(scan.spheres, reference);
					if (!scan.labeled)
					{
						std::sort(scan.spheres.begin(), scan.spheres.end(),
							[](const Pointcloud::MeasuredSphere& a, const Pointcloud::MeasuredSphere& b)
							{ return a.center[0] < b.center[0]; });
					}
					results->push_back(std::move(scan));
				}
				return results;
			},
			[this, settings, reference](const std::shared_ptr<std::vector<SphereScanResult>>& results)
			{
				std::size_t found = 0;
				for (const auto& scan : *results)
					found += scan.spheres.size();
				SetStatus(tr("Measured %1 sphere(s) in %2 scan(s).").arg(found).arg(results->size()));

				for (const SphereScanResult& scan : *results)
				{
					// Optional: with many scans the sphere layers crowd the panel.
					for (std::size_t i = 0; settings.addSphereLayers && i < scan.spheres.size(); ++i)
					{
						const auto& sphere = scan.spheres[i];
						const QString label = scan.labeled ? QString::fromStdString(reference.names[sphere.label])
														   : QString("S%1").arg(i + 1);
						preview_widget->PrependPointcloud(sphere.points, ColorAt(Sphere_Colors, i),
							tr("sphere_%1").arg(label),
							tr("Sphere %1 of %2: center (%3, %4, %5), diameter %6, form RMS %7 \u00b5m")
								.arg(label)
								.arg(scan.name)
								.arg(sphere.center[0], 0, 'f', 3)
								.arg(sphere.center[1], 0, 'f', 3)
								.arg(sphere.center[2], 0, 'f', 3)
								.arg(sphere.Diameter(), 0, 'f', 4)
								.arg(sphere.formRms * 1000.0, 0, 'f', 1));
					}
				}
				// Opened once RunAsync has left the busy state, so the table does not show the wait cursor and
				// command actions are usable again after closing it.
				QTimer::singleShot(0, this,
					[this, results, reference, settings]()
					{
						SphereResultsDialog resultsDialog(std::move(*results), reference, _rootDir,
							settings.compensate
								? tr("Compensated with %1.").arg(QFileInfo(settings.compensationPath).fileName())
								: QString(),
							this);
						connect(&resultsDialog, &SphereResultsDialog::CentersLayerRequested, this,
							[this](const Pointcloud::ColorCloudPtr& cloud, const QString& name)
							{
								// The table may open while the viewer is still publishing the sphere layers;
								// the viewer queues this edit behind them.
								preview_widget->AddPointcloud(
									cloud, QColor(), name, tr("Sphere centers of the measured scans"));
							});
						resultsDialog.exec();
					});
			});
	}

	/// Corrects the selected point clouds with a compensation model and inserts the result at the top as
	/// "comp_<name>", so the next command (for example Measure Spheres) uses it.
	void PointcloudEditorDialog::on_compensate_action_triggered(bool)
	{
		const auto pointclouds = SelectedClouds();
		if (pointclouds.empty() || !pointclouds.front().cloud || pointclouds.front().cloud->empty())
		{
			SetStatus(tr("Select at least one visible point cloud."));
			return;
		}
		const QString path = QFileDialog::getOpenFileName(this, tr("Compensation model"),
			_compensationModelPath.isEmpty() ? _rootDir : _compensationModelPath, tr("Compensation model (*.json)"),
			nullptr, QFileDialog::DontUseNativeDialog);
		if (path.isEmpty())
		{
			return;
		}
		_compensationModelPath = path;

		Pointcloud::CompensationModel model;
		QString error;
		if (!Pointcloud::LoadCompensationModel(path, model, error))
		{
			QMessageBox::warning(this, tr("Compensate"), tr("The compensation model cannot be used: %1").arg(error));
			return;
		}
		// The editor knows the workspace, not the calibration each layer was reconstructed with.
		const QString calibration = QDir(_rootDir).filePath("calibration.yml");
		const QString name = pointclouds.front().name;
		const auto answer = QMessageBox::question(this, tr("Compensate"),
			tr("%1\n\nApply it to %2? Each layer must be a raw reconstruction (camera frame), not a cloud moved, "
			   "rotated, or aligned in the editor.")
				.arg(Pointcloud::DescribeCompensationModel(
					model, QFileInfo::exists(calibration) ? calibration : QString()))
				.arg(pointclouds.size() == 1 ? tr("\"%1\"").arg(name)
											 : tr("the %1 selected point clouds").arg(pointclouds.size())),
			QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
		if (answer != QMessageBox::Yes)
		{
			return;
		}

		struct Result
		{
			Pointcloud::ColorCloudPtr cloud;
			Pointcloud::CompensationStats stats;
		};
		RunSelected<std::shared_ptr<Result>>(
			tr("Compensating selected point clouds..."),
			[model](const CloudLayer& layer)
			{
				auto result = std::make_shared<Result>();
				result->cloud = Pointcloud::Compensate(*layer.cloud, model, &result->stats);
				return result;
			},
			[this, path](const CloudLayer& layer, const std::shared_ptr<Result>& result)
			{
				const auto name = layer.name;
				const QString outside = result->stats.outside > 0
					? tr("; %1 of them outside the trained volume (physical block only, or fading)")
						  .arg(result->stats.outside)
					: QString();
				preview_widget->PrependPointcloud(result->cloud, QColor(), tr("comp_%1").arg(name),
					tr("Compensated with %1: %2 points%3")
						.arg(QFileInfo(path).fileName())
						.arg(result->stats.points)
						.arg(outside));
				SetStatus(tr("Compensated %1 points of %2 with %3%4.")
						.arg(result->stats.points)
						.arg(name)
						.arg(QFileInfo(path).fileName())
						.arg(outside));
			});
	}
} // namespace smcp
