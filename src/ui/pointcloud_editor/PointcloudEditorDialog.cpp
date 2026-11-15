// PointcloudEditorDialog, part 1 of 4: construction and signal wiring, the busy state, the layer
// selection and the editing tools. The other parts implement the same class:
//
//   PointcloudEditorCommands.cpp   the PointCloud, Fit and Measure menu commands
//   PointcloudEditorAi.cpp         AI models in WSL: discovery, configuration and the inference batch
//   PointcloudEditorFiles.cpp      loading clouds, exporting them, and editor projects

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
#include "ui/widgets/EditorWindowIcon.h"

namespace smcp
{


	/* Construction ============================================================================ */

	int PointcloudEditorDialog::Execute(const QString& rootDir, QWidget* parent)
	{
		PointcloudEditorDialog dialog(rootDir, parent);
		return dialog.exec();
	}

	PointcloudEditorDialog::PointcloudEditorDialog(const QString& rootDir, QWidget* parent, Qt::WindowFlags flags) :
		QDialog(parent, flags), _rootDir(rootDir)
	{
		setupUi(this);
		setWindowIcon(EditorWindowIcon());  // the points of the button that opens it, on a white square
		verticalLayout->setMenuBar(editor_menu_bar);
		new EditorDialogStyle(this);
		layers_panel->SetPreview(preview_widget);
		// A long status message (a fit with many results) must not widen the window: the label takes
		// the width it is given and the full text stays available as its tooltip.
		status_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		connect(preview_widget, &PointcloudPreviewWidget::BusyChanged, this,
			[this](bool busy)
			{
				_viewerBusy = busy;
				UpdateCommandAvailability();
				if (!busy && _continueAi)
					QTimer::singleShot(0, this, &PointcloudEditorDialog::ContinueAi);
			});
		connect(preview_widget, &PointcloudPreviewWidget::OperationFailed, this,
			[this](const QString& error)
			{
				_openingProjectPath.clear();
				FailTask(std::make_exception_ptr(std::runtime_error(error.toStdString())));
				SetBusy(false);
			});
		connect(preview_widget, &PointcloudPreviewWidget::ProjectRestored, this,
			[this]()
			{
				_projectPath = _openingProjectPath;
				_openingProjectPath.clear();
				setWindowTitle(tr("Point Cloud Editor - %1[*]").arg(QFileInfo(_projectPath).fileName()));
				SetModified(false);
				SetStatus(tr("Project opened: %1").arg(QDir::toNativeSeparators(_projectPath)));
			});
		_undoButton = new QToolButton(this);
		_undoButton->setText(tr("Undo"));
		_undoButton->setObjectName("undo_button");
		_redoButton = new QToolButton(this);
		_redoButton->setText(tr("Redo"));
		_redoButton->setObjectName("redo_button");
		_historyCombo = new QComboBox(this);
		_historyCombo->setObjectName("command_history");
		_historyCombo->setMinimumContentsLength(24);
		_historyCombo->setToolTip(tr("History: up to %1 commands; the oldest are dropped first when their point data "
									 "exceeds the memory budget")
				.arg(PointcloudPreviewWidget::Max_History_Entries));
		history_layout->insertWidget(1, _undoButton);
		history_layout->insertWidget(2, _redoButton);
		history_layout->insertWidget(3, _historyCombo);
		_cancelButton = new QToolButton(this);
		_cancelButton->setText(tr("Cancel command"));
		_cancelButton->setObjectName("cancel_command_button");  // drawn in the danger color by the theme
		_cancelButton->setToolTip(tr("Stop the running command; the point clouds stay as they were"));
		_cancelButton->hide();
		history_layout->insertWidget(4, _cancelButton);
		connect(_cancelButton, &QToolButton::clicked, this,
			[this]()
			{
				if (_cancel && !_cancel->exchange(true))
				{
					SetStatus(tr("Canceling..."));
					UpdateCommandAvailability();
				}
			});
		connect(_undoButton, &QToolButton::clicked, preview_widget, &PointcloudPreviewWidget::Undo);
		connect(_redoButton, &QToolButton::clicked, preview_widget, &PointcloudPreviewWidget::Redo);
		connect(_historyCombo, QOverload<int>::of(&QComboBox::activated), preview_widget,
			&PointcloudPreviewWidget::JumpToHistory);
		connect(
			preview_widget, &PointcloudPreviewWidget::HistoryChanged, this, &PointcloudEditorDialog::RefreshHistory);
		// Opening a project also emits HistoryChanged; ProjectRestored then marks the session as saved.
		connect(preview_widget, &PointcloudPreviewWidget::HistoryChanged, this, [this]() { SetModified(true); });
		auto* undoShortcut = new QShortcut(QKeySequence::Undo, this);
		auto* redoShortcut = new QShortcut(QKeySequence::Redo, this);
		auto* alternateRedo = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z), this);
		connect(undoShortcut, &QShortcut::activated, this,
			[this]()
			{
				if (!IsWorking())
					preview_widget->Undo();
			});
		connect(redoShortcut, &QShortcut::activated, this,
			[this]()
			{
				if (!IsWorking())
					preview_widget->Redo();
			});
		connect(alternateRedo, &QShortcut::activated, this,
			[this]()
			{
				if (!IsWorking())
					preview_widget->Redo();
			});
		RefreshHistory();

		view_splitter->setStretchFactor(0, 0);
		view_splitter->setStretchFactor(1, 0);
		view_splitter->setStretchFactor(2, 1);
		view_splitter->setSizes({ 280, 64, 1000 });
		align_centroids_action->setEnabled(false);
		register_action->setEnabled(false);
		merge_pointclouds_button = new QToolButton(layers_panel);
		remove_pointclouds_button = new QToolButton(layers_panel);
		load_pointcloud_button = new QToolButton(layers_panel);
		save_button = new QToolButton(layers_panel);
		load_pointcloud_button->setObjectName("load_pointcloud_button");
		save_button->setObjectName("save_button");
		merge_pointclouds_button->setObjectName("merge_pointclouds_button");
		remove_pointclouds_button->setObjectName("remove_pointclouds_button");
		remove_pointclouds_button->setIcon(QIcon(":/trash.svg"));
		remove_pointclouds_button->setToolTip(tr("Delete the highlighted point clouds (Del)"));
		remove_pointclouds_button->hide();
		merge_pointclouds_button->setIcon(QIcon(":/layers-arrow-down.svg"));
		merge_pointclouds_button->setToolTip(tr("Merge the target point clouds"));
		merge_pointclouds_button->hide();
		load_pointcloud_button->setIcon(QIcon(":/plus.svg"));
		save_button->setIcon(QIcon(":/file-down.svg"));
		load_pointcloud_button->setToolTip(tr("Add point cloud"));
		save_button->setToolTip(tr("Export the target point clouds"));
		for (auto* button :
			{ remove_pointclouds_button, merge_pointclouds_button, load_pointcloud_button, save_button })
		{
			button->setIconSize(QSize(22, 22));
			button->setAutoRaise(true);
			button->setAccessibleName(button->toolTip());
		}
		layers_panel->AddHeaderButtons(
			remove_pointclouds_button, merge_pointclouds_button, load_pointcloud_button, save_button);
		connect(remove_pointclouds_button, &QToolButton::clicked, this,
			&PointcloudEditorDialog::on_remove_pointclouds_button_clicked);
		connect(merge_pointclouds_button, &QToolButton::clicked, this,
			&PointcloudEditorDialog::on_merge_pointclouds_button_clicked);
		connect(load_pointcloud_button, &QToolButton::clicked, this,
			&PointcloudEditorDialog::on_load_pointcloud_button_clicked);
		connect(save_button, &QToolButton::clicked, this, &PointcloudEditorDialog::on_save_button_clicked);
		connect(preview_widget, &PointcloudPreviewWidget::PointcloudsChanged, this,
			[this]()
			{
				if (_toolLayerId >= 0 && preview_widget->IndexOfLayer(_toolLayerId) < 0)
				{
					StopTool();
				}
			});
		// Editing tools and commands share the targets of the panel.
		connect(layers_panel, &PointcloudLayersPanel::SelectedLayerChanged, this,
			[this]()
			{
				preview_widget->SetActiveLayers(layers_panel->SelectedLayerIds());
				if (_toolLayerId >= 0)
				{
					StartTool(_toolMode);
				}
				UpdateCommandAvailability();
			});
		connect(layers_panel, &PointcloudLayersPanel::HighlightChanged, this,
			&PointcloudEditorDialog::UpdateCommandAvailability);
		connect(layers_panel, &PointcloudLayersPanel::DeleteRequested, this, &PointcloudEditorDialog::DeleteLayers);
		connect(preview_widget, &PointcloudPreviewWidget::LayerRotating, this,
			[this](int axis, double degrees)
			{
				SetStatus(tr("Rotating around %1: %2\u00b0 (Ctrl snaps to 5\u00b0; Esc or right click cancels)")
						.arg(QString("XYZ").at(axis))
						.arg(degrees, 0, 'f', 1));
			});
		connect(preview_widget, &PointcloudPreviewWidget::LayerRotated, this, &PointcloudEditorDialog::ApplyRotation);
		connect(preview_widget, &PointcloudPreviewWidget::LayerMoving, this,
			[this](double dx, double dy, double dz)
			{
				SetStatus(tr("Moving by (%1, %2, %3) (Ctrl snaps to whole units; Esc or right click cancels)")
						.arg(dx, 0, 'f', 2)
						.arg(dy, 0, 'f', 2)
						.arg(dz, 0, 'f', 2));
			});
		connect(preview_widget, &PointcloudPreviewWidget::LayerMoved, this, &PointcloudEditorDialog::ApplyTranslation);
		connect(preview_widget, &PointcloudPreviewWidget::GizmoDragCanceled, this,
			[this]() { SetStatus(tr("Change canceled.")); });
		connect(preview_widget, &PointcloudPreviewWidget::SelectionChanged, this,
			[this]()
			{
				UpdateSelectionControls();
				const std::size_t count = preview_widget->SelectedPointCount();
				SetStatus(count == 0 ? tr("Selection cleared.")
									 : tr("%1 points selected in %2 layer(s).")
										   .arg(QLocale().toString(static_cast<qulonglong>(count)))
										   .arg(preview_widget->SelectedLayerCount()));
			});
		connect(preview_widget, &PointcloudPreviewWidget::PointcloudsChanged, this,
			&PointcloudEditorDialog::UpdateSelectionControls);
		UpdateSelectionControls();

		_aiModelService = new WslModelService(this);
		connect(_aiModelService, &WslModelService::OutputReceived, this,
			[this](const QString& text)
			{
				const QStringList lines = text.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
				if (lines.isEmpty())
					return;
				SetStatus(lines.back());
				if (_aiProgressDialog)
					_aiProgressDialog->SetOutput(lines.back());
			});
		StartAiModelDiscovery();
		SetStatus(QString());
		showMaximized();
	}

	PointcloudEditorDialog::~PointcloudEditorDialog() = default;

	void PointcloudEditorDialog::SetStatus(const QString& text)
	{
		status_label->setText(text);
		status_label->setToolTip(text);
	}

	/// Common end of a failed command or AI step: drops the AI batch, discards the open command and
	/// tells the user. The caller clears the busy state.
	void PointcloudEditorDialog::FailTask(std::exception_ptr error)
	{
		_aiInputs.clear();
		_aiResults.clear();
		_continueAi = false;
		_aiInferenceRunning = false;
		CloseAiProgressDialog();
		_aiTemporaryDirectory.reset();
		preview_widget->AbortCommand();
		QString message = tr("Unknown error.");
		try
		{
			std::rethrow_exception(error);
		}
		catch (const Pointcloud::OperationCanceled&)
		{
			// Asked for by the user: nothing was published, and it is not an error to report.
			SetStatus(tr("Command canceled."));
			return;
		}
		catch (const std::exception& e)
		{
			message = QString::fromUtf8(e.what());
		}
		catch (...)
		{
		}
		SetStatus(tr("Operation failed: %1").arg(message));
		QMessageBox::warning(this, tr("Operation failed"), message);
	}
	std::shared_ptr<Pointcloud::CancelFlag> PointcloudEditorDialog::NewCancelFlag()
	{
		_cancel = std::make_shared<Pointcloud::CancelFlag>(false);
		return _cancel;
	}

	void PointcloudEditorDialog::RefreshHistory()
	{
		const QSignalBlocker blocker(_historyCombo);
		_historyCombo->clear();
		_historyCombo->addItems(preview_widget->HistoryLabels());
		_historyCombo->setCurrentIndex(preview_widget->HistoryPosition());
		_undoButton->setEnabled(!IsWorking() && preview_widget->HistoryPosition() > 0);
		_redoButton->setEnabled(!IsWorking() && preview_widget->HistoryPosition() + 1 < _historyCombo->count());
		_historyCombo->setEnabled(!IsWorking());
	}
	/// Disables command actions and, unless `waitCursor` is false, shows the wait cursor while a
	/// command runs in the background.
	void PointcloudEditorDialog::SetBusy(bool busy, bool waitCursor)
	{
		_busy = busy;
		_waitWanted = busy && waitCursor;
		UpdateCommandAvailability();
	}

	void PointcloudEditorDialog::UpdateCommandAvailability()
	{
		for (QWidget* w : { static_cast<QWidget*>(layers_panel), static_cast<QWidget*>(load_pointcloud_button),
				 static_cast<QWidget*>(save_button), static_cast<QWidget*>(rotate_button),
				 static_cast<QWidget*>(move_button), static_cast<QWidget*>(select_button),
				 static_cast<QWidget*>(brush_select_button), static_cast<QWidget*>(delete_selection_button) })
		{
			w->setEnabled(!IsWorking());
		}
		// A command reads the layers it starts with; the tools stay off until it finishes.
		if (_busy && _toolLayerId >= 0)
		{
			StopTool();
		}
		if (_busy && (select_button->isChecked() || brush_select_button->isChecked()))
		{
			SetSelectionTool(false);
		}
		_cancelButton->setVisible(_busy && _cancel);
		_cancelButton->setEnabled(_cancel && !_cancel->load());
		const bool waiting = _viewerBusy || (_busy && _waitWanted);
		if (waiting && !_waitCursor)
		{
			QApplication::setOverrideCursor(Qt::WaitCursor);
			_waitCursor = true;
		}
		if (!waiting && _waitCursor)
		{
			QApplication::restoreOverrideCursor();
			_waitCursor = false;
		}
		RefreshHistory();
		for (auto* action :
			{ remove_outliers_action, compensate_action, measure_spheres_action, fit_plane_action, fit_sphere_action })
			action->setEnabled(!IsWorking());
		save_project_action->setEnabled(!IsWorking());
		save_project_as_action->setEnabled(!IsWorking());
		open_project_action->setEnabled(!IsWorking());
		const bool several = SelectedClouds().size() >= 2;
		align_centroids_action->setEnabled(!IsWorking() && several);
		register_action->setEnabled(!IsWorking() && several);
		merge_pointclouds_button->setVisible(several);
		merge_pointclouds_button->setEnabled(!IsWorking() && several);
		const bool deletable = !layers_panel->LayersToDelete().empty();
		remove_pointclouds_button->setVisible(deletable);
		remove_pointclouds_button->setEnabled(!IsWorking() && deletable);
		UpdateAiModelControls();
	}

	/// The window close button does not interrupt a command already in progress, and offers to save
	/// a session changed since it was last saved or opened.
	void PointcloudEditorDialog::reject()
	{
		if (IsWorking())
		{
			return;
		}
		if (_modified)
		{
			const auto answer = QMessageBox::question(this, tr("Close editor"),
				tr("The editor session has changes that are not saved in a project. Save them before closing?"),
				QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
			if (answer == QMessageBox::Cancel)
				return;
			if (answer == QMessageBox::Save)
			{
				SaveProject(false, true);
				return;
			}
		}
		QDialog::reject();
	}

	void PointcloudEditorDialog::SetModified(bool modified)
	{
		_modified = modified;
		setWindowModified(modified);
	}

	/* Layer selection ========================================================================= */

	void PointcloudEditorDialog::on_merge_pointclouds_button_clicked(bool)
	{
		if (IsWorking() || SelectedClouds().size() < 2)
			return;
		preview_widget->MergeSelectedPointclouds();
	}

	void PointcloudEditorDialog::on_remove_pointclouds_button_clicked(bool)
	{
		DeleteLayers();
	}

	void PointcloudEditorDialog::DeleteLayers()
	{
		if (IsWorking())
			return;
		const std::vector<int> ids = layers_panel->LayersToDelete();
		if (ids.empty())
		{
			SetStatus(tr("Highlight the point clouds to delete in the Point Clouds panel."));
			return;
		}
		PointcloudPreviewWidget::LayerInfo first;
		preview_widget->FindLayer(ids.front(), first);
		const QString question = ids.size() == 1
			? tr("Delete \"%1\" from the editor? This can be undone. Files on disk are not changed.").arg(first.name)
			: tr("Delete %1 point clouds from the editor? This can be undone. Files on disk are not changed.")
				  .arg(ids.size());
		if (QMessageBox::question(this, ids.size() == 1 ? tr("Delete point cloud") : tr("Delete point clouds"), question,
				QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
			return;

		// Rows are removed from the last one up, so the positions of the others stay valid; the
		// whole removal is one command and one undo step.
		std::vector<int> rows;
		for (int id : ids)
		{
			const int row = preview_widget->IndexOfLayer(id);
			if (row >= 0)
				rows.push_back(row);
		}
		std::sort(rows.begin(), rows.end(), std::greater<int>());
		try
		{
			preview_widget->BeginCommand(ids.size() == 1 ? tr("Remove point cloud") : tr("Delete point clouds"));
			for (int row : rows)
				preview_widget->RemovePointcloud(row);
			preview_widget->EndCommand();
		}
		catch (const std::logic_error&)
		{
			preview_widget->AbortCommand();
			SetStatus(tr("Wait for the current operation to finish before deleting point clouds."));
		}
	}

	std::vector<PointcloudEditorDialog::CloudLayer> PointcloudEditorDialog::SelectedClouds() const
	{
		// In the order of the panel selection; hidden and empty layers do not take part in commands.
		const std::vector<CloudLayer> layers = preview_widget->Pointclouds();
		std::vector<CloudLayer> result;
		for (int id : layers_panel->SelectedLayerIds())
		{
			const auto layer = std::find_if(layers.begin(), layers.end(), [id](const CloudLayer& l) { return l.id == id; });
			if (layer != layers.end() && layer->visible && layer->cloud && !layer->cloud->empty())
				result.push_back(*layer);
		}
		return result;
	}
	Pointcloud::ColorCloudPtr PointcloudEditorDialog::FirstSelectedCloud() const
	{
		const auto clouds = SelectedClouds();
		return clouds.empty() ? nullptr : clouds.front().cloud;
	}


	/* Tools: transform gizmo and point selection ============================================== */

	void PointcloudEditorDialog::on_rotate_button_toggled(bool checked)
	{
		if (checked)
		{
			StartTool(PointcloudPreviewWidget::GizmoMode::Rotate);
		}
		else
		{
			StopTool();
			SetStatus(QString());
		}
	}

	void PointcloudEditorDialog::on_move_button_toggled(bool checked)
	{
		if (checked)
		{
			StartTool(PointcloudPreviewWidget::GizmoMode::Move);
		}
		else
		{
			StopTool();
			SetStatus(QString());
		}
	}

	void PointcloudEditorDialog::StartTool(PointcloudPreviewWidget::GizmoMode mode)
	{
		const int layerId = layers_panel->SelectedLayerId();
		PointcloudPreviewWidget::LayerInfo layer;
		if (!preview_widget->FindLayer(layerId, layer))
		{
			StopTool();
			SetStatus(tr("Check and highlight a point cloud in the Point Clouds panel first."));
			return;
		}

		const bool rotate = mode == PointcloudPreviewWidget::GizmoMode::Rotate;
		SetSelectionTool(false);
		_toolMode = mode;
		_toolLayerId = layerId;
		preview_widget->SetGizmo(mode, layerId);
		{
			const QSignalBlocker rotateBlocker(rotate_button);
			const QSignalBlocker moveBlocker(move_button);
			rotate_button->setChecked(rotate);
			move_button->setChecked(!rotate);
		}
		const QString name = layer.name;
		SetStatus(rotate ? tr("Drag a ring to rotate %1: X red, Y green, Z blue. Ctrl snaps to 5\u00b0; Esc or right "
							  "click cancels.")
							   .arg(name)
						 : tr("Drag an arrow to move %1 along X (red), Y (green), or Z (blue), or the center circle to "
							  "move it in the view "
							  "plane. Ctrl snaps to whole units; Esc or right click cancels.")
							   .arg(name));
	}

	void PointcloudEditorDialog::StopTool()
	{
		_toolMode = PointcloudPreviewWidget::GizmoMode::None;
		_toolLayerId = -1;
		preview_widget->SetGizmo(PointcloudPreviewWidget::GizmoMode::None, -1);
		const QSignalBlocker rotateBlocker(rotate_button);
		const QSignalBlocker moveBlocker(move_button);
		rotate_button->setChecked(false);
		move_button->setChecked(false);
	}

	void PointcloudEditorDialog::on_select_button_toggled(bool checked)
	{
		if (checked)
		{
			StopTool();
			SetSelectionTool(true);
			SetStatus(
				tr("Drag a box with the left button to select points of the visible layers. Shift adds, Ctrl removes, "
				   "a click clears; the middle button orbits."));
		}
		else
		{
			SetSelectionTool(false);
			SetStatus(QString());
		}
	}

	void PointcloudEditorDialog::on_brush_select_button_toggled(bool checked)
	{
		if (checked)
		{
			StopTool();
			SetSelectionTool(true, true);
			SetStatus(tr("Paint with the left button to add points to the selection. Ctrl + wheel changes the brush "
						 "radius; Esc cancels the stroke."));
		}
		else
			SetSelectionTool(false);
	}

	void PointcloudEditorDialog::SetSelectionTool(bool enabled, bool radial)
	{
		preview_widget->SetSelectionMode(enabled && !radial);
		preview_widget->SetBrushSelectionMode(enabled && radial);
		const QSignalBlocker blocker(select_button);
		const QSignalBlocker brushBlocker(brush_select_button);
		select_button->setChecked(enabled && !radial);
		brush_select_button->setChecked(enabled && radial);
	}

	void PointcloudEditorDialog::UpdateSelectionControls()
	{
		const std::size_t count = preview_widget->SelectedPointCount();
		delete_selection_button->setVisible(count > 0);
		const auto description =
			tr("Delete selected points (%1)").arg(QLocale().toString(static_cast<qulonglong>(count)));
		delete_selection_button->setToolTip(description);
		delete_selection_button->setAccessibleName(description);
	}

	/// Removes the selected points from each layer (a new cloud, since the original may be shared with
	/// other layers or the file cache). A layer left without points is removed.
	void PointcloudEditorDialog::on_delete_selection_button_clicked(bool)
	{
		if (IsWorking() || !preview_widget->SelectedPointCount())
			return;
		const auto answer = QMessageBox::question(this, tr("Delete selected points"),
			tr("Delete %1 points? This can be undone in the command history. Files on disk are not changed.")
				.arg(preview_widget->SelectedPointCount()),
			QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
		if (answer == QMessageBox::Yes)
			preview_widget->DeleteSelectedPoints();
	}

	void PointcloudEditorDialog::ApplyTranslation(int layerId, double dx, double dy, double dz)
	{
		PointcloudPreviewWidget::LayerInfo layer;
		if (!preview_widget->FindLayer(layerId, layer))
		{
			return;
		}
		const QString name = layer.name;
		const QString change = tr("moved by (%1, %2, %3)").arg(dx, 0, 'f', 2).arg(dy, 0, 'f', 2).arg(dz, 0, 'f', 2);
		preview_widget->TransformSelected(cv::Matx44d(1, 0, 0, dx, 0, 1, 0, dy, 0, 0, 1, dz, 0, 0, 0, 1), change);
		SetStatus(tr("%1 %2.").arg(name).arg(change));
	}

	/// The viewer previewed the same rotation in OpenGL coordinates, (x, -y, -z), which is a 180 degree
	/// turn around X, so a rotation around a cloud axis keeps its angle there.
	void PointcloudEditorDialog::ApplyRotation(int layerId, int axis, double degrees)
	{
		PointcloudPreviewWidget::LayerInfo layer;
		if (!preview_widget->FindLayer(layerId, layer))
		{
			return;
		}

		const double angle = qDegreesToRadians(degrees);
		const double c = std::cos(angle), s = std::sin(angle);
		const cv::Matx33d r = axis == 0 ? cv::Matx33d(1, 0, 0, 0, c, -s, 0, s, c)
			: axis == 1					? cv::Matx33d(c, 0, s, 0, 1, 0, -s, 0, c)
										: cv::Matx33d(c, -s, 0, s, c, 0, 0, 0, 1);
		const cv::Vec3d pivot = preview_widget->SelectionCentroid();
		const cv::Vec3d t = pivot - r * pivot;
		const cv::Matx44d transform(r(0, 0), r(0, 1), r(0, 2), t[0], r(1, 0), r(1, 1), r(1, 2), t[1], r(2, 0), r(2, 1),
			r(2, 2), t[2], 0, 0, 0, 1);

		const QString rotation = tr("rotated %1 %2\u00b0").arg(QString("XYZ").at(axis)).arg(degrees, 0, 'f', 1);
		preview_widget->TransformSelected(transform, rotation);
		SetStatus(tr("Rotated selected point clouds around their centroid."));
	}

} // namespace smcp
