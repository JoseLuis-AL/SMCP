#include "core/pointcloud_ops.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <opencv2/core/core_c.h>  // CV_PCA_DATA_AS_ROW (OpenCV 2.4)
#include <opencv2/flann/flann.hpp>

namespace smcp::pointcloud
{

bool is_finite(const ColorPoint& p)
{
	return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

namespace
{
	/* Utilidades ========================================================================== */

	inline cv::Vec3f to_vec(const ColorPoint& p) { return cv::Vec3f(p.x, p.y, p.z); }

	/// Devuelve los indices de los puntos con coordenadas finitas.
	std::vector<int> finite_indices(const ColorCloud& cloud)
	{
		std::vector<int> indices;
		indices.reserve(cloud.size());
		for (int i = 0; i < static_cast<int>(cloud.size()); ++i)
		{
			if (is_finite(cloud[i]))
			{
				indices.push_back(i);
			}
		}
		return indices;
	}

	/// Copia los puntos indicados por `indices` (o su complemento si `negative`).
	ColorCloudPtr extract(const ColorCloud& cloud, const std::vector<int>& indices, bool negative)
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

	/// Toma `count` indices distintos de `pool` (solo se barajan las primeras posiciones).
	template <typename Rng>
	void sample_indices(std::vector<int>& pool, int count, Rng& rng)
	{
		const int n = static_cast<int>(pool.size());
		for (int i = 0; i < count; ++i)
		{
			std::uniform_int_distribution<int> dist(i, n - 1);
			std::swap(pool[i], pool[dist(rng)]);
		}
	}

	/// Numero de iteraciones necesarias para encontrar una muestra sin outliers con
	/// probabilidad `probability`, dada la fraccion de inliers `w` y `sample_size` puntos
	/// por muestra (mismo criterio adaptativo que RANSAC clasico).
	double required_iterations(double w, int sample_size, double probability)
	{
		const double p_no_outliers = std::max(std::numeric_limits<double>::epsilon(),
			std::min(1.0 - std::numeric_limits<double>::epsilon(), std::pow(w, sample_size)));
		return std::log(1.0 - probability) / std::log(1.0 - p_no_outliers);
	}

	/// RANSAC generico sobre la nube `cloud` restringida a `candidates`.
	/// `make_model(sample_indices, model)` devuelve false si la muestra es degenerada;
	/// `distance(model, point)` devuelve la distancia punto-modelo.
	template <typename Model, typename MakeModel, typename Distance, typename Rng>
	bool ransac(const ColorCloud& cloud, const std::vector<int>& candidates, int sample_size, int max_iterations,
		double threshold, MakeModel make_model, Distance distance, Rng& rng, Model& best_model, std::vector<int>& best_inliers)
	{
		const int n = static_cast<int>(candidates.size());
		if (n < sample_size)
		{
			return false;
		}

		std::vector<int> pool = candidates;
		std::vector<int> sample(sample_size);
		std::vector<int> inliers;
		inliers.reserve(n);
		best_inliers.clear();

		double k = static_cast<double>(max_iterations);
		int iteration = 0;
		int skipped = 0;
		const int max_skipped = max_iterations * 10;

		while (iteration < k && iteration < max_iterations && skipped < max_skipped)
		{
			sample_indices(pool, sample_size, rng);
			for (int i = 0; i < sample_size; ++i)
			{
				sample[i] = pool[i];
			}

			Model model;
			if (!make_model(sample, model))
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

			if (inliers.size() > best_inliers.size())
			{
				best_inliers = inliers;
				best_model = model;

				const double w = static_cast<double>(best_inliers.size()) / static_cast<double>(n);
				k = required_iterations(w, sample_size, 0.99);
			}
			++iteration;
		}

		return !best_inliers.empty();
	}

	/* Planos ============================================================================== */

	bool plane_from_points(const ColorCloud& cloud, const std::vector<int>& sample, PlaneModel& model)
	{
		const cv::Vec3f p0 = to_vec(cloud[sample[0]]);
		const cv::Vec3f p1 = to_vec(cloud[sample[1]]);
		const cv::Vec3f p2 = to_vec(cloud[sample[2]]);
		const cv::Vec3f n = (p1 - p0).cross(p2 - p0);
		const double length = cv::norm(n);
		if (length < 1e-9)
		{
			return false;  // puntos colineales
		}
		model.normal = n * static_cast<float>(1.0 / length);
		model.d = -model.normal.dot(p0);
		return true;
	}

	inline double plane_distance(const PlaneModel& model, const ColorPoint& p)
	{
		return std::fabs(model.normal.dot(to_vec(p)) + model.d);
	}

	/// Ajuste por minimos cuadrados (PCA): centroide y autovector de menor autovalor.
	bool refine_plane(const ColorCloud& cloud, const std::vector<int>& inliers, PlaneModel& model)
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
		const cv::Mat normal = pca.eigenvectors.row(2);  // menor autovalor
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

	/* Esferas ============================================================================= */

	bool sphere_from_points(const ColorCloud& cloud, const std::vector<int>& sample, double min_radius, double max_radius,
		SphereModel& model)
	{
		// Centro c tal que |p_i - c| = |p_0 - c| para i = 1..3:
		//   2 (p_i - p_0) . c = |p_i|^2 - |p_0|^2
		const cv::Vec3d p0 = to_vec(cloud[sample[0]]);
		cv::Matx33d A;
		cv::Vec3d b;
		for (int i = 1; i < 4; ++i)
		{
			const cv::Vec3d pi = to_vec(cloud[sample[i]]);
			const cv::Vec3d row = 2.0 * (pi - p0);
			A(i - 1, 0) = row[0];
			A(i - 1, 1) = row[1];
			A(i - 1, 2) = row[2];
			b[i - 1] = pi.dot(pi) - p0.dot(p0);
		}
		if (std::fabs(cv::determinant(A)) < 1e-9)
		{
			return false;  // puntos coplanares
		}
		const cv::Vec3d c = A.solve(b, cv::DECOMP_LU);
		const double radius = cv::norm(p0 - c);
		if (!std::isfinite(radius) || radius < min_radius || radius > max_radius)
		{
			return false;
		}
		model.center = cv::Point3f(static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]));
		model.radius = static_cast<float>(radius);
		return true;
	}

