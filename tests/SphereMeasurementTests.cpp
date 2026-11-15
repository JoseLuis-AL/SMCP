#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>

#include "core/SphereMeasurement.h"

namespace
{
	using smcp::Pointcloud::ColorCloud;
	using smcp::Pointcloud::ColorPoint;

	bool Check(bool condition, const char* message)
	{
		if (!condition) std::cerr << "FAIL: " << message << '\n';
		return condition;
	}

	ColorPoint Point(double x, double y, double z)
	{
		ColorPoint p;
		p.x = static_cast<float>(x);
		p.y = static_cast<float>(y);
		p.z = static_cast<float>(z);
		return p;
	}

	/// Visible cap of a sphere seen from +Z (the side a camera above the table captures), with noise.
	void AddSphere(ColorCloud& cloud, const cv::Vec3d& center, double radius, double noise, std::mt19937& rng)
	{
		std::normal_distribution<double> gaussian(0.0, noise);
		std::uniform_real_distribution<double> uniform(-1.0, 1.0);
		for (int i = 0; i < 6000; ++i)
		{
			cv::Vec3d n(uniform(rng), uniform(rng), uniform(rng));
			const double length = cv::norm(n);
			if (length < 1e-6 || length > 1.0) continue;
			n *= 1.0 / length;
			if (n[2] < -0.1) continue;
			const cv::Vec3d p = center + (radius + gaussian(rng)) * n;
			cloud.push_back(Point(p[0], p[1], p[2]));
		}
	}

	/// Table plane, three spheres of 50 mm radius resting on it, and scattered outliers.
	ColorCloud Scene(const std::vector<cv::Vec3d>& centers, std::mt19937& rng)
	{
		ColorCloud cloud;
		std::normal_distribution<double> gaussian(0.0, 0.1);
		for (double x = -150; x <= 350; x += 2.5)
			for (double y = -150; y <= 300; y += 2.5)
				cloud.push_back(Point(x, y, gaussian(rng)));
		for (const auto& center : centers)
			AddSphere(cloud, center, 50.0, 0.1, rng);
		std::uniform_real_distribution<double> box(-150.0, 350.0);
		for (int i = 0; i < 1500; ++i)
			cloud.push_back(Point(box(rng), box(rng), std::abs(box(rng)) * 0.3));
		std::shuffle(cloud.begin(), cloud.end(), rng);
		return cloud;
	}
}

