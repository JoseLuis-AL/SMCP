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

#include "CaptureDialog.hpp"

#include <QDesktopWidget>
#include <QMessageBox>
#include <QTimer>
#include <QTime>

#include <iostream>
#include <strmif.h>
#include <opencv2/imgproc/imgproc.hpp>

#include "Config.h"
#include "Application.hpp"
#include "im_util.hpp"

CaptureDialog::CaptureDialog(QWidget* parent, Qt::WindowFlags flags) :
	QDialog(parent, flags),
	_projector(),
	_videoInput(this),
	_capture(false),
	_session(),
	_waitTime(0),
	_total(0),
	_cancel(false)
{
	// UI configuration.
	setupUi(this);
	camera_resolution_label->clear();

	// Connect signals.
	connect(APP, SIGNAL(root_dir_changed(const QString&)), this, SLOT(_on_root_dir_changed(const QString&)));

	current_message_label->setVisible(false);
	progress_label->setVisible(false);
	progress_bar->setVisible(false);

	// Spinnaker Init.
	_spinSystemPtr = Spinnaker::System::GetInstance();

	update_screen_combo();
	update_camera_combo();

	projector_patterns_spin->setValue(APP->config.value("capture/pattern_count", 10).toInt());
	camera_exposure_spin->setMaximum(9999);
	camera_exposure_spin->setValue(APP->config.value("capture/exposure_time", 500).toInt());
	output_dir_line->setText(APP->get_root_dir());

	continuous_spin->setValue(APP->config.value("capture/continuous", 100).toInt());
	continuous_spin->setMaximum(9999);
	continuous_spin->setMinimum(10);

	// Spinnaker camera configuration.
	camera_black_level_spin->setValue(
		APP->config.value(Config::Capture::CAMERA_BLACK_LEVEL, Config::Capture::CAMERA_BLACK_LEVEL_DEFAULT_VALUE).toDouble());
	camera_gain_spin->setValue(
		APP->config.value(Config::Capture::CAMERA_GAIN, Config::Capture::CAMERA_GAIN_DEFAULT_VALUE).toDouble());
	camera_black_level_spin->setEnabled(false);
	camera_gain_spin->setEnabled(false);

	// Other configuration.
	size_t rotation = APP->config.value("capture/rotation", 0).toUInt();
	if (rotation == 0) { rot_000_radio->setChecked(true); }
	if (rotation == 90) { rot_090_radio->setChecked(true); }
	if (rotation == 280) { rot_180_radio->setChecked(true); }
	if (rotation == 270) { rot_270_radio->setChecked(true); }
	capture_sets_spin->setValue(
		APP->config.value(Config::Capture::CAPTURE_SETS, Config::Capture::CAPTURE_SETS_DEFAULT_VALUE).toInt());

	// Alignment.
	alignment_mode_check->setChecked(false);

	// Projector test buttons.
	test_prev_button->setEnabled(false);
	test_next_button->setEnabled(false);

	// Add new CameraSettings to signals.
	//qRegisterMetaType<SpinnakerUtils::Camera::Configuration>("SpinnakerUtils::Camera::Configuration");

	// Update the projector view.
	_projector.set_screen(screen_combo->currentIndex());

	// Start camera preview.
	start_camera();
}

CaptureDialog::~CaptureDialog()
{
	stop_camera();

	//save user selection
	QSettings& config = APP->config;
	int projector_screen = screen_combo->currentIndex();
	if (projector_screen >= 0)
	{
		config.setValue("capture/projector_screen", projector_screen);
	}
	QString camera_name = camera_combo->currentText();
	if (!camera_name.isEmpty())
	{
		config.setValue("capture/camera_name", camera_name);
	}
	config.setValue("capture/pattern_count", projector_patterns_spin->value());
	config.setValue("capture/exposure_time", camera_exposure_spin->value());
	config.setValue("capture/continuous", continuous_spin->value());
	config.setValue(Config::Capture::CAMERA_GAIN, camera_gain_spin->value());
	config.setValue(Config::Capture::CAMERA_BLACK_LEVEL, camera_black_level_spin->value());
	config.setValue(Config::Capture::CAMERA_GAMMA, camera_gamma_spin->value());

	config.setValue(Config::Capture::CAPTURE_SETS, capture_sets_spin->value());

	int rotation = 0;
	if (rot_000_radio->isChecked()) { rotation = 0; }
	if (rot_090_radio->isChecked()) { rotation = 90; }
	if (rot_180_radio->isChecked()) { rotation = 180; }
	if (rot_270_radio->isChecked()) { rotation = 270; }
	APP->config.setValue("capture/rotation", rotation);

	// Deinit Spinnaker.
	// TODO: Check the exception triggered by the ReleaseInstance.
	try
	{
		_spinSystemPtr->ReleaseInstance();
	}
	catch (Spinnaker::Exception& e)
	{
		std::cout << "Error:" << e.GetErrorMessage();
	}
}

