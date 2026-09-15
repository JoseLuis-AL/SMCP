#include "core/PointcloudOps.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <thread>

#include <opencv2/core/core_c.h>  // CV_PCA_DATA_AS_ROW (OpenCV 2.4)
#include <opencv2/flann/flann.hpp>

namespace smcp::Pointcloud
{

namespace
{
	/// Points per kd-tree leaf: small leaves keep exact searches cheap for 3D data.
	constexpr int Kd_Tree_Leaf_Size = 10;

	/// Queries per knnSearch call; bounds the per-thread index/distance buffers to about 1 MB.
	constexpr int Knn_Query_Block = 2048;
} // namespace

bool IsFinite(const ColorPoint& p)
{
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

namespace
{
	/* Utilities =========================================================================== */

	inline cv::Vec3f ToVec(const ColorPoint& p) { return cv::Vec3f(p.x, p.y, p.z); }

	/// Returns the indices of the points with finite coordinates.
	std::vector<int> FiniteIndices(const ColorCloud& cloud)
	{
		std::vector<int> indices;
		indices.reserve(cloud.size());
		for (int i = 0; i < static_cast<int>(cloud.size()); ++i)
		{
			if (IsFinite(cloud[i]))
			{
				indices.push_back(i);
			}
		}
		return indices;
	}

	/// Copies the points listed in `indices` (or their complement when `negative`).
	ColorCloudPtr Extract(const ColorCloud& cloud, const std::vector<int>& indices, bool negative)
	{
		auto out = std::make_shared<ColorCloud>();
		if (!negative)
		{
			out->reserve(indices.size());
			for (int i : indices)
			{
				out->push_back(cloud[i]);
			}
			return out;
		}

		std::vector<char> keep(cloud.size(), 1);
		for (int i : indices)
		{
			keep[i] = 0;
		}
		out->reserve(cloud.size() - indices.size());
		for (std::size_t i = 0; i < cloud.size(); ++i)
		{
			if (keep[i])
			{
				out->push_back(cloud[i]);
			}
		}
		return out;
	}

	/// Takes `count` distinct indices from `pool` (only the leading positions are shuffled).
	template <typename Rng>
	void SampleIndices(std::vector<int>& pool, int count, Rng& rng)
	{
		const int n = static_cast<int>(pool.size());
		for (int i = 0; i < count; ++i)
		{
			std::uniform_int_distribution<int> dist(i, n - 1);
			std::swap(pool[i], pool[dist(rng)]);
		}
	}

	/// Number of iterations needed to find an outlier-free sample with probability
	/// `probability`, given the inlier fraction `w` and `sampleSize` points per sample
	/// (the same adaptive criterion as classic RANSAC).
	double RequiredIterations(double w, int sampleSize, double probability)
	{
		const double pNoOutliers = std::max(std::numeric_limits<double>::epsilon(),
			std::min(1.0 - std::numeric_limits<double>::epsilon(), std::pow(w, sampleSize)));
		return std::log(1.0 - probability) / std::log(1.0 - pNoOutliers);
	}

	/// Generic RANSAC over the cloud `cloud` restricted to `candidates`.
	/// `makeModel(SampleIndices, model)` returns false when the sample is degenerate;
	/// `distance(model, point)` returns the point-to-model distance.
	template <typename Model, typename MakeModel, typename Distance, typename Rng>
	bool Ransac(const ColorCloud& cloud, const std::vector<int>& candidates, int sampleSize, int maxIterations,
		double threshold, MakeModel makeModel, Distance distance, Rng& rng, Model& bestModel, std::vector<int>& bestInliers)
	{
		const int n = static_cast<int>(candidates.size());
		if (n < sampleSize)
		{
			return false;
		}

		std::vector<int> pool = candidates;
		std::vector<int> sample(sampleSize);
		std::vector<int> inliers;
		inliers.reserve(n);
		bestInliers.clear();

		double k = static_cast<double>(maxIterations);
		int iteration = 0;
		int skipped = 0;
		const int maxSkipped = maxIterations * 10;

		while (iteration < k && iteration < maxIterations && skipped < maxSkipped)
		{
			SampleIndices(pool, sampleSize, rng);
			for (int i = 0; i < sampleSize; ++i)
			{
				sample[i] = pool[i];
			}

			Model model;
			if (!makeModel(sample, model))
			{
				++skipped;
				continue;
			}

			inliers.clear();
			for (int idx : candidates)
			{
				if (distance(model, cloud[idx]) < threshold)
				{
					inliers.push_back(idx);
				}
			}

			if (inliers.size() > bestInliers.size())
			{
				bestInliers = inliers;
				bestModel = model;

				const double w = static_cast<double>(bestInliers.size()) / static_cast<double>(n);
				k = RequiredIterations(w, sampleSize, 0.99);
			}
			++iteration;
		}

		return !bestInliers.empty();
	}

	/* Planes ============================================================================== */

	bool PlaneFromPoints(const ColorCloud& cloud, const std::vector<int>& sample, PlaneModel& model)
	{
		const cv::Vec3f p0 = ToVec(cloud[sample[0]]);
		const cv::Vec3f p1 = ToVec(cloud[sample[1]]);
		const cv::Vec3f p2 = ToVec(cloud[sample[2]]);
		const cv::Vec3f n = (p1 - p0).cross(p2 - p0);
		const double length = cv::norm(n);
		if (length < 1e-9)
		{
			return false;  // collinear points
		}
		model.normal = n * static_cast<float>(1.0 / length);
		model.d = -model.normal.dot(p0);
		return true;
	}

	inline double PlaneDistance(const PlaneModel& model, const ColorPoint& p)
	{
		return std::fabs(model.normal.dot(ToVec(p)) + model.d);
	}

	/// Least-squares fit (PCA): centroid and eigenvector of the smallest eigenvalue.
	bool RefinePlane(const ColorCloud& cloud, const std::vector<int>& inliers, PlaneModel& model)
	{
		if (inliers.size() < 3)
		{
			return false;
		}
		cv::Mat data(static_cast<int>(inliers.size()), 3, CV_64F);
		for (int i = 0; i < data.rows; ++i)
		{
			const ColorPoint& p = cloud[inliers[i]];
			data.at<double>(i, 0) = p.x;
			data.at<double>(i, 1) = p.y;
			data.at<double>(i, 2) = p.z;
		}
		cv::PCA pca(data, cv::Mat(), CV_PCA_DATA_AS_ROW, 3);
		const cv::Mat normal = pca.eigenvectors.row(2);  // smallest eigenvalue
		const cv::Vec3f n(static_cast<float>(normal.at<double>(0)), static_cast<float>(normal.at<double>(1)),
			static_cast<float>(normal.at<double>(2)));
		const double length = cv::norm(n);
		if (length < 1e-12)
		{
			return false;
		}
		model.normal = n * static_cast<float>(1.0 / length);
		const cv::Vec3f centroid(static_cast<float>(pca.mean.at<double>(0)), static_cast<float>(pca.mean.at<double>(1)),
			static_cast<float>(pca.mean.at<double>(2)));
		model.d = -model.normal.dot(centroid);
		return true;
	}

	/* Spheres ============================================================================= */

	bool SphereFromPoints(const ColorCloud& cloud, const std::vector<int>& sample, double minRadius, double maxRadius,
		SphereModel& model)
	{
		// Center c such that |pI - c| = |p0 - c| for i = 1..3:
		//   2 (pI - p0) . c = |pI|^2 - |p0|^2
		const cv::Vec3d p0 = ToVec(cloud[sample[0]]);
		cv::Matx33d A;
		cv::Vec3d b;
		for (int i = 1; i < 4; ++i)
		{
			const cv::Vec3d pi = ToVec(cloud[sample[i]]);
			const cv::Vec3d row = 2.0 * (pi - p0);
			A(i - 1, 0) = row[0];
			A(i - 1, 1) = row[1];
			A(i - 1, 2) = row[2];
			b[i - 1] = pi.dot(pi) - p0.dot(p0);
		}
		if (std::fabs(cv::determinant(A)) < 1e-9)
		{
			return false;  // coplanar points
		}
		const cv::Vec3d c = A.solve(b, cv::DECOMP_LU);
		const double radius = cv::norm(p0 - c);
		if (!std::isfinite(radius) || radius < minRadius || radius > maxRadius)
		{
			return false;
		}
		model.center = cv::Point3f(static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]));
		model.radius = static_cast<float>(radius);
		return true;
	}

