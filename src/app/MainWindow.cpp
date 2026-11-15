/*
Copyright (c) 2014, Daniel Moreno and Gabriel Taubin
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

#include "app/MainWindow.h"

#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QButtonGroup>
#include <QLabel>
#include <QStatusBar>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>

#include <QPainter>
#include <QIntValidator>
#include <QDoubleValidator>

#include <algorithm>
#include <iostream>

#include "core/StructuredLight.h"

#include "app/Application.h"
#include "core/IoUtil.h"

#include "ui/AboutDialog.h"
#include "ui/CalibrationDialog.h"
#ifdef USE_SPINNAKER
#include "ui/CaptureDialog.h"
#endif
#include "core/Compensation.h"
#include "export/IOExport.h"
#include "core/BusyCursorGuard.h"
#include "ui/pointcloud_editor/PointcloudEditorDialog.h"

namespace smcp
{

MainWindow::MainWindow(QWidget* parent, Qt::WindowFlags flags) :
	QMainWindow(parent, flags)
{
	setupUi(this);

#ifdef Q_OS_OSX
	image_tree->setFocusPolicy(Qt::ClickFocus);
#endif //Q_OS_OSX

	image_tree->setModel(&(APP->model));
	connect(APP, &Application::root_dir_changed, this, &MainWindow::_on_root_dir_changed);
	connect(image_tree->selectionModel(), &QItemSelectionModel::currentChanged,
		this, &MainWindow::_on_image_tree_currentChanged);

	QSettings& config = APP->config;

	setWindowTitle(Settings::App::Window_Name);

	threshold_spin->blockSignals(true);
	threshold_spin->setRange(0, 255);
	threshold_spin->setValue(config.value(Settings::Decode::Threshold, Settings::Decode::Threshold_Default_Value).toUInt());
	threshold_spin->blockSignals(false);

	b_line->blockSignals(true);
	b_line->setValidator(new QDoubleValidator(0.0, 1.0, 6, this));
	b_line->setText(config.value(Settings::Decode::B, Settings::Decode::B_Default_Value).toString());
	b_line->blockSignals(false);

	m_spin->blockSignals(true);
	m_spin->setRange(0, 255);
	m_spin->blockSignals(false);
	m_spin->setValue(config.value(Settings::Decode::M, Settings::Decode::M_Default_Value).toUInt());

	corner_count_x_spin->blockSignals(true);
	corner_count_x_spin->setRange(1, 255);
	corner_count_x_spin->blockSignals(false);
	corner_count_x_spin->setValue(config.value(Settings::Chessboard::Columns, Settings::Chessboard::Columns_Default_Value).toUInt());

	corner_count_y_spin->blockSignals(true);
	corner_count_y_spin->setRange(1, 255);
	corner_count_y_spin->blockSignals(false);
	corner_count_y_spin->setValue(config.value(Settings::Chessboard::Rows, Settings::Chessboard::Rows_Default_Value).toUInt());

	corners_width_line->blockSignals(true);
	corners_width_line->setValidator(new QDoubleValidator(this));
	corners_width_line->setText(config.value(Settings::Chessboard::Width, Settings::Chessboard::Width_Default_Value).toString());
	corners_width_line->blockSignals(false);

	corners_height_line->blockSignals(true);
	corners_height_line->setValidator(new QDoubleValidator(this));
	corners_height_line->setText(config.value(Settings::Chessboard::Height, Settings::Chessboard::Height_Default_Value).toString());
	corners_height_line->blockSignals(false);

	homography_window_spin->blockSignals(true);
	homography_window_spin->setRange(0, 1024);
	homography_window_spin->setValue(config.value(Settings::Calibration::H_Win, Settings::Calibration::H_Win_Default_Value).toUInt());
	homography_window_spin->blockSignals(false);

	joint_refinement_check->blockSignals(true);
	joint_refinement_check->setChecked(config.value(Settings::Calibration::Joint_Refinement, Settings::Calibration::Joint_Refinement_Default_Value).toBool());
	joint_refinement_check->blockSignals(false);

	subpixel_corners_check->blockSignals(true);
	subpixel_corners_check->setChecked(config.value(Settings::Calibration::Subpixel_Corners, Settings::Calibration::Subpixel_Corners_Default_Value).toBool());
	subpixel_corners_check->blockSignals(false);

	plane_triangulation_check->blockSignals(true);
	plane_triangulation_check->setChecked(config.value(Settings::Reconstruction::Plane_Triangulation, Settings::Reconstruction::Plane_Triangulation_Default_Value).toBool());
	plane_triangulation_check->blockSignals(false);

	//displayOriginalRadio->blockSignals(true);
	//displayOriginalRadio->setChecked(true);
	//displayOriginalRadio->blockSignals(false);

	max_dist_line->blockSignals(true);
	max_dist_line->setValidator(new QDoubleValidator(this));
	max_dist_line->setText(config.value(Settings::Reconstruction::Max_Dist, Settings::Reconstruction::Max_Dist_Default_Value).toString());
	max_dist_line->blockSignals(false);

	normals_check->blockSignals(true);
	normals_check->setChecked(config.value(Settings::Reconstruction::Save_Normals, Settings::Reconstruction::Save_Normals_Default_Value).toBool());
	normals_check->blockSignals(false);

	colors_check->blockSignals(true);
	colors_check->setChecked(config.value(Settings::Reconstruction::Save_Colors, Settings::Reconstruction::Save_Colors_Default_Value).toBool());
	colors_check->blockSignals(false);

	subpixel_check->blockSignals(true);
	subpixel_check->setChecked(config.value(Settings::Reconstruction::Subpixel, Settings::Reconstruction::Subpixel_Default_Value).toBool());
	subpixel_check->blockSignals(false);

	compensation_check->blockSignals(true);
	compensation_check->setChecked(config.value(Settings::Reconstruction::Compensate, Settings::Reconstruction::Compensate_Default_Value).toBool());
	compensation_check->blockSignals(false);
	UpdateCompensationTooltip();

	//set contextual menu for layout changing
	auto horizontalAction = new QAction("Horizontal", current_image_group);
	auto verticalAction = new QAction("Vertical", current_image_group);
	auto layoutActionGroup = new QActionGroup(current_image_group);
	layoutActionGroup->addAction(horizontalAction);
	layoutActionGroup->addAction(verticalAction);
	horizontalAction->setCheckable(true);
	verticalAction->setCheckable(true);
	horizontalAction->setChecked(true);
	verticalAction->setChecked(false);
	connect(horizontalAction, SIGNAL(triggered(bool)), this, SLOT(_on_horizontal_layout_action_triggered(bool)));
	connect(verticalAction, SIGNAL(triggered(bool)), this, SLOT(_on_vertical_layout_action_triggered(bool)));
	current_image_group->addActions(layoutActionGroup->actions());
	current_image_group->setContextMenuPolicy(Qt::ActionsContextMenu);

	// Group view buttons so only one can be checked at a time (exclusive behavior).
	auto viewButtonGroup = new QButtonGroup(this);
	viewButtonGroup->setExclusive(true);
	viewButtonGroup->addButton(image_view_button);
	viewButtonGroup->addButton(pattern_view_button);
	viewButtonGroup->addButton(projector_view_button);
	viewButtonGroup->addButton(view_3d_view_button);

	// Calibration in use, always visible at the right of the status bar.
	_calibrationStatus = new QLabel(this);
	statusBar()->addPermanentWidget(_calibrationStatus);

	// Workflow shortcuts, in the order of the Actions column.
	const struct { QToolButton* button; const char* keys; const char* description; } actions[] = {
		{ capture_action_button, "Ctrl+1", "Capture a new set with the camera and the projector" },
		{ extract_corners_action_button, "Ctrl+2", "Find the chessboard in the selected sets" },
		{ decode_action_button, "Ctrl+3", "Decode the Gray-code patterns of the selected sets" },
		{ calibration_action_button, "Ctrl+4", "Calibrate camera and projector with the selected sets" },
		{ reconstruction_action_button, "Ctrl+5", "Reconstruct the current set and export the point cloud" },
		{ pointcloud_action_button, "Ctrl+6", "Open the point cloud editor" },
	};
	for (const auto& action : actions)
	{
		action.button->setShortcut(QKeySequence(action.keys));
		action.button->setProperty("baseToolTip", QString("%1 (%2)").arg(action.description, action.keys));
		action.button->setToolTip(action.button->property("baseToolTip").toString());
	}

	// Selecting or unselecting sets changes what can run.
	connect(&(APP->model), &QAbstractItemModel::dataChanged, this, [this]() { UpdateWorkflowState(); });

	// Set view.
	ResetView();
	UpdateWorkflowState();

	show_message("Ready");
}

MainWindow::~MainWindow()
{}

void MainWindow::on_change_dir_action_triggered(bool checked)
{
	APP->ChangeRootDir(this);
}

void MainWindow::on_load_calibration_action_triggered(bool checked)
{
	APP->LoadCalibration(this);
	UpdateWorkflowState();
}

void MainWindow::on_save_calibration_action_triggered(bool checked)
{
	APP->SaveCalibration(this);
}

void MainWindow::on_display_calibration_action_triggered(bool checked)
{
	CalibrationDialog dialog(this, Qt::WindowCloseButtonHint);
	dialog.exec();
	UpdateWorkflowState();  // a calibration can be loaded from that dialog
}

/// <summary>
/// Loads and displays the corresponding image(s) in the preview labels based on the active view.
/// </summary>
/// <param name="current">The newly selected model index.</param>
/// <param name="previous">The previously selected model index (not used).</param>
/// <remarks>
/// Resolves the set level and image row from the model hierarchy, then loads image content according to the active view button. Detected chessboard
/// corners (camera or projector) are overlaid as circles on the image when available. The busy cursor is managed automatically via BusyCursorGuard
/// (RAII).
/// </remarks>
void MainWindow::_on_image_tree_currentChanged(const QModelIndex& current, const QModelIndex& previous) const
{
	// Clear previous image content.
	image1_label->Clear();
	image2_label->Clear();

	if (!current.isValid())
	{
		return;
	}

	Application* const app = APP;

	// Resolve set level and image row from the model hierarchy.
	unsigned level = 0, row = 0;
	const QModelIndex parent = app->model.parent(current);
	if (parent.parent().isValid())
	{
		// Child item: parent is the set, current is the specific image.
		level = parent.row();
		row = current.row();
	}
	else
	{
		// Top-level item: treat as set root and show the first image.
		level = current.row();
		row = 0;
	}

	cv::Mat image1, image2;

	// Show busy cursor while loading image data (restored automatically on scope exit).
	const BusyCursorGuard cursorGuard;
	QApplication::processEvents();

	// Load image content based on the active view button (mutually exclusive).
	if (image_view_button->isChecked())
	{
		image1 = app->GetImage(level, row, ColorImageRole);
	}
	else if (pattern_view_button->isChecked() && app->patternList.size() > level)
	{
		app->MakePatternImages(level, image1, image2);
	}
	else if (projector_view_button->isChecked())
	{
		image1 = app->GetProjectorView(level);
	}

	// Resolve detected corners to overlay: camera corners for standard views,
	// projector corners (scaled to image resolution) for the projector view.
	std::vector<cv::Point2f> corners;
	if (!projector_view_button->isChecked() && level < app->cornersCamera.size())
	{
		corners = app->cornersCamera.at(level);
	}
	else if (projector_view_button->isChecked() && level < app->cornersProjector.size())
	{
		corners = app->cornersProjector.at(level);

		// Scale projector corner coordinates to match the displayed image resolution.
		const float scaleX = image1.cols * 1.f / app->GetProjectorWidth();
		const float scaleY = image1.rows * 1.f / app->GetProjectorHeight();
		for (auto& corner : corners)
		{
			corner.x *= scaleX;
			corner.y *= scaleY;
		}
	}

	// Overlay detected corners as red circles on each available image.
	if (!corners.empty())
	{
		const auto drawCorners = [&corners](cv::Mat& img)
			{
				if (!img.rows) { return; }
				img = img.clone();
				for (const auto& corner : corners)
				{
					cv::circle(img, corner, 6, CV_RGB(255, 0, 0), 2);
				}
			};

		drawCorners(image1);
		drawCorners(image2);
	}

	// Update the preview labels with the loaded images.
	image1_label->setPixmap(QPixmap::fromImage(IoUtil::ToQImage(image1)));
	image2_label->setPixmap(QPixmap::fromImage(IoUtil::ToQImage(image2)));
}

void MainWindow::show_message(const QString& message) const
{
	if (!message.isEmpty())
	{
		statusBar()->showMessage(message);
	}
	else
	{
		statusBar()->clearMessage();
	}
	QApplication::processEvents();
}

void MainWindow::on_corner_count_x_spin_valueChanged(int i)
{
	APP->config.setValue(Settings::Chessboard::Columns, i);
}

void MainWindow::on_corner_count_y_spin_valueChanged(int i)
{
	APP->config.setValue(Settings::Chessboard::Rows, i);
}

void MainWindow::on_corners_width_line_editingFinished() const
{
	APP->config.setValue(Settings::Chessboard::Width, corners_width_line->text().toDouble());
}

void MainWindow::on_corners_height_line_editingFinished() const
{
	APP->config.setValue(Settings::Chessboard::Height, corners_height_line->text().toDouble());
}

void MainWindow::on_threshold_button_clicked(bool checked)
{
	int row = GetCurrentSet();
	if (row < 0)
	{
		//nothing selected
		return;
	}

	APP->GetProjectorView(row, true);
	UpdateCurrentImage();
}

void MainWindow::on_threshold_spin_valueChanged(int i)
{
	APP->config.setValue(Settings::Decode::Threshold, i);
}

void MainWindow::on_b_line_editingFinished() const
{
	APP->config.setValue(Settings::Decode::B, b_line->text().toDouble());
}

void MainWindow::on_m_spin_valueChanged(int i)
{
	APP->config.setValue(Settings::Decode::M, i);
}

void MainWindow::on_joint_refinement_check_stateChanged(int state)
{
	APP->config.setValue(Settings::Calibration::Joint_Refinement, (state == Qt::Checked));
}

void MainWindow::on_subpixel_corners_check_stateChanged(int state)
{
	APP->config.setValue(Settings::Calibration::Subpixel_Corners, (state == Qt::Checked));
}

void MainWindow::on_plane_triangulation_check_stateChanged(int state)
{
	APP->config.setValue(Settings::Reconstruction::Plane_Triangulation, (state == Qt::Checked));
}

void MainWindow::on_homography_window_spin_valueChanged(int i)
{
	APP->config.setValue(Settings::Calibration::H_Win, i);
}

void MainWindow::on_max_dist_line_editingFinished() const
{
	APP->config.setValue(Settings::Reconstruction::Max_Dist, max_dist_line->text().toDouble());
}

void MainWindow::on_normals_check_stateChanged(int state)
{
	APP->config.setValue(Settings::Reconstruction::Save_Normals, (state == Qt::Checked));
}

void MainWindow::on_colors_check_stateChanged(int state)
{
	APP->config.setValue(Settings::Reconstruction::Save_Colors, (state == Qt::Checked));
}

void MainWindow::on_subpixel_check_stateChanged(int state)
{
	APP->config.setValue(Settings::Reconstruction::Subpixel, (state == Qt::Checked));
}

void MainWindow::on_compensation_check_stateChanged(int state)
{
	if (state == Qt::Checked && APP->config.value(Settings::Reconstruction::Compensation_File).toString().isEmpty())
	{
		on_compensation_button_clicked();
		if (APP->config.value(Settings::Reconstruction::Compensation_File).toString().isEmpty())
		{
			// No model chosen: leave compensation off.
			compensation_check->blockSignals(true);
			compensation_check->setChecked(false);
			compensation_check->blockSignals(false);
			return;
		}
	}
	APP->config.setValue(Settings::Reconstruction::Compensate, (state == Qt::Checked));
}

/// Chooses the model and shows its summary, so the user sees its validation and calibration before
/// enabling it.
void MainWindow::on_compensation_button_clicked(bool)
{
	const QString current = APP->config.value(Settings::Reconstruction::Compensation_File).toString();
	const QString path = QFileDialog::getOpenFileName(this, "Compensation model",
		current.isEmpty() ? APP->GetRootDir() : current, "Compensation model (*.json)");
	if (path.isEmpty())
	{
		return;
	}
	Pointcloud::CompensationModel model;
	QString error;
	if (!Pointcloud::LoadCompensationModel(path, model, error))
	{
		QMessageBox::warning(this, "Compensation", QString("The compensation model cannot be used: %1").arg(error));
		return;
	}
	APP->config.setValue(Settings::Reconstruction::Compensation_File, path);
	UpdateCompensationTooltip();
	QMessageBox::information(this, "Compensation", QString("%1\n\n%2")
		.arg(QDir::toNativeSeparators(path))
		.arg(Pointcloud::DescribeCompensationModel(model, APP->config.value(Settings::Calibration::File).toString())));
}

void MainWindow::UpdateCompensationTooltip()
{
	const QString path = APP->config.value(Settings::Reconstruction::Compensation_File).toString();
	compensation_check->setToolTip(path.isEmpty()
		? QString("Correct every reconstruction with a compensation model (choose one with Model...)")
		: QString("Correct every reconstruction with %1").arg(QDir::toNativeSeparators(path)));
}

bool MainWindow::ApplyCompensation(Scan3d::Pointcloud& pointcloud)
{
	if (!compensation_check->isChecked() || !pointcloud.points.data)
	{
		return true;
	}
	const QString path = APP->config.value(Settings::Reconstruction::Compensation_File).toString();
	Pointcloud::CompensationModel model;
	QString error;
	if (!Pointcloud::LoadCompensationModel(path, model, error))
	{
		QMessageBox::warning(this, "Compensation",
			QString("The reconstruction was not compensated: %1").arg(error));
		return true;
	}
	const QString calibration = APP->config.value(Settings::Calibration::File).toString();
	if (Pointcloud::CompensationCalibrationDiffers(model, calibration))
	{
		const auto answer = QMessageBox::question(this, "Compensation",
			QString("The compensation model was trained with a different calibration than %1, so its correction "
					"may not fit this reconstruction.\n\nApply it anyway?").arg(QDir::toNativeSeparators(calibration)),
			QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
		if (answer == QMessageBox::Cancel)
		{
			return false;
		}
		if (answer == QMessageBox::No)
		{
			show_message("Reconstruction not compensated (different calibration).");
			return true;
		}
	}

	Pointcloud::CompensationStats stats;
	{
		const BusyCursorGuard cursorGuard;
		Pointcloud::Compensate(pointcloud.points, model, &stats);
	}
	const QString message = QString("Compensated %1 points with %2 (%3 outside the trained volume).")
		.arg(stats.points).arg(QFileInfo(path).fileName()).arg(stats.outside);
	show_message(message);
	qInfo().noquote() << message;
	return true;
}

void MainWindow::on_quit_action_triggered(bool checked)
{
	close();
	APP->quit();
}

void MainWindow::on_save_vertical_image_action_triggered(bool checked)
{
	if (image1_label->pixmap()->isNull())
	{
		QMessageBox::critical(this, "Error", "Vertical image is empy.");
		return;
	}
	QString filename = QFileDialog::getSaveFileName(this, "Save vertical image", "saved_image_vertical.png",
		"Images (*.png)");
	if (!filename.isEmpty())
	{
		image1_label->pixmap()->save(filename);
		show_message(QString("Vertical Image saved: %1").arg(filename));
	}
}

void MainWindow::on_save_horizontal_image_action_triggered(bool checked)
{
	if (image2_label->pixmap()->isNull())
	{
		QMessageBox::critical(this, "Error", "Horizontal image is empy.");
		return;
	}
	QString filename = QFileDialog::getSaveFileName(this, "Save horizontal image", "saved_image_horizontal.png",
		"Images (*.png)");
	if (!filename.isEmpty())
	{
		image2_label->pixmap()->save(filename);
		show_message(QString("Horizontal Image saved: %1").arg(filename));
	}
}

void MainWindow::_on_root_dir_changed(const QString& dirname)
{
	//update user interface
	image1_label->Clear();
	image2_label->Clear();

	ResetView();
	UpdateWorkflowState();

	setWindowTitle(QString("%1 - %2").arg(Settings::App::Window_Name, dirname));

	QModelIndex index = APP->model.index(0, 0);
	image_tree->selectionModel()->clearSelection();
	if (APP->model.rowCount() > 0)
	{
		image_tree->blockSignals(true);
		image_tree->selectionModel()->select(index, QItemSelectionModel::SelectCurrent);
		_on_image_tree_currentChanged(index, index);
		image_tree->blockSignals(false);
		show_message(QString("%1 set read").arg(APP->model.rowCount()));
	}
}

void MainWindow::on_about_action_triggered(bool checked)
{
	AboutDialog dialog(this, Qt::WindowCloseButtonHint);
	dialog.exec();
}

void MainWindow::UpdateCurrentImage(QModelIndex current)
{
	QModelIndex index = current;
	if (!index.isValid())
	{
		index = image_tree->selectionModel()->currentIndex();
	}
	if (!index.isValid())
	{
		index = APP->model.index(0, 0);
	}
	_on_image_tree_currentChanged(index, index);
}

void MainWindow::_on_horizontal_layout_action_triggered(bool checked) const
{
	QLayout* oldLayout = current_image_group->layout();
	if (oldLayout)
	{
		delete oldLayout;
	}
	auto newLayout = new QHBoxLayout(current_image_group);
	newLayout->addWidget(image1_label);
	newLayout->addWidget(image2_label);
	newLayout->addWidget(pointcloud_preview);
	current_image_group->setLayout(newLayout);
}

void MainWindow::_on_vertical_layout_action_triggered(bool checked) const
{
	QLayout* oldLayout = current_image_group->layout();
	if (oldLayout)
	{
		delete oldLayout;
	}
	auto newLayout = new QVBoxLayout(current_image_group);
	newLayout->addWidget(image1_label);
	newLayout->addWidget(image2_label);
	newLayout->addWidget(pointcloud_preview);
	current_image_group->setLayout(newLayout);
}

void MainWindow::on_reconstruct_dump_action_triggered(bool checked)
{
	QString rootDir = APP->GetRootDir();
	QString filename = QFileDialog::getOpenFileName(this, "Open decoded files", rootDir, "Decoded dump (*.sl)");
	if (filename.isEmpty())
	{
		return;
	}

	int type = 0;
	cv::Mat2f patternImage;
	cv::Mat2b minMaxImage;
	cv::Mat3b colorImage;
	APP->LoadDump(qPrintable(filename), type, patternImage, minMaxImage, colorImage);

	show_message("Reconstruction...");

	Scan3d::Pointcloud& pointcloud = APP->pointcloud;
	APP->ReconstructModelDump(patternImage, minMaxImage, colorImage, pointcloud, this);

	if (!pointcloud.points.data)
	{
		//no points: reconstruction canceled or failed
		show_message("Reconstruction failed");
		return;
	}
	if (!ApplyCompensation(pointcloud))
	{
		return;
	}

	ExportPointcloud(pointcloud, rootDir + "/pointcloud");
	ResetView();
}

int MainWindow::GetCurrentSet(void) const
{
	QModelIndex index = image_tree->selectionModel()->currentIndex();
	if (!index.isValid())
	{
		index = APP->model.index(0, 0);
	}
	while (index.parent().parent().isValid())
	{
		index = index.parent();
	}
	if (!index.isValid())
	{
		return -1;
	}
	return index.row();
}

void MainWindow::on_select_all_button_clicked(bool checked)
{
	APP->SelectAll();
}

void MainWindow::on_select_none_button_clicked(bool checked)
{
	APP->SelectNone();
}

/* ACTION BUTTONS ========================================================================== */