void CaptureDialog::set_current_message(const QString& text) const
{
	current_message_label->setText(text);
}

void CaptureDialog::set_progress_total(unsigned value)
{
	_total = value;
	progress_bar->setMaximum(_total);
}

void CaptureDialog::set_progress_value(unsigned value)
{
	progress_bar->setValue(value);
}

void CaptureDialog::reset(void)
{
	progress_bar->setMaximum(0);
	current_message_label->clear();
	close_cancel_button->setText("Cancel");
	_cancel = false;
}

void CaptureDialog::finish(void)
{
	//progress_bar->setValue(_total);
	close_cancel_button->setText("Close");
}

bool CaptureDialog::canceled(void) const
{
	return _cancel;
}

void CaptureDialog::on_close_cancel_button_clicked(bool checked)
{
	if (close_cancel_button->text() == "Close")
	{
		accept();
	}
	else if (!_cancel)
	{
		_cancel = true;
	}
}

int CaptureDialog::update_screen_combo(void)
{
	//disable signals
	screen_combo->blockSignals(true);

	//save current value
	int current = screen_combo->currentIndex();

	//update combo
	QStringList list;
	screen_combo->clear();
	QDesktopWidget* desktop = QApplication::desktop();
	int screens = desktop->screenCount();
	for (int i = 0; i < screens; i++)
	{
		const QRect rect = desktop->screenGeometry(i);
		list.append(QString("Screen %1 [%2x%3]").arg(i).arg(rect.width()).arg(rect.height()));
	}
	screen_combo->addItems(list);

	//set current value
	int saved_value = APP->config.value("capture/projector_screen", 1).toInt();
	int default_screen = (saved_value < screen_combo->count() ? saved_value : 0);
	screen_combo->setCurrentIndex((-1 < current && current < screen_combo->count() ? current : default_screen));

	//enable signals
	screen_combo->blockSignals(false);

	return screen_combo->count();
}

int CaptureDialog::update_camera_combo(void)
{
	//disable signals
	camera_combo->blockSignals(true);

	//save current value
	const QString current = camera_combo->currentText();

	// Add legacy cameras.
	camera_combo->clear();
	camera_combo->addItems(VideoInput::list_devices());

	// Get spinnaker cameras.
	_spinCameraList.Clear();
	_spinCameraList = _spinSystemPtr->GetCameras();
	_spinNumCam = _spinCameraList.GetSize();

	Spinnaker::CameraPtr p_cam = nullptr;
	for (unsigned int i = 0; i < _spinNumCam; i++)
	{
		p_cam = _spinCameraList.GetByIndex(i);
		p_cam->Init();

		// Add camera name to combo box text.
		Spinnaker::GenApi::INodeMap& node_map = p_cam->GetTLDeviceNodeMap();
		Spinnaker::GenApi::CStringPtr p_device_model_name = node_map.GetNode("DeviceModelName");
		if (Spinnaker::GenApi::IsReadable(p_device_model_name))
		{
			camera_combo->addItem(p_device_model_name->GetValue().c_str());
		}
	}
	p_cam = nullptr;

	// Set the current index and the camera type.
	int index = camera_combo->findText(current);

	// Load the index value.
	if (index < 0)
	{
		const QString saved_value = APP->config.value("capture/camera_name").toString();
		index = camera_combo->findText(saved_value);
	}
	camera_combo->setCurrentIndex((index < 0 ? 0 : index));

	//enable signals
	camera_combo->blockSignals(false);
	return camera_combo->count();
}

