#include "export/IOExport.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include "core/StructuredLight.h"

namespace smcp
{

/* Internal Helpers ==================================================================== */
namespace
{
	/// <summary>
	/// Collects the indices of all valid (non-NaN) points in the pointcloud. Optionally also checks normals for validity.
	/// </summary>
	/// <param name="pointcloud">Source pointcloud to scan.</param>
	/// <param name="checkNormals">If true, also reject points with invalid normals.</param>
	/// <returns>Vector of valid point indices.</returns>
	std::vector<int> CollectValidIndices(const Scan3d::Pointcloud& pointcloud, bool checkNormals)
	{
		const auto* pointsData = pointcloud.points.ptr<cv::Vec3f>(0);
		const auto* normalsData = checkNormals ? pointcloud.normals.ptr<cv::Vec3f>(0) : nullptr;
		const int total = static_cast<int>(pointcloud.points.total());

		std::vector<int> indices;
		indices.reserve(total);

		for (int i = 0; i < total; ++i)
		{
			// Skip invalid points.
			if (StructuredLight::Invalid(pointsData[i]))
			{
				continue;
			}

			// Skip points with invalid normals (if checking).
			if (normalsData && StructuredLight::Invalid(normalsData[i]))
			{
				continue;
			}

			indices.push_back(i);
		}

		return indices;
	}

