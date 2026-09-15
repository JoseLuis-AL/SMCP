#include "CaptureQDialog.h"

#include <QDesktopWidget>
#include <QMessageBox>
#include <QTimer>
#include <QTime>
#include <QDebug>
#include <QScreen>

#include <iostream>
#include <strmif.h>
#include <opencv2/imgproc/imgproc.hpp>

#include "Application.hpp"
#include "CameraUtilities.h"
#include "im_util.hpp"
#include "Literals.h"
#include "Settings.h"

/// <summary>
/// Initializes the capture dialog, setting up the UI, Spinnaker system, and camera preview.
/// </summary>
/// <param name="parent">Parent widget.</param>
/// <param name="flags">Window flags for the dialog.</param>
/// <remarks>
/// Connects application signals, initializes the Spinnaker system, populates screen and camera combo boxes, restores saved settings, and starts the
/// camera preview.
/// </remarks>
SMCP::CaptureQDialog::CaptureQDialog(QWidget* parent, Qt::WindowFlags flags) : QDialog(parent, flags)
{
	// Build the UI from the .ui form.
	setupUi(this);

	// Initialize subsystems.
	init_spinnaker();
	init_signals();
	init_controls();

	// Populate combo boxes with available devices.
	update_camera_combo();
	const int screen_idx = update_screen_combo();

	// Configure the projector for the selected screen.
	projector_widget.set_screen(screen_idx);

	// Start the camera live preview.
	start_camera();
}

/// <summary>
/// Saves settings and releases all camera and projector resources.
/// </summary>
/// <remarks>
/// Persists capture, projector, and camera settings before stopping the projector, stopping the camera, and releasing the Spinnaker system instance.
/// </remarks>
SMCP::CaptureQDialog::~CaptureQDialog()
{
	QSettings& appSettings = APP->getSettings();

	// Capture values.
	appSettings.setValue(Settings::Capture::WaitTime, wait_time_spin->value());
	appSettings.setValue(Settings::Capture::Continuous, continuous_spin->value());

	// Free projector.
	appSettings.setValue(Settings::Projector::Screen, screen_combo->currentIndex());
	appSettings.setValue(Settings::Projector::PatternCount, projector_patterns_spin->value());
	projector_widget.stop();

	// Free camera.
	stop_camera();
	appSettings.setValue(Settings::Camera::BlackLevel, camera_black_level_spin->value());
	appSettings.setValue(Settings::Camera::ExposureTime, camera_exposure_spin->value());
	appSettings.setValue(Settings::Camera::Gain, camera_gain_spin->value());
	appSettings.setValue(Settings::Camera::Gamma, camera_gamma_spin->value());
	appSettings.setValue(Settings::Camera::SerialNumber, camera_serial_number);

	// De-Initialize the spinnaker system.
	try
	{
		camera_ptr = nullptr;
		camera_list.Clear();
		spinnaker_system_ptr->ReleaseInstance();
	}
	catch (Spinnaker::Exception& e)
	{
		std::cout << "[CaptureWidget]: " << e.GetErrorMessage() << "\n";
	}
}

/* INITIALIZATION ========================================================================== */

/// <summary>
/// Initializes the Spinnaker camera system instance.
/// </summary>
void SMCP::CaptureQDialog::init_spinnaker()
{
	spinnaker_system_ptr = Spinnaker::System::GetInstance();
}

/// <summary>
/// Connects all required signals to their corresponding slots.
/// </summary>
void SMCP::CaptureQDialog::init_signals()
{
	connect(APP, &Application::root_dir_changed, this, &CaptureQDialog::_on_root_dir_changed);
}

