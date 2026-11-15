// Measures the spheres of point-cloud files against a reference artifact, like Measure > Spheres
// in the editor but from the command line, to compare processing options on existing clouds.
// It only reads the clouds.
//
//   SMCP_measure_spheres --artifact <artifact.json> [--tolerance T] [--ransac D] [--brief]
//                        [--export <folder>] <cloud.xyz>...
//
// The nominal diameter is the mean of the artifact's diameters, and --tolerance (a fraction, 0.25
// by default) must be wide enough to accept all of them. For every cloud it prints the error of
// each diameter and center distance and the form RMS of each sphere, in micrometres, and one
// summary line; --brief prints the summary line only.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <opencv2/flann/flann.hpp>

#include "core/SphereMeasurement.h"

namespace
{
	using namespace smcp::Pointcloud;

	bool LoadArtifact(const QString& path, ArtifactReference& reference)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
			return false;
		const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
		QStringList names;
		for (const QJsonValue& value : root.value("spheres").toArray())
		{
			names << value.toObject().value("name").toString();
			reference.names.push_back(names.back().toStdString());
			reference.diameters.push_back(value.toObject().value("diameter").toDouble());
		}
		const std::size_t count = reference.names.size();
		reference.distances.assign(count, std::vector<double>(count, 0.0));
		for (const QJsonValue& value : root.value("distances").toArray())
		{
			const int from = names.indexOf(value.toObject().value("from").toString());
			const int to = names.indexOf(value.toObject().value("to").toString());
			if (from >= 0 && to >= 0 && from != to)
				reference.distances[from][to] = reference.distances[to][from] = value.toObject().value("value").toDouble();
		}
		return count >= 2;
	}

	bool ReadXyz(const std::string& path, ColorCloud& cloud)
	{
		FILE* file = fopen(path.c_str(), "r");
		if (!file)
			return false;
		char line[512];
		while (fgets(line, sizeof(line), file))
		{
			ColorPoint p;
			if (sscanf(line, "%f %f %f", &p.x, &p.y, &p.z) == 3)
				cloud.push_back(p);
		}
		fclose(file);
		return !cloud.empty();
	}

	/// Median distance from a point to its nearest neighbour, over a sample of the points.
	double Spacing(const ColorCloud& points)
	{
		if (points.size() < 10)
			return 0.0;
		cv::Mat data(static_cast<int>(points.size()), 3, CV_32F);
		for (int i = 0; i < data.rows; i++)
		{
			data.at<float>(i, 0) = points[i].x;
			data.at<float>(i, 1) = points[i].y;
			data.at<float>(i, 2) = points[i].z;
		}
		cv::flann::Index index(data, cv::flann::KDTreeIndexParams(1));
		const int step = std::max(1, data.rows / 2000);
		std::vector<double> distances;
		for (int i = 0; i < data.rows; i += step)
		{
			cv::Mat indices, squared;
			index.knnSearch(data.row(i), indices, squared, 2, cv::flann::SearchParams(64));
			distances.push_back(std::sqrt(squared.at<float>(0, 1)));
		}
		std::nth_element(distances.begin(), distances.begin() + distances.size() / 2, distances.end());
		return distances[distances.size() / 2];
	}
} // namespace

