// Looks for periodic errors in the projector coordinates decoded on a flat chessboard. It only
// reads the workspace: nothing is written into it.
//
//   SMCP_band_analysis <workspace> <set index> [--b B] [--m M] [--threshold N] [--classic] [--fold N]
//
// On a plane the projector coordinate of a camera pixel is a smooth function of the pixel (a
// homography bent a little by the two lenses). The tool fits a 4th-degree polynomial to the
// sub-pixel coordinates inside the board (cam_NN.txt) and studies what is left:
//   * its mean per projector column (row) and the strongest periods of that profile;
//   * its mean near each stripe border, grouped by the Gray bit that changes there;
//   * its mean by position inside a projector pixel.
// A period that is a power of two, or a bias that depends on the bit, points at the decoding;
// any other period points at the projector (for example a resampled image).
// With --classic the whole decoded coordinates are analysed instead of the sub-pixel ones.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/imgproc/imgproc.hpp>

#include "core/StructuredLight.h"

namespace
{
	struct Sample
	{
		float x, y;	   ///< Camera pixel, normalized to about [-1, 1].
		float value;   ///< Projector coordinate.
		float residual;
	};

	int g_fold = 0;  ///< --fold N: also print the profile folded with period N.

	constexpr int Degree = 4;
	constexpr int Terms = (Degree + 1) * (Degree + 2) / 2;

	void Monomials(float x, float y, double* out)
	{
		int k = 0;
		for (int i = 0; i <= Degree; i++)
			for (int j = 0; i + j <= Degree; j++)
				out[k++] = std::pow(x, i) * std::pow(y, j);
	}

	/// Least-squares polynomial; two more passes without the samples beyond three deviations.
	double FitSmooth(std::vector<Sample>& samples)
	{
		std::vector<char> keep(samples.size(), 1);
		double rms = 0.0;
		for (int pass = 0; pass < 3; pass++)
		{
			cv::Mat ata = cv::Mat::zeros(Terms, Terms, CV_64F), atb = cv::Mat::zeros(Terms, 1, CV_64F);
			double m[Terms];
			for (size_t s = 0; s < samples.size(); s++)
			{
				if (!keep[s])
					continue;
				Monomials(samples[s].x, samples[s].y, m);
				for (int i = 0; i < Terms; i++)
				{
					atb.at<double>(i) += m[i] * samples[s].value;
					for (int j = i; j < Terms; j++)
						ata.at<double>(i, j) += m[i] * m[j];
				}
			}
			cv::completeSymm(ata);
			cv::Mat coefficients;
			cv::solve(ata, atb, coefficients, cv::DECOMP_SVD);
			double sum = 0.0;
			size_t used = 0;
			for (size_t s = 0; s < samples.size(); s++)
			{
				Monomials(samples[s].x, samples[s].y, m);
				double model = 0.0;
				for (int i = 0; i < Terms; i++)
					model += coefficients.at<double>(i) * m[i];
				samples[s].residual = static_cast<float>(samples[s].value - model);
				if (keep[s])
				{
					sum += samples[s].residual * samples[s].residual;
					used++;
				}
			}
			rms = std::sqrt(sum / std::max<size_t>(1, used));
			for (size_t s = 0; s < samples.size(); s++)
				keep[s] = std::abs(samples[s].residual) <= 3.0 * rms;
		}
		// Samples beyond three deviations are marked so the statistics below skip them.
		for (size_t s = 0; s < samples.size(); s++)
			if (!keep[s])
				samples[s].residual = std::numeric_limits<float>::quiet_NaN();
		return rms;
	}