/// <summary>
/// Configures all UI controls with their initial values from the application configuration. Sets valid ranges before loading values to prevent silent
/// clamping by the spin boxes. Blocks signals during initialization to avoid emitting premature change notifications to subsystems that are not yet
/// ready (e.g. the camera worker thread).
/// </summary>
void SMCP::CaptureQDialog::init_controls() const
{
	// Hide progress indicators until a capture starts.
	current_message_label->setVisible(false);
	progress_label->setVisible(false);
	progress_bar->setVisible(false);

	// Output directory.
	output_dir_line->setText(APP->get_root_dir());

	// Block signals on all spin boxes to prevent premature valueChanged emissions.
	const QSignalBlocker block_patterns(projector_patterns_spin);
	const QSignalBlocker block_wait(wait_time_spin);
	const QSignalBlocker block_exposure(camera_exposure_spin);
	const QSignalBlocker block_continuous(continuous_spin);
	const QSignalBlocker block_black_level(camera_black_level_spin);
	const QSignalBlocker block_gain(camera_gain_spin);
	const QSignalBlocker block_gamma(camera_gamma_spin);

	// Projector pattern count.
	projector_patterns_spin->setValue(
		APP->config.value(Settings::Projector::PatternCount, Settings::Projector::PatternCountDefaultValue).toInt());

	// Capture wait time between projected patterns.
	wait_time_spin->setValue(
		APP->config.value(Settings::Capture::WaitTime, Settings::Capture::WaitTimeDefaultValue).toInt());

	// Camera exposure (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_exposure_spin->setMinimum(0);
	camera_exposure_spin->setMaximum(50000);
	camera_exposure_spin->setValue(
		APP->config.value(Settings::Camera::ExposureTime, Settings::Camera::ExposureTimeDefaultValue).toDouble());

	// Continuous capture interval (set range before value to avoid clamping).
	continuous_spin->setMinimum(10);
	continuous_spin->setMaximum(9999);
	continuous_spin->setValue(
		APP->config.value(Settings::Capture::Continuous, Settings::Capture::ContinuousDefaultValue).toInt());

	// Camera black level (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_black_level_spin->setMinimum(0);
	camera_black_level_spin->setMaximum(100);
	camera_black_level_spin->setValue(
		APP->config.value(Settings::Camera::BlackLevel, Settings::Camera::BlackLevelDefaultValue).toDouble());

	// Camera gain (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_gain_spin->setMinimum(0);
	camera_gain_spin->setMaximum(50);
	camera_gain_spin->setValue(
		APP->config.value(Settings::Camera::Gain, Settings::Camera::GainDefaultValue).toDouble());

	// Camera gamma (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_gamma_spin->setMinimum(0);
	camera_gamma_spin->setMaximum(4);
	camera_gamma_spin->setValue(
		APP->config.value(Settings::Camera::Gamma, Settings::Camera::GammaDefaultValue).toDouble());

	// Alignment mode starts disabled.
	alignment_mode_check->setChecked(false);

	// Test navigation buttons start disabled.
	test_prev_button->setEnabled(false);
	test_next_button->setEnabled(false);
}

/* IMAGE PREVIEW =========================================================================== */

/// <summary>
/// Updates the preview in the UI with the latest frame from the camera and displays its corresponding grayscale statistics.
/// </summary>
/// <param name="new_frame"></param>
/// <param name="gray_stats"></param>
void SMCP::CaptureQDialog::_on_new_camera_frame(const QPixmap& new_frame, const QString& gray_stats) const
{
	cameraPreview->setImage(new_frame);				// Set the new camera frame.
	camera_gray_stats_label->setText(gray_stats);	// Set frame gray statistics.
}

/* CAMERA ================================================================================== */

