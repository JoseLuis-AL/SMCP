// Measures, on existing capture sets, how much a sub-pixel reconstruction could gain over the
// current one. It only reads the images: nothing is written next to them.
//
//   SMCP_decode_stats <set directory>... [--b B] [--m M] [--threshold N]
//
// For every set it decodes the Gray code exactly like the application and reports:
//   * how many camera pixels see each projector pixel (the "footprint" the reconstruction
//     averages into one point);
//   * how many lit camera pixels the robust decoding leaves uncertain;
//   * how many footprints are clipped by uncertain or dark neighbours, which biases their centroid;
//   * the noise of the centroids, estimated from their second difference along a projector row
//     (zero on a smooth surface, so what remains is localization error).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/StructuredLight.h"

namespace
{
	double Median(std::vector<double> values)
	{
		if (values.empty())
			return 0.0;
		std::nth_element(values.begin(), values.begin() + values.size() / 2, values.end());
		return values[values.size() / 2];
	}

	/// Robust standard deviation (1.4826 x median absolute deviation): depth edges do not inflate it.
	double RobustSigma(std::vector<double> values)
	{
		if (values.empty())
			return 0.0;
		const double median = Median(values);
		for (double& value : values)
			value = std::abs(value - median);
		return 1.4826 * Median(values);
	}

	double Percent(double part, double whole) { return whole > 0 ? 100.0 * part / whole : 0.0; }

	struct Options
	{
		smcp::StructuredLight::DecodeParams decode;
		int threshold = 20;
	};