int main()
{
	bool success = true;
	std::mt19937 rng(7);

	// Geometric fit with rejection: a noisy cap plus far outliers, starting from a rough guess.
	{
		ColorCloud points;
		AddSphere(points, cv::Vec3d(10, -20, 30), 50.0, 0.05, rng);
		for (int i = 0; i < 300; ++i) points.push_back(Point(10 + i % 7, -20, 30 + 56 + i % 3));  // 6 mm off the surface
		smcp::Pointcloud::SphereModel initial;
		initial.center = cv::Point3f(12, -18, 33);
		initial.radius = 47;
		const auto fit = smcp::Pointcloud::FitSphereGeometric(points, initial, 2.5, 20);
		std::cout << "geometric fit: center error " << cv::norm(fit.center - cv::Vec3d(10, -20, 30)) << ", radius error "
				  << fit.radius - 50.0 << ", kept " << fit.inliers.size() << " of " << points.size() << ", form RMS "
				  << fit.formRms << '\n';
		success &= Check(fit.valid, "geometric fit is valid");
		success &= Check(cv::norm(fit.center - cv::Vec3d(10, -20, 30)) < 0.01, "geometric fit recovers the center");
		success &= Check(std::abs(fit.radius - 50.0) < 0.01, "geometric fit recovers the radius");
		success &= Check(fit.inliers.size() <= points.size() - 300, "outliers are rejected");
		success &= Check(fit.formRms < 0.07, "form error matches the noise");
	}

	// Scene measurement and labeling against the reference artifact.
	const std::vector<cv::Vec3d> centers = { cv::Vec3d(0, 0, 50), cv::Vec3d(200, 10, 50), cv::Vec3d(80, 150, 50) };
	smcp::Pointcloud::ArtifactReference reference;
	reference.names = { "A", "B", "C" };
	reference.diameters = { 100.0, 100.0, 100.0 };
	reference.distances.assign(3, std::vector<double>(3, 0.0));
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			if (i != j) reference.distances[i][j] = cv::norm(centers[i] - centers[j]);

	const ColorCloud scene = Scene(centers, rng);
	smcp::Pointcloud::SphereMeasurementParams params;
	auto spheres = smcp::Pointcloud::MeasureSpheres(scene, params);
	success &= Check(spheres.size() == 3, "three spheres are detected");
	success &= Check(smcp::Pointcloud::LabelSpheres(spheres, reference), "spheres are labeled");
	for (const auto& sphere : spheres)
	{
		if (sphere.label < 0) continue;
		const double centerError = cv::norm(sphere.center - centers[sphere.label]);
		std::cout << reference.names[sphere.label] << ": center error " << centerError << ", diameter error "
				  << sphere.Diameter() - 100.0 << ", points " << sphere.points->size() << ", form RMS " << sphere.formRms
				  << ", form range " << sphere.formRange << '\n';
		success &= Check(centerError < 0.02, "sphere center within 0.02 mm");
		success &= Check(std::abs(sphere.Diameter() - 100.0) < 0.03, "sphere diameter within 0.03 mm");
	}
	success &= Check(spheres.size() == 3 && spheres[0].label == 0 && spheres[1].label == 1 && spheres[2].label == 2,
		"spheres are sorted by label");

	// Fixed scale: measurands of the labeled scene, then the same scene scaled by 1.008.
	{
		const auto measurands = smcp::Pointcloud::Measurands(spheres, reference);
		success &= Check(measurands.size() == 6, "three diameters and three distances are measurands");
		auto scaled = spheres;
		for (auto& sphere : scaled)
		{
			sphere.center *= 1.008;
			sphere.radius *= 1.008;
		}
		const double k = smcp::Pointcloud::FixedScale(smcp::Pointcloud::Measurands(scaled, reference));
		std::cout << "fixed scale for a +0.8 % error: " << k << '\n';
		success &= Check(std::abs(k * 1.008 - 1.0) < 2e-4, "fixed scale undoes a uniform scale error");
		success &= Check(smcp::Pointcloud::FixedScale({}) == 1.0, "no measurands means no correction");
	}

	// Coverage: centers moving along a tilted line correlate Y and Z; an independent Y breaks it.
	{
		std::vector<cv::Vec3d> line, spread;
		for (int i = 0; i < 10; ++i)
		{
			line.emplace_back(10.0 * (i % 3), 5.0 * i, 900.0 + 30.0 * i);
			spread.emplace_back(10.0 * (i % 3), 40.0 * ((i * 7) % 5), 900.0 + 30.0 * i);
		}
		const auto tilted = smcp::Pointcloud::Coverage(line);
		const auto independent = smcp::Pointcloud::Coverage(spread);
		std::cout << "coverage: tilted YZ " << tilted.correlationYZ << ", independent YZ " << independent.correlationYZ
				  << ", Z range " << tilted.Range()[2] << '\n';
		success &= Check(tilted.correlationYZ > 0.99, "a tilted line correlates Y and Z");
		success &= Check(std::abs(independent.correlationYZ) < 0.5, "independent heights do not correlate");
		success &= Check(std::abs(tilted.Range()[2] - 270.0) < 1e-9, "range is max minus min");
	}

	// Labeling cannot work with a different number of spheres.
	auto two = spheres;
	two.pop_back();
	success &= Check(!smcp::Pointcloud::LabelSpheres(two, reference), "labeling needs every reference sphere");

	std::cout << (success ? "All sphere measurement tests passed.\n" : "Sphere measurement tests failed.\n");
	return success ? 0 : 1;
}