	inline double sphere_distance(const SphereModel& model, const ColorPoint& p)
	{
		const double dx = p.x - model.center.x;
		const double dy = p.y - model.center.y;
		const double dz = p.z - model.center.z;
		return std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - model.radius);
	}

	/// Ajuste algebraico por minimos cuadrados: x^2+y^2+z^2 + A x + B y + C z + D = 0.
	bool refine_sphere(const ColorCloud& cloud, const std::vector<int>& inliers, double min_radius, double max_radius,
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
		if (radius < min_radius || radius > max_radius)
		{
			return false;
		}
		model.center = cv::Point3f(static_cast<float>(center.x), static_cast<float>(center.y), static_cast<float>(center.z));
		model.radius = static_cast<float>(radius);
		return true;
	}

	template <typename Model, typename Distance>
	std::vector<int> collect_inliers(const ColorCloud& cloud, const std::vector<int>& candidates, const Model& model,
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

ColorCloudPtr remove_statistical_outliers(const ColorCloud& cloud, const OutlierRemovalParams& params)
{
	const std::vector<int> valid = finite_indices(cloud);
	auto out = std::make_shared<ColorCloud>();

	const int k = std::max(1, params.mean_k);
	if (static_cast<int>(valid.size()) <= k)
	{
		*out = cloud;  // demasiado pocos puntos para evaluar vecindarios
		return out;
	}

	cv::Mat features(static_cast<int>(valid.size()), 3, CV_32F);
	for (int i = 0; i < features.rows; ++i)
	{
		const ColorPoint& p = cloud[valid[i]];
		features.at<float>(i, 0) = p.x;
		features.at<float>(i, 1) = p.y;
		features.at<float>(i, 2) = p.z;
	}

	// kd-tree exacto (un arbol, busqueda sin limite de comprobaciones).
	cv::flann::Index index(features, cv::flann::KDTreeIndexParams(1));
	cv::Mat indices(features.rows, k + 1, CV_32S);
	cv::Mat dists(features.rows, k + 1, CV_32F);
	index.knnSearch(features, indices, dists, k + 1, cv::flann::SearchParams(-1));

	// Distancia media de cada punto a sus k vecinos (la columna 0 es el propio punto).
	std::vector<double> mean_dist(features.rows, 0.0);
	double sum = 0.0;
	double sum_sq = 0.0;
	for (int i = 0; i < features.rows; ++i)
	{
		double acc = 0.0;
		for (int j = 1; j <= k; ++j)
		{
			acc += std::sqrt(static_cast<double>(dists.at<float>(i, j)));
		}
		mean_dist[i] = acc / k;
		sum += mean_dist[i];
		sum_sq += mean_dist[i] * mean_dist[i];
	}
	const double n = static_cast<double>(features.rows);
	const double mean = sum / n;
	const double variance = std::max(0.0, (sum_sq - sum * sum / n) / (n - 1.0));
	const double threshold = mean + params.stddev_mult * std::sqrt(variance);

	out->reserve(valid.size());
	for (int i = 0; i < features.rows; ++i)
	{
		if (mean_dist[i] <= threshold)
		{
			out->push_back(cloud[valid[i]]);
		}
	}
	return out;
}

/* Planos ================================================================================== */

std::vector<PlaneFit> fit_planes(const ColorCloud& cloud, const PlaneFitParams& params, ColorCloudPtr* remaining)
{
	std::vector<PlaneFit> planes;
	std::mt19937 rng(0x5EED);  // semilla fija: resultados reproducibles

	const std::size_t min_inliers = params.min_inliers > 0
		? params.min_inliers
		: std::max<std::size_t>(200u, cloud.size() / 300u);

	ColorCloudPtr working = std::make_shared<ColorCloud>(cloud);
	auto make_model = [&](const std::vector<int>& sample, PlaneModel& model) { return plane_from_points(*working, sample, model); };

	while (!working->empty() && static_cast<int>(planes.size()) < params.max_planes)
	{
		const std::vector<int> candidates = finite_indices(*working);
		PlaneModel model;
		std::vector<int> inliers;
		if (!ransac(*working, candidates, 3, params.max_iterations, params.distance_threshold, make_model, plane_distance, rng,
				model, inliers))
		{
			break;
		}

		if (params.optimize_coefficients)
		{
			PlaneModel refined = model;
			if (refine_plane(*working, inliers, refined))
			{
				const std::vector<int> refined_inliers =
					collect_inliers(*working, candidates, refined, params.distance_threshold, plane_distance);
				if (refined_inliers.size() >= inliers.size())
				{
					model = refined;
					inliers = refined_inliers;
				}
			}
		}

		if (inliers.size() < min_inliers)
		{
			break;
		}

		PlaneFit fit;
		fit.points = extract(*working, inliers, false);
		fit.model = model;
		planes.push_back(fit);

		working = extract(*working, inliers, true);
	}

	if (remaining)
	{
		*remaining = working;
	}
	return planes;
}

/* Esferas ================================================================================= */

std::vector<SphereFit> fit_spheres(const ColorCloud& cloud, const SphereFitParams& params, ColorCloudPtr* remaining)
{
	std::vector<SphereFit> spheres;
	std::mt19937 rng(0x5EED);

	const std::size_t min_inliers = params.min_inliers > 0
		? params.min_inliers
		: std::max<std::size_t>(50u, cloud.size() / 100u);

	ColorCloudPtr working = std::make_shared<ColorCloud>(cloud);
	auto make_model = [&](const std::vector<int>& sample, SphereModel& model) {
		return sphere_from_points(*working, sample, params.min_radius, params.max_radius, model);
	};

	while (!working->empty() && static_cast<int>(spheres.size()) < params.max_spheres)
	{
		const std::vector<int> candidates = finite_indices(*working);
		SphereModel model;
		std::vector<int> inliers;
		if (!ransac(*working, candidates, 4, params.max_iterations, params.distance_threshold, make_model, sphere_distance, rng,
				model, inliers))
		{
			break;
		}

		if (params.optimize_coefficients)
		{
			SphereModel refined = model;
			if (refine_sphere(*working, inliers, params.min_radius, params.max_radius, refined))
			{
				const std::vector<int> refined_inliers =
					collect_inliers(*working, candidates, refined, params.distance_threshold, sphere_distance);
				if (refined_inliers.size() >= inliers.size())
				{
					model = refined;
					inliers = refined_inliers;
				}
			}
		}

		if (inliers.size() < min_inliers)
		{
			break;
		}

		SphereFit fit;
		fit.points = extract(*working, inliers, false);
		fit.model = model;
		spheres.push_back(fit);

		working = extract(*working, inliers, true);
	}

	if (remaining)
	{
		*remaining = working;
	}
	return spheres;
}

} // namespace smcp::pointcloud
