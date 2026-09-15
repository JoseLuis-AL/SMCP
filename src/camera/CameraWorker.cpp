#include "camera/CameraWorker.h"

#include <opencv2/opencv.hpp>

#include <QDebug>
#include <QMutexLocker>

#include "app/Application.h"
#include "camera/CameraUtilities.h"
#include "common/CvMatConvert.h"

namespace smcp
{

using namespace Spinnaker;
using namespace Spinnaker::GenApi;

CameraWorker::CameraWorker(CameraPtr camera, const CameraSettings& startSettings, QObject* parent) : QObject(parent),
_settings(startSettings),
_camera(std::move(camera))
{
	// Init the image processor.
	_imageProcessor.SetColorProcessing(SPINNAKER_COLOR_PROCESSING_ALGORITHM_NONE);
}

CameraWorker::~CameraWorker()
{
	if (!_stopFlag.load(std::memory_order_relaxed)) stop();
}

void CameraWorker::start()
{
	try
	{
		// 1. Initialize the spinnaker camera.
		_camera->Init();

		// 2. Disable all camera auto settings.
		CameraUtilities::DisableCameraAutoSettings(_camera);

		// 3. Set the camera saved values.
		onNewCameraBlackLevel(_settings.BlackLevel);
		onNewCameraExposureTime(_settings.ExposureTime);
		onNewCameraGain(_settings.Gain);
		onNewCameraGamma(_settings.Gamma);

		// 4. Get the complete camera settings.
		_settings = CameraUtilities::GetCameraSettings(_camera);
		emit newCameraSettingsSignal(_settings);

		// Debug values.
		qInfo() << "[CameraWorker::start] --> Camera start with settings:" << "\n"
			<< "Black Level:" << _settings.BlackLevel << '\n'
			<< "Exposure Time:" << _settings.ExposureTime << '\n'
			<< "Gain:" << _settings.Gain << '\n'
			<< "Gamma:" << _settings.Gamma;

		// Update UI.
		Application::processEvents();

		// 5. Begin camera acquisition.
		_camera->BeginAcquisition();

		// 6. Acquire loop.
		acquireLoop();

		// 7. Stop the image acquisition.
		_camera->EndAcquisition();

		// 8. Restore camera auto settings.
		CameraUtilities::RestoreCameraAutoSettings(_camera);

		// 9. De-initialize the spinnaker camera.
		_camera->DeInit();
	}
	catch (Spinnaker::Exception& e)
	{
		qCritical() << "[CameraWorker::start] --> Spinnaker exception:" << e.GetErrorMessage();
	}

	// 10. Emit finished signal to kill the worker (always, even on error).
	emit finishedSignal();
}

void CameraWorker::stop()
{
	_stopFlag.store(true);
}

void CameraWorker::acquireLoop()
{
	// Camera dimensions.
	const int w = static_cast<int>(_camera->Width.GetValue());
	const int h = static_cast<int>(_camera->Height.GetValue());

	// Image gray stats.
	ImageStatistics statistics;
	statistics.EnableGreyOnly();
	unsigned int minPx = 0;
	unsigned int maxPx = 0;
	float meanPx = 0.0f;

	// Precompute cross drawing parameters.
	const cv::Point center(w / 2, h / 2);
	const cv::Point hStart(0, center.y);
	const cv::Point hEnd(w - 1, center.y);
	const cv::Point vStart(center.x, 0);
	const cv::Point vEnd(center.x, h - 1);
	const cv::Scalar crossColor{ 0x44, 0x3B, 0xE4 };
	const int crossThickness = 10;

	// Loop.
	while (!_stopFlag.load(std::memory_order_relaxed))
	{
		// Grab the image (Bug 5: timeout to prevent indefinite blocking).
		ImagePtr rawImage;
		try
		{
			rawImage = _camera->GetNextImage(1000);
		}
		catch (Spinnaker::Exception&)
		{
			continue;
		}

		// Bug 1: Skip incomplete frames with continue.
		if (rawImage->IsIncomplete() || rawImage->GetImageStatus() != SPINNAKER_IMAGE_STATUS_NO_ERROR)
		{
			rawImage->Release();
			qWarning() << "[CameraWorker::acquireLoop] --> Captured image is incomplete.";
			continue;
		}

		// Calculate gray stats.
		rawImage->CalculateStatistics(statistics);
		statistics.GetStatistics(SPINNAKER_STATISTICS_CHANNEL_GREY,
			/*pRangeMin       */ nullptr,
			/*pRangeMax       */ nullptr,
			/*pPixelValueMin  */ &minPx,
			/*pPixelValueMax  */ &maxPx,
			/*pNumPixelValues */ nullptr,
			/*pMeanPixelValues*/ &meanPx,
			/*ppHistogram*/ nullptr);
		QString grayStats = QString("Mean: %1 | Min: %2 | Max: %3").arg(meanPx).arg(minPx).arg(maxPx);

		// Convert to Mat.
		const ImagePtr imageConv = _imageProcessor.Convert(rawImage, PixelFormat_BGR8);
		auto mat = cv::Mat(
			static_cast<int>(imageConv->GetHeight()),
			static_cast<int>(imageConv->GetWidth()),
			CV_8UC3,
			imageConv->GetData(),
			imageConv->GetStride());

		// Show cross.
		if (_showCross && !_needToStoreImage)
		{
			cv::line(mat, hStart, hEnd, crossColor, crossThickness);
			cv::line(mat, vStart, vEnd, crossColor, crossThickness);
		}

		// Send the new image to GUI thread.
		emit newFrameReadySignal(CvMatConvert::ToQPixmap(mat), grayStats);

		// Save the image (Bug 3: protect with mutex, Bug 4: reset counter).
		{
			QMutexLocker locker(&_captureMutex);
			if (_needToStoreImage)
			{
				// Check stored images.
				if (_nImageStored < static_cast<int>(_imageArray.size()))
				{
					_imageArray[_nImageStored++] = mat.clone();
				}
				// End storage images.
				else
				{
					_imageBuffer.emplace_back(_fileName, _imageArray);

					// Complete storage.
					_nImageStored = 0;
					_needToStoreImage = false;
					emit imageStoredSignal();
				}
			}
		}

		// Release the mat.
		mat.release();

		// Bug 2: Release the converted image to prevent memory leak.
		imageConv->Release();

		// Release the raw image memory.
		rawImage->Release();

		// Bug 7: Use QCoreApplication for worker threads.
		QCoreApplication::processEvents();
	}
}

void CameraWorker::onNewCameraBlackLevel(double newValue) const
{
	if (IsAvailable(_camera->BlackLevel) && IsWritable(_camera->BlackLevel))
	{
		// Check values.
		const double min = _camera->BlackLevel.GetMin();
		const double max = _camera->BlackLevel.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_camera->BlackLevel.SetValue(newValue);
	}
}

void CameraWorker::onNewCameraExposureTime(double newValue) const
{
	if (IsAvailable(_camera->ExposureTime) && IsWritable(_camera->ExposureTime))
	{
		// Check the values.
		const double min = _camera->ExposureTime.GetMin();
		const double max = _camera->ExposureTime.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_camera->ExposureTime.SetValue(newValue);
	}
}

void CameraWorker::onNewCameraGain(double newValue) const
{
	if (IsAvailable(_camera->Gain) && IsWritable(_camera->Gain))
	{
		// Check the values.
		const double min = _camera->Gain.GetMin();
		const double max = _camera->Gain.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_camera->Gain.SetValue(newValue);
	}
}

void CameraWorker::onNewCameraGamma(double newValue) const
{
	if (IsAvailable(_camera->Gamma) && IsWritable(_camera->Gamma))
	{
		// Check the values.
		const double min = _camera->Gamma.GetMin();
		const double max = _camera->Gamma.GetMax();
		newValue = newValue < min ? min : newValue;
		newValue = newValue > max ? max : newValue;

		// Set the new value.
		_camera->Gamma.SetValue(newValue);
	}
}

void CameraWorker::onAlignmentMode(const bool isActive)
{
	_showCross = isActive;
}

void CameraWorker::onNeedToStoreImage(const QString& fileName)
{
	QMutexLocker locker(&_captureMutex);

	// Image name.
	_fileName = fileName;

	// Flag to save image.
	_needToStoreImage = true;

	// Image counter.
	_nImageStored = 0;
}

void CameraWorker::onStartCapture(const int nPatterns)
{
	QMutexLocker locker(&_captureMutex);

	_imageBuffer.clear();
	_imageBuffer.reserve(nPatterns + 1);

	_needToStoreImage = false;
}

void CameraWorker::onEndCapture()
{
	const int imageCount = static_cast<int>(_imageBuffer.size());
	for (int i = 0; i < imageCount; i++)
	{
		// Get image information.
		std::string filename = _imageBuffer[i].first.toStdString(); // <-- Image filename.

		cv::Mat mat1 = _imageBuffer[i].second[0]; // <-- First image
		cv::Mat mat2 = _imageBuffer[i].second[1]; // <-- Second image
		cv::Mat mat3 = _imageBuffer[i].second[2]; // <-- Third image.

		cv::Mat f1, f2, f3;
		mat1.convertTo(f1, CV_32FC3);
		mat2.convertTo(f2, CV_32FC3);
		mat3.convertTo(f3, CV_32FC3);

		cv::Mat avgF = (f1 + f2 + f3) * (1.0f / 3.0f); // <-- Image average.

		cv::Mat image;
		avgF.convertTo(image, CV_8UC3);

		// Store images.
		if (cv::imwrite(filename, image))
		{
			emit imageSaved(imageCount, i + 1);
		}
	}

	// Images saved.
	emit allImagesSavedSignal();
}
} // namespace smcp
