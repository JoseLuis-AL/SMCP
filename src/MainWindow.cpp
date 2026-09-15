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

#include "MainWindow.hpp"
#include "MainWindow.hpp"

#include <QMessageBox>
#include <QFileDialog>
#include <QButtonGroup>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/calib3d/calib3d.hpp>

#include <QPainter>
#include <QIntValidator>
#include <QDoubleValidator>
#include <QDesktopWidget>

#include <algorithm>
#include <iostream>

#include "structured_light.hpp"

#include "Application.hpp"
#include "io_util.hpp"

#include "AboutDialog.hpp"
#include "CaptureDialog.hpp"
#include "CalibrationDialog.hpp"
#include "CaptureQDialog.h"
#include "IOExport.h"
#include "BusyCursorGuard.h"

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

	setWindowTitle(WINDOW_TITLE);

	threshold_spin->blockSignals(true);
	threshold_spin->setRange(0, 255);
	threshold_spin->setValue(config.value(THRESHOLD_CONFIG, THRESHOLD_DEFAULT).toUInt());
	threshold_spin->blockSignals(false);

	b_line->blockSignals(true);
	b_line->setValidator(new QDoubleValidator(0.0, 1.0, 6, this));
	b_line->setText(config.value(ROBUST_B_CONFIG, ROBUST_B_DEFAULT).toString());
	b_line->blockSignals(false);

	m_spin->blockSignals(true);
	m_spin->setRange(0, 255);
	m_spin->blockSignals(false);
	m_spin->setValue(config.value(ROBUST_M_CONFIG, ROBUST_M_DEFAULT).toUInt());

	corner_count_x_spin->blockSignals(true);
	corner_count_x_spin->setRange(1, 255);
	corner_count_x_spin->blockSignals(false);
	corner_count_x_spin->setValue(config.value("main/corner_count_x", DEFAULT_CORNER_X).toUInt());

	corner_count_y_spin->blockSignals(true);
	corner_count_y_spin->setRange(1, 255);
	corner_count_y_spin->blockSignals(false);
	corner_count_y_spin->setValue(config.value("main/corner_count_y", DEFAULT_CORNER_Y).toUInt());

	corners_width_line->blockSignals(true);
	corners_width_line->setValidator(new QDoubleValidator(this));
	corners_width_line->setText(config.value("main/corners_width", DEFAULT_CORNER_WIDTH).toString());
	corners_width_line->blockSignals(false);

	corners_height_line->blockSignals(true);
	corners_height_line->setValidator(new QDoubleValidator(this));
	corners_height_line->setText(config.value("main/corners_height", DEFAULT_CORNER_HEIGHT).toString());
	corners_height_line->blockSignals(false);

	homography_window_spin->blockSignals(true);
	homography_window_spin->setRange(0, 1024);
	homography_window_spin->setValue(config.value(HOMOGRAPHY_WINDOW_CONFIG, HOMOGRAPHY_WINDOW_DEFAULT).toUInt());
	homography_window_spin->blockSignals(false);

	//display_original_radio->blockSignals(true);
	//display_original_radio->setChecked(true);
	//display_original_radio->blockSignals(false);

	max_dist_line->blockSignals(true);
	max_dist_line->setValidator(new QDoubleValidator(this));
	max_dist_line->setText(config.value(MAX_DIST_CONFIG, MAX_DIST_DEFAULT).toString());
	max_dist_line->blockSignals(false);

	normals_check->blockSignals(true);
	normals_check->setChecked(config.value(SAVE_NORMALS_CONFIG, SAVE_NORMALS_DEFAULT).toBool());
	normals_check->blockSignals(false);

	colors_check->blockSignals(true);
	colors_check->setChecked(config.value(SAVE_COLORS_CONFIG, SAVE_COLORS_DEFAULT).toBool());
	colors_check->blockSignals(false);

	binary_file_check->blockSignals(true);
	binary_file_check->setChecked(config.value(SAVE_BINARY_CONFIG, SAVE_BINARY_DEFAULT).toBool());
	binary_file_check->blockSignals(false);

	//set contextual menu for layout changing
	auto horizontal_action = new QAction("Horizontal", current_image_group);
	auto vertical_action = new QAction("Vertical", current_image_group);
	auto layout_action_group = new QActionGroup(current_image_group);
	layout_action_group->addAction(horizontal_action);
	layout_action_group->addAction(vertical_action);
	horizontal_action->setCheckable(true);
	vertical_action->setCheckable(true);
	horizontal_action->setChecked(true);
	vertical_action->setChecked(false);
	connect(horizontal_action, SIGNAL(triggered(bool)), this, SLOT(_on_horizontal_layout_action_triggered(bool)));
	connect(vertical_action, SIGNAL(triggered(bool)), this, SLOT(_on_vertical_layout_action_triggered(bool)));
	current_image_group->addActions(layout_action_group->actions());
	current_image_group->setContextMenuPolicy(Qt::ActionsContextMenu);

	// Group view buttons so only one can be checked at a time (exclusive behavior).
	auto view_button_group = new QButtonGroup(this);
	view_button_group->setExclusive(true);
	view_button_group->addButton(image_view_button);
	view_button_group->addButton(pattern_view_button);
	view_button_group->addButton(projector_view_button);
	view_button_group->addButton(view_3d_view_button);

	// Set view.
	reset_view();

	show_message("Ready");
}