/// <summary>
/// Opens the capture dialog to configure and execute a new image acquisition session.
/// </summary>
/// <param name="clicked">Indicates whether the button is in a clicked state (not used).</param>
/// <remarks>
/// The dialog is opened in modal mode, blocking interaction with the main window until the capture session is completed or dismissed by the user.
/// </remarks>
void MainWindow::on_capture_action_button_clicked(bool clicked)
{
#ifdef USE_SPINNAKER
	// Create and open the capture dialog in modal mode.
	CaptureDialog dialog(this);
	dialog.setModal(true);
	dialog.exec();
#else
	QMessageBox::information(this, tr("Capture"),
		tr("This build does not include camera support (SMCP_WITH_SPINNAKER=OFF)."));
#endif
}

void MainWindow::RunProcessing(const QString& title, const QString& busyMessage, const std::function<void()>& operation, bool keepOpen)
{
	show_message(busyMessage);

	// Reset previous processing state and show the (modal) dialog while the operation runs.
	APP->ProcessingReset();
	APP->processingDialog.setWindowTitle(title);
	APP->processingDialog.show();
	QApplication::processEvents();

	operation();
	UpdateCurrentImage();

	// A clean run closes by itself; errors, warnings and results wait for the user to read them.
	APP->processingDialog.Finish();
	if (keepOpen || APP->processingDialog.NeedsAttention())
	{
		APP->processingDialog.exec();
	}
	APP->processingDialog.hide();
	QApplication::processEvents();

	ResetView();
	UpdateWorkflowState();
}

