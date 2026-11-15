#include "core/PointcloudOps.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <random>
#include <thread>

#include <opencv2/core/core_c.h>  // CV_PCA_DATA_AS_ROW (OpenCV 2.4)
#include <opencv2/flann/flann.hpp>

#include "core/Parallel.h"

namespace smcp::Pointcloud
{

namespace
{
	/// Points per kd-tree leaf: small leaves keep exact searches cheap for 3D data.
	constexpr int Kd_Tree_Leaf_Size = 10;

	/// Queries per knnSearch call; bounds the per-thread index/distance buffers to about 1 MB.
	constexpr int Knn_Query_Block = 2048;

	/// Neighbors (the point included) used to estimate the target normals for point-to-plane ICP.
	constexpr int Normal_Neighbors = 10;

	/// Points evaluated per thread at least when a RANSAC hypothesis is scored or a cloud is transformed.
	constexpr long long Min_Points_Per_Thread = 100000;

	/// KDTreeSingleIndex, not cv::flann::Index with KDTreeIndexParams: the randomized kd-tree's
	/// "exact" search over-estimates branch distances when an axis repeats along the path (always,
	/// in 3D), so it prunes true neighbors; it is also ~5x slower.
	using KdTree = cvflann::KDTreeSingleIndex<cvflann::L2_Simple<float>>;
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

	/// Packs the listed points as kd-tree rows of [x, y, z].
	std::vector<float> Features(const ColorCloud& cloud, const std::vector<int>& indices)
	{
		std::vector<float> features(indices.size() * 3);
		for (std::size_t i = 0; i < indices.size(); ++i)
		{
			const ColorPoint& p = cloud[indices[i]];
			features[3 * i] = p.x;
			features[3 * i + 1] = p.y;
			features[3 * i + 2] = p.z;
		}
		return features;
	}

	/// Runs `work(first, last)` over [0, rows) split into one contiguous range per thread, with at
	/// least Knn_Query_Block rows per thread.
	template <typename Work>
	void ParallelRanges(int rows, Work work)
	{
		const int threadCount = static_cast<int>(std::clamp<unsigned>(std::thread::hardware_concurrency(), 1u,
			static_cast<unsigned>(std::max(1, rows / Knn_Query_Block))));
		std::vector<std::thread> workers;
		workers.reserve(threadCount - 1);
		for (int t = 1; t < threadCount; ++t)
		{
			workers.emplace_back(work, static_cast<int>(static_cast<long long>(rows) * t / threadCount),
				static_cast<int>(static_cast<long long>(rows) * (t + 1) / threadCount));
		}
		work(0, rows / threadCount);
		for (auto& worker : workers)
		{
			worker.join();
		}
	}

