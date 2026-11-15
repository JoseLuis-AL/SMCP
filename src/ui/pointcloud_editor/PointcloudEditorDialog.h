/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Brown University nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DANIEL MORENO AND GABRIEL TAUBIN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "ai/WslModelService.h"
#include "core/AsyncTask.h"
#include "core/PointcloudOps.h"
#include "ui/pointcloud_editor/PointcloudLayersPanel.h"
#include "ui/pointcloud_editor/SphereMeasurementDialog.h"
#include "ui/preview/PointcloudPreviewWidget.h"
#include "ui_PointcloudEditorDialog.h"

class QComboBox;
class QTemporaryDir;
class QToolButton;

namespace smcp
{
	class AiInferenceProgressDialog;

	/// <summary>
	/// Point cloud editor: adds multiple .xyz files from the working directory, removes outliers,
	/// fits geometric models (plane, sphere) with RANSAC, registers the clouds with ICP, compares
	/// all results in the 3D viewer and exports the list.
	/// </summary>
	/// <remarks>
	/// This class is the controller of the editor window. The scene, its history and the drawing
	/// belong to PointcloudPreviewWidget; the list of layers to PointcloudLayersPanel; the numerical
	/// work to core/PointcloudOps and friends. What lives here is the glue: each menu command asks
	/// for its parameters, runs the work on a worker thread (RunAsync / RunSelected) and publishes
	/// the result as one undoable viewer command.
	///
	/// How a command runs, how the busy state works and how to add a new command are described in
	/// docs/EDITOR_ASYNC_HISTORY.md.
	/// </remarks>
	class PointcloudEditorDialog : public QDialog, public Ui::PointcloudEditorDialog
	{
		Q_OBJECT

	public:
		/// Creates, runs, and destroys the modal editor. The constructor is private so the dialog, which
		/// owns background work and a WSL process, is always used as a scoped modal window.
		static int Execute(const QString& rootDir, QWidget* parent = nullptr);
		~PointcloudEditorDialog() override;

		/// Closing is refused while a command runs, and offers to save a modified session.
		void reject() override;

	public slots:
		// Layer list header and project.
		void on_load_pointcloud_button_clicked(bool checked = false);
		void on_merge_pointclouds_button_clicked(bool checked = false);
		void on_remove_pointclouds_button_clicked(bool checked = false);
		/// Removes the layers the panel offers for deletion (LayersToDelete), after a confirmation,
		/// as one undoable command. The header button and the Delete key both end here.
		void DeleteLayers();
		void on_save_button_clicked(bool checked = false);
		void on_save_project_action_triggered(bool checked = false);
		void on_save_project_as_action_triggered(bool checked = false);
		void on_open_project_action_triggered(bool checked = false);

		// Commands (PointCloud, Fit and Measure menus).
		void on_remove_outliers_action_triggered(bool checked = false);
		void on_align_centroids_action_triggered(bool checked = false);
		void on_register_action_triggered(bool checked = false);
		void on_measure_spheres_action_triggered(bool checked = false);
		void on_compensate_action_triggered(bool checked = false);
		void on_fit_plane_action_triggered(bool checked = false);
		void on_fit_sphere_action_triggered(bool checked = false);

		// Tools.
		void on_rotate_button_toggled(bool checked);
		void on_move_button_toggled(bool checked);
		void on_select_button_toggled(bool checked);
		void on_brush_select_button_toggled(bool checked);
		void on_delete_selection_button_clicked(bool checked = false);