MainWindow::~MainWindow()
{}

void MainWindow::on_change_dir_action_triggered(bool checked)
{
	APP->change_root_dir(this);
}

void MainWindow::on_load_calibration_action_triggered(bool checked)
{
	APP->load_calibration(this);
	// TODO: Remove glwidget->update_camera();
}

void MainWindow::on_save_calibration_action_triggered(bool checked)
{
	APP->save_calibration(this);
}

void MainWindow::on_display_calibration_action_triggered(bool checked)
{
	CalibrationDialog dialog(this, Qt::WindowCloseButtonHint);
	dialog.exec();
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
	image1_label->clear();
	image2_label->clear();

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
	const SMCP::BusyCursorGuard cursor_guard;
	QApplication::processEvents();

	// Load image content based on the active view button (mutually exclusive).
	if (image_view_button->isChecked())
	{
		image1 = app->get_image(level, row, ColorImageRole);
	}
	else if (pattern_view_button->isChecked() && app->pattern_list.size() > level)
	{
		app->make_pattern_images(level, image1, image2);
	}
	else if (projector_view_button->isChecked())
	{
		image1 = app->get_projector_view(level);
	}

	// Resolve detected corners to overlay: camera corners for standard views,
	// projector corners (scaled to image resolution) for the projector view.
	std::vector<cv::Point2f> corners;
	if (!projector_view_button->isChecked() && level < app->corners_camera.size())
	{
		corners = app->corners_camera.at(level);
	}
	else if (projector_view_button->isChecked() && level < app->corners_projector.size())
	{
		corners = app->corners_projector.at(level);

		// Scale projector corner coordinates to match the displayed image resolution.
		const float scale_x = image1.cols * 1.f / app->get_projector_width();
		const float scale_y = image1.rows * 1.f / app->get_projector_height();
		for (auto& corner : corners)
		{
			corner.x *= scale_x;
			corner.y *= scale_y;
		}
	}

	// Overlay detected corners as red circles on each available image.
	if (!corners.empty())
	{
		const auto draw_corners = [&corners](cv::Mat& img)
			{
				if (!img.rows) { return; }
				img = img.clone();
				for (const auto& corner : corners)
				{
					cv::circle(img, corner, 6, CV_RGB(255, 0, 0), 2);
				}
			};

		draw_corners(image1);
		draw_corners(image2);
	}

	// Update the preview labels with the loaded images.
	image1_label->setPixmap(QPixmap::fromImage(io_util::qImage(image1)));
	image2_label->setPixmap(QPixmap::fromImage(io_util::qImage(image2)));
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
	APP->config.setValue("main/corner_count_x", i);
}

void MainWindow::on_corner_count_y_spin_valueChanged(int i)
{
	APP->config.setValue("main/corner_count_y", i);
}

void MainWindow::on_corners_width_line_editingFinished() const
{
	APP->config.setValue("main/corners_width", corners_width_line->text().toDouble());
}

void MainWindow::on_corners_height_line_editingFinished() const
{
	APP->config.setValue("main/corners_height", corners_height_line->text().toDouble());
}

void MainWindow::on_threshold_button_clicked(bool checked)
{
	int row = get_current_set();
	if (row < 0)
	{
		//nothing selected
		return;
	}

	APP->get_projector_view(row, true);
	update_current_image();
}

void MainWindow::on_threshold_spin_valueChanged(int i)
{
	APP->config.setValue(THRESHOLD_CONFIG, i);
}