	/// Searches the k nearest neighbors of every query row ([x, y, z]) and calls
	/// `visit(row, indices, squaredDistances)` with k entries each. Queries are independent and the
	/// index is read-only, so blocks run in parallel; `visit` must only write data owned by `row`.
	/// When `cancel` is set the search stops early and leaves the remaining rows unvisited: the
	/// worker threads must not throw, so the caller calls ThrowIfCanceled() right after.
	template <typename Visit>
	void SearchKnn(KdTree& index, const float* queries, int rows, int k, Visit visit, const CancelFlag* cancel = nullptr)
	{
		ParallelRanges(rows, [&](int first, int last) {
			std::vector<int> indices(static_cast<std::size_t>(Knn_Query_Block) * k);
			std::vector<float> dists(indices.size());
			for (int begin = first; begin < last; begin += Knn_Query_Block)
			{
				if (cancel && cancel->load(std::memory_order_relaxed))
				{
					return;
				}
				const int count = std::min(Knn_Query_Block, last - begin);
				const cvflann::Matrix<float> blockQueries(const_cast<float*>(queries) + 3 * static_cast<std::size_t>(begin), count, 3);
				cvflann::Matrix<int> blockIndices(indices.data(), count, k);
				cvflann::Matrix<float> blockDists(dists.data(), count, k);
				index.knnSearch(blockQueries, blockIndices, blockDists, k, cvflann::SearchParams(-1));

				for (int i = 0; i < count; ++i)
				{
					visit(begin + i, indices.data() + static_cast<std::size_t>(i) * k, dists.data() + static_cast<std::size_t>(i) * k);
				}
			}
		});
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

	/// Number of candidates closer than `threshold` to the model.
	template <typename Model, typename Distance>
	std::size_t CountInliers(const ColorCloud& cloud, const std::vector<int>& candidates, const Model& model,
		double threshold, Distance distance)
	{
		std::atomic<std::size_t> total{ 0 };
		Parallel::ParallelFor(static_cast<long long>(candidates.size()), Min_Points_Per_Thread, [&](long long first, long long last) {
			std::size_t count = 0;
			for (long long i = first; i < last; ++i)
			{
				count += distance(model, cloud[candidates[i]]) < threshold ? 1 : 0;
			}
			total.fetch_add(count, std::memory_order_relaxed);
		});
		return total.load();
	}

	/// Generic RANSAC over the cloud `cloud` restricted to `candidates`.
	/// `makeModel(SampleIndices, model)` returns false when the sample is degenerate;
	/// `distance(model, point)` returns the point-to-model distance.
	template <typename Model, typename MakeModel, typename Distance, typename Rng>
	bool Ransac(const ColorCloud& cloud, const std::vector<int>& candidates, int sampleSize, int maxIterations,
		double threshold, MakeModel makeModel, Distance distance, Rng& rng, Model& bestModel, std::vector<int>& bestInliers,
		const CancelFlag* cancel = nullptr)
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
			ThrowIfCanceled(cancel);
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

			// Most hypotheses lose: count their inliers (in parallel on large clouds) and build the
			// list only for a new best one. The count and the list do not depend on the thread count.
			if (CountInliers(cloud, candidates, model, threshold, distance) > bestInliers.size())
			{
				inliers.clear();
				for (int idx : candidates)
				{
					if (distance(model, cloud[idx]) < threshold)
					{
						inliers.push_back(idx);
					}
				}
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

ColorCloudPtr RemoveStatisticalOutliers(const ColorCloud& cloud, const OutlierRemovalParams& params, const CancelFlag* cancel)
{
	ThrowIfCanceled(cancel);
	const std::vector<int> valid = FiniteIndices(cloud);
	auto out = std::make_shared<ColorCloud>();

	const int k = std::max(1, params.meanK);
	if (static_cast<int>(valid.size()) <= k)
	{
		*out = cloud;  // too few points to evaluate neighborhoods
		return out;
	}

	const int rows = static_cast<int>(valid.size());
	std::vector<float> features = Features(cloud, valid);
	const cvflann::Matrix<float> dataset(features.data(), rows, 3);
	KdTree index(dataset, cvflann::KDTreeSingleIndexParams(Kd_Tree_Leaf_Size));
	index.buildIndex();

	// Mean distance from each point to its k neighbors (column 0 is the point itself).
	std::vector<double> meanDist(rows, 0.0);
	SearchKnn(index, features.data(), rows, k + 1, [&](int row, const int*, const float* dists) {
		double acc = 0.0;
		for (int j = 1; j <= k; ++j)
		{
			acc += std::sqrt(static_cast<double>(dists[j]));
		}
		meanDist[row] = acc / k;
	}, cancel);
	ThrowIfCanceled(cancel);

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

std::vector<PlaneFit> FitPlanes(const ColorCloud& cloud, const PlaneFitParams& params, ColorCloudPtr* remaining,
	const CancelFlag* cancel)
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
				model, inliers, cancel))
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

std::vector<SphereFit> FitSpheres(const ColorCloud& cloud, const SphereFitParams& params, ColorCloudPtr* remaining,
	const CancelFlag* cancel)
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
				model, inliers, cancel))
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

/* Registration ============================================================================ */

cv::Vec3d Centroid(const ColorCloud& cloud)
{
	cv::Vec3d sum(0.0, 0.0, 0.0);
	std::size_t count = 0;
	for (const ColorPoint& p : cloud)
	{
		if (IsFinite(p))
		{
			sum += cv::Vec3d(p.x, p.y, p.z);
			++count;
		}
	}
	return count > 0 ? sum * (1.0 / static_cast<double>(count)) : sum;
}

namespace
{
	cv::Matx44d RigidTransform(const cv::Matx33d& rotation, const cv::Vec3d& translation)
	{
		cv::Matx44d transform = cv::Matx44d::eye();
		for (int r = 0; r < 3; ++r)
		{
			for (int c = 0; c < 3; ++c)
			{
				transform(r, c) = rotation(r, c);
			}
			transform(r, 3) = translation[r];
		}
		return transform;
	}