	inline double SphereDistance(const SphereModel& model, const ColorPoint& p)
	{
		const double dx = p.x - model.center.x;
		const double dy = p.y - model.center.y;
		const double dz = p.z - model.center.z;
		return std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - model.radius);
	}

	/// Algebraic least-squares fit: x^2+y^2+z^2 + A x + B y + C z + D = 0.
	bool RefineSphere(const ColorCloud& cloud, const std::vector<int>& inliers, double minRadius, double maxRadius,
		SphereModel& model)
	{
		if (inliers.size() < 4)
		{
			return false;
		}
		cv::Mat M(static_cast<int>(inliers.size()), 4, CV_64F);
		cv::Mat rhs(static_cast<int>(inliers.size()), 1, CV_64F);
		for (int i = 0; i < M.rows; ++i)
		{
			const ColorPoint& p = cloud[inliers[i]];
			M.at<double>(i, 0) = p.x;
			M.at<double>(i, 1) = p.y;
			M.at<double>(i, 2) = p.z;
			M.at<double>(i, 3) = 1.0;
			rhs.at<double>(i, 0) = -(static_cast<double>(p.x) * p.x + static_cast<double>(p.y) * p.y + static_cast<double>(p.z) * p.z);
		}
		cv::Mat coeffs;
		if (!cv::solve(M, rhs, coeffs, cv::DECOMP_SVD))
		{
			return false;
		}
		const double a = coeffs.at<double>(0), b = coeffs.at<double>(1), c = coeffs.at<double>(2), d = coeffs.at<double>(3);
		const cv::Point3d center(-a / 2.0, -b / 2.0, -c / 2.0);
		const double r2 = center.dot(center) - d;
		if (r2 <= 0.0)
		{
			return false;
		}
		const double radius = std::sqrt(r2);
		if (radius < minRadius || radius > maxRadius)
		{
			return false;
		}
		model.center = cv::Point3f(static_cast<float>(center.x), static_cast<float>(center.y), static_cast<float>(center.z));
		model.radius = static_cast<float>(radius);
		return true;
	}

	template <typename Model, typename Distance>
	std::vector<int> CollectInliers(const ColorCloud& cloud, const std::vector<int>& candidates, const Model& model,
		double threshold, Distance distance)
	{
		std::vector<int> inliers;
		for (int idx : candidates)
		{
			if (distance(model, cloud[idx]) < threshold)
			{
				inliers.push_back(idx);
			}
		}
		return inliers;
	}
} // namespace

