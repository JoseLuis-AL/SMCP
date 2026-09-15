#pragma once

#include <string>
#include "scan3d.hpp"

namespace SMCP::IOExport
{
// PLY export flags (bitmask).
	enum PlyFlag : unsigned
	{
		PlyPoints = 0x01,
		PlyColors = 0x02,
		PlyNormals = 0x04,
		PlyBinary = 0x08
	};

	// XYZ export format.
	enum class XyzFormat
	{
		Xyz,     // X Y Z
		XyzRgb   // X Y Z R G B
	};

	/// <summary>
	/// Writes the pointcloud to a PLY file (binary or ASCII). Filters out invalid points and normals before writing.
	/// </summary>
	/// <param name="filename">Output file path.</param>
	/// <param name="pointcloud">Source pointcloud data.</param>
	/// <param name="flags">Bitmask of PlyFlag values controlling the output format.</param>
	/// <returns>True if the file was written successfully.</returns>
	bool write_ply(const std::string& filename, const scan3d::Pointcloud& pointcloud, unsigned flags);

	/// <summary>
	/// Writes the pointcloud to an ASCII file. Outputs space-separated columns: X Y Z, or X Y Z R G B.
	/// </summary>
	/// <param name="filename">Output file path (.xyz, .txt, or .pts).</param>
	/// <param name="pointcloud">Source pointcloud data.</param>
	/// <param name="format">Column format (Xyz or XyzRgb).</param>
	/// <returns>True if the file was written successfully.</returns>
	bool write_xyz(const std::string& filename, const scan3d::Pointcloud& pointcloud, XyzFormat format = XyzFormat::Xyz);
}