	inline cv::Vec3d Apply(const cv::Matx44d& t, const cv::Vec3d& p)
	{
		return cv::Vec3d(t(0, 0) * p[0] + t(0, 1) * p[1] + t(0, 2) * p[2] + t(0, 3),
			t(1, 0) * p[0] + t(1, 1) * p[1] + t(1, 2) * p[2] + t(1, 3),
			t(2, 0) * p[0] + t(2, 1) * p[1] + t(2, 2) * p[2] + t(2, 3));
	}

	/// Rotation Rz(gamma) * Ry(beta) * Rx(alpha); to first order it equals I + [w]x with w = (alpha, beta, gamma).
	cv::Matx33d RotationFromAngles(double alpha, double beta, double gamma)
	{
		const double ca = std::cos(alpha), sa = std::sin(alpha);
		const double cb = std::cos(beta), sb = std::sin(beta);
		const double cg = std::cos(gamma), sg = std::sin(gamma);
		return cv::Matx33d(
			cg * cb, cg * sb * sa - sg * ca, cg * sb * ca + sg * sa,
			sg * cb, sg * sb * sa + cg * ca, sg * sb * ca - cg * sa,
			-sb, cb * sa, cb * ca);
	}

	/// Unit normal per kd-tree row: eigenvector of the smallest eigenvalue of the covariance of its
	/// Normal_Neighbors nearest neighbors.
	std::vector<cv::Vec3f> EstimateNormals(KdTree& index, const std::vector<float>& features, int rows,
		const CancelFlag* cancel)
	{
		const int k = std::min(Normal_Neighbors, rows);
		std::vector<cv::Vec3f> normals(rows);
		SearchKnn(index, features.data(), rows, k, [&](int row, const int* neighbors, const float*) {
			cv::Vec3d mean(0.0, 0.0, 0.0);
			for (int j = 0; j < k; ++j)
			{
				const float* q = features.data() + 3 * static_cast<std::size_t>(neighbors[j]);
				mean += cv::Vec3d(q[0], q[1], q[2]);
			}
			mean *= 1.0 / k;

			cv::Matx33d covariance = cv::Matx33d::zeros();
			for (int j = 0; j < k; ++j)
			{
				const float* q = features.data() + 3 * static_cast<std::size_t>(neighbors[j]);
				const cv::Vec3d d = cv::Vec3d(q[0], q[1], q[2]) - mean;
				covariance += d * d.t();
			}

			cv::Vec3d eigenvalues;
			cv::Matx33d eigenvectors;
			cv::eigen(covariance, eigenvalues, eigenvectors);  // descending eigenvalues
			normals[row] = cv::Vec3f(static_cast<float>(eigenvectors(2, 0)), static_cast<float>(eigenvectors(2, 1)),
				static_cast<float>(eigenvectors(2, 2)));
		}, cancel);
		ThrowIfCanceled(cancel);
		return normals;
	}

	struct Correspondence
	{
		int sourceRow;
		int targetRow;
		float squaredDistance;
	};

	/// Least-squares rigid transform moving every source point onto its target point (Kabsch).
	bool PointToPointStep(const std::vector<Correspondence>& pairs, const std::vector<float>& source,
		const std::vector<float>& target, cv::Matx44d& step)
	{
		const auto point = [](const std::vector<float>& features, int row) {
			const float* p = features.data() + 3 * static_cast<std::size_t>(row);
			return cv::Vec3d(p[0], p[1], p[2]);
		};

		cv::Vec3d sourceMean(0.0, 0.0, 0.0);
		cv::Vec3d targetMean(0.0, 0.0, 0.0);
		for (const Correspondence& c : pairs)
		{
			sourceMean += point(source, c.sourceRow);
			targetMean += point(target, c.targetRow);
		}
		sourceMean *= 1.0 / static_cast<double>(pairs.size());
		targetMean *= 1.0 / static_cast<double>(pairs.size());

		cv::Matx33d h = cv::Matx33d::zeros();
		for (const Correspondence& c : pairs)
		{
			h += (point(source, c.sourceRow) - sourceMean) * (point(target, c.targetRow) - targetMean).t();
		}

		// H = U S V^T  ->  R = V U^T, flipping the last axis of V if that yields a reflection.
		const cv::SVD svd{ cv::Mat(h) };
		const cv::Matx33d u = svd.u;
		const cv::Matx33d v = cv::Matx33d(svd.vt).t();
		const double sign = cv::determinant(v * u.t()) < 0.0 ? -1.0 : 1.0;
		const cv::Matx33d rotation = v * cv::Matx33d(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, sign) * u.t();
		if (!cv::checkRange(rotation))
		{
			return false;
		}
		step = RigidTransform(rotation, targetMean - rotation * sourceMean);
		return true;
	}