/// <summary>
/// Finds the chessboard in every selected set and shows the corners over the current image.
/// </summary>
void MainWindow::on_extract_corners_action_button_clicked(bool checked)
{
	RunProcessing("Corner detection", "Searching chessboard corners...", []() { APP->ExtractChessboardCorners(); }, false);

	int found = 0;
	for (const auto& corners : APP->cornersCamera)
	{
		found += corners.empty() ? 0 : 1;
	}
	show_message(QString("Chessboard found in %1 of %2 selected sets").arg(found).arg(APP->SelectedSetCount()));
}

/// <summary>
/// Decodes every selected set; sets already decoded with the current parameters are reused.
/// </summary>
void MainWindow::on_decode_action_button_clicked(bool checked)
{
	RunProcessing("Decode", "Decoding...", []() { APP->DecodeAll(); }, false);

	const auto decoded = std::count_if(APP->patternList.begin(), APP->patternList.end(), [](const cv::Mat& m) { return !m.empty(); });
	show_message(QString("%1 sets decoded").arg(decoded));
}

/// <summary>
/// Runs the calibration pipeline. Its dialog stays open so the results can be read.
/// </summary>
void MainWindow::on_calibration_action_button_clicked(bool checked)
{
	RunProcessing("Calibration", "Running calibration...", []() { APP->Calibrate(); }, true);

	show_message(APP->calib.IsValid()
		? QString("Calibration finished with %1 sets (stereo error %2 px)").arg(APP->CalibratedSetCount()).arg(APP->calib.stereoError, 0, 'f', 3)
		: QString("Calibration not completed"));
}

