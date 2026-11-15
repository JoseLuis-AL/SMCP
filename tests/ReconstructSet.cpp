// Reconstructs one capture set with a given calibration, from the command line, to compare
// calibrations and reconstruction methods on existing captures. It only reads the set.
//
//   SMCP_reconstruct_set <set directory> --calibration <file.yml> --out <cloud.xyz>
//                        [--subpixel] [--plane] [--threshold N] [--maxdist D] [--b B] [--m M]
//
// --subpixel: one point per camera pixel; --plane: projector-plane triangulation.
//
// The cloud is written as "x y z r g b" lines, like the XYZ export of the application.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <QString>

#include "core/CalibrationData.h"
#include "core/Scan3d.h"
#include "core/StructuredLight.h"

int main(int argc, char** argv)
{
	namespace fs = std::filesystem;
	smcp::StructuredLight::DecodeParams decode;
	int threshold = 20;
	double maxDist = 100.0;
	bool subpixel = false;
	smcp::Scan3d::Triangulation triangulation = smcp::Scan3d::Triangulation::RayMidpoint;
	std::string set, calibrationPath, output;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
		if (arg == "--calibration") calibrationPath = next();
		else if (arg == "--out") output = next();
		else if (arg == "--subpixel") subpixel = true;
		else if (arg == "--plane") triangulation = smcp::Scan3d::Triangulation::ProjectorPlane;
		else if (arg == "--threshold") threshold = std::stoi(next());
		else if (arg == "--maxdist") maxDist = std::stod(next());
		else if (arg == "--b") decode.b = std::stof(next());
		else if (arg == "--m") decode.m = static_cast<unsigned>(std::stoi(next()));
		else set = arg;
	}
	if (set.empty() || calibrationPath.empty() || output.empty())
	{
		std::cout << "usage: SMCP_reconstruct_set <set directory> --calibration <file.yml> --out <cloud.xyz> [--subpixel] [--plane] "
					 "[--threshold N] [--maxdist D] [--b B] [--m M]"
				  << std::endl;
		return 2;
	}
	smcp::CalibrationData calib;
	if (!calib.LoadCalibration(QString::fromLocal8Bit(calibrationPath.c_str())))
	{
		std::cout << "cannot read the calibration " << calibrationPath << std::endl;
		return 2;
	}

	std::vector<std::string> images;
	for (const auto& entry : fs::directory_iterator(fs::u8path(set)))
	{
		const std::string extension = entry.path().extension().string();
		if (entry.is_regular_file() && (extension == ".png" || extension == ".jpg" || extension == ".bmp"))
			images.push_back(entry.path().u8string());
	}
	std::sort(images.begin(), images.end());
	cv::Size projector(1024, 768);
	{
		std::ifstream info(fs::u8path(set) / "projector_info.txt");
		int width = 0, height = 0;
		if (info >> width >> height && width > 0 && height > 0)
			projector = cv::Size(width, height);
	}

	smcp::StructuredLight::DecodedSet decoded;
	std::string error;
	if (!smcp::StructuredLight::DecodeGraySet(images, projector, decode, decoded, error))
	{
		std::cout << "decoding failed: " << error << std::endl;
		return 1;
	}
	const cv::Mat color = smcp::StructuredLight::GetColorImage(images.front());
	smcp::Scan3d::Pointcloud cloud;
	if (subpixel)
	{
		std::vector<cv::Mat> gray;
		if (!smcp::StructuredLight::LoadGraySet(images, gray, error))
		{
			std::cout << "cannot read the images: " << error << std::endl;
			return 1;
		}
		const cv::Mat refined = smcp::StructuredLight::RefinePattern(gray, decoded.pattern, decoded.minMax, projector, threshold);
		smcp::Scan3d::ReconstructModelSubpixel(cloud, calib, refined, decoded.minMax, color, projector, threshold, maxDist, nullptr,
			triangulation);
	}
	else
	{
		smcp::Scan3d::ReconstructModel(cloud, calib, decoded.pattern, decoded.minMax, color, projector, threshold, maxDist, nullptr,
			triangulation);
	}

	FILE* file = fopen(output.c_str(), "w");
	if (!file || !cloud.points.data)
	{
		std::cout << "nothing to write to " << output << std::endl;
		return 1;
	}
	long long written = 0;
	for (int h = 0; h < cloud.points.rows; h++)
		for (int w = 0; w < cloud.points.cols; w++)
		{
			const cv::Vec3f& p = cloud.points.at<cv::Vec3f>(h, w);
			if (smcp::StructuredLight::Invalid(p))
				continue;
			const cv::Vec3b& c = cloud.colors.at<cv::Vec3b>(h, w);
			std::fprintf(file, "%.5f %.5f %.5f %d %d %d\n", p[0], p[1], p[2], c[2], c[1], c[0]);
			written++;
		}
	fclose(file);
	std::cout << written << " points -> " << output << std::endl;
	return 0;
}