	void Analyse(const char* name, std::vector<Sample>& samples, int limit, int offset, int totalBits)
	{
		if (samples.size() < 1000)
		{
			std::printf("%s: too few samples\n", name);
			return;
		}
		const double rms = FitSmooth(samples);
		std::printf("\n%s: %zu samples, residual RMS after the smooth fit %.4f projector px\n", name, samples.size(), rms);

		// Mean residual per projector column (row).
		std::vector<double> sum(limit, 0.0);
		std::vector<int> count(limit, 0);
		// ... by position inside the projector pixel ...
		double phaseSum[10] = {};
		int phaseCount[10] = {};
		// ... and near each border, by the bit that changes there.
		std::vector<double> bitSum(totalBits, 0.0), bitSquares(totalBits, 0.0);
		std::vector<int> bitCount(totalBits, 0);
		std::vector<double> borderSum(limit, 0.0);
		std::vector<int> borderCount(limit, 0);
		for (const Sample& s : samples)
		{
			if (std::isnan(s.residual))
				continue;
			const int c = static_cast<int>(std::floor(s.value + 0.5f));
			if (c < 0 || c >= limit)
				continue;
			sum[c] += s.residual;
			count[c]++;
			const float inside = s.value - (c - 0.5f);  // 0..1 across the projector pixel
			const int bin = std::min(9, std::max(0, static_cast<int>(inside * 10)));
			phaseSum[bin] += s.residual;
			phaseCount[bin]++;
			const int border = static_cast<int>(std::floor(s.value));  // border between `border` and `border`+1
			if (std::abs(s.value - (border + 0.5f)) < 0.2f && border >= 0 && border + 1 < limit)
			{
				borderSum[border] += s.residual;
				borderCount[border]++;
			}
		}
		for (int border = 0; border + 1 < limit; border++)
		{
			if (borderCount[border] < 30)
				continue;
			const unsigned n = static_cast<unsigned>(border + offset);
			unsigned changed = (n ^ (n >> 1)) ^ ((n + 1) ^ ((n + 1) >> 1));
			int bit = 0;
			while (changed > 1)
			{
				changed >>= 1;
				bit++;
			}
			const double mean = borderSum[border] / borderCount[border];
			bitSum[bit] += mean;
			bitSquares[bit] += mean * mean;
			bitCount[bit]++;
		}

		std::printf("  by position inside the projector pixel (0 = left/top edge):\n   ");
		for (int bin = 0; bin < 10; bin++)
			std::printf(" %+.3f", phaseCount[bin] ? phaseSum[bin] / phaseCount[bin] : 0.0);
		std::printf("\n  near the border where each Gray bit changes (bit 0 = finest stripes): mean, spread, borders\n");
		for (int bit = 0; bit < totalBits; bit++)
		{
			if (bitCount[bit] < 1)
				continue;
			const double mean = bitSum[bit] / bitCount[bit];
			const double spread = std::sqrt(std::max(0.0, bitSquares[bit] / bitCount[bit] - mean * mean));
			std::printf("    bit %d: %+.4f  %.4f  %d\n", bit, mean, spread, bitCount[bit]);
		}

		// The profile folded with the period given by --fold: one mean per residue of the coordinate.
		if (g_fold > 1)
		{
			std::vector<double> foldSum(g_fold, 0.0);
			std::vector<int> foldCount(g_fold, 0);
			for (int c = 0; c < limit; c++)
				if (count[c] >= 50)
				{
					foldSum[c % g_fold] += sum[c] / count[c];
					foldCount[c % g_fold]++;
				}
			std::printf("  mean by coordinate modulo %d:", g_fold);
			for (int k = 0; k < g_fold; k++)
				std::printf(" %+.3f", foldCount[k] ? foldSum[k] / foldCount[k] : 0.0);
			std::printf("\n");
		}

		// Spectrum of the per-column profile over its longest stretch of well-populated columns.
		int bestStart = 0, bestLength = 0;
		for (int start = 0; start < limit;)
		{
			int end = start;
			while (end < limit && count[end] >= 50)
				end++;
			if (end - start > bestLength)
			{
				bestStart = start;
				bestLength = end - start;
			}
			start = end + 1;
		}
		if (bestLength < 64)
		{
			std::printf("  profile too short for a spectrum (%d)\n", bestLength);
			return;
		}
		std::vector<double> profile(bestLength);
		double profileMean = 0.0, profileSquares = 0.0;
		for (int i = 0; i < bestLength; i++)
		{
			profile[i] = sum[bestStart + i] / count[bestStart + i];
			profileMean += profile[i];
		}
		profileMean /= bestLength;
		for (double& value : profile)
		{
			value -= profileMean;
			profileSquares += value * value;
		}
		std::printf("  profile over projector %d..%d: RMS %.4f px (of the %.4f total)\n", bestStart, bestStart + bestLength - 1,
			std::sqrt(profileSquares / bestLength), rms);
		struct Peak
		{
			double period, amplitude;
		};
		std::vector<Peak> peaks;
		for (double period = 2.0; period <= 128.0; period += (period < 16 ? 0.02 : period < 40 ? 0.05 : 0.25))
		{
			double re = 0.0, im = 0.0;
			for (int i = 0; i < bestLength; i++)
			{
				const double angle = 2.0 * CV_PI * (bestStart + i) / period;
				re += profile[i] * std::cos(angle);
				im += profile[i] * std::sin(angle);
			}
			peaks.push_back({ period, 2.0 * std::sqrt(re * re + im * im) / bestLength });
		}
		// Local maxima only, strongest first.
		std::vector<Peak> maxima;
		for (size_t i = 1; i + 1 < peaks.size(); i++)
			if (peaks[i].amplitude > peaks[i - 1].amplitude && peaks[i].amplitude >= peaks[i + 1].amplitude)
				maxima.push_back(peaks[i]);
		std::sort(maxima.begin(), maxima.end(), [](const Peak& a, const Peak& b) { return a.amplitude > b.amplitude; });
		std::printf("  strongest periods (projector px : amplitude px):");
		for (size_t i = 0; i < std::min<size_t>(8, maxima.size()); i++)
			std::printf("  %.2f:%.4f", maxima[i].period, maxima[i].amplitude);
		std::printf("\n");
	}
} // namespace

