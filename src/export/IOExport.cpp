#include "export/IOExport.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <iomanip>

#include "core/structured_light.h"

namespace smcp
{

/* Internal Helpers ==================================================================== */
namespace
{
	/// <summary>
	/// Collects the indices of all valid (non-NaN) points in the pointcloud. Optionally also checks normals for validity.
	/// </summary>
	/// <param name="pointcloud">Source pointcloud to scan.</param>
	/// <param name="check_normals">If true, also reject points with invalid normals.</param>
	/// <returns>Vector of valid point indices.</returns>
	std::vector<int> collect_valid_indices(const scan3d::Pointcloud& pointcloud, bool check_normals)
	{
		const auto* points_data = pointcloud.points.ptr<cv::Vec3f>(0);
		const auto* normals_data = check_normals ? pointcloud.normals.ptr<cv::Vec3f>(0) : nullptr;
		const int total = static_cast<int>(pointcloud.points.total());

		std::vector<int> indices;
		indices.reserve(total);

		for (int i = 0; i < total; ++i)
		{
			// Skip invalid points.
			if (sl::INVALID(points_data[i]))
			{
				continue;
			}

			// Skip points with invalid normals (if checking).
			if (normals_data && sl::INVALID(normals_data[i]))
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
	/// <param name="need_colors">Whether colors will be used.</param>
	/// <param name="need_normals">Whether normals will be used.</param>
	/// <returns>True if the pointcloud is valid for the requested operation.</returns>
	bool validate_pointcloud(const scan3d::Pointcloud& pointcloud, bool need_colors, bool need_normals)
	{
		if (pointcloud.points.empty())
		{
			return false;
		}

		// Check that optional data matches point dimensions.
		const int rows = pointcloud.points.rows;
		const int cols = pointcloud.points.cols;

		if (need_colors && !pointcloud.colors.empty()
			&& (pointcloud.colors.rows != rows || pointcloud.colors.cols != cols))
		{
			return false;
		}

		if (need_normals && !pointcloud.normals.empty()
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
bool IOExport::write_ply(const std::string& filename, const scan3d::Pointcloud& pointcloud, unsigned flags)
{
	const bool use_binary = (flags & PlyBinary);
	const bool use_colors = (flags & PlyColors) && !pointcloud.colors.empty();
	const bool use_normals = (flags & PlyNormals) && !pointcloud.normals.empty();

	// Validate input data.
	if (!validate_pointcloud(pointcloud, use_colors, use_normals))
	{
		return false;
	}

	// Collect valid point indices.
	const auto indices = collect_valid_indices(pointcloud, use_normals);
	if (indices.empty())
	{
		return false;
	}

	// Data pointers.
	const auto* points_data = pointcloud.points.ptr<cv::Vec3f>(0);
	const auto* colors_data = use_colors ? pointcloud.colors.ptr<cv::Vec3b>(0) : nullptr;
	const auto* normals_data = use_normals ? pointcloud.normals.ptr<cv::Vec3f>(0) : nullptr;

	// Open file.
	const auto mode = std::ios::out | std::ios::trunc | (use_binary ? std::ios::binary : std::ios::openmode{});
	std::ofstream out(filename, mode);
	if (!out.is_open())
	{
		return false;
	}

	// Write PLY header.
	const char* format_str = use_binary ? "binary_little_endian 1.0" : "ascii 1.0";
	out << "ply\n"
		<< "format " << format_str << "\n"
		<< "comment scan3d-capture generated\n"
		<< "element vertex " << indices.size() << "\n"
		<< "property float x\n"
		<< "property float y\n"
		<< "property float z\n";

	if (use_normals)
	{
		out << "property float nx\n"
			<< "property float ny\n"
			<< "property float nz\n";
	}

	if (use_colors)
	{
		out << "property uchar red\n"
			<< "property uchar green\n"
			<< "property uchar blue\n";
	}

	out << "end_header\n";

	// Write vertex data.
	for (const int idx : indices)
	{
		const cv::Vec3f& p = points_data[idx];

		if (use_binary)
		{
			// Points.
			out.write(reinterpret_cast<const char*>(p.val), 3 * sizeof(float));

			// Normals.
			if (normals_data)
			{
				const cv::Vec3f& n = normals_data[idx];
				out.write(reinterpret_cast<const char*>(n.val), 3 * sizeof(float));
			}

			// Colors (BGR → RGB).
			if (colors_data)
			{
				const cv::Vec3b& c = colors_data[idx];
				const unsigned char rgb[3] = { c[2], c[1], c[0] };
				out.write(reinterpret_cast<const char*>(rgb), 3);
			}
		}
		else
		{
			// Points.
			out << p[0] << ' ' << p[1] << ' ' << p[2];

			// Normals.
			if (normals_data)
			{
				const cv::Vec3f& n = normals_data[idx];
				out << ' ' << n[0] << ' ' << n[1] << ' ' << n[2];
			}

			// Colors (BGR → RGB).
			if (colors_data)
			{
				const cv::Vec3b& c = colors_data[idx];
				out << ' ' << static_cast<int>(c[2])
					<< ' ' << static_cast<int>(c[1])
					<< ' ' << static_cast<int>(c[0]);
			}

			out << '\n';
		}
	}

	out.close();
	std::cerr << "[write_ply] Saved " << indices.size() << " points (" << filename << ")\n";
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
bool IOExport::write_xyz(const std::string& filename, const scan3d::Pointcloud& pointcloud, XyzFormat format)
{
	const bool use_colors = (format == XyzFormat::XyzRgb) && !pointcloud.colors.empty();

	// Validate input data.
	if (!validate_pointcloud(pointcloud, use_colors, false))
	{
		return false;
	}

	// Collect valid point indices (no normal check for XYZ export).
	const auto indices = collect_valid_indices(pointcloud, false);
	if (indices.empty())
	{
		return false;
	}

	// Data pointers.
	const auto* points_data = pointcloud.points.ptr<cv::Vec3f>(0);
	const auto* colors_data = use_colors ? pointcloud.colors.ptr<cv::Vec3b>(0) : nullptr;

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
		const cv::Vec3f& p = points_data[idx];
		out << p[0] << ' ' << p[1] << ' ' << p[2];

		// Colors (BGR → RGB).
		if (colors_data)
		{
			const cv::Vec3b& c = colors_data[idx];
			out << ' ' << static_cast<int>(c[2])
				<< ' ' << static_cast<int>(c[1])
				<< ' ' << static_cast<int>(c[0]);
		}

		out << '\n';
	}

	out.close();
	std::cerr << "[write_xyz] Saved " << indices.size() << " points (" << filename << ")\n";
	return true;
}

/* Colored clouds (editor) ================================================================= */

bool IOExport::read_xyz(const std::string& filename, pointcloud::ColorCloud& cloud)
{
	std::ifstream in(filename);
	if (!in.is_open())
	{
		return false;
	}

	cloud.clear();
	std::string line;
	while (std::getline(in, line))
	{
		std::istringstream fields(line);
		pointcloud::ColorPoint pt;
		if (!(fields >> pt.x >> pt.y >> pt.z))
		{
			continue;  // linea vacia o con menos de tres campos
		}
		float r = 0.0f, g = 0.0f, b = 0.0f;
		if (fields >> r >> g >> b)
		{
			pt.r = static_cast<std::uint8_t>(r);
			pt.g = static_cast<std::uint8_t>(g);
			pt.b = static_cast<std::uint8_t>(b);
		}
		cloud.push_back(pt);
	}
	return true;
}

bool IOExport::write_xyz(const std::string& filename, const pointcloud::ColorCloud& cloud)
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