bool CaptureDialog::start_camera(void)
{
	// Check camera index.
	const int index = camera_combo->currentIndex();
	if (_videoInput.get_camera_index() == index && _currentCameraType == CameraType::LEGACY)
	{
		return true;
	}

	// Set the Busy cursor
	QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	QApplication::processEvents();

	// Stop current camera.
	stop_camera();

	// Reselect the current camera.
	const int num_legacy_cam = VideoInput::list_devices().size();
	if (index < num_legacy_cam)
	{
		_currentCameraType = CameraType::LEGACY;
	}
	else
	{
		_currentCameraType = CameraType::SPINNAKER;
		_currentSpinIdx = camera_combo->currentIndex() - num_legacy_cam;
		_currentSpinCameraPtr = _spinCameraList.GetByIndex(_currentSpinIdx);
	}

	// Start legacy camera.
	if (_currentCameraType == CameraType::LEGACY)
	{
		_videoInput.set_camera_index(index);
		_videoInput.start();
		_videoInput.waitForStart();

		if (!_videoInput.isRunning())
		{
			//error
			//TODO: display error --------
			camera_resolution_label->clear();
			camera_resolution_label->setContextMenuPolicy(Qt::NoContextMenu);
		}

		// Connect the display with the signal.
		connect(&_videoInput, SIGNAL(new_image(cv::Mat)), this, SLOT(_on_new_camera_image(cv::Mat)),
			Qt::DirectConnection);

		//setup resolutions context menu
		QStringList resolution_list = VideoInput::list_device_resolutions(index);
		if (!resolution_list.empty())
		{
			//clean previous actions
			QList<QAction*> actions = camera_resolution_label->actions();
			foreach(const auto action, actions)
			{
				camera_resolution_label->removeAction(action);
				delete action;
			}

			//add new actions
			foreach(auto res, resolution_list)
			{
				auto action = new QAction(res, this);
				connect(action, SIGNAL(triggered(bool)), this, SLOT(_on_resolution_change()));
				camera_resolution_label->addAction(action);
			}
			camera_resolution_label->setContextMenuPolicy(Qt::ActionsContextMenu);
		}

		// Disable camera configuration.

		// Restore regular cursor.
		QApplication::restoreOverrideCursor();
		QApplication::processEvents();

		return true;
	}

	// Start Spinnaker camera.
	if (_currentCameraType == CameraType::SPINNAKER)
	{
		StartSpinnakerCamera();

		// Restore regular cursor.
		QApplication::restoreOverrideCursor();
		QApplication::processEvents();

		return true;
	}

	// Restore regular cursor.
	QApplication::restoreOverrideCursor();
	QApplication::processEvents();

	return true;
}

void CaptureDialog::stop_camera(void)
{
	if (_currentCameraType == CameraType::LEGACY)
	{
		// First, disconnect the display signal.
		disconnect(&_videoInput, SIGNAL(new_image(cv::Mat)), this, SLOT(_on_new_camera_image(cv::Mat)));

		//clean up
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor)); //busy cursor
		QApplication::processEvents(); //process pending signals
		cameraPreview->clear();
		camera_resolution_label->clear();

		//Stop the thread
		if (_videoInput.isRunning())
		{
			_videoInput.stop();
			_videoInput.wait();
		}
	}

	if (_currentCameraType == CameraType::SPINNAKER)
	{
		// Stop the Spinnaker acquisition.
		StopSpinnakerCamera();
	}

	//restore regular cursor
	QApplication::restoreOverrideCursor();
	QApplication::processEvents();
}

void CaptureDialog::_on_root_dir_changed(const QString& dirname)
{
	output_dir_line->setText(dirname);
}

void CaptureDialog::_on_new_camera_image(cv::Mat image)
{
	size_t rotation = 0;
	if (rot_000_radio->isChecked()) { rotation = 0; }
	if (rot_090_radio->isChecked()) { rotation = 90; }
	if (rot_180_radio->isChecked()) { rotation = 180; }
	if (rot_270_radio->isChecked()) { rotation = 270; }
	if (rotation > 0)
	{
		image = im_util::rotate_image(image, rotation);
	}
	cameraPreview->setImage(image);
	camera_resolution_label->setText(QString("[%1x%2]").arg(image.cols).arg(image.rows));

	// Save the image if is in the capture mode.
	if (_capture)
	{
		cv::imwrite(
			QString("%1/cam_%2.png").arg(_session).arg(_projector.get_current_pattern() + 1, 2, 10, QLatin1Char('0')).
			toStdString(), image);
		_capture = false;
		_projector.clear_updated();
	}
}

