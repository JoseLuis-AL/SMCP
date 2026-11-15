#include <chrono>
#include <functional>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>

#include "core/PointcloudOps.h"

namespace
{
	using smcp::Pointcloud::ColorCloud;
	using smcp::Pointcloud::ColorPoint;

	bool Check(bool condition, const char* message)
	{
		if (!condition) std::cerr << "FAIL: " << message << '\n';
		return condition;
	}

	/// Wavy surface sampled on a grid in [x0, x1) x [0, 100): its curvature constrains all six degrees of freedom.
	ColorCloud Surface(float x0, float x1)
	{
		ColorCloud cloud;
		for (float x = x0; x < x1; x += 1.0f)
		{
			for (float y = 0.0f; y < 100.0f; y += 1.0f)
			{
				ColorPoint p;
				p.x = x;
				p.y = y;
				p.z = 10.0f * std::sin(x / 15.0f) + 8.0f * std::cos(y / 12.0f) + 0.002f * x * y;
				cloud.push_back(p);
			}
		}
		return cloud;
	}

	cv::Matx44d Displacement()
	{
		// About 6 degrees around a tilted axis plus a translation of a few units.
		const double a = 0.06, b = -0.04, g = 0.08;
		const cv::Matx33d rx(1, 0, 0, 0, std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a));
		const cv::Matx33d ry(std::cos(b), 0, std::sin(b), 0, 1, 0, -std::sin(b), 0, std::cos(b));
		const cv::Matx33d rz(std::cos(g), -std::sin(g), 0, std::sin(g), std::cos(g), 0, 0, 0, 1);
		const cv::Matx33d r = rz * ry * rx;
		return cv::Matx44d(r(0, 0), r(0, 1), r(0, 2), 3.0, r(1, 0), r(1, 1), r(1, 2), -2.0, r(2, 0), r(2, 1), r(2, 2), 4.0,
			0, 0, 0, 1);
	}

	/// Largest distance between the points of `original` and the same points displaced and then registered.
	double MaxError(const ColorCloud& original, const cv::Matx44d& displacement, const cv::Matx44d& registration)
	{
		const auto moved = smcp::Pointcloud::TransformCloud(original, registration * displacement);
		double maxError = 0.0;
		for (std::size_t i = 0; i < original.size(); ++i)
		{
			const double dx = (*moved)[i].x - original[i].x;
			const double dy = (*moved)[i].y - original[i].y;
			const double dz = (*moved)[i].z - original[i].z;
			maxError = std::max(maxError, std::sqrt(dx * dx + dy * dy + dz * dz));
		}
		return maxError;
	}

	/// Gaussian noise with standard deviation `sigma` on every coordinate.
	ColorCloud Noisy(ColorCloud cloud, float sigma, unsigned seed)
	{
		std::mt19937 rng(seed);
		std::normal_distribution<float> noise(0.0f, sigma);
		for (ColorPoint& p : cloud)
		{
			p.x += noise(rng);
			p.y += noise(rng);
			p.z += noise(rng);
		}
		return cloud;
	}

	bool Registers(const char* name, const ColorCloud& sourceSurface, const ColorCloud& target,
		const smcp::Pointcloud::IcpParams& params, double tolerance = 0.01)
	{
		const cv::Matx44d displacement = Displacement();
		const auto source = smcp::Pointcloud::TransformCloud(sourceSurface, displacement);
		const auto result = smcp::Pointcloud::AlignIcp(*source, target, params);
		const double error = result.valid ? MaxError(sourceSurface, displacement, result.transform) : -1.0;
		std::cout << name << ": valid=" << result.valid << " converged=" << result.converged
				  << " iterations=" << result.iterations << " rmse=" << result.rmse << " maxError=" << error << '\n';

		bool success = true;
		success &= Check(result.valid, "registration is valid");
		success &= Check(result.converged, "registration converges");
		success &= Check(result.valid && error < tolerance, "registration recovers the displacement");
		return success;
	}
}