/// <summary>
/// Slot triggered when the reconstruction action button is clicked. Runs the 3D reconstruction pipeline for the currently selected set and saves the
/// resulting pointcloud to disk in the format chosen by the user.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state (not used).</param>
/// <remarks>
/// Requires a set to be selected; aborts silently if none is active. After reconstruction, optionally computes normals based on the current
/// configuration. Prompts the user with a save dialog supporting XYZ (PolyWorks ASCII) and PLY (ASCII or Binary) formats. On success, resets the view
/// to reflect the updated processing state.
/// </remarks>
void MainWindow::on_reconstruction_action_button_clicked(bool checked)
{
	int row = GetCurrentSet();
	if (row < 0)
	{
		// Abort if no set is selected.
		return;
	}

	show_message("Reconstruction...");

	// Run reconstruction and validate that points were produced.
	Scan3d::Pointcloud& pointcloud = APP->pointcloud;
	APP->ReconstructModel(row, pointcloud, this);
	UpdateWorkflowState();
	if (!pointcloud.points.data)
	{
		// Reconstruction was canceled or failed; no pointcloud available.
		show_message("Reconstruction failed");
		return;
	}
	if (!ApplyCompensation(pointcloud))
	{
		return;
	}

	// Default filename from the scan set name.
	const QString setName = APP->model.data(APP->model.index(row, 0), Qt::DisplayRole).toString();
	ExportPointcloud(pointcloud, QDir(APP->GetRootDir()).filePath(setName));

	// The cloud can be inspected in the 3D view whether or not it was saved.
	ResetView();
}