/// <summary>
/// Update the interface dropdown menu with the connected Spinnaker cameras and automatically restore the selection of the last used camera by reading
/// its saved serial number.
/// </summary>
void SMCP::CaptureQDialog::update_camera_combo()
{
	// Disable combo box signals.
	camera_combo->blockSignals(true);

	// Retrieve the list of Spinnaker cameras.
	camera_list.Clear();
	camera_list = spinnaker_system_ptr->GetCameras();
	n_cameras = static_cast<int>(camera_list.GetSize());

	// Check cameras.
	if (n_cameras == 0)
	{
		camera_combo->addItem("No camera found");
		qDebug() << "No Spinnaker camera detected.";
		return;
	}

	// Try to the get current camera.
	const QSettings& app_settings = APP->getSettings();
	const QString saved_camera_serial_number = app_settings.value(Settings::Camera::SerialNumber, "").toString();
	camera_idx = 0;

	// Iterate trough each camera, initialize it, read its name and de-initialize it.
	for (int i = 0; i < n_cameras; ++i)
	{
		Spinnaker::CameraPtr temp_camera_ptr = camera_list.GetByIndex(i);
		temp_camera_ptr->Init();

		// Access the "DeviceNodeName".
		std::string temp_model_name;
		std::string temp_camera_serial_number;
		if (CameraUtilities::getCameraInfo(temp_camera_ptr, temp_model_name, temp_camera_serial_number))
		{
			QString temp_camera_name = QString::fromStdString(temp_model_name).append(" ").append(temp_camera_serial_number.c_str());

			// Add to combo.
			camera_combo->addItem(temp_camera_name);

			// Check camera serial number.
			if (saved_camera_serial_number == QString::fromStdString(temp_camera_serial_number))
			{
				camera_idx = i;
				camera_serial_number = saved_camera_serial_number;
			}
		}

		// De-initialize the camera.
		temp_camera_ptr->DeInit();
	}

	// Select the camera.
	camera_combo->setCurrentIndex(camera_idx);

	// Enable combo box signals.
	camera_combo->blockSignals(false);
}

/// <summary>
/// Restarts the camera when the camera selection changes.
/// </summary>
/// <param name="index">Index of the newly selected camera.</param>
void SMCP::CaptureQDialog::on_camera_combo_currentIndexChanged(int index)
{
	if (camera_list.GetSize() == 0) return;

	camera_idx = index;
	start_camera();
}

/// <summary>
/// Start the current selected camera.
/// </summary>
void SMCP::CaptureQDialog::start_camera()
{
	// Check valid index.
	if (camera_idx < 0 || camera_idx > n_cameras) return;

	// Busy cursor.
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	QApplication::processEvents();

	// Stop the current camera.
	stop_camera();

	// Give some time to release resources.
	wait_time(1);

	// Configure the camera.
	try
	{
		// Get the new camera index.
		camera_ptr = camera_list.GetByIndex(camera_idx);

		// Setup camera thread.
		setup_camera_thread();
	}
	catch (Spinnaker::Exception& e)
	{
		QMessageBox::critical(
			nullptr,
			tr("Error while opening the camera."),
			tr(""));

		qCritical() << "[Spinnaker Error]: " << e.GetErrorMessage() << '\n';
	}

	// Restore cursor.
	QApplication::restoreOverrideCursor();
}

/// <summary>
/// Stops the current camera and free resources.
/// </summary>
void SMCP::CaptureQDialog::stop_camera()
{
	// Busy cursor.
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	QApplication::processEvents();

	// Check valid camera.
	if (camera_ptr == nullptr)
	{
		QApplication::restoreOverrideCursor();
		return;
	}

	// Clean camera preview.
	cameraPreview->clear();

	// Stop the current camera capture worker.
	if (camera_worker) camera_worker->stop();
	camera_thread.quit();
	camera_thread.wait();
	camera_worker = nullptr;

	// Restore the cursor.
	QApplication::restoreOverrideCursor();
}

