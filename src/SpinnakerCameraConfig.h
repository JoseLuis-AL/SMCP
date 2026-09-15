#pragma once

#include "QtCore/QtCore"

struct SpinnakerCameraConfig
{
	// Black level.
	double BlackLevelValue = 0;
	double BlackLevelMinValue = 0;
	double BlackLevelMaxValue = 0;

	// Gain.
	double GainValue = 0;
	double GainMinValue = 0;
	double GainMaxValue = 0;

	// ExposureTime.
	double ExposureTimeValue = 0;
	double ExposureTimeMinValue = 0;
	double ExposureTimeMaxValue = 0;

	// Gamma.
	double GammaValue = 0;
	double GammaMinValue = 0;
	double GammaMaxValue = 0;
};

Q_DECLARE_METATYPE(SpinnakerCameraConfig);