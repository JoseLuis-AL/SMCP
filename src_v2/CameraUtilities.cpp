#include "CameraUtilities.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;

bool SMCP::CameraUtilities::getCameraInfo(const CameraPtr& cameraPtr, std::string& modelName, std::string& serialNumber)
{
	// Check camera.
	if (cameraPtr == nullptr) return false;

	// Get node data.
	const INodeMap& nodeMap = cameraPtr->GetTLDeviceNodeMap();
	const CStringPtr modelNamePtr = nodeMap.GetNode("DeviceModelName");
	const CStringPtr serialNumberPtr = nodeMap.GetNode("DeviceSerialNumber");
	if (IsReadable(modelNamePtr) && IsReadable(serialNumberPtr))
	{
		modelName = modelNamePtr->GetValue();
		serialNumber = serialNumberPtr->GetValue();
		return true;
	}
	return false;
}

void SMCP::CameraUtilities::disableCameraAutoSettings(const Spinnaker::CameraPtr& cameraPtr)
{
	if (IsAvailable(cameraPtr->BlackLevelAuto) && IsWritable(cameraPtr->BlackLevelAuto))
	{
		cameraPtr->BlackLevelAuto.SetValue(BlackLevelAuto_Off);
	}
	else
	{
		std::cout << "---> [Camera Settings]: BlackLevelAuto not available\n";
	}

	if (IsAvailable(cameraPtr->ExposureAuto) && IsWritable(cameraPtr->ExposureAuto))
	{
		cameraPtr->ExposureAuto.SetValue(ExposureAuto_Off);
	}
	else
	{
		std::cout << "---> [Camera Settings]: ExposureAuto not available\n";
	}

	if (IsAvailable(cameraPtr->GainAuto) && IsWritable(cameraPtr->GainAuto))
	{
		cameraPtr->GainAuto.SetValue(GainAuto_Off);
	}
	else
	{
		std::cout << "---> [Camera Settings]: GainAuto not available\n";
	}
}

void SMCP::CameraUtilities::restoreCameraAutoSettings(const Spinnaker::CameraPtr& cameraPtr)
{
	if (IsAvailable(cameraPtr->BlackLevelAuto) && IsWritable(cameraPtr->BlackLevelAuto))
	{
		cameraPtr->BlackLevelAuto.SetValue(BlackLevelAuto_Continuous);
	}
	else
	{
		std::cout << "---> [Camera Settings]: BlackLevelAuto not available\n";
	}

	if (IsAvailable(cameraPtr->ExposureAuto) && IsWritable(cameraPtr->ExposureAuto))
	{
		cameraPtr->ExposureAuto.SetValue(ExposureAuto_Continuous);
	}
	else
	{
		std::cout << "---> [Camera Settings]: ExposureAuto not available\n";
	}

	if (IsAvailable(cameraPtr->GainAuto) && IsWritable(cameraPtr->GainAuto))
	{
		cameraPtr->GainAuto.SetValue(GainAuto_Continuous);
	}
	else
	{
		std::cout << "---> [Camera Settings]: GainAuto not available\n";
	}
}

SMCP::CameraSettings SMCP::CameraUtilities::getCameraSettings(const CameraPtr& cameraPtr)
{
	CameraSettings settings;
	if (!cameraPtr) return settings;

	if (IsAvailable(cameraPtr->BlackLevel) && IsWritable(cameraPtr->BlackLevel))
	{
		settings.BlackLevel = cameraPtr->BlackLevel.GetValue();
		settings.BlackLevelMax = cameraPtr->BlackLevel.GetMax();
		settings.BlackLevelMin = cameraPtr->BlackLevel.GetMin();
	}
	if (IsAvailable(cameraPtr->Gain) && IsWritable(cameraPtr->Gain))
	{
		settings.Gain = cameraPtr->Gain.GetValue();
		settings.GainMax = cameraPtr->Gain.GetMax();
		settings.GainMin = cameraPtr->Gain.GetMin();
	}
	if (IsAvailable(cameraPtr->ExposureTime) && IsWritable(cameraPtr->ExposureTime))
	{
		settings.ExposureTime = cameraPtr->ExposureTime.GetValue();
		settings.ExposureTimeMax = cameraPtr->ExposureTime.GetMax();
		settings.ExposureTimeMin = cameraPtr->ExposureTime.GetMin();
	}
	if (IsAvailable(cameraPtr->Gamma) && IsWritable(cameraPtr->Gamma))
	{
		settings.Gamma = cameraPtr->Gamma.GetValue();
		settings.GammaMax = cameraPtr->Gamma.GetMax();
		settings.GammaMin = cameraPtr->Gamma.GetMin();
	}

	return settings;
}