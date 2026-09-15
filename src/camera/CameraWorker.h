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