/// <summary>
/// Configure and start an independent workflow to manage the camera, loading its previous settings and connecting all the necessary signals for
/// asynchronous communication with the main interface.
/// </summary>
void SMCP::CaptureQDialog::setup_camera_thread()
{
	// CAMERA SETTINGS ===================================================
	const QSettings& appSettings = APP->getSettings();
	CameraSettings cameraSettings;

	cameraSettings.BlackLevel = appSettings.value(
		Settings::Camera::BlackLevel, Settings::Camera::BlackLevelDefaultValue).toDouble();
	cameraSettings.ExposureTime = appSettings.value(
		Settings::Camera::ExposureTime, Settings::Camera::ExposureTimeDefaultValue).toDouble();
	cameraSettings.Gain = appSettings.value(
		Settings::Camera::Gain, Settings::Camera::GainDefaultValue).toDouble();
	cameraSettings.Gamma = appSettings.value(
		Settings::Camera::Gamma, Settings::Camera::GammaDefaultValue).toDouble();

	// Create the camera worker and move to thread.
	camera_worker = new CameraWorker(camera_ptr, cameraSettings);
	camera_worker->moveToThread(&camera_thread);

	// THREAD SIGNALS ====================================================
	connect(&camera_thread, &QThread::started, camera_worker, &CameraWorker::start);
	connect(&camera_thread, &QThread::finished, camera_worker, &CameraWorker::deleteLater);
	connect(camera_worker, &CameraWorker::finishedSignal, &camera_thread, &QThread::quit);

	// CAMERA SIGNALS ===================================================
	connect(this, &CaptureQDialog::onNewCameraBlackLevelSignal, camera_worker, &CameraWorker::onNewCameraBlackLevel);
	connect(this, &CaptureQDialog::onNewCameraExposureTimeSignal, camera_worker, &CameraWorker::onNewCameraExposureTime);
	connect(this, &CaptureQDialog::onNewCameraGainSignal, camera_worker, &CameraWorker::onNewCameraGain);
	connect(this, &CaptureQDialog::onNewCameraGammaSignal, camera_worker, &CameraWorker::onNewCameraGamma);
	connect(camera_worker, &CameraWorker::newCameraSettingsSignal, this, &CaptureQDialog::_on_new_camera_settings);

	// IMAGE SIGNALS ====================================================
	connect(camera_worker, &CameraWorker::newFrameReadySignal, this, &CaptureQDialog::_on_new_camera_frame);

	connect(this, &CaptureQDialog::startCaptureSignal, camera_worker, &CameraWorker::onStartCapture);
	connect(this, &CaptureQDialog::endCaptureSignal, camera_worker, &CameraWorker::onEndCapture);

	connect(this, &CaptureQDialog::needStoreImageSignal, camera_worker, &CameraWorker::onNeedToStoreImage);
	connect(camera_worker, &CameraWorker::imageStoredSignal, this, &CaptureQDialog::_on_image_stored);

	connect(camera_worker, &CameraWorker::imageSaved, this, &CaptureQDialog::_on_image_saved);
	connect(camera_worker, &CameraWorker::allImagesSavedSignal, this, &CaptureQDialog::_on_all_images_saved);

	// ALIGNMENT SIGNALS ================================================
	connect(this, &CaptureQDialog::_on_alignment_signal, camera_worker, &CameraWorker::onAlignmentMode);

	// Start thread.
	camera_thread.start();
}

/* CAMERA SETTINGS ========================================================================== */

/// <summary>
/// It emits a signal to update the camera's black level each time the user modifies its value in the UI.
/// </summary>
/// <param name="new_value"></param>
void SMCP::CaptureQDialog::on_camera_black_level_spin_valueChanged(double new_value)
{
	emit onNewCameraBlackLevelSignal(new_value);
}

/// <summary>
/// It emits a signal to update the camera's exposure time each time the user modifies its value in the UI.
/// </summary>
/// <param name="new_value"></param>
void SMCP::CaptureQDialog::on_camera_exposure_spin_valueChanged(double new_value)
{
	emit onNewCameraExposureTimeSignal(new_value);
}

/// <summary>
/// It emits a signal to update the camera's gain each time the user modifies its value in the UI.
/// </summary>
/// <param name="new_value"></param>
void SMCP::CaptureQDialog::on_camera_gain_spin_valueChanged(double new_value)
{
	emit onNewCameraGainSignal(new_value);
}

/// <summary>
/// It emits a signal to update the camera's gamma each time the user modifies its value in the UI.
/// </summary>
/// <param name="new_value"></param>
void SMCP::CaptureQDialog::on_camera_gamma_spin_valueChanged(double new_value)
{
	emit onNewCameraGammaSignal(new_value);
}

