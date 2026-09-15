#pragma once

// Unica fuente de claves y valores por defecto de la configuracion (QSettings).
// La aplicacion guarda los ajustes en el ambito de usuario con el formato nativo
// de la plataforma; en Windows: HKEY_CURRENT_USER\Software\<Organization>\<AppName>.
namespace smcp::Settings
{
namespace App
{
	const auto Organization = "CENAM";
	const auto AppName = "SMCP";
	const auto WindowName = "CENAM - Sistema de Medicion - Camara Proyector";

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
	// Esquinas interiores (columnas x filas) y tamano de cada cuadro (mm).
	const auto Columns = "Chessboard/Columns";
	constexpr int ColumnsDefaultValue = 22;

	const auto Rows = "Chessboard/Rows";
	constexpr int RowsDefaultValue = 15;

	const auto Width = "Chessboard/Width";
	constexpr double WidthDefaultValue = 15;

	const auto Height = "Chessboard/Height";
	constexpr double HeightDefaultValue = 15;
}

namespace Decode
{
	const auto Threshold = "Decode/Threshold";
	constexpr int ThresholdDefaultValue = 20;

	// Decodificacion robusta (Moreno & Taubin): b es un factor en [0, 1] y m un numero de niveles.
	const auto B = "Decode/B";
	constexpr double BDefaultValue = 0.5;

	const auto M = "Decode/M";
	constexpr int MDefaultValue = 5;
}

namespace Calibration
{
	const auto HWin = "Calibration/HWin";
	constexpr int HWinDefaultValue = 60;

	const auto ShadowThreshold = "Calibration/ShadowThreshold";
	constexpr int ShadowThresholdDefaultValue = 0;

	const auto File = "Calibration/File";
}

namespace Reconstruction
{
	const auto MaxDist = "Reconstruction/MaxDist";
	constexpr double MaxDistDefaultValue = 100.0;

	const auto SaveNormals = "Reconstruction/SaveNormals";
	constexpr bool SaveNormalsDefaultValue = true;

	const auto SaveColors = "Reconstruction/SaveColors";
	constexpr bool SaveColorsDefaultValue = true;

	const auto SaveBinary = "Reconstruction/SaveBinary";
	constexpr bool SaveBinaryDefaultValue = true;
}
} // namespace smcp::Settings
