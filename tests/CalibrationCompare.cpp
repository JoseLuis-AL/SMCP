// Compares ways of solving the calibration on the corners already saved in a workspace
// (world_NN.txt, cam_NN.txt, proj_NN.txt). It only reads the workspace.
//
//   SMCP_calibration_compare <workspace> [--folds N] [--write <folder>] [--subpixel-corners]
//                            [--window N] [--b B] [--m M]
//
// The sets are split into folds. Each variant is calibrated without one fold and judged on the
// boards of that fold, which it never saw: every corner is triangulated from its camera and
// projector positions, and the reconstructed board is compared with the real one.
//   * length error: distances between corners against the board's known distances (all pairs, and
//     the pairs 150 to 220 mm apart, the range of the ball-bar center distances);
//   * flatness: RMS distance of the reconstructed corners to their best plane;
//   * ray distance: mean gap between the camera ray and the projector ray of a corner.
// A variant that only fits its own sets better (lower reprojection error) but does not improve
// these numbers is not a better calibration.
// --write saves the calibration of every variant, solved with all the sets, as <variant>.yml.
// --subpixel-corners decodes the sets again and adds the variants whose projector corners come from
// the sub-pixel projector coordinates ("subpx" and "subpx+jnt"); it takes a few seconds per set.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <QString>

#include <opencv2/highgui/highgui.hpp>

#include "core/Calibration.h"
#include "core/Scan3d.h"

namespace
{
	using smcp::Calibration::SolveOptions;

	struct Board
	{
		std::vector<cv::Point3f> world;
		std::vector<cv::Point2f> camera, projector;
		std::vector<cv::Point2f> projectorSubpixel;  ///< With --subpixel-corners.
		std::filesystem::path set;
	};

	template <typename Point>
	std::vector<Point> ReadPoints(const std::filesystem::path& path)
	{
		std::vector<Point> points;
		std::ifstream file(path);
		std::string line;
		while (std::getline(file, line))
		{
			float values[3] = { 0, 0, 0 };
			const int count = std::sscanf(line.c_str(), "%f %f %f", &values[0], &values[1], &values[2]);
			if (count >= 2)
			{
				Point point;
				point.x = values[0];
				point.y = values[1];
				if (sizeof(Point) == sizeof(cv::Point3f))
					reinterpret_cast<float*>(&point)[2] = values[2];
				points.push_back(point);
			}
		}
		return points;
	}

	class Silent : public smcp::ProgressSink
	{
	public:
		void Message(const std::string&) override {}
		void Status(const std::string&) override {}
		void Step(int, int) override {}
		bool Canceled() const override { return false; }
	};

	struct Metrics
	{
		double lengthSquares = 0.0, lengthSum = 0.0, barSquares = 0.0, barSum = 0.0, planeSquares = 0.0, raySum = 0.0;
		long long lengths = 0, bars = 0, corners = 0;

		void Add(const Board& board, const smcp::CalibrationData& calib)
		{
			std::vector<cv::Point3d> points(board.world.size());
			const cv::Mat Rt = calib.R.t();
			for (std::size_t i = 0; i < points.size(); i++)
			{
				double distance = 0.0;
				smcp::Scan3d::TriangulateStereo(calib.camK, calib.camKc, calib.projK, calib.projKc, Rt, calib.T, board.camera[i],
					board.projector[i], points[i], &distance);
				raySum += distance;
			}
			for (std::size_t i = 0; i < points.size(); i++)
				for (std::size_t j = i + 1; j < points.size(); j++)
				{
					const double known = cv::norm(board.world[i] - board.world[j]);
					const double error = cv::norm(points[i] - points[j]) - known;
					lengthSquares += error * error;
					lengthSum += error;
					lengths++;
					if (known >= 150.0 && known <= 220.0)
					{
						barSquares += error * error;
						barSum += error;
						bars++;
					}
				}
			// Flatness: smallest eigenvalue of the covariance of the corners.
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
			cv::Mat values;
			cv::eigen(cv::Mat(covariance), values);
			planeSquares += values.at<double>(2);
			corners += static_cast<long long>(points.size());
		}
	};

