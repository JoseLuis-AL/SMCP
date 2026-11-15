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

// Evaluation of the SMCP compensation models written by the pinn-smcp trainer (scripts-wsl): a physical
// block that scales each point along its camera ray plus a small bounded network, both in the camera
// frame of a raw reconstruction.

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include <QString>
#include <opencv2/core/core.hpp>

#include "core/PointcloudOps.h"

namespace smcp::Pointcloud
{
/// p' = p * (1 + s) + w * r. s = a0 + a1*zn + a2*zn^2 + a3*rho2 + a4*rho2^2 + a5*zn*rho2 with
/// zn = (z - zCenter) / zScale and rho2 = (x^2 + y^2) / z^2. r is a tanh perceptron on the point
/// normalized to [lower, upper], bounded to +/- outputBound per axis; w is 1 inside that volume and
/// fades linearly to 0 within extrapolationMargin (a fraction of the volume size) outside it.
struct CompensationModel
{
	struct Layer
	{
		int inputs = 0;
		int outputs = 0;
		std::vector<double> weight;  ///< Row-major, outputs x inputs.
		std::vector<double> bias;
	};

	std::array<double, 6> coefficients{};
	double zCenter = 0.0;
	double zScale = 1.0;
	cv::Vec3d lower;
	cv::Vec3d upper;
	double outputBound = 0.0;
	double extrapolationMargin = 0.1;
	std::vector<Layer> layers;

	double fixedScale = 1.0;
	std::string calibrationSha256;  ///< Empty when the model was trained without a calibration file.
	int trainingScans = 0;
	/// Cross-validated RMS errors (um) reported by the trainer; negative when it did not validate.
	double validationFixedScale = -1.0;
	double validationPhysicsOnly = -1.0;
	double validationPinn = -1.0;

	/// Corrected point; `outside` tells whether it lies beyond the trained volume. Points with z <= 0 (behind
	/// the camera) are returned unchanged.
	cv::Vec3d Correct(const cv::Vec3d& point, bool* outside = nullptr) const;
};

struct CompensationStats
{
	std::size_t points = 0;   ///< Finite points corrected.
	std::size_t outside = 0;  ///< Of those, points beyond the trained volume (physical block mostly).
};

/// Reads a model and checks it against its test vectors, so an evaluator that does not reproduce the
/// trainer exactly is rejected instead of silently applying a different correction.
bool LoadCompensationModel(const QString& path, CompensationModel& model, QString& error);

/// SHA-256 of a file in lowercase hexadecimal, or an empty string when it cannot be read.
std::string FileSha256(const QString& path);

/// Human-readable summary of a model: its training, cross-validated errors, and whether it was trained
/// with the calibration at `calibrationPath` (empty or missing file: not compared).
QString DescribeCompensationModel(const CompensationModel& model, const QString& calibrationPath);

/// True when the model stores a calibration hash and the file at `calibrationPath` has a different one.
bool CompensationCalibrationDiffers(const CompensationModel& model, const QString& calibrationPath);

/// Corrected copy of a colored cloud; non-finite points are copied unchanged.
ColorCloudPtr Compensate(const ColorCloud& cloud, const CompensationModel& model, CompensationStats* stats = nullptr);

/// Corrects a CV_32FC3 point matrix (a reconstruction) in place, skipping NaN points.
void Compensate(cv::Mat& points, const CompensationModel& model, CompensationStats* stats = nullptr);
} // namespace smcp::Pointcloud