void CaptureDialog::_on_resolution_change()
{
	auto action = qobject_cast<QAction*>(sender());
	if (!action) { return; }

	QStringList res = action->text().split('x');
	if (res.length() < 2) { return; }

	unsigned int cols = res.at(0).toUInt();
	unsigned int rows = res.at(1).toUInt();
	_videoInput.setImageSize(cols, rows);
}

void CaptureDialog::_on_new_projector_image(QPixmap image)
{
	projector_image->setPixmap(image);
}

void CaptureDialog::on_screen_combo_currentIndexChanged(int index)
{
	_projector.set_screen(index);
}

void CaptureDialog::on_camera_combo_currentIndexChanged(int index)
{
	camera_resolution_label->clear();
	start_camera();
}

void CaptureDialog::on_output_dir_line_textEdited(const QString& text)
{
	APP->set_root_dir(text);
}

void CaptureDialog::on_output_dir_button_clicked(bool checked)
{
	APP->change_root_dir(this);
}

void CaptureDialog::wait(int msecs)
{
	QTime timer;
	timer.start();
	while (timer.elapsed() < msecs)
	{
		QApplication::processEvents();
	}
}

void CaptureDialog::on_capture_button_clicked(bool checked)
{
	Capture();
}

void CaptureDialog::on_capture_set_button_clicked(bool checked)
{
	const unsigned int captureSets = capture_sets_spin->value();
	for (unsigned int i = 0; i < captureSets; i++)
	{
		Capture();
	}
}

void CaptureDialog::on_test_check_stateChanged(int state)
{
	//adjust the GUI
	bool checked = (state == Qt::Checked);
	test_prev_button->setEnabled(checked);
	test_next_button->setEnabled(checked);
	screen_combo->setEnabled(!checked);
	projector_patterns_spin->setEnabled(!checked);

	if (checked)
	{
		capture_button->setEnabled(false);
		capture_set_button->setEnabled(false);

		//Start preview
		//connect projector display signal
		connect(&_projector, SIGNAL(new_image(QPixmap)), this, SLOT(_on_new_projector_image(QPixmap)));

		//open projector
		_projector.set_pattern_count(projector_patterns_spin->value());
		_projector.start();
		_projector.next();
		_projector.next();

		//continuous
		if (continuous_check->isChecked())
		{
			QTimer::singleShot(continuous_spin->value(), this, SLOT(auto_next()));
		}
	}
	else
	{
		if (!alignment_mode_check->isChecked())
		{
			capture_button->setEnabled(true);
			capture_set_button->setEnabled(true);
		}

		//Stop preview
		//close projector
		_projector.stop();

		//disconnect projector display signal
		disconnect(&_projector, SIGNAL(new_image(QPixmap)), this, SLOT(_on_new_projector_image(QPixmap)));
	}
}

void CaptureDialog::on_test_prev_button_clicked(bool checked)
{
	_projector.clear_updated();
	_projector.prev();
}

void CaptureDialog::on_test_next_button_clicked(bool checked)
{
	_projector.clear_updated();
	_projector.next();
}

void CaptureDialog::auto_next()
{
	if (test_check->isChecked() && continuous_check->isChecked())
	{
		_projector.clear_updated();

		if (_projector.finished())
		{
			_projector.start();
		}

		_projector.next();

		QTimer::singleShot(continuous_spin->value(), this, SLOT(auto_next()));

		QApplication::processEvents();
	}
}

// Alignment mode.
void CaptureDialog::on_alignment_mode_check_stateChanged(int newState)
{
	if (newState == Qt::CheckState::Checked)
	{
		emit SetAlignModeSignal(true);
		_projector.set_draw_cross(true);

		_projector.clear_updated();

		test_check->setCheckState(Qt::CheckState::Checked);
	}
	else
	{
		emit SetAlignModeSignal(false);

		_projector.set_draw_cross(false);
		_projector.clear_updated();

		test_check->setCheckState(Qt::CheckState::Unchecked);
	}
}

