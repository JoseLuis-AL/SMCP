/*
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
*/

#pragma once

// Measurement of reference spheres in structured-light scans: detection, geometric least-squares fit with
// outlier rejection, and labeling against a reference artifact measured on a CMM. Independent of Qt.

#include <cstddef>
#include <string>
#include <vector>

#include <opencv2/core/core.hpp>

#include "core/PointcloudOps.h"

namespace smcp::Pointcloud
{
/// Reference artifact measured on a CMM: named spheres, their diameters, and the distances between their
/// centers, in the units of the clouds (mm). A value of 0 means unknown.
struct ArtifactReference
{
	std::vector<std::string> names;
	std::vector<double> diameters;
	std::vector<std::vector<double>> distances;  ///< Symmetric, distances[i][j] for i != j.
	std::string name;                            ///< Artifact name from its file; may be empty.

	std::size_t Count() const { return names.size(); }
};

struct SphereMeasurementParams
{
	double nominalDiameter = 100.0;
	double radiusTolerance = 0.05;   ///< Detection accepts radii within nominal ± this fraction.
	double ransacThreshold = 1.0;    ///< Distance to the surface that counts as a RANSAC inlier.
	int ransacIterations = 2000;
	std::size_t expectedSpheres = 3;
	bool removePlanes = true;        ///< Removes dominant planes (table, background) before detecting.
	double rejectionSigma = 2.5;     ///< Points beyond this many standard deviations are rejected.
	int maxRejectionIterations = 20;
};

struct SphereFitResult
{
	bool valid = false;
	cv::Vec3d center;
	double radius = 0.0;
	std::vector<int> inliers;  ///< Indices of the points kept by the last iteration.
	double formRms = 0.0;      ///< RMS of the radial residuals of the kept points.
	double formRange = 0.0;    ///< Maximum minus minimum radial residual (peak to valley).
	int rejectionIterations = 0;
};

struct MeasuredSphere
{
	cv::Vec3d center;
	double radius = 0.0;
	double formRms = 0.0;
	double formRange = 0.0;
	ColorCloudPtr points;  ///< Points kept by the final fit.
	int label = -1;        ///< Index in the reference, or -1 when unlabeled.

	double Diameter() const { return 2.0 * radius; }
};

/// Geometric least-squares fit of |p - c| - r (Levenberg-Marquardt from `initial`). The fit is repeated
/// after rejecting the points whose residual exceeds `rejectionSigma` standard deviations, until the kept
/// set no longer changes or `maxRejectionIterations` is reached.
SphereFitResult FitSphereGeometric(const ColorCloud& points, const SphereModel& initial, double rejectionSigma,
	int maxRejectionIterations);

/// Detects up to `expectedSpheres` spheres of the nominal diameter (RANSAC, after removing dominant planes
/// when requested) and refines each one with FitSphereGeometric over the points near its surface.
/// Spheres are returned in detection order and unlabeled.
std::vector<MeasuredSphere> MeasureSpheres(const ColorCloud& cloud, const SphereMeasurementParams& params,
	const CancelFlag* cancel = nullptr);

/// One value of a scan compared with the artifact: a sphere diameter or a distance between two centers.
struct Measurand
{
	double measured = 0.0;
	double reference = 0.0;
	bool diameter = false;
};

/// Diameters and center distances of labeled spheres that have a reference value (> 0).
std::vector<Measurand> Measurands(const std::vector<MeasuredSphere>& spheres, const ArtifactReference& reference);

/// Fixed scale k minimizing sum((k * measured - reference)^2): the least-squares correction by a single
/// factor. Returns 1 when there are no measurands.
double FixedScale(const std::vector<Measurand>& measurands);

/// Spread of sphere centers: per-axis range and the Pearson correlation between axes. Correlations
/// are 0 when an axis does not vary or there are fewer than three centers.
struct CoverageStats
{
	std::size_t count = 0;
	cv::Vec3d minimum;
	cv::Vec3d maximum;
	double correlationXY = 0.0;
	double correlationXZ = 0.0;
	double correlationYZ = 0.0;

	cv::Vec3d Range() const { return maximum - minimum; }
};
CoverageStats Coverage(const std::vector<cv::Vec3d>& centers);

/// Labels the spheres with the reference permutation whose center distances and diameters best match it
/// and sorts them by label. Requires as many spheres as reference spheres (at most 8); returns false
/// otherwise, leaving the spheres unlabeled.
bool LabelSpheres(std::vector<MeasuredSphere>& spheres, const ArtifactReference& reference);
} // namespace smcp::Pointcloud