/// <summary>
/// Update the ranges and values of the controls in the UI based on the settings and limits reported by the camera.
/// </summary>
/// <param name="settings"></param>
void SMCP::CaptureQDialog::_on_new_camera_settings(const CameraSettings& settings) const
{
	// Block signals while updating ranges and values to prevent feedback loops
	// (setValue triggers valueChanged → emits signal to CameraWorker → worker reports back → infinite loop).
	const QSignalBlocker block_black_level(camera_black_level_spin);
	const QSignalBlocker block_exposure(camera_exposure_spin);
	const QSignalBlocker block_gain(camera_gain_spin);
	const QSignalBlocker block_gamma(camera_gamma_spin);

	// Black level.
	camera_black_level_spin->setMinimum(settings.BlackLevelMin);
	camera_black_level_spin->setMaximum(settings.BlackLevelMax);
	camera_black_level_spin->setValue(settings.BlackLevel);

	// Exposure time.
	camera_exposure_spin->setMinimum(settings.ExposureTimeMin);
	camera_exposure_spin->setMaximum(settings.ExposureTimeMax);
	camera_exposure_spin->setValue(settings.ExposureTime);

	// Gain.
	camera_gain_spin->setMinimum(settings.GainMin);
	camera_gain_spin->setMaximum(settings.GainMax);
	camera_gain_spin->setValue(settings.Gain);

	// Gamma.
	camera_gamma_spin->setMinimum(settings.GammaMin);
	camera_gamma_spin->setMaximum(settings.GammaMax);
	camera_gamma_spin->setValue(settings.Gamma);
}

/* IMAGE PROCESSING ======================================================================== */

/// <summary>
/// Releases the logical lock indicating that the camera has finished capturing.
/// </summary>
void SMCP::CaptureQDialog::_on_image_stored()
{
	is_storing_image = false;
}

/// <summary>
/// Dynamically update the progress bar in the UI so that the user can see the progress of the writing process to the disk.
/// </summary>
/// <param name="total_images_to_save"></param>
/// <param name="current_image_saved"></param>
void SMCP::CaptureQDialog::_on_image_saved(int total_images_to_save, int current_image_saved) const
{
	progress_bar->setMaximum(total_images_to_save);
	progress_bar->setValue(current_image_saved);
}

/// <summary>
/// Release the final lock by notifying the system that the entire batch of captures has been successfully saved to the output directory.
/// </summary>
void SMCP::CaptureQDialog::_on_all_images_saved()
{
	is_saving_image = false;
}

/* PROJECTOR =============================================================================== */

/// <summary>
/// Refreshes the screen combo box with the currently detected displays.
/// </summary>
/// <returns>Total number of detected screens after the update.</returns>
/// <remarks>
/// Restores the previous selection if still valid, otherwise falls back to the saved configuration value, or defaults to the first screen.
/// </remarks>
int SMCP::CaptureQDialog::update_screen_combo() const
{
	// Block signals with RAII (automatically unblocked when leaving scope).
	const QSignalBlocker blocker(screen_combo);

	// Save current selection before clearing.
	const int previous_index = screen_combo->currentIndex();
	screen_combo->clear();

	// Populate the combo with detected screens.
	const QList<QScreen*> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); ++i)
	{
		const QSize size = screens[i]->geometry().size();
		screen_combo->addItem(
			QStringLiteral("Screen %1 [%2x%3]").arg(i).arg(size.width()).arg(size.height()));
	}

	// Determine which index to select.
	const int count = screen_combo->count();
	const int saved_index = APP->config.value(Settings::Projector::Screen, Settings::Projector::ScreenDefaultValue).toInt();

	if (previous_index >= 0 && previous_index < count)
	{
		// Previous selection is still valid.
		screen_combo->setCurrentIndex(previous_index);
	}
	else if (saved_index >= 0 && saved_index < count)
	{
		// Restore from saved configuration.
		screen_combo->setCurrentIndex(saved_index);
	}
	else
	{
		// Fallback to the first screen.
		screen_combo->setCurrentIndex(0);
	}

	return count;
}

/// <summary>
/// Updates the projector widget screen when the screen selection changes.
/// </summary>
/// <param name="index">Index of the newly selected screen.</param>
void SMCP::CaptureQDialog::on_screen_combo_currentIndexChanged(int index)
{
	projector_widget.set_screen(index);
}