void MainWindow::on_b_line_editingFinished() const
{
	APP->config.setValue(ROBUST_B_CONFIG, b_line->text().toDouble());
}

void MainWindow::on_m_spin_valueChanged(int i)
{
	APP->config.setValue(ROBUST_M_CONFIG, i);
}

void MainWindow::on_homography_window_spin_valueChanged(int i)
{
	APP->config.setValue(HOMOGRAPHY_WINDOW_CONFIG, i);
}

void MainWindow::on_max_dist_line_editingFinished() const
{
	APP->config.setValue(MAX_DIST_CONFIG, max_dist_line->text().toDouble());
}

void MainWindow::on_normals_check_stateChanged(int state)
{
	APP->config.setValue(SAVE_NORMALS_CONFIG, (state == Qt::Checked));
}

void MainWindow::on_colors_check_stateChanged(int state)
{
	APP->config.setValue(SAVE_COLORS_CONFIG, (state == Qt::Checked));
}

void MainWindow::on_binary_file_check_stateChanged(int state)
{
	APP->config.setValue(SAVE_BINARY_CONFIG, (state == Qt::Checked));
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
	image1_label->clear();
	image2_label->clear();

	reset_view();

	setWindowTitle(QString("%1 - %2").arg(WINDOW_TITLE, dirname));

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

void MainWindow::update_current_image(QModelIndex current)
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
	QLayout* old_layout = current_image_group->layout();
	if (old_layout)
	{
		delete old_layout;
	}
	auto new_layout = new QHBoxLayout(current_image_group);
	new_layout->addWidget(image1_label);
	new_layout->addWidget(image2_label);
	new_layout->addWidget(pointcloud_preview);
	current_image_group->setLayout(new_layout);
}

void MainWindow::_on_vertical_layout_action_triggered(bool checked) const
{
	QLayout* old_layout = current_image_group->layout();
	if (old_layout)
	{
		delete old_layout;
	}
	auto new_layout = new QVBoxLayout(current_image_group);
	new_layout->addWidget(image1_label);
	new_layout->addWidget(image2_label);
	new_layout->addWidget(pointcloud_preview);
	current_image_group->setLayout(new_layout);
}

void MainWindow::on_reconstruct_dump_action_triggered(bool checked)
{
	QString root_dir = APP->get_root_dir();
	QString filename = QFileDialog::getOpenFileName(this, "Open decoded files", root_dir, "Decoded dump (*.sl)");
	if (filename.isEmpty())
	{
		return;
	}

	int type = 0;
	cv::Mat2f pattern_image;
	cv::Mat2b min_max_image;
	cv::Mat3b color_image;
	APP->load_dump(qPrintable(filename), type, pattern_image, min_max_image, color_image);

	show_message("Reconstruction...");

	//parameters
	bool normals = APP->config.value(SAVE_NORMALS_CONFIG, SAVE_NORMALS_DEFAULT).toBool();
	bool colors = APP->config.value(SAVE_COLORS_CONFIG, SAVE_COLORS_DEFAULT).toBool();
	bool binary = APP->config.value(SAVE_BINARY_CONFIG, SAVE_BINARY_DEFAULT).toBool();

	scan3d::Pointcloud& pointcloud = APP->pointcloud;
	APP->reconstruct_model_dump(pattern_image, min_max_image, color_image, pointcloud, this);

	if (!pointcloud.points.data)
	{
		//no points: reconstruction canceled or failed
		show_message("Reconstruction failed");
		return;
	}

	//compute normals
	if (normals)
	{
		//busy cursor
		show_message("Computing normals...");
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
		QApplication::processEvents();

		APP->compute_normals(pointcloud);

		//restore regular cursor
		QApplication::restoreOverrideCursor();
		QApplication::processEvents();
	}

	//save the points
	QString name = root_dir + "/pointcloud";
	filename = QFileDialog::getSaveFileName(this, "Save pointcloud", name + ".ply", "Pointclouds (*.ply)");
	if (!filename.isEmpty())
	{
		//busy cursor
		show_message(QString("Saving to %1...").arg(filename));
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
		QApplication::processEvents();

		unsigned ply_flags = io_util::PlyPoints
			| (colors ? io_util::PlyColors : 0)
			| (normals ? io_util::PlyNormals : 0)
			| (binary ? io_util::PlyBinary : 0);

		io_util::write_ply(filename.toStdString(), pointcloud, ply_flags);

		//restore regular cursor
		QApplication::restoreOverrideCursor();
		QApplication::processEvents();
		show_message(QString("Pointcloud saved: %1").arg(filename));
		std::cout << QString("Pointcloud saved: %1").arg(filename).toStdString() << std::endl;
	}
}

