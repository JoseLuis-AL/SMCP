// Compares the classic reconstruction (one point per projector pixel) with the sub-pixel one (one
// point per camera pixel) on the calibration sets of an existing workspace. It only reads the
// workspace: nothing is written into it.
//
//   SMCP_reconstruct_compare <workspace> [--sets i,j,...] [--b B] [--m M] [--threshold N] [--maxdist D]
//
// Every calibration set shows a flat chessboard whose corners are in cam_NN.txt. The points of each
// cloud that fall inside the board are fitted with a plane; the RMS distance to it measures the
// noise of the method on a surface of known shape. Points farther than three deviations are
// dropped twice before the final figure, so a few outliers do not decide it.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <QString>

#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "core/CalibrationData.h"
#include "core/Scan3d.h"
#include "core/StructuredLight.h"

namespace
{
	struct PlaneFit
	{
		int points = 0;
		double rms = 0.0;	  ///< After trimming.
		double rawRms = 0.0;  ///< All points inside the board.
		double p95 = 0.0;	  ///< 95th percentile of |distance|, all points.
		cv::Point3d mean;	  ///< A point of the final plane.
		cv::Vec3d normal;	  ///< Its unit normal.
	};

	PlaneFit FitPlane(std::vector<cv::Point3d> points)
	{
		PlaneFit fit;
		for (int pass = 0; pass < 3 && points.size() >= 3; pass++)
		{
			cv::Point3d mean(0, 0, 0);
			for (const auto& p : points)
				mean += p;
			mean *= 1.0 / points.size();
			cv::Matx33d covariance = cv::Matx33d::zeros();
			for (const auto& p : points)
			{
				const cv::Vec3d d(p.x - mean.x, p.y - mean.y, p.z - mean.z);
				covariance += d * d.t();
			}
			cv::Mat values, vectors;
			cv::eigen(cv::Mat(covariance), values, vectors);
			const cv::Vec3d normal(vectors.at<double>(2, 0), vectors.at<double>(2, 1), vectors.at<double>(2, 2));
			std::vector<double> distances(points.size());
			double sum = 0.0;
			for (size_t i = 0; i < points.size(); i++)
			{
				distances[i] = normal.dot(cv::Vec3d(points[i].x - mean.x, points[i].y - mean.y, points[i].z - mean.z));
				sum += distances[i] * distances[i];
			}
			const double rms = std::sqrt(sum / points.size());
			if (pass == 0)
			{
				fit.rawRms = rms;
				std::vector<double> absolute(distances);
				for (double& d : absolute)
					d = std::abs(d);
				std::nth_element(absolute.begin(), absolute.begin() + absolute.size() * 95 / 100, absolute.end());
				fit.p95 = absolute[absolute.size() * 95 / 100];
			}
			fit.rms = rms;
			fit.points = static_cast<int>(points.size());
			fit.mean = mean;
			fit.normal = normal;
			if (pass == 2)
				break;
			std::vector<cv::Point3d> kept;
			for (size_t i = 0; i < points.size(); i++)
				if (std::abs(distances[i]) <= 3.0 * rms)
					kept.push_back(points[i]);
			points.swap(kept);
		}
		return fit;
	}

	/// Points of an organized cloud whose camera projection falls inside `board` (camera pixels).
	/// `blocks` receives, for each of them, the index of the Block x Block pixel square of the
	/// camera image it falls in.
	constexpr int Block = 28;  // about 8 projector pixels in the 2026 setup
	std::vector<cv::Point3d> InsideBoard(const smcp::Scan3d::Pointcloud& cloud, const smcp::CalibrationData& calib,
		const std::vector<cv::Point2f>& board, std::vector<int>& blocks)
	{
		blocks.clear();
		std::vector<cv::Point3d> all;
		for (int h = 0; h < cloud.points.rows; h++)
			for (int w = 0; w < cloud.points.cols; w++)
			{
				const cv::Vec3f& p = cloud.points.at<cv::Vec3f>(h, w);
				if (!smcp::StructuredLight::Invalid(p))
					all.emplace_back(p[0], p[1], p[2]);
			}
		std::vector<cv::Point3d> inside;
		if (all.empty())
			return inside;
		std::vector<cv::Point2d> pixels;
		cv::projectPoints(all, cv::Vec3d(0, 0, 0), cv::Vec3d(0, 0, 0), calib.camK, calib.camKc, pixels);
		for (size_t i = 0; i < all.size(); i++)
			if (cv::pointPolygonTest(board, cv::Point2f(static_cast<float>(pixels[i].x), static_cast<float>(pixels[i].y)), false) > 0)
			{
				inside.push_back(all[i]);
				blocks.push_back(static_cast<int>(pixels[i].y) / Block * 4096 + static_cast<int>(pixels[i].x) / Block);
			}
		return inside;
	}