int main()
{
	bool success = true;
	const ColorCloud full = Surface(0.0f, 100.0f);

	smcp::Pointcloud::IcpParams params;
	params.overlapRatio = 1.0;
	success &= Registers("point-to-plane", full, full, params);

	params.pointToPlane = false;
	params.maxIterations = 200;
	success &= Registers("point-to-point", full, full, params);

	// Partial overlap: the source covers x in [0, 70), the target x in [30, 100).
	params.pointToPlane = true;
	params.maxIterations = 50;
	params.overlapRatio = 0.5;
	success &= Registers("partial overlap", Surface(0.0f, 70.0f), Surface(30.0f, 100.0f), params);

	// Scanner-like noise with the editor's default parameters; the error is measured on the noisy points.
	success &= Registers("noisy, defaults", Noisy(full, 0.1f, 1), Noisy(full, 0.1f, 2), smcp::Pointcloud::IcpParams(), 0.1);

	const auto identity = smcp::Pointcloud::AlignIcp(full, full, smcp::Pointcloud::IcpParams());
	success &= Check(identity.valid && identity.converged && identity.rmse < 1e-6, "identical clouds need no motion");

	const auto empty = smcp::Pointcloud::AlignIcp(ColorCloud(), full, smcp::Pointcloud::IcpParams());
	success &= Check(!empty.valid, "an empty source is rejected");

	{
		// Large clouds: hypotheses are scored by several threads. The fit must be exact and repeatable.
		ColorCloud scene;
		std::mt19937 rng(11);
		std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
		std::normal_distribution<float> noise(0.0f, 0.01f);
		for (int i = 0; i < 300000; ++i)
		{   // plane z = 0.1 x - 0.2 y + 5
			ColorPoint p;
			p.x = 100.0f * unit(rng);
			p.y = 100.0f * unit(rng);
			p.z = 0.1f * p.x - 0.2f * p.y + 5.0f + noise(rng);
			scene.push_back(p);
		}
		for (int i = 0; i < 150000; ++i)
		{   // sphere of radius 20 centered at (10, -30, 80)
			cv::Vec3f d(unit(rng), unit(rng), unit(rng));
			const float length = static_cast<float>(cv::norm(d));
			if (length < 1e-3f)
				continue;
			d *= (20.0f + noise(rng)) / length;
			ColorPoint p;
			p.x = 10.0f + d[0];
			p.y = -30.0f + d[1];
			p.z = 80.0f + d[2];
			scene.push_back(p);
		}

		smcp::Pointcloud::PlaneFitParams planeParams;
		planeParams.maxPlanes = 1;
		planeParams.distanceThreshold = 0.05;
		smcp::Pointcloud::ColorCloudPtr rest;
		const auto planes = smcp::Pointcloud::FitPlanes(scene, planeParams, &rest);
		success &= Check(planes.size() == 1, "one plane found in a large cloud");
		if (planes.size() == 1)
		{
			const auto& model = planes[0].model;
			const cv::Vec3f expected = cv::normalize(cv::Vec3f(0.1f, -0.2f, -1.0f));
			success &= Check(std::fabs(std::fabs(model.normal.dot(expected)) - 1.0f) < 1e-4f, "plane normal of a large cloud");
			// Every point within the threshold of the returned model, and no other, is an inlier.
			std::size_t inliers = 0;
			for (const ColorPoint& p : scene)
				inliers += std::fabs(model.normal.dot(cv::Vec3f(p.x, p.y, p.z)) + model.d) < planeParams.distanceThreshold;
			success &= Check(planes[0].points->size() == inliers, "plane inliers of a large cloud are exact");
			success &= Check(rest && rest->size() == scene.size() - inliers, "remaining points of a large cloud");

			const auto again = smcp::Pointcloud::FitPlanes(scene, planeParams);
			success &= Check(again.size() == 1 && again[0].model.normal == model.normal && again[0].model.d == model.d
					&& again[0].points->size() == planes[0].points->size(), "large plane fit is repeatable");
		}

		smcp::Pointcloud::SphereFitParams sphereParams;
		sphereParams.maxSpheres = 1;
		sphereParams.distanceThreshold = 0.05;
		sphereParams.minRadius = 5.0;
		sphereParams.maxRadius = 50.0;
		const auto spheres = smcp::Pointcloud::FitSpheres(rest ? *rest : scene, sphereParams);
		success &= Check(spheres.size() == 1 && std::fabs(spheres[0].model.radius - 20.0f) < 0.01f
				&& cv::norm(spheres[0].model.center - cv::Point3f(10.0f, -30.0f, 80.0f)) < 0.01, "sphere found in a large cloud");

		// A transform is applied by several threads: every point must match the direct formula.
		const cv::Matx44d motion = Displacement();
		const auto moved = smcp::Pointcloud::TransformCloud(scene, motion);
		bool same = moved && moved->size() == scene.size();
		for (std::size_t i = 0; same && i < scene.size(); ++i)
		{
			const ColorPoint& p = scene[i];
			const float x = static_cast<float>(motion(0, 0) * p.x + motion(0, 1) * p.y + motion(0, 2) * p.z + motion(0, 3));
			const float y = static_cast<float>(motion(1, 0) * p.x + motion(1, 1) * p.y + motion(1, 2) * p.z + motion(1, 3));
			const float z = static_cast<float>(motion(2, 0) * p.x + motion(2, 1) * p.y + motion(2, 2) * p.z + motion(2, 3));
			same = (*moved)[i].x == x && (*moved)[i].y == y && (*moved)[i].z == z && (*moved)[i].r == p.r;
		}
		success &= Check(same, "transform of a large cloud matches the direct formula");
	}

	{
		// Cancellation: a set flag makes every long operation leave with OperationCanceled, and a
		// flag that is never set changes nothing.
		using smcp::Pointcloud::CancelFlag;
		using smcp::Pointcloud::OperationCanceled;
		const auto cancels = [](const char* name, const std::function<void(const CancelFlag*)>& run)
		{
			CancelFlag flag(true);
			bool canceled = false;
			try
			{
				run(&flag);
			}
			catch (const OperationCanceled&)
			{
				canceled = true;
			}
			if (!canceled)
				std::cerr << "FAIL: " << name << " ignores a cancel request\n";
			return canceled;
		};
		success &= cancels("outlier removal",
			[&](const CancelFlag* c) { smcp::Pointcloud::RemoveStatisticalOutliers(full, smcp::Pointcloud::OutlierRemovalParams(), c); });
		success &= cancels("plane fit",
			[&](const CancelFlag* c) { smcp::Pointcloud::FitPlanes(full, smcp::Pointcloud::PlaneFitParams(), nullptr, c); });
		success &= cancels("sphere fit",
			[&](const CancelFlag* c) { smcp::Pointcloud::FitSpheres(full, smcp::Pointcloud::SphereFitParams(), nullptr, c); });
		success &= cancels("ICP",
			[&](const CancelFlag* c) { smcp::Pointcloud::AlignIcp(full, full, smcp::Pointcloud::IcpParams(), c); });

		CancelFlag idle(false);
		const auto withFlag = smcp::Pointcloud::RemoveStatisticalOutliers(full, smcp::Pointcloud::OutlierRemovalParams(), &idle);
		const auto withoutFlag = smcp::Pointcloud::RemoveStatisticalOutliers(full, smcp::Pointcloud::OutlierRemovalParams());
		success &= Check(withFlag->size() == withoutFlag->size(), "an unset cancel flag does not change the result");

		// A request that arrives while RANSAC is running stops it long before it would finish:
		// spheres are searched for in a cloud that has none, so every iteration is spent.
		ColorCloud noise;
		std::mt19937 rng(3);
		std::uniform_real_distribution<float> unit(-100.0f, 100.0f);
		for (int i = 0; i < 400000; ++i)
		{
			ColorPoint p;
			p.x = unit(rng);
			p.y = unit(rng);
			p.z = unit(rng);
			noise.push_back(p);
		}
		smcp::Pointcloud::SphereFitParams slow;
		slow.maxIterations = 200000;
		CancelFlag flag(false);
		std::thread canceller([&flag]()
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(150));
				flag.store(true);
			});
		const auto started = std::chrono::steady_clock::now();
		bool stopped = false;
		try
		{
			smcp::Pointcloud::FitSpheres(noise, slow, nullptr, &flag);
		}
		catch (const OperationCanceled&)
		{
			stopped = true;
		}
		canceller.join();
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
		success &= Check(stopped && seconds < 5.0, "a cancel request stops a running RANSAC promptly");
	}

	std::cout << (success ? "All point cloud registration tests passed.\n" : "Point cloud registration tests failed.\n");
	return success ? 0 : 1;
}
