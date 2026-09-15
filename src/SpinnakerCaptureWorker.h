#ifndef __SPINNAKER_CAPTURE_WORKER_HPP__
#define __SPINNAKER_CAPTURE_WORKER_HPP__

#include <CameraPtr.h>
#include <ImageProcessor.h>
#include <QObject>
#include <opencv2/core/core.hpp>

#include "SpinnakerCameraConfig.h"

class SpinnakerCaptureWorker final : public QObject
{
	Q_OBJECT

public:
	SpinnakerCaptureWorker(Spinnaker::CameraPtr camera_ptr, QObject* parent = nullptr);
	~SpinnakerCaptureWorker() override;

public slots:
	void Start();
	void Stop();
	void AcquireLoop();

	void OnNeedToSaveImage(QString session, int projectorPattern);
	void OnNewCameraGain(double newValue);
	void OnNewCameraBlackLevel(double newValue);
	void OnNewCameraExposureTime(double newValue);
	void OnNewCameraGamma(double newValue);

	void OnSetAlignMode(bool isAlignModeActive);

signals:
	void ImageSavedSignal();
	void NewCameraConfigValuesSignal(SpinnakerCameraConfig);
	void NewImageReadySignal(const QPixmap& pixmap);
	void NewGrayStatsSignal(float mean, unsigned int min, unsigned int max);
	void FinishedSignal();

private:
	void DisableCameraAutoConfiguration() const;
	void RestoreCameraAutoConfiguration() const;

	SpinnakerCameraConfig GetCameraConfigValues() const;

	bool _needToSaveImage{ false };
	std::string _imageName;

	bool _isAlignModeActive{ false };

	std::atomic_bool _stop{ false };
	Spinnaker::CameraPtr _cameraPtr;

	Spinnaker::ImageProcessor _imageProcessor;

	SpinnakerCameraConfig _cameraConfigValues;
};

#endif
