#pragma once

#include <Spinnaker.h>

#include "CameraSettings.h"

namespace SMCP
{
	class CameraUtilities
	{
	public:
		static void disableCameraAutoSettings(const Spinnaker::CameraPtr& cameraPtr);
		static void restoreCameraAutoSettings(const Spinnaker::CameraPtr& cameraPtr);
		static bool getCameraInfo(const Spinnaker::CameraPtr& cameraPtr, std::string& modelName, std::string& serialNumber);
		static CameraSettings getCameraSettings(const Spinnaker::CameraPtr& cameraPtr);
	};
}