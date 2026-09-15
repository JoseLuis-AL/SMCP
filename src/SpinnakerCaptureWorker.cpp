#include "Application.hpp"
#include "Config.h"
#include <ImageStatistics.h>
#include "SpinnakerCaptureWorker.h"

#include <opencv2/highgui/highgui.hpp>

#include "ImageUtil.hpp"

using namespace Spinnaker;

SpinnakerCaptureWorker::SpinnakerCaptureWorker(CameraPtr camera_ptr, QObject* parent) :
	_cameraPtr(std::move(camera_ptr))
{
	_imageProcessor.SetColorProcessing(SPINNAKER_COLOR_PROCESSING_ALGORITHM_NONE);
}

SpinnakerCaptureWorker::~SpinnakerCaptureWorker()
{
	if (!_stop.load())
	{
		Stop();
	}
}

void SpinnakerCaptureWorker::Start()
{
	std::cout << "-> Start Spinnaker acquisition." << '\n';

	// Init the spinnaker camera.
	_cameraPtr->Init();

	// Set the pixel format for more quality.
	//_cameraPtr->PixelFormat.SetValue(PixelFormat_Mono8);

	// Configure the FLIR camera configuration
	DisableCameraAutoConfiguration();

	// Set the camera values.
	OnNewCameraGain(APP->config.value(
		Config::Capture::CAMERA_GAIN, Config::Capture::CAMERA_GAIN_DEFAULT_VALUE).toDouble());
	OnNewCameraBlackLevel(APP->config.value(
		Config::Capture::CAMERA_BLACK_LEVEL, Config::Capture::CAMERA_BLACK_LEVEL_DEFAULT_VALUE).toDouble());
	OnNewCameraExposureTime(APP->config.value(
		Config::Capture::CAMERA_EXPOSURE_TIME, Config::Capture::CAMERA_EXPOSURE_TIME_DEFAULT_VALUE).toDouble());
	OnNewCameraGamma(APP->config.value(
		Config::Capture::CAMERA_GAMMA, Config::Capture::CAMERA_GAMMA_DEFAULT_VALUE).toDouble());

	// Get the camera configuration.
	emit NewCameraConfigValuesSignal(GetCameraConfigValues());

	// Begin image acquisition.
	_cameraPtr->BeginAcquisition();

	AcquireLoop();

	// End image acquisition.
	_cameraPtr->EndAcquisition();

	// Restore the FLIR camera configuration.
	RestoreCameraAutoConfiguration();
	_cameraPtr->DeInit();

	// Emit finish signal.
	emit FinishedSignal();
}

void SpinnakerCaptureWorker::Stop()
{
	_stop.store(true);
}

void SpinnakerCaptureWorker::AcquireLoop()
{
	// Camera dimensions.
	const int w = static_cast<int>(_cameraPtr->Width.GetValue());
	const int h = static_cast<int>(_cameraPtr->Height.GetValue());

	// Preallocate mat.
	cv::Mat frame(h, w, CV_8UC3);

	// Image gray statistics.
	ImageStatistics stats;
	stats.EnableGreyOnly();
	unsigned int minPx = 0;
	unsigned int maxPx = 0;
	float meanPx = 0.f;

	// Precompute cross drawing parameters.
	const cv::Point center(w / 2, h / 2);
	const cv::Point hStart(0, center.y);
	const cv::Point hEnd(w - 1, center.y);
	const cv::Point vStart(center.x, 0);
	const cv::Point vEnd(center.x, h - 1);
	const cv::Scalar crossColor{ 0x44, 0x3B, 0xE4 };
	const int crossThickness = 10;

	// Loop.
	while (!_stop.load(std::memory_order_relaxed))
	{
		// Grab the image.
		ImagePtr imagePtr = _cameraPtr->GetNextImage(100); // 100ms timeout.
		if (imagePtr->IsIncomplete())
		{
			imagePtr->Release();
			continue;
		}

		// Calculate grey stats.
		imagePtr->CalculateStatistics(stats);
		stats.GetStatistics(SPINNAKER_STATISTICS_CHANNEL_GREY,
			/*pRangeMin*/ nullptr,
			/*pRangeMax*/ nullptr,
			/*pPixelValueMin*/ &minPx,
			/*pPixelValueMax*/ &maxPx,
			/*pNumPixelValues*/ nullptr,
			/*pMeanPixelValues*/ &meanPx,
			/*ppHistogram*/ nullptr);
		emit NewGrayStatsSignal(meanPx, minPx, maxPx);

		// Convert to Mat.
		const ImagePtr imageConv = _imageProcessor.Convert(imagePtr, PixelFormat_BGR8);
		auto mat = cv::Mat(
			static_cast<int>(imageConv->GetHeight()),
			static_cast<int>(imageConv->GetWidth()),
			CV_8UC3,
			imageConv->GetData(),
			imageConv->GetStride());

		// Draw cross on camera center.
		if (_isAlignModeActive && !_needToSaveImage)
		{
			cv::line(mat, hStart, hEnd, crossColor, crossThickness);
			cv::line(mat, vStart, vEnd, crossColor, crossThickness);
		}

		// Send the new image to GUI thread.
		emit NewImageReadySignal(ImageUtil::cvMatToQPixmap(mat));

		// Save the image.
		if (_needToSaveImage)
		{
			// Save the image.
			cv::imwrite(_imageName, mat);

			_needToSaveImage = false;
			emit ImageSavedSignal();
		}

		// Release the mat.
		mat.release();

		// Release memory.
		imagePtr->Release();

		// Process some events.
		QApplication::processEvents();
	}
}