	/// Median, over the blocks with at least `minimum` points, of the RMS distance to a plane fitted
	/// to the block alone. A block is too small for the shape of the board or for the calibration
	/// to matter, so this is the point-to-point noise of the method.
	double LocalNoise(const std::vector<cv::Point3d>& points, const std::vector<int>& blocks, size_t minimum)
	{
		std::vector<size_t> order(points.size());
		for (size_t i = 0; i < order.size(); i++)
			order[i] = i;
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return blocks[a] < blocks[b]; });
		std::vector<double> values;
		for (size_t start = 0; start < order.size();)
		{
			size_t end = start;
			std::vector<cv::Point3d> group;
			while (end < order.size() && blocks[order[end]] == blocks[order[start]])
				group.push_back(points[order[end++]]);
			if (group.size() >= minimum)
				values.push_back(FitPlane(group).rms);
			start = end;
		}
		if (values.empty())
			return 0.0;
		std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
		return values[values.size() / 2];
	}

	/// Distance of every point of an organized cloud to the plane, as a gray image: mid gray on the
	/// plane, black and white at -range and +range millimetres, and black where there is no point.
	void SaveResiduals(const smcp::Scan3d::Pointcloud& cloud, const PlaneFit& fit, double range, const std::string& path)
	{
		cv::Mat image(cloud.points.rows, cloud.points.cols, CV_8UC1, cv::Scalar(0));
		for (int h = 0; h < image.rows; h++)
			for (int w = 0; w < image.cols; w++)
			{
				const cv::Vec3f& p = cloud.points.at<cv::Vec3f>(h, w);
				if (smcp::StructuredLight::Invalid(p))
					continue;
				const double d = fit.normal.dot(cv::Vec3d(p[0] - fit.mean.x, p[1] - fit.mean.y, p[2] - fit.mean.z));
				image.at<unsigned char>(h, w) = cv::saturate_cast<unsigned char>(128.0 + 127.0 * d / range);
			}
		cv::imwrite(path, image);
	}

	double Seconds(std::chrono::steady_clock::time_point since)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
	}
} // namespace