	/// <summary>
	/// Validates that the pointcloud has consistent dimensions for the requested data.
	/// </summary>
	/// <param name="pointcloud">Pointcloud to validate.</param>
	/// <param name="needColors">Whether colors will be used.</param>
	/// <param name="needNormals">Whether normals will be used.</param>
	/// <returns>True if the pointcloud is valid for the requested operation.</returns>
	bool ValidatePointcloud(const Scan3d::Pointcloud& pointcloud, bool needColors, bool needNormals)
	{
		if (pointcloud.points.empty())
		{
			return false;
		}

		// Check that optional data matches point dimensions.
		const int rows = pointcloud.points.rows;
		const int cols = pointcloud.points.cols;

		if (needColors && !pointcloud.colors.empty()
			&& (pointcloud.colors.rows != rows || pointcloud.colors.cols != cols))
		{
			return false;
		}

		if (needNormals && !pointcloud.normals.empty()
			&& (pointcloud.normals.rows != rows || pointcloud.normals.cols != cols))
		{
			return false;
		}

		return true;
	}
}

/// <summary>
/// Writes the pointcloud to a PLY file (binary or ASCII). Filters out invalid points and normals before writing.
/// </summary>
/// <param name="filename">Output file path.</param>
/// <param name="pointcloud">Source pointcloud data.</param>
/// <param name="flags">Bitmask of PlyFlag values controlling the output format.</param>
/// <returns>True if the file was written successfully.</returns>
bool IOExport::WritePly(const std::string& filename, const Scan3d::Pointcloud& pointcloud, unsigned flags)
{
	const bool useBinary = (flags & PlyBinary);
	const bool useColors = (flags & PlyColors) && !pointcloud.colors.empty();
	const bool useNormals = (flags & PlyNormals) && !pointcloud.normals.empty();

	// Validate input data.
	if (!ValidatePointcloud(pointcloud, useColors, useNormals))
	{
		return false;
	}

	// Collect valid point indices.
	const auto indices = CollectValidIndices(pointcloud, useNormals);
	if (indices.empty())
	{
		return false;
	}

	// Data pointers.
	const auto* pointsData = pointcloud.points.ptr<cv::Vec3f>(0);
	const auto* colorsData = useColors ? pointcloud.colors.ptr<cv::Vec3b>(0) : nullptr;
	const auto* normalsData = useNormals ? pointcloud.normals.ptr<cv::Vec3f>(0) : nullptr;

	// Open file.
	const auto mode = std::ios::out | std::ios::trunc | (useBinary ? std::ios::binary : std::ios::openmode{});
	std::ofstream out(filename, mode);
	if (!out.is_open())
	{
		return false;
	}

	// Write PLY header.
	const char* formatStr = useBinary ? "binary_little_endian 1.0" : "ascii 1.0";
	out << "ply\n"
		<< "format " << formatStr << "\n"
		<< "comment scan3d-capture generated\n"
		<< "element vertex " << indices.size() << "\n"
		<< "property float x\n"
		<< "property float y\n"
		<< "property float z\n";

	if (useNormals)
	{
		out << "property float nx\n"
			<< "property float ny\n"
			<< "property float nz\n";
	}

	if (useColors)
	{
		out << "property uchar red\n"
			<< "property uchar green\n"
			<< "property uchar blue\n";
	}

	out << "end_header\n";

	// Write vertex data.
	for (const int idx : indices)
	{
		const cv::Vec3f& p = pointsData[idx];

		if (useBinary)
		{
			// Points.
			out.write(reinterpret_cast<const char*>(p.val), 3 * sizeof(float));

			// Normals.
			if (normalsData)
			{
				const cv::Vec3f& n = normalsData[idx];
				out.write(reinterpret_cast<const char*>(n.val), 3 * sizeof(float));
			}

			// Colors (BGR → RGB).
			if (colorsData)
			{
				const cv::Vec3b& c = colorsData[idx];
				const unsigned char rgb[3] = { c[2], c[1], c[0] };
				out.write(reinterpret_cast<const char*>(rgb), 3);
			}
		}
		else
		{
			// Points.
			out << p[0] << ' ' << p[1] << ' ' << p[2];

			// Normals.
			if (normalsData)
			{
				const cv::Vec3f& n = normalsData[idx];
				out << ' ' << n[0] << ' ' << n[1] << ' ' << n[2];
			}

			// Colors (BGR → RGB).
			if (colorsData)
			{
				const cv::Vec3b& c = colorsData[idx];
				out << ' ' << static_cast<int>(c[2])
					<< ' ' << static_cast<int>(c[1])
					<< ' ' << static_cast<int>(c[0]);
			}

			out << '\n';
		}
	}

	out.close();
	std::cerr << "[WritePly] Saved " << indices.size() << " points (" << filename << ")\n";
	return true;
}

/// <summary>
/// Writes the pointcloud to an ASCII file compatible with PolyWorks. Outputs space-separated columns with no header, no faces, no alpha. Format Xyz:
/// X Y Z Format XyzRgb: X Y Z R G B
/// </summary>
/// <param name="filename">Output file path (.xyz, .txt, or .pts).</param>
/// <param name="pointcloud">Source pointcloud data.</param>
/// <param name="format">Column format (Xyz or XyzRgb).</param>
/// <returns>True if the file was written successfully.</returns>
bool IOExport::WriteXyz(const std::string& filename, const Scan3d::Pointcloud& pointcloud, XyzFormat format)
{
	const bool useColors = (format == XyzFormat::XyzRgb) && !pointcloud.colors.empty();

	// Validate input data.
	if (!ValidatePointcloud(pointcloud, useColors, false))
	{
		return false;
	}

	// Collect valid point indices (no normal check for XYZ export).
	const auto indices = CollectValidIndices(pointcloud, false);
	if (indices.empty())
	{
		return false;
	}

	// Data pointers.
	const auto* pointsData = pointcloud.points.ptr<cv::Vec3f>(0);
	const auto* colorsData = useColors ? pointcloud.colors.ptr<cv::Vec3b>(0) : nullptr;

	// Open file.
	std::ofstream out(filename, std::ios::out | std::ios::trunc);
	if (!out.is_open())
	{
		return false;
	}

	// Set consistent floating-point precision.
	out << std::setprecision(8);

	// Write raw vertex data (no header).
	for (const int idx : indices)
	{
		const cv::Vec3f& p = pointsData[idx];
		out << p[0] << ' ' << p[1] << ' ' << p[2];

		// Colors (BGR → RGB).
		if (colorsData)
		{
			const cv::Vec3b& c = colorsData[idx];
			out << ' ' << static_cast<int>(c[2])
				<< ' ' << static_cast<int>(c[1])
				<< ' ' << static_cast<int>(c[0]);
		}

		out << '\n';
	}

	out.close();
	std::cerr << "[WriteXyz] Saved " << indices.size() << " points (" << filename << ")\n";
	return true;
}

/* Colored clouds (editor) ================================================================= */

namespace
{
	/// Below this size a chunk is not worth an extra thread.
	constexpr std::size_t Min_Parse_Chunk_Bytes = 512 * 1024;

	/// Rough lower bound of bytes per "X Y Z R G B" line, used only to reserve memory.
	constexpr std::size_t Approx_Bytes_Per_Line = 30;

	inline const char* SkipBlanks(const char* p, const char* end)
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\v' || *p == '\f'))
		{
			++p;
		}
		return p;
	}

	/// Parses one float the way `istream >> float` does for this format: leading blanks and an
	/// optional sign are accepted, "inf"/"nan" are rejected. On failure `p` is left untouched.
	inline bool ParseFloat(const char*& p, const char* end, float& out)
	{
		const char* c = SkipBlanks(p, end);
		const bool plusSign = c < end && *c == '+';
		if (plusSign)
		{
			++c;
		}
		const char* digits = (!plusSign && c < end && *c == '-') ? c + 1 : c;
		if (digits >= end || !((*digits >= '0' && *digits <= '9') || *digits == '.'))
		{
			return false;
		}
		const auto result = std::from_chars(c, end, out);
		if (result.ec != std::errc())
		{
			return false;
		}
		p = result.ptr;
		return true;
	}