void SpinnakerCaptureWorker::OnNeedToSaveImage(QString session, int projectorPattern)
{
	_needToSaveImage = true;
	_imageName = QString("%1/cam_%2.png").arg(session).arg(projectorPattern + 1, 2, 10, QLatin1Char('0')).toStdString();
}

void SpinnakerCaptureWorker::OnNewCameraBlackLevel(double newValue)
{
	// Check camera.
	if (!_cameraPtr) return;

	// Check camera setting.
	if (IsAvailable(_cameraPtr->BlackLevel) && IsWritable(_cameraPtr->BlackLevel))
	{
		// Check the values.
		const double min = _cameraPtr->BlackLevel.GetMin();
		const double max = _cameraPtr->BlackLevel.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_cameraPtr->BlackLevel.SetValue(newValue);

		// Save configuration values.
		_cameraConfigValues.BlackLevelValue = newValue;
		_cameraConfigValues.BlackLevelMinValue = min;
		_cameraConfigValues.BlackLevelMaxValue = max;
	}
}

void SpinnakerCaptureWorker::OnNewCameraGain(double newValue)
{
	// Check camera.
	if (!_cameraPtr) return;

	// Check camera setting.
	if (IsAvailable(_cameraPtr->Gain) && IsWritable(_cameraPtr->Gain))
	{
		// Check the values.
		const double min = _cameraPtr->Gain.GetMin();
		const double max = _cameraPtr->Gain.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_cameraPtr->Gain.SetValue(newValue);

		// Set config values.
		_cameraConfigValues.GainValue = newValue;
		_cameraConfigValues.GainMinValue = min;
		_cameraConfigValues.GainMaxValue = max;
	}
}

void SpinnakerCaptureWorker::OnNewCameraExposureTime(double newValue)
{
	// Check camera.
	if (!_cameraPtr) return;

	// Check camera setting.
	if (IsAvailable(_cameraPtr->ExposureTime) && IsWritable(_cameraPtr->ExposureTime))
	{
		// Check the values.
		const double min = _cameraPtr->ExposureTime.GetMin();
		const double max = _cameraPtr->ExposureTime.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_cameraPtr->ExposureTime.SetValue(newValue);

		// Save config values.
		_cameraConfigValues.ExposureTimeValue = newValue;
		_cameraConfigValues.ExposureTimeMinValue = min;
		_cameraConfigValues.ExposureTimeMaxValue = max;
	}
}

void SpinnakerCaptureWorker::OnNewCameraGamma(double newValue)
{
	// Check camera.
	if (!_cameraPtr) return;

	// Check camera setting.
	if (IsAvailable(_cameraPtr->Gamma) && IsWritable(_cameraPtr->Gamma))
	{
		// Check the values.
		const double min = _cameraPtr->Gamma.GetMin();
		const double max = _cameraPtr->Gamma.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_cameraPtr->Gamma.SetValue(newValue);

		// Set config values.
		_cameraConfigValues.GammaValue = newValue;
		_cameraConfigValues.GammaMinValue = min;
		_cameraConfigValues.GammaMaxValue = max;
	}
}

void SpinnakerCaptureWorker::OnSetAlignMode(bool isAlignModeActive)
{
	_isAlignModeActive = isAlignModeActive;
}

