/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Brown University nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DANIEL MORENO AND GABRIEL TAUBIN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

// Operations on colored point clouds (x, y, z, r, g, b) used by the point cloud editor:
// statistical outlier removal and RANSAC fitting of planes and spheres.
// No external dependencies beyond OpenCV (flann for nearest neighbors).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <opencv2/core/core.hpp>

namespace smcp::Pointcloud
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

/// Finite coordinates (no NaN and no infinities).
bool IsFinite(const ColorPoint& point);

/* Outliers ================================================================================ */

struct OutlierRemovalParams
{
	int meanK = 50;           ///< Neighbors considered per point.
	double stddevMult = 1.0;  ///< Threshold = mean + stddevMult * standard deviation.
};

/// Statistical Outlier Removal: discards the points whose mean distance to their
/// meanK neighbors exceeds the global mean plus stddevMult standard deviations.
ColorCloudPtr RemoveStatisticalOutliers(const ColorCloud& cloud, const OutlierRemovalParams& params);

/* Planes ================================================================================== */

struct PlaneModel
{
	cv::Vec3f normal{ 0.0f, 0.0f, 1.0f };  ///< Unit normal.
	float d = 0.0f;                          ///< normal . p + d = 0
};

struct PlaneFitParams
{
	int maxPlanes = 5;               ///< Maximum number of planes to extract.
	int maxIterations = 1000;        ///< RANSAC iterations per plane.
	double distanceThreshold = 0.5;  ///< Maximum point-plane distance to count as an inlier.
	std::size_t minInliers = 0;      ///< Minimum inliers; 0 = max(200, n / 300).
	bool optimizeCoefficients = true;///< Least-squares refit using the inliers.
};

struct PlaneFit
{
	ColorCloudPtr points;  ///< Inliers of the plane.
	PlaneModel model;
};

/// Extracts dominant planes iteratively (RANSAC + inlier removal).
/// If `remaining` is passed, it receives the points that belong to no plane.
std::vector<PlaneFit> FitPlanes(const ColorCloud& cloud, const PlaneFitParams& params, ColorCloudPtr* remaining = nullptr);

/* Spheres ================================================================================= */

struct SphereModel
{
	cv::Point3f center;
	float radius = 0.0f;
};

struct SphereFitParams
{
	int maxSpheres = 5;
	int maxIterations = 1000;
	double distanceThreshold = 0.25;  ///< Maximum distance to the surface to count as an inlier.
	double minRadius = 1.0;
	double maxRadius = 10000.0;
	std::size_t minInliers = 0;       ///< 0 = max(50, n / 100).
	bool optimizeCoefficients = true;
};

struct SphereFit
{
	ColorCloudPtr points;
	SphereModel model;
};

/// Extracts spheres iteratively (RANSAC + inlier removal).
/// If `remaining` is passed, it receives the points that belong to no sphere.
std::vector<SphereFit> FitSpheres(const ColorCloud& cloud, const SphereFitParams& params, ColorCloudPtr* remaining = nullptr);
} // namespace smcp::Pointcloud