	void ParseXyzRange(const char* p, const char* end, Pointcloud::ColorCloud& cloud)
	{
		while (p < end)
		{
			const char* newline = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
			const char* lineEnd = newline ? newline : end;

			Pointcloud::ColorPoint pt;
			const char* c = p;
			if (ParseFloat(c, lineEnd, pt.x) && ParseFloat(c, lineEnd, pt.y) && ParseFloat(c, lineEnd, pt.z))
			{
				float r = 0.0f, g = 0.0f, b = 0.0f;
				if (ParseFloat(c, lineEnd, r) && ParseFloat(c, lineEnd, g) && ParseFloat(c, lineEnd, b))
				{
					pt.r = static_cast<std::uint8_t>(r);
					pt.g = static_cast<std::uint8_t>(g);
					pt.b = static_cast<std::uint8_t>(b);
				}
				cloud.push_back(pt);
			}
			p = newline ? newline + 1 : end;
		}
	}
} // namespace

bool IOExport::ReadXyz(const std::string& filename, Pointcloud::ColorCloud& cloud)
{
	std::ifstream in(filename, std::ios::binary | std::ios::ate);
	if (!in.is_open())
	{
		return false;
	}

	// Reading the whole file at once and parsing with std::from_chars is ~10x faster than
	// getline + istringstream; parsing chunks in parallel adds another ~4x on large clouds.
	const std::streamoff size = in.tellg();
	std::vector<char> data(size > 0 ? static_cast<std::size_t>(size) : 0);
	in.seekg(0, std::ios::beg);
	if (!data.empty() && !in.read(data.data(), static_cast<std::streamsize>(data.size())))
	{
		data.resize(static_cast<std::size_t>(in.gcount()));
	}

	const char* begin = data.data();
	const char* end = begin + data.size();
	cloud.clear();

	// Chunk boundaries fall right after a newline, so no line is split between threads and the
	// concatenated result keeps the file order.
	const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
	const std::size_t maxChunks = std::max<std::size_t>(1, data.size() / Min_Parse_Chunk_Bytes);
	const std::size_t chunkCount = std::min<std::size_t>(hardwareThreads, maxChunks);

	std::vector<const char*> bounds{ begin };
	for (std::size_t i = 1; i < chunkCount; ++i)
	{
		const char* guess = begin + data.size() * i / chunkCount;
		if (guess <= bounds.back())
		{
			continue;
		}
		const char* newline = static_cast<const char*>(std::memchr(guess, '\n', static_cast<std::size_t>(end - guess)));
		if (!newline)
		{
			break;
		}
		bounds.push_back(newline + 1);
	}
	bounds.push_back(end);

	const std::size_t parts = bounds.size() - 1;
	if (parts == 1)
	{
		cloud.reserve(data.size() / Approx_Bytes_Per_Line);
		ParseXyzRange(begin, end, cloud);
		return true;
	}

	std::vector<Pointcloud::ColorCloud> chunks(parts);
	std::vector<std::thread> workers;
	workers.reserve(parts - 1);
	const auto parseChunk = [&bounds, &chunks](std::size_t i) {
		chunks[i].reserve(static_cast<std::size_t>(bounds[i + 1] - bounds[i]) / Approx_Bytes_Per_Line);
		ParseXyzRange(bounds[i], bounds[i + 1], chunks[i]);
	};
	for (std::size_t i = 1; i < parts; ++i)
	{
		workers.emplace_back(parseChunk, i);
	}
	parseChunk(0);
	for (auto& worker : workers)
	{
		worker.join();
	}

	std::size_t total = 0;
	for (const auto& chunk : chunks)
	{
		total += chunk.size();
	}
	cloud.reserve(total);
	for (const auto& chunk : chunks)
	{
		cloud.insert(cloud.end(), chunk.begin(), chunk.end());
	}
	return true;
}

bool IOExport::WriteXyz(const std::string& filename, const Pointcloud::ColorCloud& cloud)
{
	if (cloud.empty())
	{
		return false;
	}
	std::ofstream out(filename);
	if (!out.is_open())
	{
		return false;
	}
	out << std::fixed << std::setprecision(6);
	for (const auto& pt : cloud)
	{
		out << pt.x << ' ' << pt.y << ' ' << pt.z << ' '
			<< static_cast<int>(pt.r) << ' ' << static_cast<int>(pt.g) << ' ' << static_cast<int>(pt.b) << '\n';
	}
	return out.good();
}
} // namespace smcp