	/// Linearized least squares of the point-to-plane distances: minimizes
	/// sum(((I + [w]x) p + t - q) . n)^2 over x = (w, t), with Jacobian row (p x n, n).
	bool PointToPlaneStep(const std::vector<Correspondence>& pairs, const std::vector<float>& source,
		const std::vector<float>& target, const std::vector<cv::Vec3f>& normals, cv::Matx44d& step)
	{
		cv::Matx66d ata = cv::Matx66d::zeros();
		cv::Vec6d atb(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		for (const Correspondence& c : pairs)
		{
			const float* sp = source.data() + 3 * static_cast<std::size_t>(c.sourceRow);
			const float* tp = target.data() + 3 * static_cast<std::size_t>(c.targetRow);
			const cv::Vec3d p(sp[0], sp[1], sp[2]);
			const cv::Vec3d q(tp[0], tp[1], tp[2]);
			const cv::Vec3d n = normals[c.targetRow];
			const cv::Vec3d pxn = p.cross(n);
			const cv::Vec6d row(pxn[0], pxn[1], pxn[2], n[0], n[1], n[2]);
			const double residual = (p - q).dot(n);
			for (int r = 0; r < 6; ++r)
			{
				for (int col = 0; col < 6; ++col)
				{
					ata(r, col) += row[r] * row[col];
				}
				atb[r] -= row[r] * residual;
			}
		}

		// SVD gives the minimum-norm solution, so directions the planes cannot constrain (sliding
		// along a single plane) stay unchanged instead of drifting.
		cv::Mat x;
		if (!cv::solve(cv::Mat(ata), cv::Mat(atb), x, cv::DECOMP_SVD) || !cv::checkRange(x))
		{
			return false;
		}
		step = RigidTransform(RotationFromAngles(x.at<double>(0), x.at<double>(1), x.at<double>(2)),
			cv::Vec3d(x.at<double>(3), x.at<double>(4), x.at<double>(5)));
		return true;
	}
} // namespace

IcpResult AlignIcp(const ColorCloud& source, const ColorCloud& target, const IcpParams& params, const CancelFlag* cancel)
{
	IcpResult result;
	ThrowIfCanceled(cancel);

	std::vector<int> sourceIndices = FiniteIndices(source);
	const std::vector<int> targetIndices = FiniteIndices(target);
	if (sourceIndices.size() < 3 || targetIndices.size() < 3)
	{
		return result;
	}

	if (params.maxSourcePoints > 0 && sourceIndices.size() > params.maxSourcePoints)
	{
		std::mt19937 rng(0x5EED);  // fixed seed: reproducible results
		SampleIndices(sourceIndices, static_cast<int>(params.maxSourcePoints), rng);
		sourceIndices.resize(params.maxSourcePoints);
	}

	const int targetRows = static_cast<int>(targetIndices.size());
	std::vector<float> targetFeatures = Features(target, targetIndices);
	const cvflann::Matrix<float> dataset(targetFeatures.data(), targetRows, 3);
	KdTree index(dataset, cvflann::KDTreeSingleIndexParams(Kd_Tree_Leaf_Size));
	index.buildIndex();

	const bool pointToPlane = params.pointToPlane;
	const std::vector<cv::Vec3f> normals = pointToPlane ? EstimateNormals(index, targetFeatures, targetRows, cancel)
		: std::vector<cv::Vec3f>();

	cv::Matx44d transform = cv::Matx44d::eye();
	if (params.alignCentroids)
	{
		transform = RigidTransform(cv::Matx33d::eye(), Centroid(target) - Centroid(source));
	}

	const int sourceRows = static_cast<int>(sourceIndices.size());
	const std::vector<float> sourceFeatures = Features(source, sourceIndices);
	std::vector<float> moved(sourceFeatures.size());
	std::vector<int> nearest(sourceRows);
	std::vector<float> nearestDistance(sourceRows);
	std::vector<Correspondence> pairs;
	pairs.reserve(sourceRows);

	const double maxSquaredDistance = params.maxCorrespondenceDistance > 0.0
		? params.maxCorrespondenceDistance * params.maxCorrespondenceDistance
		: std::numeric_limits<double>::infinity();
	const double overlap = std::clamp(params.overlapRatio, 0.01, 1.0);

	// An RMSE this small is an exact fit at float precision; its relative change is only rounding noise.
	cv::Vec3d targetMin = Centroid(target);
	cv::Vec3d targetMax = targetMin;
	for (int i = 0; i < targetRows; ++i)
	{
		for (int axis = 0; axis < 3; ++axis)
		{
			targetMin[axis] = std::min<double>(targetMin[axis], targetFeatures[3 * i + axis]);
			targetMax[axis] = std::max<double>(targetMax[axis], targetFeatures[3 * i + axis]);
		}
	}
	const double exactRmse = 1e-6 * cv::norm(targetMax - targetMin);
	const std::size_t minPairs = pointToPlane ? 6 : 3;
	double previousRmse = std::numeric_limits<double>::infinity();

	// Each pass measures the current transform; the loop then either stops or applies one more update.
	for (;;)
	{
		for (int i = 0; i < sourceRows; ++i)
		{
			const cv::Vec3d p = Apply(transform, cv::Vec3d(sourceFeatures[3 * i], sourceFeatures[3 * i + 1], sourceFeatures[3 * i + 2]));
			moved[3 * i] = static_cast<float>(p[0]);
			moved[3 * i + 1] = static_cast<float>(p[1]);
			moved[3 * i + 2] = static_cast<float>(p[2]);
		}
		SearchKnn(index, moved.data(), sourceRows, 1, [&](int row, const int* neighbor, const float* squaredDistance) {
			nearest[row] = neighbor[0];
			nearestDistance[row] = squaredDistance[0];
		}, cancel);
		ThrowIfCanceled(cancel);

		pairs.clear();
		for (int i = 0; i < sourceRows; ++i)
		{
			if (nearestDistance[i] <= maxSquaredDistance)
			{
				pairs.push_back(Correspondence{ i, nearest[i], nearestDistance[i] });
			}
		}
		// Trimmed ICP: only the closest fraction of the pairs, so non-overlapping regions do not pull the fit.
		const std::size_t keep = static_cast<std::size_t>(std::ceil(overlap * static_cast<double>(pairs.size())));
		if (keep < pairs.size())
		{
			std::nth_element(pairs.begin(), pairs.begin() + keep, pairs.end(),
				[](const Correspondence& a, const Correspondence& b) { return a.squaredDistance < b.squaredDistance; });
			pairs.resize(keep);
		}
		if (pairs.size() < minPairs)
		{
			result.valid = false;
			break;
		}

		double sum = 0.0;
		for (const Correspondence& c : pairs)
		{
			sum += c.squaredDistance;
		}
		const double rmse = std::sqrt(sum / static_cast<double>(pairs.size()));
		result.valid = true;
		result.transform = transform;
		result.rmse = rmse;
		result.correspondences = pairs.size();

		if (rmse <= exactRmse
			|| (std::isfinite(previousRmse) && std::fabs(previousRmse - rmse) <= params.convergenceThreshold * previousRmse))
		{
			result.converged = true;
			break;
		}
		if (result.iterations >= params.maxIterations)
		{
			break;
		}
		previousRmse = rmse;

		cv::Matx44d step;
		const bool stepped = pointToPlane ? PointToPlaneStep(pairs, moved, targetFeatures, normals, step)
			: PointToPointStep(pairs, moved, targetFeatures, step);
		if (!stepped && !(pointToPlane && PointToPointStep(pairs, moved, targetFeatures, step)))
		{
			break;
		}
		transform = step * transform;
		++result.iterations;
	}

	return result;
}

ColorCloudPtr TransformCloud(const ColorCloud& cloud, const cv::Matx44d& transform)
{
	auto out = std::make_shared<ColorCloud>(cloud);
	ColorPoint* points = out->data();
	Parallel::ParallelFor(static_cast<long long>(out->size()), Min_Points_Per_Thread, [&](long long first, long long last) {
		for (long long i = first; i < last; ++i)
		{
			ColorPoint& p = points[i];
			const cv::Vec3d moved = Apply(transform, cv::Vec3d(p.x, p.y, p.z));
			p.x = static_cast<float>(moved[0]);
			p.y = static_cast<float>(moved[1]);
			p.z = static_cast<float>(moved[2]);
		}
	});
	return out;
}

} // namespace smcp::Pointcloud