	private:
		explicit PointcloudEditorDialog(const QString& rootDir, QWidget* parent = nullptr,
			Qt::WindowFlags flags = Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

		/* Types ---------------------------------------------------------------------------------- */

		/// A layer as commands receive it: its id, name and (immutable) point data.
		using CloudLayer = PointcloudPreviewWidget::ListedPointcloud;

		/// Geometric models supported by the Fit menu.
		enum class FitModel
		{
			Plane,
			Sphere
		};

		/// Result of loading a file on the worker thread. The viewer prepares the GPU buffer when it publishes.
		struct LoadedPointcloud
		{
			Pointcloud::ColorCloudPtr cloud;
			bool hasFinitePoints = false; ///< False when no point can be drawn.
		};

		/// Parsed cloud kept in memory, valid while the file keeps the same size and modification time.
		struct CachedPointcloud
		{
			Pointcloud::ColorCloudPtr cloud;
			qint64 fileSize = 0;
			QDateTime lastModified;
			std::uint64_t lastUse = 0;
		};

		/// Result of registering the visible clouds onto the first one on the worker thread.
		struct Registration
		{
			struct Aligned
			{
				QString name;
				Pointcloud::ColorCloudPtr cloud; ///< Null when ICP failed for this cloud.
				Pointcloud::IcpResult icp;
			};

			Pointcloud::ColorCloudPtr merged; ///< The first cloud plus every cloud registered onto it.
			std::vector<Aligned> aligned;
		};

		/* Running commands ------------------------------------------------------------------------- */

		/// Runs `work` on a worker thread and then `done` on the GUI thread, inside one viewer command:
		/// every layer `done` adds or changes becomes a single history entry labeled `status`. While
		/// it runs the editor is busy (actions and tools disabled). An exception from either function
		/// ends in FailTask(), which leaves the scene as it was.
		template <typename Result>
		void RunAsync(const QString& status, std::function<Result()> work, std::function<void(const Result&)> done)
		{
			// Only the AI batch may chain a step while the editor is already busy.
			if (_busy && !_aiInferenceRunning && !_aiProgressDialog)
			{
				_cancel.reset();
				return;
			}
			SetBusy(true);
			SetStatus(status);
			RunBackground<Result>(
				this, std::move(work),
				[this, done, status](const Result& result)
				{
					preview_widget->BeginCommand(status);
					try
					{
						done(result);
						preview_widget->EndCommand();
					}
					catch (...)
					{
						preview_widget->AbortCommand();
						throw;
					}
				},
				[this](std::exception_ptr error) { FailTask(error); },
				[this]()
				{
					_cancel.reset();
					SetBusy(_aiInferenceRunning || _continueAi, false);
					if (_continueAi && !preview_widget->IsWorking())
						QTimer::singleShot(0, this, &PointcloudEditorDialog::ContinueAi);
				});
		}

		/// RunAsync over every selected visible cloud: `work` computes one result per cloud on the
		/// worker, `done` publishes each of them, and the whole batch is one history entry.
		template <typename Result>
		void RunSelected(const QString& status, std::function<Result(const CloudLayer&)> work,
			std::function<void(const CloudLayer&, const Result&)> done)
		{
			const auto inputs = SelectedClouds();
			if (inputs.empty())
			{
				_cancel.reset();
				SetStatus(tr("Select at least one visible point cloud."));
				return;
			}
			using Results = std::vector<std::pair<CloudLayer, Result>>;
			RunAsync<Results>(
				status,
				[inputs, work]()
				{
					Results results;
					for (const auto& input : inputs)
						results.emplace_back(input, work(input));
					return results;
				},
				[done](const Results& results)
				{
					for (const auto& result : results)
						done(result.first, result.second);
				});
		}

		/// Makes the command about to start cancellable: returns the flag its worker must pass to the
		/// core functions (they throw Pointcloud::OperationCanceled when it is set) and shows the
		/// Cancel button until the command ends. Call it right before RunAsync / RunSelected.
		std::shared_ptr<Pointcloud::CancelFlag> NewCancelFlag();
		void FailTask(std::exception_ptr error);
		/// `waitCursor` is false for AI inference, which shows its own modal progress window instead.
		void SetBusy(bool busy, bool waitCursor = true);
		/// True while a command of this dialog runs or the viewer is publishing an edit.
		bool IsWorking() const { return _busy || _viewerBusy; }
		/// Enables menu actions and tools unless a command is running.
		void UpdateCommandAvailability();
		void SetStatus(const QString& text);
		void RefreshHistory();

		/// Selected visible, non-empty layers in the order of the panel selection: the input of commands.
		std::vector<CloudLayer> SelectedClouds() const;
		Pointcloud::ColorCloudPtr FirstSelectedCloud() const;

		/* Commands --------------------------------------------------------------------------------- */

		void ConfigureFit(FitModel model);
		void FitPlanes();
		void FitSpheres();
		void MeasureSpheres();

		/* Project and files ------------------------------------------------------------------------ */

		/// Saves the editor project on a worker thread. Asks for a file when `chooseFile` is true or the
		/// session has no project file yet; closes the editor after a successful save when `closeAfterSave`.
		void SaveProject(bool chooseFile, bool closeAfterSave);
		/// Marks the session as changed (or saved) since the project was last saved or opened.
		void SetModified(bool modified);
		Pointcloud::ColorCloudPtr FindCachedPointcloud(
			const QString& path, qint64 fileSize, const QDateTime& lastModified);
		void StoreCachedPointcloud(const QString& path, qint64 fileSize, const QDateTime& lastModified,
			const Pointcloud::ColorCloudPtr& cloud);

		/* Tools ------------------------------------------------------------------------------------ */

		/// Shows the gizmo of `mode` (Rotate or Move) on the layer selected in the panel; the tool
		/// buttons are exclusive, so this also unchecks the other one.
		void StartTool(PointcloudPreviewWidget::GizmoMode mode);
		/// Hides the gizmo and unchecks both tool buttons.
		void StopTool();
		/// Turns box selection on or off and keeps the Select button in sync (it excludes Rotate and Move).
		void SetSelectionTool(bool enabled, bool radial = false);
		/// Shows Delete Selected, with the count, only while points are selected.
		void UpdateSelectionControls();
		/// Bakes a gizmo rotation (axis 0..2 = X, Y, Z) into the layer's points around their centroid.
		void ApplyRotation(int layerId, int axis, double degrees);
		/// Bakes a gizmo translation (cloud coordinates) into the layer's points.
		void ApplyTranslation(int layerId, double dx, double dy, double dz);

		/* AI models (WSL) -----------------------------------------------------------------------------
		   A batch runs the chosen model on each selected cloud, one after another:
		   ConfigureAiModel -> StartAiInference -> [StartNextAiInference -> FinishAiInference ->
		   LoadAiResult] per cloud -> the results are added together as one command. */

		void StartAiModelDiscovery();
		void SetAvailableAiModels(const QVector<AiModelInfo>& models, const QString& error = QString());
		void ConfigureAiModel(const QString& modelId);
		/// Enables model actions only when models are available and no command is running.
		void UpdateAiModelControls();
		void StartAiInference(const AiModelInfo& model, const QJsonObject& configuration);
		void StartNextAiInference();
		/// Starts the next cloud of the batch once the viewer has published the previous step.
		void ContinueAi();
		void FinishAiInference(bool success, const QString& diagnostics);
		void LoadAiResult();
		/// The cloud in progress could not be processed: notes why and goes on with the next one.
		void SkipAiCloud(const QString& reason);
		/// Adds the results obtained so far to the viewer (inside the command the caller has open).
		void AddAiResults();
		/// Ends the batch outside a viewer command: publishes the results and reports the outcome.
		void FinishAiBatch(const QString& interruption = QString());
		/// One line for the status bar: how many clouds were processed and which ones failed.
		QString AiBatchSummary(const QString& interruption) const;
		void CloseAiProgressDialog();

		/* State ------------------------------------------------------------------------------------ */

		/// Directory the .xyz files are looked up in.
		QString _rootDir;

		// Busy state. `_busy`: a command of this dialog is running. `_viewerBusy`: the viewer is
		// publishing an edit (mirrors PointcloudPreviewWidget::BusyChanged).
		bool _busy = false;
		bool _viewerBusy = false;
		bool _waitWanted = false;  ///< The running command wants the wait cursor (AI shows a dialog instead).
		bool _waitCursor = false;  ///< The wait cursor override is currently installed.
		/// Flag of the running command when it can be cancelled; null otherwise.
		std::shared_ptr<Pointcloud::CancelFlag> _cancel;

		// Project.
		QString _projectPath;
		QString _openingProjectPath;  ///< Project being opened, until the viewer reports ProjectRestored.
		bool _modified = false;       ///< Commands or history moves since the last save or open.

		/// Clouds already parsed, keyed by absolute file path, so reopening a file skips reading it.
		std::map<QString, CachedPointcloud> _cloudCache;
		std::uint64_t _cacheUseCounter = 0;

		// Last parameters of each command, offered again the next time its dialog opens.
		Pointcloud::OutlierRemovalParams _outlierRemovalParams;
		Pointcloud::PlaneFitParams _planeFitParams;
		Pointcloud::SphereFitParams _sphereFitParams;
		Pointcloud::IcpParams _icpParams;
		bool _icpAlignToMerged = true;
		SphereMeasurementDialog::Settings _sphereMeasurement;
		QString _compensationModelPath;

		/// Active transform tool and the layer carrying its gizmo (LayerInfo::id; -1 when no tool is open).
		PointcloudPreviewWidget::GizmoMode _toolMode = PointcloudPreviewWidget::GizmoMode::None;
		int _toolLayerId = -1;

		// AI models and the batch in progress.
		WslModelService* _aiModelService = nullptr;
		QHash<QString, AiModelInfo> _aiModels;
		QHash<QString, QJsonObject> _aiConfigurations;  ///< Last configuration accepted for each model.
		bool _aiConfigurationLoading = false;
		AiModelInfo _batchAiModel;
		QJsonObject _batchAiConfiguration;
		std::vector<CloudLayer> _aiInputs;   ///< Clouds of the batch still to process; the front one is running.
		std::vector<CloudLayer> _aiResults;  ///< Results so far, added to the viewer when the batch ends.
		int _aiBatchTotal = 0;               ///< Clouds the batch started with.
		QStringList _aiFailures;             ///< "cloud: reason" of the clouds that could not be processed.
		bool _aiInferenceRunning = false;
		bool _aiCancelRequested = false;
		bool _continueAi = false;            ///< The next cloud starts when the viewer becomes idle.
		std::unique_ptr<QTemporaryDir> _aiTemporaryDirectory;
		QString _aiResultPath;
		QString _aiResultName;
		AiInferenceProgressDialog* _aiProgressDialog = nullptr;

		// Widgets created in code (the rest come from the .ui form).
		QToolButton* load_pointcloud_button = nullptr;
		QToolButton* merge_pointclouds_button = nullptr;
		QToolButton* remove_pointclouds_button = nullptr;
		QToolButton* save_button = nullptr;
		QToolButton* _undoButton = nullptr;
		QToolButton* _redoButton = nullptr;
		QComboBox* _historyCombo = nullptr;
		QToolButton* _cancelButton = nullptr;
	};
} // namespace smcp
