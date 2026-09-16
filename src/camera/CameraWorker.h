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

#include <array>
#include <CameraPtr.h>
#include <ImageProcessor.h>
#include <QMutex>
#include <QObject>
#include <opencv2/core/core.hpp>

#include "camera/CameraSettings.h"

using cvMatArray3 = std::array<cv::Mat, 3>;
using cvMatNamedVector = std::vector<std::pair<QString, cvMatArray3>>;

namespace smcp
{
class CameraWorker final : public QObject
{
	Q_OBJECT

public:
	explicit CameraWorker(Spinnaker::CameraPtr camera, const CameraSettings& startSettings,
		QObject* parent = nullptr);
	~CameraWorker() override;

public slots:
	// Initialization.
	void start();
	void stop();
	void acquireLoop();

	// Camera settings.
	void onNewCameraBlackLevel(double newValue) const;
	void onNewCameraExposureTime(double newValue) const;
	void onNewCameraGain(double newValue) const;
	void onNewCameraGamma(double newValue) const;

	// Alignment.
	void onAlignmentMode(bool isActive);

	// Capture.
	void onNeedToStoreImage(const QString& fileName);
	void onStartCapture(int nPatterns);
	void onEndCapture();

signals:
	// Capture.
	void newFrameReadySignal(const QPixmap& pixmap, const QString& grayStats);
	void imageStoredSignal();
	void imageSaved(int total, int current);
	void allImagesSavedSignal();

	// Camera settings.
	void newCameraSettingsSignal(const CameraSettings& cameraSettings);

	// Worker signal.
	void finishedSignal();

private:
	// Settings.
	CameraSettings _settings;

	// Alignment.
	bool _showCross{ false };

	// Capture.
	QString _fileName;
	Spinnaker::CameraPtr _camera;
	bool _needToStoreImage{ false };
	cvMatArray3 _imageArray;
	cvMatNamedVector _imageBuffer;
	int _nImageStored{ 0 };

	// Worker.
	std::atomic_bool _stopFlag;

	// Thread synchronization.
	mutable QMutex _captureMutex;

	// Image processing.
	Spinnaker::ImageProcessor _imageProcessor;
};
}