SpinnakerCameraConfig SpinnakerCaptureWorker::GetCameraConfigValues() const
{
	SpinnakerCameraConfig currentCameraConfig;

	if (!_cameraPtr) return currentCameraConfig;

	// Black level.
	if (IsAvailable(_cameraPtr->BlackLevel) && IsWritable(_cameraPtr->BlackLevel))
	{
		currentCameraConfig.BlackLevelValue = _cameraPtr->BlackLevel.GetValue();
		currentCameraConfig.BlackLevelMaxValue = _cameraPtr->BlackLevel.GetMax();
		currentCameraConfig.BlackLevelMinValue = _cameraPtr->BlackLevel.GetMin();
	}

	// Gain.
	if (IsAvailable(_cameraPtr->Gain) && IsWritable(_cameraPtr->Gain))
	{
		currentCameraConfig.GainValue = _cameraPtr->Gain.GetValue();
		currentCameraConfig.GainMaxValue = _cameraPtr->Gain.GetMax();
		currentCameraConfig.GainMinValue = _cameraPtr->Gain.GetMin();
	}

	// ExposureTime.
	if (IsAvailable(_cameraPtr->ExposureTime) && IsWritable(_cameraPtr->ExposureTime))
	{
		currentCameraConfig.ExposureTimeValue = _cameraPtr->ExposureTime.GetValue();
		currentCameraConfig.ExposureTimeMaxValue = _cameraPtr->ExposureTime.GetMax();
		currentCameraConfig.ExposureTimeMinValue = _cameraPtr->ExposureTime.GetMin();
	}

	// Gamma.
	if (IsAvailable(_cameraPtr->Gamma) && IsWritable(_cameraPtr->Gamma))
	{
		currentCameraConfig.GammaValue = _cameraPtr->Gamma.GetValue();
		currentCameraConfig.GammaMaxValue = _cameraPtr->Gamma.GetMax();
		currentCameraConfig.GammaMinValue = _cameraPtr->Gamma.GetMin();
	}

	return currentCameraConfig;
}

void SpinnakerCaptureWorker::DisableCameraAutoConfiguration() const
{
	// GainAuto.
	if (IsAvailable(_cameraPtr->GainAuto) && IsWritable(_cameraPtr->GainAuto))
	{
		_cameraPtr->GainAuto.SetValue(GainAuto_Off);
	}
	else
	{
		std::cout << "---> [Camera configuration error]: GainAuto no available.\n";
	}

	// ExposureAuto.
	if (IsAvailable(_cameraPtr->ExposureAuto) && IsWritable(_cameraPtr->ExposureAuto))
	{
		_cameraPtr->ExposureAuto.SetValue(ExposureAuto_Off);
	}
	else
	{
		std::cout << "---> [Camera configuration error]: ExposureAuto no available.\n";
	}

	// BalanceWhiteAuto.
	if (IsAvailable(_cameraPtr->BalanceWhiteAuto) && IsWritable(_cameraPtr->BalanceWhiteAuto))
	{
		_cameraPtr->BalanceWhiteAuto.SetValue(BalanceWhiteAuto_Off);
	}
	else
	{
		std::cout << "---> [Camera configuration error]: BalanceWhiteAuto no available.\n";
	}
}

void SpinnakerCaptureWorker::RestoreCameraAutoConfiguration() const
{
	// GainAuto.
	if (IsAvailable(_cameraPtr->GainAuto) && IsWritable(_cameraPtr->GainAuto))
	{
		_cameraPtr->GainAuto.SetValue(GainAuto_Continuous);
	}
	else
	{
		std::cout << "---> [Camera configuration error]: GainAuto no available.\n";
	}

	// ExposureAuto.
	if (IsAvailable(_cameraPtr->ExposureAuto) && IsWritable(_cameraPtr->ExposureAuto))
	{
		_cameraPtr->ExposureAuto.SetValue(ExposureAuto_Continuous);
	}
	else
	{
		std::cout << "---> [Camera configuration error]: ExposureAuto no available.\n";
	}

	// BalanceWhiteAuto.
	if (IsAvailable(_cameraPtr->BalanceWhiteAuto) && IsWritable(_cameraPtr->BalanceWhiteAuto))
	{
		_cameraPtr->BalanceWhiteAuto.SetValue(BalanceWhiteAuto_Continuous);
	}
	else
	{
		std::cout << "---> [Camera configuration error]: BalanceWhiteAuto no available.\n";
	}
}