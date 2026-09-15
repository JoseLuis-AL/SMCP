#pragma once

namespace smcp
{
struct CameraSettings
{
	double BlackLevel = 0;
	double BlackLevelMin = 0;
	double BlackLevelMax = 0;

	double ExposureTime = 0;
	double ExposureTimeMin = 0;
	double ExposureTimeMax = 0;

	double Gain = 0;
	double GainMin = 0;
	double GainMax = 0;

	double Gamma = 0;
	double GammaMin = 0;
	double GammaMax = 0;
};
}