	/// Triangulated corners of a board and their signed distance to the best plane (positive towards
	/// the camera).
	std::vector<double> PlaneResiduals(const Board& board, const smcp::CalibrationData& calib)
	{
		std::vector<cv::Point3d> points(board.world.size());
		const cv::Mat Rt = calib.R.t();
		for (std::size_t i = 0; i < points.size(); i++)
			smcp::Scan3d::TriangulateStereo(calib.camK, calib.camKc, calib.projK, calib.projKc, Rt, calib.T, board.camera[i],
				board.projector[i], points[i]);
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
		cv::Vec3d normal(vectors.at<double>(2, 0), vectors.at<double>(2, 1), vectors.at<double>(2, 2));
		if (normal[2] > 0)
			normal = -normal;  // towards the camera, which looks along +z
		std::vector<double> residuals(points.size());
		for (std::size_t i = 0; i < points.size(); i++)
			residuals[i] = normal.dot(cv::Vec3d(points[i].x - mean.x, points[i].y - mean.y, points[i].z - mean.z));
		return residuals;
	}

	/// How much of the flatness error is the same in every pose (the mean residual of each corner over
	/// all the boards). A common part can come from the board or from the calibration when the boards
	/// sit in similar positions; calibrating with a bent board model was tried and made the boards
	/// left out of the calibration worse, so in the workspaces studied it is not the board.
	void BoardShape(const std::vector<Board>& boards, const smcp::CalibrationData& calib)
	{
		const std::size_t corners = boards.front().world.size();
		std::vector<double> sum(corners, 0.0);
		std::vector<std::vector<double>> all;
		for (const Board& board : boards)
		{
			if (board.world.size() != corners)
				continue;
			all.push_back(PlaneResiduals(board, calib));
			for (std::size_t i = 0; i < corners; i++)
				sum[i] += all.back()[i];
		}
		double total = 0.0, common = 0.0, rest = 0.0;
		for (std::size_t i = 0; i < corners; i++)
		{
			const double mean = sum[i] / all.size();
			common += mean * mean;
			for (const auto& residuals : all)
			{
				total += residuals[i] * residuals[i];
				rest += (residuals[i] - mean) * (residuals[i] - mean);
			}
		}
		std::printf("\nflatness pattern over %zu boards (classic calibration, all sets): flatness RMS %.1f um; the part common to "
					"every pose %.1f um; what is left without it %.1f um\n",
			all.size(), 1000.0 * std::sqrt(total / (corners * all.size())), 1000.0 * std::sqrt(common / corners),
			1000.0 * std::sqrt(rest / (corners * all.size())));
		// The common part on a coarse grid of the board (rows of corners top to bottom).
		float maxX = 0.f, maxY = 0.f;
		for (const auto& w : boards.front().world)
		{
			maxX = std::max(maxX, w.x);
			maxY = std::max(maxY, w.y);
		}
		std::printf("  common part (um) on a 5 x 5 grid of the board:\n");
		for (int gy = 0; gy < 5; gy++)
		{
			std::printf("   ");
			for (int gx = 0; gx < 5; gx++)
			{
				double cell = 0.0;
				int count = 0;
				for (std::size_t i = 0; i < corners; i++)
				{
					const auto& w = boards.front().world[i];
					if (std::min(4, static_cast<int>(5.f * w.x / (maxX + 1e-3f))) == gx &&
						std::min(4, static_cast<int>(5.f * w.y / (maxY + 1e-3f))) == gy)
					{
						cell += sum[i] / all.size();
						count++;
					}
				}
				std::printf(" %+7.0f", count ? 1000.0 * cell / count : 0.0);
			}
			std::printf("\n");
		}
	}

	struct Variant
	{
		const char* name;
		SolveOptions options;
		bool subpixelCorners = false;
	};
} // namespace