/* Outliers ================================================================================ */

ColorCloudPtr RemoveStatisticalOutliers(const ColorCloud& cloud, const OutlierRemovalParams& params)
{
	const std::vector<int> valid = FiniteIndices(cloud);
	auto out = std::make_shared<ColorCloud>();

	const int k = std::max(1, params.meanK);
	if (static_cast<int>(valid.size()) <= k)
	{
		*out = cloud;  // too few points to evaluate neighborhoods
		return out;
	}

	const int rows = static_cast<int>(valid.size());
	std::vector<float> features(static_cast<std::size_t>(rows) * 3);
	for (int i = 0; i < rows; ++i)
	{
		const ColorPoint& p = cloud[valid[i]];
		features[3 * i] = p.x;
		features[3 * i + 1] = p.y;
		features[3 * i + 2] = p.z;
	}

	// KDTreeSingleIndex, not cv::flann::Index with KDTreeIndexParams: the randomized kd-tree's
	// "exact" search over-estimates branch distances when an axis repeats along the path (always,
	// in 3D), so it prunes true neighbors; it is also ~5x slower.
	const cvflann::Matrix<float> dataset(features.data(), rows, 3);
	cvflann::KDTreeSingleIndex<cvflann::L2_Simple<float>> index(dataset, cvflann::KDTreeSingleIndexParams(Kd_Tree_Leaf_Size));
	index.buildIndex();

	// Mean distance from each point to its k neighbors (column 0 is the point itself). Queries are
	// independent and the index is read-only, so blocks run in parallel writing disjoint entries.
	std::vector<double> meanDist(rows, 0.0);
	const auto searchRange = [&](int first, int last) {
		std::vector<int> indices(static_cast<std::size_t>(Knn_Query_Block) * (k + 1));
		std::vector<float> dists(indices.size());
		for (int begin = first; begin < last; begin += Knn_Query_Block)
		{
			const int count = std::min(Knn_Query_Block, last - begin);
			const cvflann::Matrix<float> queries(features.data() + 3 * static_cast<std::size_t>(begin), count, 3);
			cvflann::Matrix<int> blockIndices(indices.data(), count, k + 1);
			cvflann::Matrix<float> blockDists(dists.data(), count, k + 1);
			index.knnSearch(queries, blockIndices, blockDists, k + 1, cvflann::SearchParams(-1));

			for (int i = 0; i < count; ++i)
			{
				double acc = 0.0;
				for (int j = 1; j <= k; ++j)
				{
					acc += std::sqrt(static_cast<double>(dists[static_cast<std::size_t>(i) * (k + 1) + j]));
				}
				meanDist[begin + i] = acc / k;
			}
		}
	};

	const int threadCount = static_cast<int>(std::clamp<unsigned>(std::thread::hardware_concurrency(), 1u,
		static_cast<unsigned>(std::max(1, rows / Knn_Query_Block))));
	std::vector<std::thread> workers;
	workers.reserve(threadCount - 1);
	for (int t = 1; t < threadCount; ++t)
	{
		workers.emplace_back(searchRange, static_cast<int>(static_cast<long long>(rows) * t / threadCount),
			static_cast<int>(static_cast<long long>(rows) * (t + 1) / threadCount));
	}
	searchRange(0, rows / threadCount);
	for (auto& worker : workers)
	{
		worker.join();
	}

	double sum = 0.0;
	double sumSq = 0.0;
	for (int i = 0; i < rows; ++i)
	{
		sum += meanDist[i];
		sumSq += meanDist[i] * meanDist[i];
	}
	const double n = static_cast<double>(rows);
	const double mean = sum / n;
	const double variance = std::max(0.0, (sumSq - sum * sum / n) / (n - 1.0));
	const double threshold = mean + params.stddevMult * std::sqrt(variance);

	out->reserve(valid.size());
	for (int i = 0; i < rows; ++i)
	{
		if (meanDist[i] <= threshold)
		{
			out->push_back(cloud[valid[i]]);
		}
	}
	return out;
}