int main(int argc, char** argv)
{
	namespace fs = std::filesystem;
	smcp::StructuredLight::DecodeParams decode;
	int threshold = 20, index = -1;
	bool classic = false;
	std::string root;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
		if (arg == "--b") decode.b = std::stof(next());
		else if (arg == "--m") decode.m = static_cast<unsigned>(std::stoi(next()));
		else if (arg == "--threshold") threshold = std::stoi(next());
		else if (arg == "--classic") classic = true;
		else if (arg == "--fold") g_fold = std::stoi(next());
		else if (root.empty()) root = arg;
		else index = std::stoi(arg);
	}
	if (root.empty() || index < 0)
	{
		std::cout << "usage: SMCP_band_analysis <workspace> <set index> [--b B] [--m M] [--threshold N] [--classic] [--fold N]" << std::endl;
		return 2;
	}
	std::vector<fs::path> sets;
	for (const auto& entry : fs::directory_iterator(fs::u8path(root)))
		if (entry.is_directory())
			sets.push_back(entry.path());
	std::sort(sets.begin(), sets.end());
	if (index >= static_cast<int>(sets.size()))
	{
		std::cout << "the workspace has " << sets.size() << " sets" << std::endl;
		return 2;
	}
	char name[32];
	std::snprintf(name, sizeof(name), "cam_%02d.txt", index);
	std::ifstream corners(fs::u8path(root) / name);
	std::vector<cv::Point2f> cornerPixels, board;
	for (float x, y; corners >> x >> y;)
		cornerPixels.emplace_back(x, y);
	if (cornerPixels.size() < 4)
	{
		std::cout << name << " has no corners" << std::endl;
		return 2;
	}
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
	std::vector<cv::Mat> gray;
	if (!smcp::StructuredLight::DecodeGraySet(images, projector, decode, decoded, error) ||
		!smcp::StructuredLight::LoadGraySet(images, gray, error))
	{
		std::cout << "decoding failed: " << error << std::endl;
		return 1;
	}
	const cv::Mat coordinates = classic ? decoded.pattern
		: smcp::StructuredLight::RefinePattern(gray, decoded.pattern, decoded.minMax, projector, threshold);
	const int totalBits = (static_cast<int>(images.size()) / 2 - 1) / 2;
	const int offset[2] = { ((1 << totalBits) - projector.width) / 2, ((1 << totalBits) - projector.height) / 2 };

	std::printf("%s set %d (%s), projector %dx%d, %d bits, %s coordinates\n", root.c_str(), index,
		sets[index].filename().string().c_str(), projector.width, projector.height, totalBits, classic ? "whole" : "sub-pixel");
	const cv::Rect box = cv::boundingRect(board);
	const float cx = box.x + 0.5f * box.width, cy = box.y + 0.5f * box.height;
	const float scale = 2.0f / std::max(box.width, box.height);
	std::vector<Sample> columns, rows;
	for (int y = std::max(0, box.y); y < std::min(coordinates.rows, box.y + box.height); y++)
		for (int x = std::max(0, box.x); x < std::min(coordinates.cols, box.x + box.width); x++)
		{
			const cv::Vec2f& value = coordinates.at<cv::Vec2f>(y, x);
			const cv::Vec2b& range = decoded.minMax.at<cv::Vec2b>(y, x);
			if (range[1] - range[0] < threshold || cv::pointPolygonTest(board, cv::Point2f(static_cast<float>(x), static_cast<float>(y)), false) <= 0)
				continue;
			const float nx = (x - cx) * scale, ny = (y - cy) * scale;
			if (!std::isnan(value[0]))
				columns.push_back({ nx, ny, value[0], 0.f });
			if (!std::isnan(value[1]))
				rows.push_back({ nx, ny, value[1], 0.f });
		}
	Analyse("projector columns", columns, projector.width, offset[0], totalBits);
	Analyse("projector rows", rows, projector.height, offset[1], totalBits);
	return 0;
}