/// <summary>
/// Updates the projector preview with a new image.
/// </summary>
/// <param name="image">Pixmap to display in the projector preview widget.</param>
void SMCP::CaptureQDialog::_on_new_projector_image(QPixmap image) const
{
	projector_image->setPixmap(image);
}

/// <summary>
/// Handles the test mode checkbox state change. When checked, starts the projector preview by connecting the display signal, opening the projector,
/// and advancing to the first Gray code pattern. When unchecked, stops the projector and restores the GUI to its idle state.
/// </summary>
/// <param name="state">The new checkbox state (Qt::CheckState).</param>
void SMCP::CaptureQDialog::on_test_check_stateChanged(int state)
{
	const bool is_checked = (state == Qt::Checked);

	// Toggle GUI controls based on test mode.
	test_prev_button->setEnabled(is_checked);
	test_next_button->setEnabled(is_checked);
	screen_combo->setEnabled(!is_checked);
	projector_patterns_spin->setEnabled(!is_checked);
	continuous_check->setEnabled(!is_checked);
	wait_time_spin->setEnabled(!is_checked);

	if (is_checked)
	{
		capture_button->setEnabled(false);

		// Disable alignment mode.
		if (!alignment_mode_check->isChecked()) alignment_mode_check->setEnabled(false);

		// Connect the projector display signal for live preview.
		connect(&projector_widget, &ProjectorWidget::new_image, this, &CaptureQDialog::_on_new_projector_image);

		// Configure and start the projector.
		projector_widget.set_pattern_count(projector_patterns_spin->value());
		projector_widget.start();

		// Skip white (index 0) and black (index 1) to reach the first Gray code pattern.
		projector_widget.next();
		projector_widget.next();

		// Start automatic advance if continuous mode is enabled.
		if (continuous_check->isChecked())
		{
			QTimer::singleShot(continuous_spin->value(), this, &CaptureQDialog::auto_next);
		}
	}
	else
	{
		// Stop the projector and disconnect the display signal.
		projector_widget.stop();
		disconnect(&projector_widget, &ProjectorWidget::new_image, this, &CaptureQDialog::_on_new_projector_image);

		// Re-enable capture only if alignment mode is not active.
		capture_button->setEnabled(!alignment_mode_check->isChecked());

		// Enabled alignment mode.
		if (!alignment_mode_check->isChecked())
		{
			alignment_mode_check->setEnabled(true);
		}
	}
}

/// <summary>
/// Navigates the projector widget to the previous pattern.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
void SMCP::CaptureQDialog::on_test_prev_button_clicked(bool checked)
{
	projector_widget.clear_updated();
	projector_widget.prev();
}

/// <summary>
/// Navigates the projector widget to the next pattern.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
void SMCP::CaptureQDialog::on_test_next_button_clicked(bool checked)
{
	projector_widget.clear_updated();
	projector_widget.next();
}

/// <summary>
/// Automatically advances to the next projector pattern when test and continuous modes are active.
/// </summary>
/// <remarks>
/// Restarts the sequence from the beginning if the last pattern has been reached, then schedules itself recursively using the interval set in the
/// continuous spin box.
/// </remarks>
void SMCP::CaptureQDialog::auto_next()
{
	if (test_check->isChecked() && continuous_check->isChecked())
	{
		projector_widget.clear_updated();

		if (projector_widget.finished())
		{
			projector_widget.start();
		}

		projector_widget.next();

		QTimer::singleShot(continuous_spin->value(), this, SLOT(auto_next()));

		QApplication::processEvents();
	}
}

/* CAPTURE ================================================================================= */

