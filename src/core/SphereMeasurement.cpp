#include "core/SphereMeasurement.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <numeric>

namespace smcp::Pointcloud
{

namespace
{
	/// Levenberg-Marquardt iterations of one geometric fit.
	constexpr int Max_Fit_Iterations = 100;

	/// Width of the band around a detected sphere whose points enter the geometric fit, in RANSAC thresholds.
	constexpr double Candidate_Band = 3.0;

	struct GeometricFit
	{
		cv::Vec3d center;
		double radius = 0.0;
	};

	double SumOfSquares(const ColorCloud& points, const std::vector<int>& indices, const GeometricFit& fit)
	{
		double sum = 0.0;
		for (int i : indices)
		{
			const cv::Vec3d d = cv::Vec3d(points[i].x, points[i].y, points[i].z) - fit.center;
			const double residual = cv::norm(d) - fit.radius;
			sum += residual * residual;
		}
		return sum;
	}

	/// Levenberg-Marquardt on r_i = |p_i - c| - r with Jacobian row (-(p_i - c) / |p_i - c|, -1).
	GeometricFit FitOnce(const ColorCloud& points, const std::vector<int>& indices, GeometricFit fit)
	{
		double lambda = 1e-3;
		double cost = SumOfSquares(points, indices, fit);
		for (int iteration = 0; iteration < Max_Fit_Iterations; ++iteration)
		{
			cv::Matx44d jtj = cv::Matx44d::zeros();
			cv::Vec4d jtr(0.0, 0.0, 0.0, 0.0);
			for (int i : indices)
			{
				const cv::Vec3d d = cv::Vec3d(points[i].x, points[i].y, points[i].z) - fit.center;
				const double length = cv::norm(d);
				if (length < 1e-12)
				{
					continue;
				}
				const cv::Vec4d row(-d[0] / length, -d[1] / length, -d[2] / length, -1.0);
				const double residual = length - fit.radius;
				for (int r = 0; r < 4; ++r)
				{
					for (int c = 0; c < 4; ++c)
					{
						jtj(r, c) += row[r] * row[c];
					}
					jtr[r] += row[r] * residual;
				}
			}

			cv::Matx44d damped = jtj;
			for (int k = 0; k < 4; ++k)
			{
				damped(k, k) += lambda * std::max(jtj(k, k), 1e-12);
			}
			cv::Vec4d step;
			if (!cv::solve(damped, -jtr, step, cv::DECOMP_CHOLESKY))
			{
				break;
			}

			const GeometricFit candidate{ fit.center + cv::Vec3d(step[0], step[1], step[2]), fit.radius + step[3] };
			const double candidateCost = SumOfSquares(points, indices, candidate);
			if (candidateCost < cost)
			{
				const double change = cv::norm(step);
				fit = candidate;
				cost = candidateCost;
				lambda = std::max(lambda * 0.1, 1e-12);
				if (change < 1e-10 * (1.0 + fit.radius))
				{
					break;
				}
			}
			else
			{
				lambda *= 10.0;
				if (lambda > 1e12)
				{
					break;
				}
			}
		}
		return fit;
	}

