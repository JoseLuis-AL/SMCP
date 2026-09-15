#pragma once

// Operaciones sobre nubes de puntos con color (x, y, z, r, g, b) usadas por el editor
// de nubes: eliminacion estadistica de outliers y ajuste RANSAC de planos y esferas.
// Sin dependencias externas mas alla de OpenCV (flann para vecinos mas cercanos).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <opencv2/core/core.hpp>

namespace smcp::pointcloud
{
struct ColorPoint
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
	std::uint8_t r = 200;
	std::uint8_t g = 200;
	std::uint8_t b = 200;
};

using ColorCloud = std::vector<ColorPoint>;
using ColorCloudPtr = std::shared_ptr<ColorCloud>;

/// Coordenadas finitas (sin NaN ni infinitos).
bool is_finite(const ColorPoint& point);

/* Outliers ================================================================================ */

struct OutlierRemovalParams
{
	int mean_k = 50;           ///< Vecinos considerados por punto.
	double stddev_mult = 1.0;  ///< Umbral = media + stddev_mult * desviacion tipica.
};

/// Statistical Outlier Removal: descarta los puntos cuya distancia media a sus
/// mean_k vecinos supera la media global mas stddev_mult desviaciones tipicas.
ColorCloudPtr remove_statistical_outliers(const ColorCloud& cloud, const OutlierRemovalParams& params);

/* Planos ================================================================================== */

struct PlaneModel
{
	cv::Vec3f normal{ 0.0f, 0.0f, 1.0f };  ///< Normal unitaria.
	float d = 0.0f;                          ///< normal . p + d = 0
};

struct PlaneFitParams
{
	int max_planes = 5;               ///< Numero maximo de planos a extraer.
	int max_iterations = 1000;        ///< Iteraciones RANSAC por plano.
	double distance_threshold = 0.5;  ///< Distancia maxima punto-plano para ser inlier.
	std::size_t min_inliers = 0;      ///< Minimo de inliers; 0 = max(200, n / 300).
	bool optimize_coefficients = true;///< Reajuste por minimos cuadrados con los inliers.
};

struct PlaneFit
{
	ColorCloudPtr points;  ///< Inliers del plano.
	PlaneModel model;
};

/// Extrae planos dominantes de forma iterativa (RANSAC + eliminacion de inliers).
/// Si se pasa `remaining`, recibe los puntos que no pertenecen a ningun plano.
std::vector<PlaneFit> fit_planes(const ColorCloud& cloud, const PlaneFitParams& params, ColorCloudPtr* remaining = nullptr);

/* Esferas ================================================================================= */

struct SphereModel
{
	cv::Point3f center;
	float radius = 0.0f;
};

struct SphereFitParams
{
	int max_spheres = 5;
	int max_iterations = 1000;
	double distance_threshold = 0.25;  ///< Distancia maxima a la superficie para ser inlier.
	double min_radius = 1.0;
	double max_radius = 10000.0;
	std::size_t min_inliers = 0;       ///< 0 = max(50, n / 100).
	bool optimize_coefficients = true;
};

struct SphereFit
{
	ColorCloudPtr points;
	SphereModel model;
};

/// Extrae esferas de forma iterativa (RANSAC + eliminacion de inliers).
/// Si se pasa `remaining`, recibe los puntos que no pertenecen a ninguna esfera.
std::vector<SphereFit> fit_spheres(const ColorCloud& cloud, const SphereFitParams& params, ColorCloudPtr* remaining = nullptr);
} // namespace smcp::pointcloud