void MainWindow::ExportPointcloud(Scan3d::Pointcloud& pointcloud, const QString& defaultPath)
{
	const bool normals = APP->config.value(Settings::Reconstruction::Save_Normals, Settings::Reconstruction::Save_Normals_Default_Value).toBool();
	const bool colors = APP->config.value(Settings::Reconstruction::Save_Colors, Settings::Reconstruction::Save_Colors_Default_Value).toBool();

	// The format, including ASCII or binary PLY, is chosen in the save dialog.
	const QString filter = "PolyWorks ASCII (*.xyz);;PLY ASCII (*.ply);;PLY Binary (*.ply)";
	QString selectedFilter;
	const QString filename = QFileDialog::getSaveFileName(
		this, "Save pointcloud", defaultPath + ".xyz", filter, &selectedFilter);

	if (filename.isEmpty())
	{
		show_message("Reconstruction finished (not saved)");
		return;
	}

	show_message(QStringLiteral("Saving to %1...").arg(filename));
	{
		const BusyCursorGuard cursorGuard;
		QApplication::processEvents();

		bool success = false;

		if (selectedFilter.startsWith("PolyWorks"))
		{
			// Direct ASCII export.
			const auto xyzFormat = colors
				? IOExport::XyzFormat::XyzRgb
				: IOExport::XyzFormat::Xyz;
			success = IOExport::WriteXyz(filename.toStdString(), pointcloud, xyzFormat);
		}
		else
		{
			// Normals only exist in PLY files, so they are computed only for them.
			if (normals)
			{
				APP->ComputeNormals(pointcloud);
			}
			const bool isBinary = selectedFilter.contains("Binary");
			unsigned plyFlags = IoUtil::PlyPoints
				| (colors ? IoUtil::PlyColors : 0)
				| (normals ? IoUtil::PlyNormals : 0)
				| (isBinary ? IoUtil::PlyBinary : 0);
			success = IoUtil::WritePly(filename.toStdString(), pointcloud, plyFlags);
		}

		if (!success)
		{
			show_message("Failed to save pointcloud.");
			QMessageBox::warning(this, "Save pointcloud", QString("The point cloud could not be saved to %1").arg(QDir::toNativeSeparators(filename)));
			return;
		}
	}

	const QString message = QStringLiteral("Pointcloud saved: %1").arg(filename);
	show_message(message);
	qInfo().noquote() << message;
}