int MainWindow::get_current_set(void) const
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
	APP->select_all();
}

void MainWindow::on_select_none_button_clicked(bool checked)
{
	APP->select_none();
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
	// Create and open the capture dialog in modal mode.
	SMCP::CaptureQDialog dialog(this);
	dialog.setModal(true);
	dialog.exec();
}

/// <summary>
/// Runs the chessboard corner detection pipeline and updates the current image view with the results.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state (not used).</param>
/// <remarks>
/// Resets any previous processing state before running corner detection. Once the extraction completes, the current image is refreshed to reflect the
/// detected corners. Progress and output are shown through the processing dialog.
/// </remarks>
void MainWindow::on_extract_corners_action_button_clicked(bool checked)
{
	show_message("Searching chessboard corners...");

	// Reset previous processing state and prepare the dialog for the corner detection run.
	APP->processing_reset();
	APP->processingDialog.setWindowTitle("Corner detection");
	APP->processingDialog.show();
	QApplication::processEvents();

	// Execute chessboard corner extraction and refresh the image view with the results.
	APP->extract_chessboard_corners_v2();
	update_current_image();

	// Mark processing as finished, display the dialog with results and wait for dismissal.
	APP->processingDialog.finish();
	APP->processingDialog.exec();
	APP->processingDialog.hide();
	QApplication::processEvents();

	reset_view();
	show_message("Ready");
}

/// <summary>
/// Runs the full decode pipeline over all captures and updates the current image view with the results.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state (not used).</param>
/// <remarks>
/// Resets any previous processing state before decoding. Once decoding completes, the current image is refreshed to reflect the updated results.
/// Progress and output are shown through the processing dialog.
/// </remarks>
void MainWindow::on_decode_action_button_clicked(bool checked)
{
	show_message("Decoding...");

	// Reset previous processing state and prepare the dialog for the decode run.
	APP->processing_reset();
	APP->processingDialog.setWindowTitle("Decode");
	APP->processingDialog.show();
	QApplication::processEvents();

	// Execute decoding over all captures and refresh the image view.
	APP->decode_all();
	update_current_image();

	// Mark processing as finished, display the dialog with results and wait for dismissal.
	APP->processingDialog.finish();
	APP->processingDialog.exec();
	APP->processingDialog.hide();
	QApplication::processEvents();

	reset_view();
	show_message("Ready");
}

