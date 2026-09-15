#pragma once

#include <Spinnaker.h>

#include "camera/CameraSettings.h"

namespace smcp
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