void MainWindow::UpdateWorkflowState()
{
	Application* const app = APP;
	if (!app || !_calibrationStatus)
	{
		return;
	}

	const int sets = app->model.rowCount();
	const int selected = app->SelectedSetCount();
	const bool calibrated = app->calib.IsValid();

	// Enabled only when the action can run; the tooltip says what is missing otherwise.
	const auto set = [](QToolButton* button, bool enabled, const QString& missing)
	{
		button->setEnabled(enabled);
		const QString base = button->property("baseToolTip").toString();
		button->setToolTip(enabled ? base : QString("%1\n\nNot available: %2").arg(base, missing));
	};
	const QString noSets = sets ? QString("select at least one set in the workspace list.")
								: QString("the workspace has no capture sets. Capture one or choose another directory.");
	set(extract_corners_action_button, selected > 0, noSets);
	set(decode_action_button, selected > 0, noSets);
	set(calibration_action_button, selected >= 3, QString("calibration needs at least 3 selected sets (%1 selected).").arg(selected));
	set(reconstruction_action_button, sets > 0 && calibrated,
		sets ? QString("load a calibration (Calibration > Load...) or run Calibrate first.") : noSets);

	if (calibrated)
	{
		const QString file = app->calib.filename.isEmpty() ? QString("not saved") : QFileInfo(app->calib.filename).fileName();
		_calibrationStatus->setText(QString("Calibration: %1  |  stereo error %2 px").arg(file).arg(app->calib.stereoError, 0, 'f', 3));
		_calibrationStatus->setToolTip(app->calib.filename.isEmpty() ? QString() : QDir::toNativeSeparators(app->calib.filename));
	}
	else
	{
		_calibrationStatus->setText("No calibration loaded");
		_calibrationStatus->setToolTip("Load one with Calibration > Load... or compute it with Calibrate");
	}
}