	double PermutationCost(const std::vector<MeasuredSphere>& spheres, const std::vector<int>& labels,
		const ArtifactReference& reference)
	{
		double cost = 0.0;
		for (std::size_t i = 0; i < spheres.size(); ++i)
		{
			const double diameter = reference.diameters[labels[i]];
			if (diameter > 0.0)
			{
				const double e = spheres[i].Diameter() - diameter;
				cost += e * e;
			}
			for (std::size_t j = i + 1; j < spheres.size(); ++j)
			{
				const double distance = reference.distances[labels[i]][labels[j]];
				if (distance > 0.0)
				{
					const double e = cv::norm(spheres[i].center - spheres[j].center) - distance;
					cost += e * e;
				}
			}
		}
		return cost;
	}
} // namespace

SphereFitResult FitSphereGeometric(const ColorCloud& points, const SphereModel& initial, double rejectionSigma,
	int maxRejectionIterations)
{
	SphereFitResult result;
	std::vector<int> all;
	all.reserve(points.size());
	for (int i = 0; i < static_cast<int>(points.size()); ++i)
	{
		if (IsFinite(points[i]))
		{
			all.push_back(i);
		}
	}
	if (all.size() < 4)
	{
		return result;
	}

	GeometricFit fit{ cv::Vec3d(initial.center.x, initial.center.y, initial.center.z), initial.radius };
	std::vector<int> kept = all;
	std::vector<double> residuals(points.size(), 0.0);
	for (int iteration = 0; iteration <= std::max(0, maxRejectionIterations); ++iteration)
	{
		fit = FitOnce(points, kept, fit);
		result.rejectionIterations = iteration;

		double sumSquares = 0.0;
		for (int i : kept)
		{
			const cv::Vec3d d = cv::Vec3d(points[i].x, points[i].y, points[i].z) - fit.center;
			residuals[i] = cv::norm(d) - fit.radius;
			sumSquares += residuals[i] * residuals[i];
		}
		const double sigma = std::sqrt(sumSquares / static_cast<double>(kept.size()));
		if (iteration == maxRejectionIterations || rejectionSigma <= 0.0 || sigma <= 0.0)
		{
			break;
		}

		// Every candidate is tested again, so points rejected early can come back once the fit moves.
		std::vector<int> next;
		next.reserve(all.size());
		for (int i : all)
		{
			const cv::Vec3d d = cv::Vec3d(points[i].x, points[i].y, points[i].z) - fit.center;
			residuals[i] = cv::norm(d) - fit.radius;
			if (std::abs(residuals[i]) <= rejectionSigma * sigma)
			{
				next.push_back(i);
			}
		}
		if (next == kept || next.size() < 4)
		{
			break;
		}
		kept = std::move(next);
	}

	double sumSquares = 0.0;
	double minResidual = std::numeric_limits<double>::max();
	double maxResidual = -std::numeric_limits<double>::max();
	for (int i : kept)
	{
		const cv::Vec3d d = cv::Vec3d(points[i].x, points[i].y, points[i].z) - fit.center;
		const double residual = cv::norm(d) - fit.radius;
		sumSquares += residual * residual;
		minResidual = std::min(minResidual, residual);
		maxResidual = std::max(maxResidual, residual);
	}
	result.valid = std::isfinite(fit.radius) && fit.radius > 0.0 && cv::checkRange(fit.center);
	result.center = fit.center;
	result.radius = fit.radius;
	result.inliers = std::move(kept);
	result.formRms = std::sqrt(sumSquares / static_cast<double>(result.inliers.size()));
	result.formRange = maxResidual - minResidual;
	return result;
}

std::vector<MeasuredSphere> MeasureSpheres(const ColorCloud& cloud, const SphereMeasurementParams& params,
	const CancelFlag* cancel)
{
	std::vector<MeasuredSphere> measured;

	ColorCloudPtr scene = std::make_shared<ColorCloud>();
	scene->reserve(cloud.size());
	std::copy_if(cloud.begin(), cloud.end(), std::back_inserter(*scene), [](const ColorPoint& p) { return IsFinite(p); });
	if (params.removePlanes)
	{
		ColorCloudPtr remaining;
		FitPlanes(*scene, PlaneFitParams(), &remaining, cancel);
		if (remaining)
		{
			scene = remaining;
		}
	}

	const double nominalRadius = 0.5 * params.nominalDiameter;
	SphereFitParams detection;
	detection.maxSpheres = static_cast<int>(params.expectedSpheres);
	detection.maxIterations = params.ransacIterations;
	detection.distanceThreshold = params.ransacThreshold;
	detection.minRadius = nominalRadius * (1.0 - params.radiusTolerance);
	detection.maxRadius = nominalRadius * (1.0 + params.radiusTolerance);
	const std::vector<SphereFit> detected = FitSpheres(*scene, detection, nullptr, cancel);

	// The refinement starts from each detection and uses every scene point near its surface, not only the
	// RANSAC inliers, so the geometric fit and the rejection decide which points belong to the sphere.
	const double band = Candidate_Band * params.ransacThreshold;
	for (const SphereFit& sphere : detected)
	{
		ThrowIfCanceled(cancel);
		ColorCloud candidates;
		const cv::Vec3d center(sphere.model.center.x, sphere.model.center.y, sphere.model.center.z);
		for (const ColorPoint& p : *scene)
		{
			if (std::abs(cv::norm(cv::Vec3d(p.x, p.y, p.z) - center) - sphere.model.radius) <= band)
			{
				candidates.push_back(p);
			}
		}

		const SphereFitResult fit = FitSphereGeometric(candidates, sphere.model, params.rejectionSigma,
			params.maxRejectionIterations);
		if (!fit.valid)
		{
			continue;
		}
		MeasuredSphere result;
		result.center = fit.center;
		result.radius = fit.radius;
		result.formRms = fit.formRms;
		result.formRange = fit.formRange;
		result.points = std::make_shared<ColorCloud>();
		result.points->reserve(fit.inliers.size());
		for (int i : fit.inliers)
		{
			result.points->push_back(candidates[i]);
		}
		measured.push_back(std::move(result));
	}
	return measured;
}

bool LabelSpheres(std::vector<MeasuredSphere>& spheres, const ArtifactReference& reference)
{
	const std::size_t count = reference.Count();
	if (count == 0 || count > 8 || spheres.size() != count || reference.diameters.size() != count
		|| reference.distances.size() != count
		|| std::any_of(reference.distances.begin(), reference.distances.end(),
			[count](const std::vector<double>& row) { return row.size() != count; }))
	{
		return false;
	}

	std::vector<int> labels(count);
	std::iota(labels.begin(), labels.end(), 0);
	std::vector<int> best = labels;
	double bestCost = std::numeric_limits<double>::max();
	do
	{
		const double cost = PermutationCost(spheres, labels, reference);
		if (cost < bestCost)
		{
			bestCost = cost;
			best = labels;
		}
	} while (std::next_permutation(labels.begin(), labels.end()));

	for (std::size_t i = 0; i < count; ++i)
	{
		spheres[i].label = best[i];
	}
	std::sort(spheres.begin(), spheres.end(),
		[](const MeasuredSphere& a, const MeasuredSphere& b) { return a.label < b.label; });
	return true;
}

std::vector<Measurand> Measurands(const std::vector<MeasuredSphere>& spheres, const ArtifactReference& reference)
{
	std::vector<Measurand> measurands;
	const auto known = [&](int label) { return label >= 0 && label < static_cast<int>(reference.Count()); };
	for (const MeasuredSphere& sphere : spheres)
	{
		if (known(sphere.label) && reference.diameters.size() == reference.Count() && reference.diameters[sphere.label] > 0.0)
		{
			measurands.push_back(Measurand{ sphere.Diameter(), reference.diameters[sphere.label], true });
		}
	}
	for (std::size_t i = 0; i < spheres.size(); ++i)
	{
		for (std::size_t j = i + 1; j < spheres.size(); ++j)
		{
			const int a = spheres[i].label;
			const int b = spheres[j].label;
			if (!known(a) || !known(b) || reference.distances.size() != reference.Count()
				|| reference.distances[a].size() != reference.Count() || reference.distances[a][b] <= 0.0)
			{
				continue;
			}
			measurands.push_back(Measurand{ cv::norm(spheres[i].center - spheres[j].center), reference.distances[a][b], false });
		}
	}
	return measurands;
}

double FixedScale(const std::vector<Measurand>& measurands)
{
	double measuredReference = 0.0;
	double measuredSquared = 0.0;
	for (const Measurand& m : measurands)
	{
		measuredReference += m.measured * m.reference;
		measuredSquared += m.measured * m.measured;
	}
	return measuredSquared > 0.0 ? measuredReference / measuredSquared : 1.0;
}

CoverageStats Coverage(const std::vector<cv::Vec3d>& centers)
{
	CoverageStats stats;
	stats.count = centers.size();
	if (centers.empty())
	{
		return stats;
	}
	stats.minimum = stats.maximum = centers.front();
	cv::Vec3d mean(0.0, 0.0, 0.0);
	for (const cv::Vec3d& c : centers)
	{
		for (int k = 0; k < 3; ++k)
		{
			stats.minimum[k] = std::min(stats.minimum[k], c[k]);
			stats.maximum[k] = std::max(stats.maximum[k], c[k]);
		}
		mean += c;
	}
	if (centers.size() < 3)
	{
		return stats;
	}
	mean *= 1.0 / static_cast<double>(centers.size());

	cv::Matx33d covariance = cv::Matx33d::zeros();
	for (const cv::Vec3d& c : centers)
	{
		const cv::Vec3d d = c - mean;
		covariance += d * d.t();
	}
	const auto correlation = [&](int a, int b) {
		const double denominator = std::sqrt(covariance(a, a) * covariance(b, b));
		return denominator > 1e-12 ? covariance(a, b) / denominator : 0.0;
	};
	stats.correlationXY = correlation(0, 1);
	stats.correlationXZ = correlation(0, 2);
	stats.correlationYZ = correlation(1, 2);
	return stats;
}

} // namespace smcp::Pointcloud