/// <summary>
/// Handles the capture button click event, running the full structured-light capture sequence.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
/// <remarks>
/// Creates a timestamped output directory, iterates through all projector patterns while emitting signals to store each captured image, and
/// re-enables UI controls upon completion.
/// </remarks>
void SMCP::CaptureQDialog::on_capture_button_clicked(bool checked)
{
	// Check if there are camera available.
	if (camera_list.GetSize() == 0)
	{
		capture_button->setChecked(false);
		return;
	}

	// Configure output directory.
	session = APP->get_root_dir() + "/" + QDateTime::currentDateTime().toString("yyyy-MMM-dd_hh.mm.ss.zzz");
	if (const QDir session_dir; !session_dir.mkpath(session))
	{
		qCritical() << "[CaptureQDialog::on_capture_button_clicked] --> Can't create output directory" << session << ".";
		return;
	}

	// Disable all configurations.
	disable_controls();

	// Initialize the projector widget.
	projector_widget.set_pattern_count(projector_patterns_spin->value());
	projector_widget.start();

	// Start camera capture.
	emit startCaptureSignal(projector_widget.get_pattern_count());

	// Save projector info.
	const QString projectorInfoFileName = QString("%1/%2").arg(session).arg(Literals::ProjectorInfoFilename);
	if (!projector_widget.save_info(projectorInfoFileName, false))
	{
		qWarning() << "[CaptureModule::on_captureButton_clicked] --> Can't save the projector info.";
	}

	// Setup and show progress indicators.
	// Total patterns = 2 (white + black) + 2N (vertical Gray codes) + 2N (horizontal Gray codes) = 4N + 2.
	const int nPatterns = 4 * projector_patterns_spin->value() + 2;
	progress_bar->setMinimum(0);
	progress_bar->setMaximum(nPatterns);
	progress_bar->setValue(0);
	progress_label->setText("Capturing...");
	progress_label->setVisible(true);
	progress_bar->setVisible(true);
	current_message_label->setVisible(false);
	QApplication::processEvents();

	// Capture loop.
	int current_pattern = 0;
	while (!projector_widget.finished())
	{
		// Update the pattern.
		projector_widget.clear_updated();
		projector_widget.next();

		wait_time(100);

		// Wait for the projector.
		while (!projector_widget.is_updated()) QApplication::processEvents();

		// Update progressbar.
		current_pattern++;
		progress_bar->setValue(current_pattern);

		// wait.
		wait_time(wait_time_spin->value());

		// Send signal.
		QString imageName = QString("%1/cam_%2.png")
			.arg(session)
			.arg(projector_widget.get_current_pattern() + 1, 2, 10, QLatin1Char('0'));
		emit needStoreImageSignal(imageName);

		is_storing_image = true;
		while (is_storing_image) QApplication::processEvents();
	}

	// De-initialize the projector.
	projector_widget.stop();

	// Switch progress indicators to saving phase.
	progress_label->setText("Saving images...");
	progress_bar->setMinimum(0);
	progress_bar->setMaximum(0);
	QApplication::processEvents();

	// Wait to save images.
	emit endCaptureSignal();

	is_saving_image = true;
	while (is_saving_image) QApplication::processEvents();

	// Hide progress indicators and show completion message.
	progress_label->setVisible(false);
	progress_bar->setVisible(false);
	current_message_label->setText("Capture complete.");
	current_message_label->setVisible(true);

	// Reset the TreeView.
	APP->set_root_dir(APP->get_root_dir());

	// Enable all configurations.
	enable_controls();
}

/* ALIGNMENT =============================================================================== */

/// <summary>
/// Handles the alignment mode checkbox state change. Enables or disables the projector alignment mode based on the checkbox state. When active, the
/// projector draws a centered cross over the projected pattern to assist with physical alignment.
/// </summary>
/// <param name="new_state">The new checkbox state (Qt::CheckState).</param>
void SMCP::CaptureQDialog::on_alignment_mode_check_stateChanged(int new_state)
{
	const bool is_active = (new_state == Qt::Checked);

	emit _on_alignment_signal(is_active);
	projector_widget.set_draw_cross(is_active);
	projector_widget.clear_updated();
	test_check->setCheckState(is_active ? Qt::Checked : Qt::Unchecked);
	test_check->setEnabled(!is_active);
}

/* QDialog FUNCTIONS ======================================================================= */

/// <summary>
/// Updates the output directory text field when the root directory changes.
/// </summary>
/// <param name="dirname">New root directory path.</param>
void SMCP::CaptureQDialog::_on_root_dir_changed(const QString& dirname) const
{
	output_dir_line->setText(dirname);
}