int main(int argc, char** argv)
{
	namespace fs = std::filesystem;
	std::string root, writeFolder;
	int folds = 5;
	bool subpixelCorners = false;
	unsigned window = 60;
	smcp::StructuredLight::DecodeParams decode;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
		if (arg == "--folds") folds = std::max(2, std::stoi(next()));
		else if (arg == "--write") writeFolder = next();
		else if (arg == "--subpixel-corners") subpixelCorners = true;
		else if (arg == "--window") window = static_cast<unsigned>(std::stoi(next()));
		else if (arg == "--b") decode.b = std::stof(next());
		else if (arg == "--m") decode.m = static_cast<unsigned>(std::stoi(next()));
		else root = arg;
	}
	if (root.empty())
	{
		std::cout << "usage: SMCP_calibration_compare <workspace> [--folds N] [--write <folder>] [--subpixel-corners] [--window N] [--b B] [--m M]" << std::endl;
		return 2;
	}

	// Sets in name order, as the application numbers them.
	std::vector<fs::path> sets;
	for (const auto& entry : fs::directory_iterator(fs::u8path(root)))
		if (entry.is_directory())
			sets.push_back(entry.path());
	std::sort(sets.begin(), sets.end());
	std::vector<Board> boards;
	cv::Size imageSize, projectorSize(1024, 768);
	for (int index = 0; index < static_cast<int>(sets.size()); index++)
	{
		char name[3][32];
		std::snprintf(name[0], 32, "world_%02d.txt", index);
		std::snprintf(name[1], 32, "cam_%02d.txt", index);
		std::snprintf(name[2], 32, "proj_%02d.txt", index);
		Board board;
		board.world = ReadPoints<cv::Point3f>(fs::u8path(root) / name[0]);
		board.camera = ReadPoints<cv::Point2f>(fs::u8path(root) / name[1]);
		board.projector = ReadPoints<cv::Point2f>(fs::u8path(root) / name[2]);
		if (board.world.size() < 4 || board.camera.size() != board.world.size() || board.projector.size() != board.world.size())
			continue;
		if (imageSize.area() == 0)
		{
			for (const auto& entry : fs::directory_iterator(sets[index]))
			{
				const cv::Mat image = cv::imread(entry.path().string(), 0);
				if (image.data)
				{
					imageSize = image.size();
					break;
				}
			}
			std::ifstream info(sets[index] / "projector_info.txt");
			int width = 0, height = 0;
			if (info >> width >> height && width > 0 && height > 0)
				projectorSize = cv::Size(width, height);
		}
		board.set = sets[index];
		boards.push_back(board);
	}
	if (boards.size() < 6 || imageSize.area() == 0)
	{
		std::cout << "the workspace needs at least 6 sets with saved corners (found " << boards.size() << ")" << std::endl;
		return 2;
	}
	folds = std::min<int>(folds, static_cast<int>(boards.size()) / 3);
	std::printf("%s: %zu boards, camera %dx%d, projector %dx%d, %d folds\n", root.c_str(), boards.size(), imageSize.width,
		imageSize.height, projectorSize.width, projectorSize.height, folds);

	std::vector<Variant> variants(2);
	variants[0] = { "classic", SolveOptions() };
	variants[1] = { "joint", SolveOptions() };
	variants[1].options.jointRefinement = true;
	if (subpixelCorners)
	{
		// Projector corners again, from the images: with the whole decoded coordinates (to check that
		// the saved ones are reproduced) and with the sub-pixel ones.
		double difference = 0.0, moved = 0.0;
		long long count = 0;
		for (Board& board : boards)
		{
			std::vector<std::string> images;
			for (const auto& entry : fs::directory_iterator(board.set))
			{
				const std::string extension = entry.path().extension().string();
				if (entry.is_regular_file() && (extension == ".png" || extension == ".jpg" || extension == ".bmp"))
					images.push_back(entry.path().u8string());
			}
			std::sort(images.begin(), images.end());
			smcp::StructuredLight::DecodedSet decoded;
			std::string error;
			std::vector<cv::Point2f> whole;
			if (!smcp::StructuredLight::DecodeGraySet(images, projectorSize, decode, decoded, error) ||
				!smcp::Calibration::ProjectorCorners(decoded, board.camera, window, 0, whole, error) ||
				!smcp::Calibration::SubpixelProjectorCorners(images, decoded, board.camera, projectorSize, window, 0,
					board.projectorSubpixel, error))
			{
				std::printf("cannot recompute the projector corners of %s: %s\n", board.set.filename().string().c_str(), error.c_str());
				return 1;
			}
			for (std::size_t i = 0; i < whole.size(); i++)
			{
				difference += cv::norm(whole[i] - board.projector[i]);
				moved += cv::norm(board.projectorSubpixel[i] - whole[i]);
				count++;
			}
		}
		std::printf("projector corners recomputed: %.4f px from the saved ones (mean); sub-pixel coordinates move them %.4f px\n",
			difference / count, moved / count);
		Variant subpx = { "subpx", SolveOptions(), true };
		Variant both = { "subpx+jnt", SolveOptions(), true };
		both.options.jointRefinement = true;
		variants.push_back(subpx);
		variants.push_back(both);
	}

	const auto solve = [&](const std::vector<int>& use, const Variant& variant, smcp::CalibrationData& calib)
	{
		const SolveOptions& options = variant.options;
		std::vector<std::vector<cv::Point3f>> world;
		std::vector<std::vector<cv::Point2f>> camera, projector;
		for (int i : use)
		{
			world.push_back(boards[i].world);
			camera.push_back(boards[i].camera);
			projector.push_back(variant.subpixelCorners ? boards[i].projectorSubpixel : boards[i].projector);
		}
		std::string error;
		Silent progress;
		if (!smcp::Calibration::Solve(world, camera, projector, imageSize, projectorSize, calib, error, progress, options))
		{
			std::printf("  solve failed: %s\n", error.c_str());
			return false;
		}
		return true;
	};

	std::printf("%-9s | %-36s | %-34s | %-9s | %-9s | %s\n", "variant", "length error, all pairs (um)",
		"length error, 150-220 mm (um)", "flatness", "ray gap", "fit of its own sets (px): cam proj stereo");
	smcp::CalibrationData classic;
	for (const Variant& variant : variants)
	{
		Metrics metrics;
		bool ok = true;
		for (int fold = 0; fold < folds && ok; fold++)
		{
			std::vector<int> train, test;
			for (int i = 0; i < static_cast<int>(boards.size()); i++)
				(i % folds == fold ? test : train).push_back(i);
			smcp::CalibrationData calib;
			ok = solve(train, variant, calib);
			for (int i : test)
				if (ok)
				{
					// A board is judged with the projector corners of its own variant.
					Board board = boards[i];
					if (variant.subpixelCorners)
						board.projector = board.projectorSubpixel;
					metrics.Add(board, calib);
				}
		}
		std::vector<int> all(boards.size());
		for (int i = 0; i < static_cast<int>(all.size()); i++)
			all[i] = i;
		smcp::CalibrationData full;
		if (!ok || !solve(all, variant, full))
			continue;
		std::printf("%-9s | rms %7.1f  mean %+7.1f            | rms %7.1f  mean %+7.1f          | %6.1f um | %6.1f um | %.3f %.3f %.3f\n",
			variant.name, 1000.0 * std::sqrt(metrics.lengthSquares / metrics.lengths), 1000.0 * metrics.lengthSum / metrics.lengths,
			1000.0 * std::sqrt(metrics.barSquares / std::max(1LL, metrics.bars)), 1000.0 * metrics.barSum / std::max(1LL, metrics.bars),
			1000.0 * std::sqrt(metrics.planeSquares / metrics.corners), 1000.0 * metrics.raySum / metrics.corners, full.camError,
			full.projError, full.stereoError);
		if (!writeFolder.empty())
			full.SaveCalibration(QString::fromStdString(writeFolder + "/" + variant.name + ".yml"));
		if (std::string(variant.name) == "classic")
			classic = full;
	}
	if (classic.IsValid())
		BoardShape(boards, classic);
	return 0;
}
