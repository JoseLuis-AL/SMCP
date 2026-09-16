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

// UI.
#include "ui_CaptureDialog.h"

// OpenCV, Spinnaker
#include <SystemPtr.h>
#include <CameraList.h>

// Qt
#include <QDialog>
#include <QThread>

// Camera & Projector handler
#include "camera/CameraWorker.h"
#include "ui/widgets/ProjectorWidget.h"

namespace smcp
{
class CaptureDialog : public QDialog, public Ui::CaptureDialog
{
	Q_OBJECT

public:
	explicit CaptureDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowMaximizeButtonHint);
	~CaptureDialog() override;

public slots:
	void on_close_cancel_button_clicked(bool checked = false);
	void _on_root_dir_changed(const QString& dirname) const;
	void on_output_dir_line_textEdited(const QString& text);
	void on_output_dir_button_clicked(bool checked = false);

	// Projector.
	void on_screen_combo_currentIndexChanged(int index);
	void on_test_check_stateChanged(int state);
	void on_test_prev_button_clicked(bool checked = false);
	void on_test_next_button_clicked(bool checked = false);
	void auto_next();

	// Preview.
	void _on_new_projector_image(QPixmap image) const;
	void _on_new_camera_frame(const QPixmap& newFrame, const QString& grayStats) const;

	// Alignment mode.
	void on_alignment_mode_check_stateChanged(int newState);

	// Camera settings.
	void on_camera_combo_currentIndexChanged(int index);
	void on_camera_black_level_spin_valueChanged(double newValue);
	void on_camera_exposure_spin_valueChanged(double newValue);
	void on_camera_gain_spin_valueChanged(double newValue);
	void on_camera_gamma_spin_valueChanged(double newValue);
	void _on_new_camera_settings(const CameraSettings& settings) const;

	void _on_image_stored();
	void _on_image_saved(int totalImagesToSave, int currentImageSaved) const;
	void _on_all_images_saved();

	// Capture.
	void on_capture_button_clicked(bool checked = false);

	// Camera signals.
signals:
	void onNewCameraBlackLevelSignal(double newValue);
	void onNewCameraExposureTimeSignal(double newValue);
	void onNewCameraGainSignal(double newValue);
	void onNewCameraGammaSignal(double newValue);

	// Alignment signals.
	void _on_alignment_signal(bool isAlignModeActive);

	// Image saving signals.
	void startCaptureSignal(int nPattern);
	void endCaptureSignal();
	void needStoreImageSignal(const QString& filename);

private:

	/* METHODS ================================================================================= */
	// Init.
	void InitSpinnaker();
	void InitSignals();
	void InitControls() const;

	// Camera.
	void StartCamera();
	void StopCamera();
	void SetupCameraThread();
	void UpdateCameraCombo();

	// Projector.
	int UpdateScreenCombo() const;

	// Utilities.
	static void WaitTime(int milliseconds);
	void EnableControls() const;
	void DisableControls() const;

	/* ATTRIBUTES ============================================================================== */
	// Projector.
	ProjectorWidget projectorWidget;

	// Camera [Spinnaker].
	Spinnaker::SystemPtr spinnakerSystemPtr;
	Spinnaker::CameraPtr cameraPtr;
	Spinnaker::CameraList cameraList;

	QThread cameraThread;
	CameraWorker* cameraWorker;
	QString cameraSerialNumber;

	int cameraIdx{ -1 };
	int nCameras{ 0 };

	// Image processing.
	bool isStoringImage{ false };
	bool isSavingImage{ false };

	// Working directory.
	QString session;
};
}