/// <summary>
/// Updates the application root directory when the output path is manually edited.
/// </summary>
/// <param name="text">New directory path entered by the user.</param>
void SMCP::CaptureQDialog::on_output_dir_line_textEdited(const QString& text)
{
	APP->set_root_dir(text);
}

/// <summary>
/// Opens a dialog to browse and change the output directory.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
void SMCP::CaptureQDialog::on_output_dir_button_clicked(bool checked)
{
	APP->change_root_dir(this);
}

/// <summary>
/// Handles the close/cancel button click, accepting the dialog only when in close mode.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
void SMCP::CaptureQDialog::on_close_cancel_button_clicked(bool checked)
{
	accept();
}

/* UTILITIES =============================================================================== */

/// <summary>
/// Blocks execution for a specified duration while keeping the UI responsive.
/// </summary>
/// <param name="milliseconds">Duration to wait in milliseconds.</param>
void SMCP::CaptureQDialog::wait_time(const int milliseconds)
{
	QTime timer;
	timer.start();
	while (timer.elapsed() < milliseconds)
	{
		QApplication::processEvents();
	}
}

/// <summary>
/// Re-enables all configurable UI controls after a capture session finishes,
/// allowing the user to modify settings for the next capture.
/// </summary>
/// <remarks>
/// Restores interactivity to three control groups:
/// - Projector: screen_combo, projector_patterns_spin, wait_time_spin,
///   continuous_check, continuous_spin, test_check, alignment_mode_check.
/// - Camera: camera_combo, camera_exposure_spin, camera_black_level_spin,
///   camera_gain_spin, camera_gamma_spin.
/// - Workspace: output_dir_line, output_dir_button.
/// Must be called symmetrically with disable_controls() once the pipeline completes
/// or is aborted to leave the dialog in a consistent, interactive state.
/// </remarks>
void SMCP::CaptureQDialog::enable_controls() const
{
	// Projector widgets.
	screen_combo->setEnabled(true);
	projector_patterns_spin->setEnabled(true);
	wait_time_spin->setEnabled(true);
	continuous_check->setEnabled(true);
	continuous_spin->setEnabled(true);
	test_check->setEnabled(true);
	alignment_mode_check->setEnabled(true);

	// Camera widgets.
	camera_combo->setEnabled(true);
	camera_black_level_spin->setEnabled(true);
	camera_gain_spin->setEnabled(true);
	camera_gamma_spin->setEnabled(true);
	camera_exposure_spin->setEnabled(true);

	// Workspace.
	output_dir_line->setEnabled(true);
	output_dir_button->setEnabled(true);
}

/// <summary>
/// Disables all configurable UI controls during an active capture session to
/// prevent the user from modifying settings while the pipeline is running.
/// </summary>
/// <remarks>
/// Locks interactivity on three control groups:
/// - Projector: screen_combo, projector_patterns_spin, wait_time_spin,
///   continuous_check, continuous_spin, test_check, alignment_mode_check.
/// - Camera: camera_combo, camera_exposure_spin, camera_black_level_spin,
///   camera_gain_spin, camera_gamma_spin.
/// - Workspace: output_dir_line, output_dir_button.
/// Must be called at the start of every capture and paired with enable_controls()
/// upon completion to restore the dialog to its interactive state.
/// </remarks>
void SMCP::CaptureQDialog::disable_controls() const
{
	// Projector widgets.
	screen_combo->setEnabled(false);
	projector_patterns_spin->setEnabled(false);
	wait_time_spin->setEnabled(false);
	continuous_check->setEnabled(false);
	continuous_spin->setEnabled(false);
	test_check->setEnabled(false);
	alignment_mode_check->setEnabled(false);

	// Camera widgets.
	camera_combo->setEnabled(false);
	camera_black_level_spin->setEnabled(false);
	camera_gain_spin->setEnabled(false);
	camera_gamma_spin->setEnabled(false);
	camera_exposure_spin->setEnabled(false);

	// Workspace.
	output_dir_line->setEnabled(false);
	output_dir_button->setEnabled(false);
}