int main(int argc, char** argv)
{
	namespace fs = std::filesystem;
	smcp::StructuredLight::DecodeParams decode;
	int threshold = 20;
	double maxDist = 100.0;
	std::set<int> wanted;
	std::string root, dump;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
		if (arg == "--b") decode.b = std::stof(next());
		else if (arg == "--m") decode.m = static_cast<unsigned>(std::stoi(next()));
		else if (arg == "--threshold") threshold = std::stoi(next());
		else if (arg == "--maxdist") maxDist = std::stod(next());
		else if (arg == "--dump") dump = next();  // folder (outside the workspace) for residual images
		else if (arg == "--sets")
		{
			std::string list = next();
			for (size_t start = 0; start < list.size();)
			{
				const size_t comma = list.find(',', start);
				wanted.insert(std::stoi(list.substr(start, comma - start)));
				start = comma == std::string::npos ? list.size() : comma + 1;
			}
		}
		else root = arg;
	}
	if (root.empty())
	{
		std::cout << "usage: SMCP_reconstruct_compare <workspace> [--sets i,j,...] [--b B] [--m M] [--threshold N] [--maxdist D]"
				  << std::endl;
		return 2;
	}

	smcp::CalibrationData calib;
	if (!calib.LoadCalibration(QString::fromUtf8((fs::u8path(root) / "calibration.yml").u8string().c_str())))
	{
		std::cout << "cannot read calibration.yml in " << root << std::endl;
		return 2;
	}
	std::vector<fs::path> sets;
	for (const auto& entry : fs::directory_iterator(fs::u8path(root)))
		if (entry.is_directory())
			sets.push_back(entry.path());
	std::sort(sets.begin(), sets.end());

	std::printf("b %.2f, m %u, threshold %d, max. ray distance %.1f\n", decode.b, decode.m, threshold, maxDist);
	std::printf("%-4s %10s %10s %9s %9s | %10s %10s %9s %9s | %6s %6s\n", "set", "classic n", "rms mm", "raw rms", "p95",
		"subpixel n", "rms mm", "raw rms", "p95", "t cls", "t sub");
	double sumClassic = 0.0, sumSubpixel = 0.0, sumHalfway = 0.0, sumLocalA = 0.0, sumLocalB = 0.0;
	int printed = 0;
	int compared = 0;
	for (int index = 0; index < static_cast<int>(sets.size()); index++)
	{
		if (!wanted.empty() && !wanted.count(index))
			continue;
		char name[32];
		std::snprintf(name, sizeof(name), "cam_%02d.txt", index);
		std::ifstream corners(fs::u8path(root) / name);
		std::vector<cv::Point2f> cornerPixels;
		for (float x, y; corners >> x >> y;)
			cornerPixels.emplace_back(x, y);
		if (cornerPixels.size() < 4)
			continue;  // not a calibration set
		std::vector<cv::Point2f> board;
		cv::convexHull(cornerPixels, board);

		std::vector<std::string> images;
		for (const auto& entry : fs::directory_iterator(sets[index]))
		{
			const std::string extension = entry.path().extension().string();
			if (entry.is_regular_file() && (extension == ".png" || extension == ".jpg" || extension == ".bmp"))
				images.push_back(entry.path().u8string());
		}
		std::sort(images.begin(), images.end());
		cv::Size projector(1024, 768);
		{
			std::ifstream info(sets[index] / "projector_info.txt");
			int width = 0, height = 0;
			if (info >> width >> height && width > 0 && height > 0)
				projector = cv::Size(width, height);
		}

		smcp::StructuredLight::DecodedSet decoded;
		std::string error;
		if (!smcp::StructuredLight::DecodeGraySet(images, projector, decode, decoded, error))
		{
			std::printf("%-4d decoding failed: %s\n", index, error.c_str());
			continue;
		}
		const cv::Mat color = smcp::StructuredLight::GetColorImage(images.front());

		auto start = std::chrono::steady_clock::now();
		smcp::Scan3d::Pointcloud classic;
		smcp::Scan3d::ReconstructModel(classic, calib, decoded.pattern, decoded.minMax, color, projector, threshold, maxDist);
		const double classicTime = Seconds(start);

		start = std::chrono::steady_clock::now();
		std::vector<cv::Mat> gray;
		smcp::Scan3d::Pointcloud subpixel, halfway;
		if (smcp::StructuredLight::LoadGraySet(images, gray, error))
		{
			const cv::Mat refined = smcp::StructuredLight::RefinePattern(gray, decoded.pattern, decoded.minMax, projector, threshold);
			smcp::Scan3d::ReconstructModelSubpixel(subpixel, calib, refined, decoded.minMax, color, projector, threshold, maxDist);
		}
		const double subpixelTime = Seconds(start);
		// The same without the images (borders halfway between pixels), to see what the intensities add.
		const cv::Mat coarse = smcp::StructuredLight::RefinePattern({}, decoded.pattern, decoded.minMax, projector, threshold);
		smcp::Scan3d::ReconstructModelSubpixel(halfway, calib, coarse, decoded.minMax, color, projector, threshold, maxDist);

		std::vector<int> blocksA, blocksB, blocksC;
		const auto pointsA = InsideBoard(classic, calib, board, blocksA);
		const auto pointsB = InsideBoard(subpixel, calib, board, blocksB);
		const auto pointsC = InsideBoard(halfway, calib, board, blocksC);
		const PlaneFit a = FitPlane(pointsA);
		const PlaneFit b = FitPlane(pointsB);
		const PlaneFit c = FitPlane(pointsC);
		// A full block holds Block*Block camera pixels; ask for half of what each method can give.
		const double perCell = b.points > 0 && a.points > 0 ? static_cast<double>(b.points) / a.points : 1.0;
		const double localA = LocalNoise(pointsA, blocksA, static_cast<size_t>(std::max(12.0, 0.5 * Block * Block / perCell)));
		const double localB = LocalNoise(pointsB, blocksB, Block * Block / 2);
		const double localC = LocalNoise(pointsC, blocksC, Block * Block / 2);
		std::printf("%-4d %10d %10.4f %9.4f %9.4f | %10d %10.4f %9.4f %9.4f | %6.2f %6.2f   (halfway: rms %.4f)  local noise: classic %.4f, sub-pixel %.4f, halfway %.4f\n", index,
			a.points, a.rms, a.rawRms, a.p95, b.points, b.rms, b.rawRms, b.p95, classicTime, subpixelTime, c.rms, localA, localB, localC);
		sumLocalA += localA;
		sumLocalB += localB;
		printed++;
		if (!dump.empty())
		{
			SaveResiduals(classic, a, 0.6, dump + "/classic_" + std::to_string(index) + ".png");
			SaveResiduals(subpixel, b, 0.6, dump + "/subpixel_" + std::to_string(index) + ".png");
		}
		if (a.points > 100 && b.points > 100)
		{
			sumClassic += a.rms;
			sumSubpixel += b.rms;
			sumHalfway += c.rms;
			compared++;
		}
	}
	if (compared)
		std::printf("mean plane RMS over %d sets: classic %.4f mm, sub-pixel %.4f mm (borders halfway %.4f mm)\n", compared,
			sumClassic / compared, sumSubpixel / compared, sumHalfway / compared);
	if (printed)
		std::printf("mean local noise over %d sets: classic %.4f mm, sub-pixel %.4f mm\n", printed, sumLocalA / printed,
			sumLocalB / printed);
	return 0;
}