/// <summary>
/// Opens the point cloud editor (modal) over the .xyz files of the working directory:
/// outlier removal, RANSAC plane/sphere fitting, 3D comparison and export.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state (not used).</param>
void MainWindow::on_pointcloud_action_button_clicked(bool checked)
{
	PointcloudEditorDialog::Execute(APP->GetRootDir(), this);
}

/* PREVIEW BUTTONS ========================================================================= */

/// <summary>
/// Updates the visibility of the main view components based on the requested view type.
/// </summary>
/// <param name="view">The view type to activate (Image, Pattern, Projector, or View3D).</param>
void MainWindow::SetView(ViewType view)
{
	// Resolve visibility flags for each component based on the requested view type.
	bool showImage1 = false;
	bool showImage2 = false;
	bool showPointcloudPreview = false;

	switch (view)
	{
	case ViewType::Image:
		showImage1 = true;
		break;

	case ViewType::Pattern:
		showImage1 = true;
		showImage2 = true;
		break;

	case ViewType::Projector:
		showImage1 = true;
		break;

	case ViewType::View3D:
		showPointcloudPreview = true;
		break;
	}

	// Apply visibility to each component.
	image1_label->setVisible(showImage1);
	image2_label->setVisible(showImage2);
	pointcloud_preview->setVisible(showPointcloudPreview);
}