// Camera settings.
void CaptureDialog::on_camera_black_level_spin_valueChanged(double newValue)
{
	emit NewCameraBlackLevelSignal(newValue);
}

void CaptureDialog::on_camera_gain_spin_valueChanged(double newValue)
{
	emit NewCameraGainSignal(newValue);
}

void CaptureDialog::on_camera_gamma_spin_valueChanged(double newValue)
{
	emit NewCameraGammaSignal(newValue);
}

void CaptureDialog::OnNewCameraConfigValues(SpinnakerCameraConfig newCameraConfig) const
{
	// Setup camera black level.
	camera_black_level_spin->setMaximum(newCameraConfig.BlackLevelMaxValue);
	camera_black_level_spin->setMinimum(newCameraConfig.BlackLevelMinValue);
	camera_black_level_spin->setValue(newCameraConfig.BlackLevelValue);

	// Setup gain.
	camera_gain_spin->setMaximum(newCameraConfig.GainMaxValue);
	camera_gain_spin->setMinimum(newCameraConfig.GainMinValue);
	camera_gain_spin->setValue(newCameraConfig.GainValue);

	// Setup gamma spin.
	camera_gamma_spin->setMaximum(newCameraConfig.GammaMaxValue);
	camera_gamma_spin->setMinimum(newCameraConfig.GammaMinValue);
	camera_gamma_spin->setValue(newCameraConfig.GammaValue);
}

void CaptureDialog::OnImageSaved()
{
	_capture = false;
	_projector.clear_updated();
}

// Capture.
void CaptureDialog::Capture()
{
	// Check if the legacy camera is running.
	if (_currentCameraType == CameraType::LEGACY && !_videoInput.isRunning())
	{
		QMessageBox::critical(this, "Error", "Camera is not ready");
		return;
	}

	// Configure the output directory.
	_session = APP->get_root_dir() + "/" + QDateTime::currentDateTime().toString("yyyy-MMM-dd_hh.mm.ss.zzz");
	if (const QDir session_dir; !session_dir.mkpath(_session))
	{
		QMessageBox::critical(this, "Error", "Cannot create output directory:\n" + _session);
		std::cout << "Failed to create directory: " << _session.toStdString() << '\n';
		return;
	}

	// Disable all UI options.
	projector_group->setEnabled(false);
	camera_group->setEnabled(false);
	other_group->setEnabled(false);
	alignment_group->setEnabled(false);
	capture_button->setEnabled(false);
	capture_set_button->setEnabled(false);
	close_cancel_button->setEnabled(false);
	camera_gain_spin->setEnabled(false);
	camera_black_level_spin->setEnabled(false);

	// Capture parameters.
	_capture = false;
	_waitTime = camera_exposure_spin->value();

	// =====
	// Setup projector.
	// =====
	connect(&_projector, SIGNAL(new_image(QPixmap)), this, SLOT(_on_new_projector_image(QPixmap)));

	// Start projector and save projector resolution and settings.
	_projector.set_pattern_count(projector_patterns_spin->value());
	_projector.start();

	size_t rotation = 0;
	if (rot_000_radio->isChecked()) { rotation = 0; }
	if (rot_090_radio->isChecked()) { rotation = 90; }
	if (rot_180_radio->isChecked()) { rotation = 180; }
	if (rot_270_radio->isChecked()) { rotation = 270; }
	if (!_projector.save_info(QString("%1/projector_info.txt").arg(_session), (rotation == 90 || rotation == 270)))
	{
		std::cout << "Can't save projector info.\n";
	}

	// Init the timer.
	wait(_waitTime);

	// Projector loop.
	while (!_projector.finished())
	{
		// Show the next pattern.
		_projector.next();

		// Wait for the projector,
		while (!_projector.is_updated())
		{
			QApplication::processEvents();
		}

		// Pause so the screen gets updated.
		wait(_waitTime);

		// Capture.
		_capture = true;

		// Emit signal to save the image.
		emit NeedSaveImageSignal(_session, _projector.get_current_pattern());

		// Wait for camera.
		while (_capture)
		{
			QApplication::processEvents();
		}
	}

	// Close projector.
	_projector.stop();

	// Disconnect projector display signal.
	disconnect(&_projector, SIGNAL(new_image(QPixmap)), this, SLOT(_on_new_projector_image(QPixmap)));

	// Re-read images.
	APP->set_root_dir(APP->get_root_dir());

	// Enable UI options.
	projector_group->setEnabled(true);
	camera_group->setEnabled(true);
	other_group->setEnabled(true);
	alignment_group->setEnabled(true);
	capture_button->setEnabled(true);
	capture_set_button->setEnabled(true);
	close_cancel_button->setEnabled(true);
	camera_gain_spin->setEnabled(true);
	camera_black_level_spin->setEnabled(true);
}

