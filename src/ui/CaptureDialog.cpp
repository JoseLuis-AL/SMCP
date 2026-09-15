#include "ui/CaptureDialog.h"

#include <QMessageBox>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDebug>
#include <QScreen>

#include <iostream>
#include <strmif.h>
#include <opencv2/imgproc/imgproc.hpp>

#include "app/Application.h"
#include "camera/CameraUtilities.h"
#include "common/Literals.h"
#include "common/Settings.h"

namespace smcp
{

/// <summary>
/// Initializes the capture dialog, setting up the UI, Spinnaker system, and camera preview.
/// </summary>
/// <param name="parent">Parent widget.</param>
/// <param name="flags">Window flags for the dialog.</param>
/// <remarks>
/// Connects application signals, initializes the Spinnaker system, populates screen and camera combo boxes, restores saved settings, and starts the
/// camera preview.
/// </remarks>
CaptureDialog::CaptureDialog(QWidget* parent, Qt::WindowFlags flags) : QDialog(parent, flags)
{
	// Build the UI from the .ui form.
	setupUi(this);

	// Initialize subsystems.
	InitSpinnaker();
	InitSignals();
	InitControls();

	// Populate combo boxes with available devices.
	UpdateCameraCombo();
	const int screenIdx = UpdateScreenCombo();

	// Configure the projector for the selected screen.
	projectorWidget.SetScreen(screenIdx);

	// Start the camera live preview.
	StartCamera();
}

/// <summary>
/// Saves settings and releases all camera and projector resources.
/// </summary>
/// <remarks>
/// Persists capture, projector, and camera settings before stopping the projector, stopping the camera, and releasing the Spinnaker system instance.
/// </remarks>
CaptureDialog::~CaptureDialog()
{
	QSettings& appSettings = APP->GetSettings();

	// Capture values.
	appSettings.setValue(Settings::Capture::Wait_Time, wait_time_spin->value());
	appSettings.setValue(Settings::Capture::Continuous, continuous_spin->value());

	// Free projector.
	appSettings.setValue(Settings::Projector::Screen, screen_combo->currentIndex());
	appSettings.setValue(Settings::Projector::Pattern_Count, projector_patterns_spin->value());
	projectorWidget.Stop();

	// Free camera.
	StopCamera();
	appSettings.setValue(Settings::Camera::Black_Level, camera_black_level_spin->value());
	appSettings.setValue(Settings::Camera::Exposure_Time, camera_exposure_spin->value());
	appSettings.setValue(Settings::Camera::Gain, camera_gain_spin->value());
	appSettings.setValue(Settings::Camera::Gamma, camera_gamma_spin->value());
	appSettings.setValue(Settings::Camera::Serial_Number, cameraSerialNumber);

	// De-Initialize the spinnaker system.
	try
	{
		cameraPtr = nullptr;
		cameraList.Clear();
		spinnakerSystemPtr->ReleaseInstance();
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
void CaptureDialog::InitSpinnaker()
{
	spinnakerSystemPtr = Spinnaker::System::GetInstance();
}

/// <summary>
/// Connects all required signals to their corresponding slots.
/// </summary>
void CaptureDialog::InitSignals()
{
	connect(APP, &Application::root_dir_changed, this, &CaptureDialog::_on_root_dir_changed);
}

/// <summary>
/// Configures all UI controls with their initial values from the application configuration. Sets valid ranges before loading values to prevent silent
/// clamping by the spin boxes. Blocks signals during initialization to avoid emitting premature change notifications to subsystems that are not yet
/// ready (e.g. the camera worker thread).
/// </summary>
void CaptureDialog::InitControls() const
{
	// Hide progress indicators until a capture starts.
	current_message_label->setVisible(false);
	progress_label->setVisible(false);
	progress_bar->setVisible(false);

	// Output directory.
	output_dir_line->setText(APP->GetRootDir());

	// Block signals on all spin boxes to prevent premature valueChanged emissions.
	const QSignalBlocker blockPatterns(projector_patterns_spin);
	const QSignalBlocker blockWait(wait_time_spin);
	const QSignalBlocker blockExposure(camera_exposure_spin);
	const QSignalBlocker blockContinuous(continuous_spin);
	const QSignalBlocker blockBlackLevel(camera_black_level_spin);
	const QSignalBlocker blockGain(camera_gain_spin);
	const QSignalBlocker blockGamma(camera_gamma_spin);

	// Projector pattern count.
	projector_patterns_spin->setValue(
		APP->config.value(Settings::Projector::Pattern_Count, Settings::Projector::Pattern_Count_Default_Value).toInt());

	// Capture wait time between projected patterns.
	wait_time_spin->setValue(
		APP->config.value(Settings::Capture::Wait_Time, Settings::Capture::Wait_Time_Default_Value).toInt());

	// Camera exposure (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_exposure_spin->setMinimum(0);
	camera_exposure_spin->setMaximum(50000);
	camera_exposure_spin->setValue(
		APP->config.value(Settings::Camera::Exposure_Time, Settings::Camera::Exposure_Time_Default_Value).toDouble());

	// Continuous capture interval (set range before value to avoid clamping).
	continuous_spin->setMinimum(10);
	continuous_spin->setMaximum(9999);
	continuous_spin->setValue(
		APP->config.value(Settings::Capture::Continuous, Settings::Capture::Continuous_Default_Value).toInt());

	// Camera black level (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_black_level_spin->setMinimum(0);
	camera_black_level_spin->setMaximum(100);
	camera_black_level_spin->setValue(
		APP->config.value(Settings::Camera::Black_Level, Settings::Camera::Black_Level_Default_Value).toDouble());

	// Camera gain (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_gain_spin->setMinimum(0);
	camera_gain_spin->setMaximum(50);
	camera_gain_spin->setValue(
		APP->config.value(Settings::Camera::Gain, Settings::Camera::Gain_Default_Value).toDouble());

	// Camera gamma (temporary range until hardware reports real limits via _on_new_camera_settings).
	camera_gamma_spin->setMinimum(0);
	camera_gamma_spin->setMaximum(4);
	camera_gamma_spin->setValue(
		APP->config.value(Settings::Camera::Gamma, Settings::Camera::Gamma_Default_Value).toDouble());

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
/// <param name="newFrame"></param>
/// <param name="grayStats"></param>
void CaptureDialog::_on_new_camera_frame(const QPixmap& newFrame, const QString& grayStats) const
{
	cameraPreview->setImage(newFrame);				// Set the new camera frame.
	camera_gray_stats_label->setText(grayStats);	// Set frame gray statistics.
}

/* CAMERA ================================================================================== */

/// <summary>
/// Update the interface dropdown menu with the connected Spinnaker cameras and automatically restore the selection of the last used camera by reading
/// its saved serial number.
/// </summary>
void CaptureDialog::UpdateCameraCombo()
{
	// Disable combo box signals.
	camera_combo->blockSignals(true);

	// Retrieve the list of Spinnaker cameras.
	cameraList.Clear();
	cameraList = spinnakerSystemPtr->GetCameras();
	nCameras = static_cast<int>(cameraList.GetSize());

	// Check cameras.
	if (nCameras == 0)
	{
		camera_combo->addItem("No camera found");
		qDebug() << "No Spinnaker camera detected.";
		return;
	}

	// Try to the get current camera.
	const QSettings& appSettings = APP->GetSettings();
	const QString savedCameraSerialNumber = appSettings.value(Settings::Camera::Serial_Number, "").toString();
	cameraIdx = 0;

	// Iterate trough each camera, initialize it, read its name and de-initialize it.
	for (int i = 0; i < nCameras; ++i)
	{
		Spinnaker::CameraPtr tempCameraPtr = cameraList.GetByIndex(i);
		tempCameraPtr->Init();

		// Access the "DeviceNodeName".
		std::string tempModelName;
		std::string tempCameraSerialNumber;
		if (CameraUtilities::GetCameraInfo(tempCameraPtr, tempModelName, tempCameraSerialNumber))
		{
			QString tempCameraName = QString::fromStdString(tempModelName).append(" ").append(tempCameraSerialNumber.c_str());

			// Add to combo.
			camera_combo->addItem(tempCameraName);

			// Check camera serial number.
			if (savedCameraSerialNumber == QString::fromStdString(tempCameraSerialNumber))
			{
				cameraIdx = i;
				cameraSerialNumber = savedCameraSerialNumber;
			}
		}

		// De-initialize the camera.
		tempCameraPtr->DeInit();
	}

	// Select the camera.
	camera_combo->setCurrentIndex(cameraIdx);

	// Enable combo box signals.
	camera_combo->blockSignals(false);
}

/// <summary>
/// Restarts the camera when the camera selection changes.
/// </summary>
/// <param name="index">Index of the newly selected camera.</param>
void CaptureDialog::on_camera_combo_currentIndexChanged(int index)
{
	if (cameraList.GetSize() == 0) return;

	cameraIdx = index;
	StartCamera();
}

/// <summary>
/// Start the current selected camera.
/// </summary>
void CaptureDialog::StartCamera()
{
	// Check valid index.
	if (cameraIdx < 0 || cameraIdx > nCameras) return;

	// Busy cursor.
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	QApplication::processEvents();

	// Stop the current camera.
	StopCamera();

	// Give some time to release resources.
	WaitTime(1);

	// Configure the camera.
	try
	{
		// Get the new camera index.
		cameraPtr = cameraList.GetByIndex(cameraIdx);

		// Setup camera thread.
		SetupCameraThread();
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
void CaptureDialog::StopCamera()
{
	// Busy cursor.
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	QApplication::processEvents();

	// Check valid camera.
	if (cameraPtr == nullptr)
	{
		QApplication::restoreOverrideCursor();
		return;
	}

	// Clean camera preview.
	cameraPreview->Clear();

	// Stop the current camera capture worker.
	if (cameraWorker) cameraWorker->stop();
	cameraThread.quit();
	cameraThread.wait();
	cameraWorker = nullptr;

	// Restore the cursor.
	QApplication::restoreOverrideCursor();
}

/// <summary>
/// Configure and start an independent workflow to manage the camera, loading its previous settings and connecting all the necessary signals for
/// asynchronous communication with the main interface.
/// </summary>
void CaptureDialog::SetupCameraThread()
{
	// CAMERA SETTINGS ===================================================
	const QSettings& appSettings = APP->GetSettings();
	CameraSettings cameraSettings;

	cameraSettings.BlackLevel = appSettings.value(
		Settings::Camera::Black_Level, Settings::Camera::Black_Level_Default_Value).toDouble();
	cameraSettings.ExposureTime = appSettings.value(
		Settings::Camera::Exposure_Time, Settings::Camera::Exposure_Time_Default_Value).toDouble();
	cameraSettings.Gain = appSettings.value(
		Settings::Camera::Gain, Settings::Camera::Gain_Default_Value).toDouble();
	cameraSettings.Gamma = appSettings.value(
		Settings::Camera::Gamma, Settings::Camera::Gamma_Default_Value).toDouble();

	// Create the camera worker and move to thread.
	cameraWorker = new CameraWorker(cameraPtr, cameraSettings);
	cameraWorker->moveToThread(&cameraThread);

	// THREAD SIGNALS ====================================================
	connect(&cameraThread, &QThread::started, cameraWorker, &CameraWorker::start);
	connect(&cameraThread, &QThread::finished, cameraWorker, &CameraWorker::deleteLater);
	connect(cameraWorker, &CameraWorker::finishedSignal, &cameraThread, &QThread::quit);

	// CAMERA SIGNALS ===================================================
	connect(this, &CaptureDialog::onNewCameraBlackLevelSignal, cameraWorker, &CameraWorker::onNewCameraBlackLevel);
	connect(this, &CaptureDialog::onNewCameraExposureTimeSignal, cameraWorker, &CameraWorker::onNewCameraExposureTime);
	connect(this, &CaptureDialog::onNewCameraGainSignal, cameraWorker, &CameraWorker::onNewCameraGain);
	connect(this, &CaptureDialog::onNewCameraGammaSignal, cameraWorker, &CameraWorker::onNewCameraGamma);
	connect(cameraWorker, &CameraWorker::newCameraSettingsSignal, this, &CaptureDialog::_on_new_camera_settings);

	// IMAGE SIGNALS ====================================================
	connect(cameraWorker, &CameraWorker::newFrameReadySignal, this, &CaptureDialog::_on_new_camera_frame);

	connect(this, &CaptureDialog::startCaptureSignal, cameraWorker, &CameraWorker::onStartCapture);
	connect(this, &CaptureDialog::endCaptureSignal, cameraWorker, &CameraWorker::onEndCapture);

	connect(this, &CaptureDialog::needStoreImageSignal, cameraWorker, &CameraWorker::onNeedToStoreImage);
	connect(cameraWorker, &CameraWorker::imageStoredSignal, this, &CaptureDialog::_on_image_stored);

	connect(cameraWorker, &CameraWorker::imageSaved, this, &CaptureDialog::_on_image_saved);
	connect(cameraWorker, &CameraWorker::allImagesSavedSignal, this, &CaptureDialog::_on_all_images_saved);

	// ALIGNMENT SIGNALS ================================================
	connect(this, &CaptureDialog::_on_alignment_signal, cameraWorker, &CameraWorker::onAlignmentMode);

	// Start thread.
	cameraThread.start();
}

/* CAMERA SETTINGS ========================================================================== */

/// <summary>
/// It emits a signal to update the camera's black level each time the user modifies its value in the UI.
/// </summary>
/// <param name="newValue"></param>
void CaptureDialog::on_camera_black_level_spin_valueChanged(double newValue)
{
	emit onNewCameraBlackLevelSignal(newValue);
}

/// <summary>
/// It emits a signal to update the camera's exposure time each time the user modifies its value in the UI.
/// </summary>
/// <param name="newValue"></param>
void CaptureDialog::on_camera_exposure_spin_valueChanged(double newValue)
{
	emit onNewCameraExposureTimeSignal(newValue);
}

/// <summary>
/// It emits a signal to update the camera's gain each time the user modifies its value in the UI.
/// </summary>
/// <param name="newValue"></param>
void CaptureDialog::on_camera_gain_spin_valueChanged(double newValue)
{
	emit onNewCameraGainSignal(newValue);
}

/// <summary>
/// It emits a signal to update the camera's gamma each time the user modifies its value in the UI.
/// </summary>
/// <param name="newValue"></param>
void CaptureDialog::on_camera_gamma_spin_valueChanged(double newValue)
{
	emit onNewCameraGammaSignal(newValue);
}

/// <summary>
/// Update the ranges and values of the controls in the UI based on the settings and limits reported by the camera.
/// </summary>
/// <param name="settings"></param>
void CaptureDialog::_on_new_camera_settings(const CameraSettings& settings) const
{
	// Block signals while updating ranges and values to prevent feedback loops
	// (setValue triggers valueChanged → emits signal to CameraWorker → worker reports back → infinite loop).
	const QSignalBlocker blockBlackLevel(camera_black_level_spin);
	const QSignalBlocker blockExposure(camera_exposure_spin);
	const QSignalBlocker blockGain(camera_gain_spin);
	const QSignalBlocker blockGamma(camera_gamma_spin);

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
void CaptureDialog::_on_image_stored()
{
	isStoringImage = false;
}

/// <summary>
/// Dynamically update the progress bar in the UI so that the user can see the progress of the writing process to the disk.
/// </summary>
/// <param name="totalImagesToSave"></param>
/// <param name="currentImageSaved"></param>
void CaptureDialog::_on_image_saved(int totalImagesToSave, int currentImageSaved) const
{
	progress_bar->setMaximum(totalImagesToSave);
	progress_bar->setValue(currentImageSaved);
}

/// <summary>
/// Release the final lock by notifying the system that the entire batch of captures has been successfully saved to the output directory.
/// </summary>
void CaptureDialog::_on_all_images_saved()
{
	isSavingImage = false;
}

/* PROJECTOR =============================================================================== */

/// <summary>
/// Refreshes the screen combo box with the currently detected displays.
/// </summary>
/// <returns>Total number of detected screens after the update.</returns>
/// <remarks>
/// Restores the previous selection if still valid, otherwise falls back to the saved configuration value, or defaults to the first screen.
/// </remarks>
int CaptureDialog::UpdateScreenCombo() const
{
	// Block signals with RAII (automatically unblocked when leaving scope).
	const QSignalBlocker blocker(screen_combo);

	// Save current selection before clearing.
	const int previousIndex = screen_combo->currentIndex();
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
	const int savedIndex = APP->config.value(Settings::Projector::Screen, Settings::Projector::Screen_Default_Value).toInt();

	if (previousIndex >= 0 && previousIndex < count)
	{
		// Previous selection is still valid.
		screen_combo->setCurrentIndex(previousIndex);
	}
	else if (savedIndex >= 0 && savedIndex < count)
	{
		// Restore from saved configuration.
		screen_combo->setCurrentIndex(savedIndex);
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
void CaptureDialog::on_screen_combo_currentIndexChanged(int index)
{
	projectorWidget.SetScreen(index);
}

/// <summary>
/// Updates the projector preview with a new image.
/// </summary>
/// <param name="image">Pixmap to display in the projector preview widget.</param>
void CaptureDialog::_on_new_projector_image(QPixmap image) const
{
	projector_image->setPixmap(image);
}

/// <summary>
/// Handles the test mode checkbox state change. When checked, starts the projector preview by connecting the display signal, opening the projector,
/// and advancing to the first Gray code pattern. When unchecked, stops the projector and restores the GUI to its idle state.
/// </summary>
/// <param name="state">The new checkbox state (Qt::CheckState).</param>
void CaptureDialog::on_test_check_stateChanged(int state)
{
	const bool isChecked = (state == Qt::Checked);

	// Toggle GUI controls based on test mode.
	test_prev_button->setEnabled(isChecked);
	test_next_button->setEnabled(isChecked);
	screen_combo->setEnabled(!isChecked);
	projector_patterns_spin->setEnabled(!isChecked);
	continuous_check->setEnabled(!isChecked);
	wait_time_spin->setEnabled(!isChecked);

	if (isChecked)
	{
		capture_button->setEnabled(false);

		// Disable alignment mode.
		if (!alignment_mode_check->isChecked()) alignment_mode_check->setEnabled(false);

		// Connect the projector display signal for live preview.
		connect(&projectorWidget, &ProjectorWidget::new_image, this, &CaptureDialog::_on_new_projector_image);

		// Configure and start the projector.
		projectorWidget.SetPatternCount(projector_patterns_spin->value());
		projectorWidget.Start();

		// Skip white (index 0) and black (index 1) to reach the first Gray code pattern.
		projectorWidget.Next();
		projectorWidget.Next();

		// Start automatic advance if continuous mode is enabled.
		if (continuous_check->isChecked())
		{
			QTimer::singleShot(continuous_spin->value(), this, &CaptureDialog::auto_next);
		}
	}
	else
	{
		// Stop the projector and disconnect the display signal.
		projectorWidget.Stop();
		disconnect(&projectorWidget, &ProjectorWidget::new_image, this, &CaptureDialog::_on_new_projector_image);

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
void CaptureDialog::on_test_prev_button_clicked(bool checked)
{
	projectorWidget.ClearUpdated();
	projectorWidget.Prev();
}

/// <summary>
/// Navigates the projector widget to the next pattern.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
void CaptureDialog::on_test_next_button_clicked(bool checked)
{
	projectorWidget.ClearUpdated();
	projectorWidget.Next();
}

/// <summary>
/// Automatically advances to the next projector pattern when test and continuous modes are active.
/// </summary>
/// <remarks>
/// Restarts the sequence from the beginning if the last pattern has been reached, then schedules itself recursively using the interval set in the
/// continuous spin box.
/// </remarks>
void CaptureDialog::auto_next()
{
	if (test_check->isChecked() && continuous_check->isChecked())
	{
		projectorWidget.ClearUpdated();

		if (projectorWidget.Finished())
		{
			projectorWidget.Start();
		}

		projectorWidget.Next();

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
void CaptureDialog::on_capture_button_clicked(bool checked)
{
	// Check if there are camera available.
	if (cameraList.GetSize() == 0)
	{
		capture_button->setChecked(false);
		return;
	}

	// Configure output directory.
	session = APP->GetRootDir() + "/" + QDateTime::currentDateTime().toString("yyyy-MMM-dd_hh.mm.ss.zzz");
	if (const QDir sessionDir; !sessionDir.mkpath(session))
	{
		qCritical() << "[CaptureDialog::on_capture_button_clicked] --> Can't create output directory" << session << ".";
		return;
	}

	// Disable all configurations.
	DisableControls();

	// Initialize the projector widget.
	projectorWidget.SetPatternCount(projector_patterns_spin->value());
	projectorWidget.Start();

	// Start camera capture.
	emit startCaptureSignal(projectorWidget.GetPatternCount());

	// Save projector info.
	const QString projectorInfoFileName = QString("%1/%2").arg(session).arg(Literals::Projector_Info_Filename);
	if (!projectorWidget.SaveInfo(projectorInfoFileName, false))
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
	int currentPattern = 0;
	while (!projectorWidget.Finished())
	{
		// Update the pattern.
		projectorWidget.ClearUpdated();
		projectorWidget.Next();

		WaitTime(100);

		// Wait for the projector.
		while (!projectorWidget.IsUpdated()) QApplication::processEvents();

		// Update progressbar.
		currentPattern++;
		progress_bar->setValue(currentPattern);

		// wait.
		WaitTime(wait_time_spin->value());

		// Send signal.
		QString imageName = QString("%1/cam_%2.png")
			.arg(session)
			.arg(projectorWidget.GetCurrentPattern() + 1, 2, 10, QLatin1Char('0'));
		emit needStoreImageSignal(imageName);

		isStoringImage = true;
		while (isStoringImage) QApplication::processEvents();
	}

	// De-initialize the projector.
	projectorWidget.Stop();

	// Switch progress indicators to saving phase.
	progress_label->setText("Saving images...");
	progress_bar->setMinimum(0);
	progress_bar->setMaximum(0);
	QApplication::processEvents();

	// Wait to save images.
	emit endCaptureSignal();

	isSavingImage = true;
	while (isSavingImage) QApplication::processEvents();

	// Hide progress indicators and show completion message.
	progress_label->setVisible(false);
	progress_bar->setVisible(false);
	current_message_label->setText("Capture complete.");
	current_message_label->setVisible(true);

	// Reset the TreeView.
	APP->SetRootDir(APP->GetRootDir());

	// Enable all configurations.
	EnableControls();
}

/* ALIGNMENT =============================================================================== */

/// <summary>
/// Handles the alignment mode checkbox state change. Enables or disables the projector alignment mode based on the checkbox state. When active, the
/// projector draws a centered cross over the projected pattern to assist with physical alignment.
/// </summary>
/// <param name="newState">The new checkbox state (Qt::CheckState).</param>
void CaptureDialog::on_alignment_mode_check_stateChanged(int newState)
{
	const bool isActive = (newState == Qt::Checked);

	emit _on_alignment_signal(isActive);
	projectorWidget.SetDrawCross(isActive);
	projectorWidget.ClearUpdated();
	test_check->setCheckState(isActive ? Qt::Checked : Qt::Unchecked);
	test_check->setEnabled(!isActive);
}

/* QDialog FUNCTIONS ======================================================================= */

/// <summary>
/// Updates the output directory text field when the root directory changes.
/// </summary>
/// <param name="dirname">New root directory path.</param>
void CaptureDialog::_on_root_dir_changed(const QString& dirname) const
{
	output_dir_line->setText(dirname);
}

/// <summary>
/// Updates the application root directory when the output path is manually edited.
/// </summary>
/// <param name="text">New directory path entered by the user.</param>
void CaptureDialog::on_output_dir_line_textEdited(const QString& text)
{
	APP->SetRootDir(text);
}

/// <summary>
/// Opens a dialog to browse and change the output directory.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
void CaptureDialog::on_output_dir_button_clicked(bool checked)
{
	APP->ChangeRootDir(this);
}

/// <summary>
/// Handles the close/cancel button click, accepting the dialog only when in close mode.
/// </summary>
/// <param name="checked">Indicates whether the button is in a checked state.</param>
void CaptureDialog::on_close_cancel_button_clicked(bool checked)
{
	accept();
}

/* UTILITIES =============================================================================== */

/// <summary>
/// Blocks execution for a specified duration while keeping the UI responsive.
/// </summary>
/// <param name="milliseconds">Duration to wait in milliseconds.</param>
void CaptureDialog::WaitTime(const int milliseconds)
{
	QElapsedTimer timer;
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
/// Must be called symmetrically with DisableControls() once the pipeline completes
/// or is aborted to leave the dialog in a consistent, interactive state.
/// </remarks>
void CaptureDialog::EnableControls() const
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
/// Must be called at the start of every capture and paired with EnableControls()
/// upon completion to restore the dialog to its interactive state.
/// </remarks>
void CaptureDialog::DisableControls() const
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
} // namespace smcp
