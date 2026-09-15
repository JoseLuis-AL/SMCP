/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Brown University nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DANIEL MORENO AND GABRIEL TAUBIN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include <string>
#include "core/PointcloudOps.h"
#include "core/Scan3d.h"

namespace smcp::IOExport
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
bool WritePly(const std::string& filename, const Scan3d::Pointcloud& pointcloud, unsigned flags);

/// <summary>
/// Writes the pointcloud to an ASCII file. Outputs space-separated columns: X Y Z, or X Y Z R G B.
/// </summary>
/// <param name="filename">Output file path (.xyz, .txt, or .pts).</param>
/// <param name="pointcloud">Source pointcloud data.</param>
/// <param name="format">Column format (Xyz or XyzRgb).</param>
/// <returns>True if the file was written successfully.</returns>
bool WriteXyz(const std::string& filename, const Scan3d::Pointcloud& pointcloud, XyzFormat format = XyzFormat::Xyz);

/// <summary>
/// Reads an ASCII point cloud (X Y Z [R G B] per line) into a colored cloud. Lines with fewer than three
/// fields are skipped; points without color get a neutral gray (200, 200, 200).
/// </summary>
/// <returns>True if the file could be opened.</returns>
bool ReadXyz(const std::string& filename, Pointcloud::ColorCloud& cloud);

/// <summary>
/// Writes a colored cloud as ASCII "X Y Z R G B" lines (six decimals for coordinates).
/// </summary>
/// <returns>True if the file was written successfully.</returns>
bool WriteXyz(const std::string& filename, const Pointcloud::ColorCloud& cloud);
}