/// <summary>
/// Executes the full calibration pipeline and reflects the results in the current view.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state (not used).</param>
/// <remarks>
/// Resets any previous processing state before running calibration. Once the calibration completes, the current image and camera are refreshed to
/// reflect the updated results. Progress and output are shown through the processing dialog.
/// </remarks>
void MainWindow::on_calibration_action_button_clicked(bool checked)
{
	show_message("Running calibration...");

	// Reset previous processing state and prepare the dialog for the calibration run.
	APP->processing_reset();
	APP->processingDialog.setWindowTitle("Calibration");
	APP->processingDialog.show();
	QApplication::processEvents();

	// Execute calibration and refresh the image and camera to reflect updated results.
	APP->calibrate();
	update_current_image();
	// TODO: Remove glwidget->update_camera();

	// Mark processing as finished, display the dialog with results and wait for dismissal.
	APP->processingDialog.finish();
	APP->processingDialog.exec();
	APP->processingDialog.hide();
	QApplication::processEvents();

	reset_view();
	show_message("Ready");
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
	int row = get_current_set();
	if (row < 0)
	{
		// Abort if no set is selected.
		return;
	}

	show_message("Reconstruction...");

	// Read export configuration flags.
	bool normals = APP->config.value(SAVE_NORMALS_CONFIG, SAVE_NORMALS_DEFAULT).toBool();
	bool colors = APP->config.value(SAVE_COLORS_CONFIG, SAVE_COLORS_DEFAULT).toBool();
	bool binary = APP->config.value(SAVE_BINARY_CONFIG, SAVE_BINARY_DEFAULT).toBool();

	// Run reconstruction and validate that points were produced.
	scan3d::Pointcloud& pointcloud = APP->pointcloud;
	APP->reconstruct_model(row, pointcloud, this);
	if (!pointcloud.points.data)
	{
		// Reconstruction was canceled or failed; no pointcloud available.
		show_message("Reconstruction failed");
		return;
	}

	// Optionally compute surface normals from the reconstructed pointcloud.
	if (normals)
	{
		show_message("Computing normals...");
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
		QApplication::processEvents();

		APP->compute_normals(pointcloud);

		QApplication::restoreOverrideCursor();
		QApplication::processEvents();
	}

	// Build default filename from the scan set name.
	const QString set_name = APP->model.data(APP->model.index(row, 0), Qt::DisplayRole).toString();
	const QString default_path = QDir(APP->get_root_dir()).filePath(set_name);

	// Offer both PLY and XYZ formats in the save dialog.
	const QString filter = "PolyWorks ASCII (*.xyz);;PLY ASCII (*.ply);;PLY Binary (*.ply)";
	QString selected_filter;
	const QString filename = QFileDialog::getSaveFileName(
		this, "Save pointcloud", default_path + ".xyz", filter, &selected_filter);

	if (filename.isEmpty())
	{
		return;
	}

	// Save based on selected format.
	show_message(QStringLiteral("Saving to %1...").arg(filename));
	{
		const SMCP::BusyCursorGuard cursor_guard;
		QApplication::processEvents();

		bool success = false;

		if (selected_filter.startsWith("PolyWorks"))
		{
			// Direct ASCII export.
			const auto xyz_format = colors
				? SMCP::IOExport::XyzFormat::XyzRgb
				: SMCP::IOExport::XyzFormat::Xyz;
			success = SMCP::IOExport::write_xyz(filename.toStdString(), pointcloud, xyz_format);
		}
		else
		{
			// Standard PLY export.
			const bool is_binary = selected_filter.contains("Binary");
			unsigned ply_flags = io_util::PlyPoints
				| (colors ? io_util::PlyColors : 0)
				| (normals ? io_util::PlyNormals : 0)
				| (is_binary ? io_util::PlyBinary : 0);
			success = io_util::write_ply(filename.toStdString(), pointcloud, ply_flags);
		}

		if (!success)
		{
			show_message("Failed to save pointcloud.");
			return;
		}
	}

	const QString message = QStringLiteral("Pointcloud saved: %1").arg(filename);
	show_message(message);
	qInfo().noquote() << message;

	reset_view();
}

void MainWindow::on_pointcloud_action_button_clicked(bool checked)
{}

/* PREVIEW BUTTONS ========================================================================= */

/// <summary>
/// Updates the visibility of the main view components based on the requested view type.
/// </summary>
/// <param name="view">The view type to activate (Image, Pattern, Projector, or View3D).</param>
void MainWindow::set_view(VIEW_TYPE view)
{
	// Resolve visibility flags for each component based on the requested view type.
	bool show_image1 = false;
	bool show_image2 = false;
	bool show_pointcloud_preview = false;

	switch (view)
	{
	case VIEW_TYPE::Image:
		show_image1 = true;
		break;

	case VIEW_TYPE::Pattern:
		show_image1 = true;
		show_image2 = true;
		break;

	case VIEW_TYPE::Projector:
		show_image1 = true;
		break;

	case VIEW_TYPE::View3D:
		show_pointcloud_preview = true;
		break;
	}

	// Apply visibility to each component.
	image1_label->setVisible(show_image1);
	image2_label->setVisible(show_image2);
	pointcloud_preview->setVisible(show_pointcloud_preview);
}

/// <summary>
/// Resets the active view to Image and updates the enabled state of all view buttons based on the current processing state.
/// </summary>
/// <remarks>
/// Enables pattern_view_button and projector_view_button only when at least one decoded pattern is available in pattern_list. Enables
/// view_3d_view_button only when a reconstructed pointcloud exists. Called after every pipeline step and during initialization to keep the UI
/// consistent with the application state.
/// </remarks>
void MainWindow::reset_view()
{
	// Determine which views are available based on current processing state.
	const bool decoded = std::any_of(APP->pattern_list.begin(), APP->pattern_list.end(),
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
		set_view(VIEW_TYPE::Image);
		update_current_image();
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
		set_view(VIEW_TYPE::Pattern);
		update_current_image();
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
		set_view(VIEW_TYPE::Projector);
		update_current_image();
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
		set_view(VIEW_TYPE::View3D);
		update_current_image();

		pointcloud_preview->loadPointcloud();
	}
}