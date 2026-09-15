#pragma once
#pragma once

namespace Settings
{
	namespace App
	{
		const auto AppName = "SMCP";
		const auto AppSettingsName = "SMCP.ini";
		const auto WindowName = "SMCP - CENAM";

		const auto RootDirectory = "App/RootDirectory";
	}

	namespace MainWindow
	{
		const auto Geometry = "Window/Geometry";
		const auto State = "Window/State";
	}

	namespace Camera
	{
		const auto SerialNumber = "Camera/SerialNumber";

		const auto BlackLevel = "Camera/BlackLevel";
		constexpr double BlackLevelDefaultValue = 10;
		const auto ExposureTime = "Camera/ExposureTime";
		constexpr double ExposureTimeDefaultValue = 4162.55; // 8325.1 - 4162.55;
		const auto Gain = "Camera/Gain";
		constexpr double GainDefaultValue = 1;
		const auto Gamma = "Camera/Gamma";
		constexpr double GammaDefaultValue = 1.0;
	}

	namespace Projector
	{
		const auto Screen = "Projector/Screen";
		constexpr int ScreenDefaultValue = 0;

		const auto PatternCount = "GrayCode/PatternCount";
		constexpr int PatternCountDefaultValue = 10;
	}

	namespace Capture
	{
		const auto WaitTime = "Capture/WaitTime";
		constexpr int WaitTimeDefaultValue = 500;

		const auto Continuous = "Capture/Continuous";
		constexpr int ContinuousDefaultValue = 100;
	}

	namespace Chessboard
	{
		const auto Columns = "Chessboard/Columns";
		constexpr int ColumnsDefaultValue = 22;

		const auto Rows = "Chessboard/Rows";
		constexpr int RowsDefaultValue = 15;

		const auto Width = "Chessboard/Width";
		constexpr int WidthDefaultValue = 15;

		const auto Height = "Chessboard/Height";
		constexpr int HeightDefaultValue = 15;
	}

	namespace Decode
	{
		const auto Threshold = "Decode/Threshold";
		constexpr int ThresholdDefaultValue = 20;

		const auto M = "Decode/M";
		constexpr double MDefaultValue = 0.5;

		const auto B = "Decode/B";
		constexpr int BDefaultValue = 1;
	}

	namespace Calibration
	{
		const auto HWin = "Calibration/HWin";
		constexpr int HWinDefaultValue = 60;

		const auto ShadowThreshold = "Calibration/ShadowThreshold";
		constexpr int ShadowThresholdDefaultValue = 0;
	}
};