int main(int argc, char** argv)
{
	SphereMeasurementParams params;
	params.radiusTolerance = 0.25;
	ArtifactReference reference;
	bool brief = false;
	std::string exportFolder;  // --export: writes <cloud>.spheres.xyz (x y z label) and <cloud>.scan.json there
	std::vector<std::string> files;
	for (int i = 1; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
		if (arg == "--artifact")
		{
			if (!LoadArtifact(QString::fromLocal8Bit(next().c_str()), reference))
			{
				std::printf("cannot read the artifact\n");
				return 2;
			}
		}
		else if (arg == "--tolerance") params.radiusTolerance = std::stod(next());
		else if (arg == "--ransac") params.ransacThreshold = std::stod(next());
		else if (arg == "--brief") brief = true;
		else if (arg == "--export") exportFolder = next();
		else files.push_back(arg);
	}
	if (reference.Count() < 2 || files.empty())
	{
		std::printf("usage: SMCP_measure_spheres --artifact <artifact.json> [--tolerance T] [--ransac D] [--brief] <cloud.xyz>...\n");
		return 2;
	}
	double nominal = 0.0;
	for (double diameter : reference.diameters)
		nominal += diameter;
	params.nominalDiameter = nominal / reference.Count();
	params.expectedSpheres = reference.Count();

	for (const std::string& path : files)
	{
		const std::string name = path.substr(path.find_last_of("/\\") + 1);
		ColorCloud cloud;
		if (!ReadXyz(path, cloud))
		{
			std::printf("SUMMARY %s | cannot read\n", name.c_str());
			continue;
		}
		std::vector<MeasuredSphere> spheres = MeasureSpheres(cloud, params);
		const bool labeled = LabelSpheres(spheres, reference);
		if (!labeled)
		{
			std::printf("SUMMARY %s | points %zu | %zu of %zu spheres found, not identified\n", name.c_str(), cloud.size(),
				spheres.size(), reference.Count());
			continue;
		}
		double diameterSquares = 0.0, distanceSquares = 0.0, formSum = 0.0, diameterSum = 0.0;
		int diameters = 0, distances = 0;
		std::size_t spherePoints = 0;
		double spacing = 0.0;
		for (const MeasuredSphere& sphere : spheres)
		{
			const double error = (sphere.Diameter() - reference.diameters[sphere.label]) * 1000.0;
			diameterSquares += error * error;
			diameterSum += error;
			formSum += sphere.formRms * 1000.0;
			diameters++;
			spherePoints += sphere.points->size();
			spacing += Spacing(*sphere.points);
			if (!brief)
				std::printf("  %-4s diameter error %+8.1f um, form RMS %6.1f um, %zu points\n",
					reference.names[sphere.label].c_str(), error, sphere.formRms * 1000.0, sphere.points->size());
		}
		for (std::size_t a = 0; a < spheres.size(); a++)
			for (std::size_t b = a + 1; b < spheres.size(); b++)
			{
				const double known = reference.distances[spheres[a].label][spheres[b].label];
				if (known <= 0.0)
					continue;
				const double error = (cv::norm(spheres[a].center - spheres[b].center) - known) * 1000.0;
				distanceSquares += error * error;
				distances++;
				if (!brief)
					std::printf("  %s-%s distance error %+8.1f um\n", reference.names[spheres[a].label].c_str(),
						reference.names[spheres[b].label].c_str(), error);
			}
		if (!exportFolder.empty())
		{
			// The sphere points with their label and a description of the scan, in the layout of
			// "Export Training Data" (the manifest is assembled from the .scan.json files).
			const std::string base = exportFolder + "/" + name.substr(0, name.find_last_of('.'));
			FILE* points = fopen((base + ".spheres.xyz").c_str(), "w");
			FILE* scan = fopen((base + ".scan.json").c_str(), "w");
			if (points && scan)
			{
				std::fprintf(scan, "{\"name\": \"%s\", \"points_file\": \"%s.spheres.xyz\", \"spheres\": [", name.c_str(),
					name.substr(0, name.find_last_of('.')).c_str());
				for (std::size_t i = 0; i < spheres.size(); i++)
				{
					const MeasuredSphere& sphere = spheres[i];
					for (const ColorPoint& p : *sphere.points)
						std::fprintf(points, "%.6f %.6f %.6f %s\n", p.x, p.y, p.z, reference.names[sphere.label].c_str());
					std::fprintf(scan, "%s{\"label\": \"%s\", \"center\": [%.6f, %.6f, %.6f], \"diameter\": %.6f, \"points\": %zu}",
						i ? ", " : "", reference.names[sphere.label].c_str(), sphere.center[0], sphere.center[1],
						sphere.center[2], sphere.Diameter(), sphere.points->size());
				}
				std::fprintf(scan, "]}\n");
			}
			if (points)
				fclose(points);
			if (scan)
				fclose(scan);
		}
		const double scale = FixedScale(Measurands(spheres, reference));
		std::printf("SUMMARY %s | points %zu, on spheres %zu, spacing %.3f mm | diameter mean %+.1f rms %.1f um | "
					"distance rms %.1f um | form rms %.1f um | k %.6f\n",
			name.c_str(), cloud.size(), spherePoints, spacing / spheres.size(), diameterSum / diameters,
			std::sqrt(diameterSquares / diameters), distances ? std::sqrt(distanceSquares / distances) : 0.0,
			formSum / diameters, scale);
	}
	return 0;
}
