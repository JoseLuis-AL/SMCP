#ifndef __CONFIGURATION_H__
#define __CONFIGURATION_H__

namespace Config::Capture
{
	inline auto CAPTURE_SETS = "capture/capture_sets";
	inline constexpr int CAPTURE_SETS_DEFAULT_VALUE = 5;

	inline auto CAMERA_GAIN = "camera/gain";
	inline constexpr int CAMERA_GAIN_DEFAULT_VALUE = 0;

	inline auto CAMERA_BLACK_LEVEL = "camera/black_level";
	inline constexpr int CAMERA_BLACK_LEVEL_DEFAULT_VALUE = 10;

	inline auto CAMERA_EXPOSURE_TIME = "camera/exposure_time";
	inline constexpr double CAMERA_EXPOSURE_TIME_DEFAULT_VALUE = 4162.55;

	inline auto CAMERA_GAMMA = "camera/gamma";
	inline constexpr double CAMERA_GAMMA_DEFAULT_VALUE = 1.0;
}

#endif