	bool Measure(const std::filesystem::path& directory, const Options& options)
	{
		namespace fs = std::filesystem;
		std::vector<std::string> images;
		for (const auto& entry : fs::directory_iterator(directory))
		{
			const std::string extension = entry.path().extension().string();
			if (entry.is_regular_file() && (extension == ".png" || extension == ".jpg" || extension == ".bmp"))
				images.push_back(entry.path().string());
		}
		std::sort(images.begin(), images.end());
		cv::Size projector(1024, 768);
		{
			std::ifstream info(directory / "projector_info.txt");
			int width = 0, height = 0;
			if (info >> width >> height && width > 0 && height > 0)
				projector = cv::Size(width, height);
		}

		smcp::StructuredLight::DecodedSet decoded;
		std::string error;
		if (!smcp::StructuredLight::DecodeGraySet(images, projector, options.decode, decoded, error))
		{
			std::cout << directory.filename().string() << ": decoding failed: " << error << std::endl;
			return false;
		}
		const cv::Mat& pattern = decoded.pattern;
		const cv::Mat& minMax = decoded.minMax;
		const int rows = pattern.rows, cols = pattern.cols;

		// 0 = dark (below the contrast threshold), 1 = lit but uncertain or outside, 2 = usable.
		cv::Mat state(rows, cols, CV_8UC1);
		double lit = 0, usable = 0;
		for (int h = 0; h < rows; h++)
			for (int w = 0; w < cols; w++)
			{
				const cv::Vec2f& p = pattern.at<cv::Vec2f>(h, w);
				const cv::Vec2b& range = minMax.at<cv::Vec2b>(h, w);
				unsigned char value = 0;
				if (range[1] - range[0] >= options.threshold)
				{
					const bool valid = !smcp::StructuredLight::Invalid(p) && p[0] >= 0.f && p[0] < projector.width &&
						p[1] >= 0.f && p[1] < projector.height;
					value = valid ? 2 : 1;
					lit += 1;
					usable += valid ? 1 : 0;
				}
				state.at<unsigned char>(h, w) = value;
			}

		// Footprints: the camera pixels of each projector pixel, as the reconstruction groups them.
		const size_t cells = static_cast<size_t>(projector.width) * projector.height;
		std::vector<unsigned> count(cells, 0);
		std::vector<double> sumX(cells, 0.0), sumY(cells, 0.0);
		std::vector<unsigned char> clipped(cells, 0);
		for (int h = 0; h < rows; h++)
			for (int w = 0; w < cols; w++)
			{
				if (state.at<unsigned char>(h, w) != 2)
					continue;
				const cv::Vec2f& p = pattern.at<cv::Vec2f>(h, w);
				const size_t index = static_cast<size_t>(p[1]) * projector.width + static_cast<size_t>(p[0]);
				count[index]++;
				sumX[index] += w;
				sumY[index] += h;
				// A neighbour that is not usable means part of this footprint is missing.
				const int dh[] = { -1, 1, 0, 0 }, dw[] = { 0, 0, -1, 1 };
				for (int k = 0; k < 4; k++)
				{
					const int hh = h + dh[k], ww = w + dw[k];
					if (hh < 0 || hh >= rows || ww < 0 || ww >= cols || state.at<unsigned char>(hh, ww) != 2)
						clipped[index] = 1;
				}
			}

		std::vector<double> sizes;
		double usedCells = 0, clippedCells = 0, single = 0, small = 0;
		for (size_t index = 0; index < cells; index++)
		{
			if (!count[index])
				continue;
			usedCells += 1;
			clippedCells += clipped[index];
			single += count[index] == 1;
			small += count[index] <= 3;
			sizes.push_back(count[index]);
		}

		// Run lengths of the column code along camera rows and of the row code along camera columns:
		// how many camera pixels one projector pixel spans in each direction.
		std::vector<double> runsX, runsY;
		for (int h = 0; h < rows; h += 4)
		{
			int run = 0;
			float previous = -1.f;
			for (int w = 0; w < cols; w++)
			{
				const bool ok = state.at<unsigned char>(h, w) == 2;
				const float code = ok ? pattern.at<cv::Vec2f>(h, w)[0] : -1.f;
				if (ok && code == previous)
					run++;
				else
				{
					if (run > 0 && previous >= 0.f && ok && std::abs(code - previous) == 1.f)
						runsX.push_back(run);  // a complete run, closed by the next projector column
					run = ok ? 1 : 0;
				}
				previous = code;
			}
		}
		for (int w = 0; w < cols; w += 4)
		{
			int run = 0;
			float previous = -1.f;
			for (int h = 0; h < rows; h++)
			{
				const bool ok = state.at<unsigned char>(h, w) == 2;
				const float code = ok ? pattern.at<cv::Vec2f>(h, w)[1] : -1.f;
				if (ok && code == previous)
					run++;
				else
				{
					if (run > 0 && previous >= 0.f && ok && std::abs(code - previous) == 1.f)
						runsY.push_back(run);
					run = ok ? 1 : 0;
				}
				previous = code;
			}
		}
		double meanRunX = 0, meanRunY = 0;
		for (double run : runsX)
			meanRunX += run;
		for (double run : runsY)
			meanRunY += run;
		meanRunX = runsX.empty() ? 0 : meanRunX / runsX.size();
		meanRunY = runsY.empty() ? 0 : meanRunY / runsY.size();

		// Centroid noise: second difference of the centroids of three consecutive projector pixels.
		// For independent errors of deviation s its deviation is s*sqrt(6).
		std::vector<double> fullX, clippedX, fullY, clippedY;
		for (int r = 0; r < projector.height; r++)
			for (int c = 1; c + 1 < projector.width; c++)
			{
				const size_t i = static_cast<size_t>(r) * projector.width + c;
				if (!count[i - 1] || !count[i] || !count[i + 1])
					continue;
				const double d = sumX[i - 1] / count[i - 1] - 2.0 * sumX[i] / count[i] + sumX[i + 1] / count[i + 1];
				(clipped[i - 1] || clipped[i] || clipped[i + 1] ? clippedX : fullX).push_back(d);
			}
		for (int r = 1; r + 1 < projector.height; r++)
			for (int c = 0; c < projector.width; c++)
			{
				const size_t i = static_cast<size_t>(r) * projector.width + c;
				const size_t up = i - projector.width, down = i + projector.width;
				if (!count[up] || !count[i] || !count[down])
					continue;
				const double d = sumY[up] / count[up] - 2.0 * sumY[i] / count[i] + sumY[down] / count[down];
				(clipped[up] || clipped[i] || clipped[down] ? clippedY : fullY).push_back(d);
			}
		const double root6 = std::sqrt(6.0);

		std::printf("%s\n", directory.filename().string().c_str());
		std::printf("  camera %dx%d, projector %dx%d, %d images\n", cols, rows, projector.width, projector.height,
			static_cast<int>(images.size()));
		std::printf("  lit camera pixels: %.1f%% of the image; uncertain among them: %.1f%%\n",
			Percent(lit, static_cast<double>(rows) * cols), Percent(lit - usable, lit));
		std::printf("  projector pixels reconstructed: %.0f (%.1f%% of the projector)\n", usedCells,
			Percent(usedCells, static_cast<double>(cells)));
		std::printf("  camera pixels per projector pixel: mean %.2f, median %.0f; span %.2f wide x %.2f high\n",
			usedCells > 0 ? usable / usedCells : 0.0, Median(sizes), meanRunX, meanRunY);
		std::printf("  footprints of 1 pixel: %.1f%%; of 3 or fewer: %.1f%%; clipped: %.1f%%\n",
			Percent(single, usedCells), Percent(small, usedCells), Percent(clippedCells, usedCells));
		std::printf("  centroid noise (camera px): complete footprints x %.3f y %.3f (n=%zu); clipped x %.3f y %.3f (n=%zu)\n",
			RobustSigma(fullX) / root6, RobustSigma(fullY) / root6, fullX.size(), RobustSigma(clippedX) / root6,
			RobustSigma(clippedY) / root6, clippedX.size());
		if (meanRunX > 0 && meanRunY > 0)
			std::printf("  the same in projector px: complete x %.3f y %.3f; clipped x %.3f y %.3f\n",
				RobustSigma(fullX) / root6 / meanRunX, RobustSigma(fullY) / root6 / meanRunY,
				RobustSigma(clippedX) / root6 / meanRunX, RobustSigma(clippedY) / root6 / meanRunY);
		return true;
	}
} // namespace

int main(int argc, char** argv)
{
	Options options;
	std::vector<std::string> directories;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
		if (arg == "--b") options.decode.b = std::stof(next());
		else if (arg == "--m") options.decode.m = static_cast<unsigned>(std::stoi(next()));
		else if (arg == "--threshold") options.threshold = std::stoi(next());
		else directories.push_back(arg);
	}
	if (directories.empty())
	{
		std::cout << "usage: SMCP_decode_stats <set directory>... [--b B] [--m M] [--threshold N]" << std::endl;
		return 2;
	}
	std::printf("b %.2f, m %u, threshold %d\n", options.decode.b, options.decode.m, options.threshold);
	bool ok = true;
	for (const std::string& directory : directories)
		ok &= Measure(std::filesystem::u8path(directory), options);
	return ok ? 0 : 1;
}