/// <summary>
/// Resets the active view to Image and updates the enabled state of all view buttons based on the current processing state.
/// </summary>
/// <remarks>
/// Enables pattern_view_button and projector_view_button only when at least one decoded pattern is available in patternList. Enables
/// view_3d_view_button only when a reconstructed pointcloud exists. Called after every pipeline step and during initialization to keep the UI
/// consistent with the application state.
/// </remarks>
void MainWindow::ResetView()
{
	// Determine which views are available based on current processing state.
	const bool decoded = std::any_of(APP->patternList.begin(), APP->patternList.end(),
		[](const cv::Mat& m) { return !m.empty(); });
	const bool reconstructed = !APP->pointcloud.points.empty();

	// Enable/disable view buttons based on available data.
	pattern_view_button->setEnabled(decoded);
	projector_view_button->setEnabled(decoded);
	view_3d_view_button->setEnabled(reconstructed);

	// Reset active view to image.
	image_view_button->click();
}

/// <summary>
/// Switches the preview to display the original captured image.
/// </summary>
/// <param name="checked">True when the button transitions to checked state.</param>
void MainWindow::on_image_view_button_clicked(bool checked)
{
	if (checked)
	{
		SetView(ViewType::Image);
		UpdateCurrentImage();
	}
}

/// <summary>
/// Switches the preview to display the decoded Gray code pattern images (horizontal and vertical).
/// </summary>
/// <param name="checked">True when the button transitions to checked state.</param>
void MainWindow::on_pattern_view_button_clicked(bool checked)
{
	if (checked)
	{
		SetView(ViewType::Pattern);
		UpdateCurrentImage();
	}
}

/// <summary>
/// Switches the preview to display the projector view reconstructed from the decoded patterns.
/// </summary>
/// <param name="checked">True when the button transitions to checked state.</param>
void MainWindow::on_projector_view_button_clicked(bool checked)
{
	if (checked)
	{
		SetView(ViewType::Projector);
		UpdateCurrentImage();
	}
}

/// <summary>
/// Switches the preview to display the reconstructed 3D pointcloud in the OpenGL widget.
/// </summary>
/// <param name="checked">True when the button transitions to checked state.</param>
void MainWindow::on_view_3d_view_button_clicked(bool checked)
{
	if (checked)
	{
		SetView(ViewType::View3D);
		UpdateCurrentImage();

		pointcloud_preview->loadPointcloud();
	}
}
} // namespace smcp