/* Planes ================================================================================== */

std::vector<PlaneFit> FitPlanes(const ColorCloud& cloud, const PlaneFitParams& params, ColorCloudPtr* remaining)
{
	std::vector<PlaneFit> planes;
	std::mt19937 rng(0x5EED);  // semilla fija: resultados reproducibles

	const std::size_t minInliers = params.minInliers > 0
		? params.minInliers
		: std::max<std::size_t>(200u, cloud.size() / 300u);

	ColorCloudPtr working = std::make_shared<ColorCloud>(cloud);
	auto makeModel = [&](const std::vector<int>& sample, PlaneModel& model) { return PlaneFromPoints(*working, sample, model); };

	while (!working->empty() && static_cast<int>(planes.size()) < params.maxPlanes)
	{
		const std::vector<int> candidates = FiniteIndices(*working);
		PlaneModel model;
		std::vector<int> inliers;
		if (!Ransac(*working, candidates, 3, params.maxIterations, params.distanceThreshold, makeModel, PlaneDistance, rng,
				model, inliers))
		{
			break;
		}

		if (params.optimizeCoefficients)
		{
			PlaneModel refined = model;
			if (RefinePlane(*working, inliers, refined))
			{
				const std::vector<int> refinedInliers =
					CollectInliers(*working, candidates, refined, params.distanceThreshold, PlaneDistance);
				if (refinedInliers.size() >= inliers.size())
				{
					model = refined;
					inliers = refinedInliers;
				}
			}
		}

		if (inliers.size() < minInliers)
		{
			break;
		}

		PlaneFit fit;
		fit.points = Extract(*working, inliers, false);
		fit.model = model;
		planes.push_back(fit);

		working = Extract(*working, inliers, true);
	}

	if (remaining)
	{
		*remaining = working;
	}
	return planes;
}

/* Spheres ================================================================================= */

std::vector<SphereFit> FitSpheres(const ColorCloud& cloud, const SphereFitParams& params, ColorCloudPtr* remaining)
{
	std::vector<SphereFit> spheres;
	std::mt19937 rng(0x5EED);

	const std::size_t minInliers = params.minInliers > 0
		? params.minInliers
		: std::max<std::size_t>(50u, cloud.size() / 100u);

	ColorCloudPtr working = std::make_shared<ColorCloud>(cloud);
	auto makeModel = [&](const std::vector<int>& sample, SphereModel& model) {
		return SphereFromPoints(*working, sample, params.minRadius, params.maxRadius, model);
	};

	while (!working->empty() && static_cast<int>(spheres.size()) < params.maxSpheres)
	{
		const std::vector<int> candidates = FiniteIndices(*working);
		SphereModel model;
		std::vector<int> inliers;
		if (!Ransac(*working, candidates, 4, params.maxIterations, params.distanceThreshold, makeModel, SphereDistance, rng,
				model, inliers))
		{
			break;
		}

		if (params.optimizeCoefficients)
		{
			SphereModel refined = model;
			if (RefineSphere(*working, inliers, params.minRadius, params.maxRadius, refined))
			{
				const std::vector<int> refinedInliers =
					CollectInliers(*working, candidates, refined, params.distanceThreshold, SphereDistance);
				if (refinedInliers.size() >= inliers.size())
				{
					model = refined;
					inliers = refinedInliers;
				}
			}
		}

		if (inliers.size() < minInliers)
		{
			break;
		}

		SphereFit fit;
		fit.points = Extract(*working, inliers, false);
		fit.model = model;
		spheres.push_back(fit);

		working = Extract(*working, inliers, true);
	}

	if (remaining)
	{
		*remaining = working;
	}
	return spheres;
}

} // namespace smcp::Pointcloud