// Spinnaker acquisition.
void CaptureDialog::OnNewImageReady(QPixmap pixmap) const
{
	// Set the new image.
	cameraPreview->setImage(pixmap);
}

void CaptureDialog::OnNewGrayStats(float mean, unsigned int min, unsigned int max) const
{
	const QString grayscale_text = QStringLiteral("Mean: %1   Min: %2   Max: %3 ")
		.arg(mean, 0, 'f', 1)
		.arg(min)
		.arg(max);
	image_grayscale_label->setText(grayscale_text);
}

bool CaptureDialog::StartSpinnakerCamera()
{
	// Enable camera configuration.
	camera_gain_spin->setEnabled(true);
	camera_black_level_spin->setEnabled(true);

	// Setup thread.
	_spinWorker = new SpinnakerCaptureWorker(_currentSpinCameraPtr);
	_spinWorker->moveToThread(&_spinThread);

	// Initialize the worker when the thread started.
	connect(&_spinThread,
		&QThread::started,
		_spinWorker,
		&SpinnakerCaptureWorker::Start);

	// Delete the worker when no longer is used.
	connect(&_spinThread,
		&QThread::finished,
		_spinWorker,
		&SpinnakerCaptureWorker::deleteLater);

	// Stop the thread when the worker finish.
	connect(_spinWorker,
		&SpinnakerCaptureWorker::FinishedSignal,
		&_spinThread,
		&QThread::quit);

	// Get camera configuration.
	connect(_spinWorker,
		&SpinnakerCaptureWorker::NewCameraConfigValuesSignal,
		this,
		&CaptureDialog::OnNewCameraConfigValues,
		Qt::QueuedConnection);

	// Update image preview.
	connect(_spinWorker,
		&SpinnakerCaptureWorker::NewImageReadySignal,
		this,
		&CaptureDialog::OnNewImageReady);

	// Update the image grey stats.
	connect(_spinWorker,
		&SpinnakerCaptureWorker::NewGrayStatsSignal,
		this,
		&CaptureDialog::OnNewGrayStats);

	// Need to save image.
	connect(this,
		&CaptureDialog::NeedSaveImageSignal,
		_spinWorker,
		&SpinnakerCaptureWorker::OnNeedToSaveImage);

	// Image saved.
	connect(_spinWorker,
		&SpinnakerCaptureWorker::ImageSavedSignal,
		this,
		&CaptureDialog::OnImageSaved);

	// Set alignment mode.
	connect(this,
		&CaptureDialog::SetAlignModeSignal,
		_spinWorker,
		&SpinnakerCaptureWorker::OnSetAlignMode);

	// Camera black level configuration.
	connect(this,
		&CaptureDialog::NewCameraBlackLevelSignal,
		_spinWorker,
		&SpinnakerCaptureWorker::OnNewCameraBlackLevel,
		Qt::QueuedConnection);

	// Camera gain configuration.
	connect(this,
		&CaptureDialog::NewCameraGainSignal,
		_spinWorker,
		&SpinnakerCaptureWorker::OnNewCameraGain,
		Qt::QueuedConnection);

	// Camera gamma configuration.
	connect(this,
		&CaptureDialog::NewCameraGammaSignal,
		_spinWorker,
		&SpinnakerCaptureWorker::OnNewCameraGamma,
		Qt::QueuedConnection);

	// Start spinnaker acquisition thread.
	_spinThread.start();

	return true;
}

void CaptureDialog::StopSpinnakerCamera()
{
	if (_spinWorker)
	{
		_spinWorker->Stop();
	}

	_spinThread.quit();
	_spinThread.wait();
	_spinWorker = nullptr;

	// Disable camera configuration.
	camera_gain_spin->setEnabled(false);
	camera_black_level_spin->setEnabled(false);
}