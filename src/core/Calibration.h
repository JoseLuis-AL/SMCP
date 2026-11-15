#pragma once

// Projector-camera calibration pipeline (Moreno & Taubin), free of any user-interface dependency.
//
//   capture sets --> chessboard corners (camera) ----------------+
//        |                                                       v
//        +--------> Gray-code decoding --> local homographies --> projector corners --> solver
//
// Run() executes the whole pipeline for a workspace. Each stage is also exposed on its own so the
// application can run "extract corners" or "decode" alone and so every stage can be tested.
// Stages are parallel where items are independent (sets, images, rows, corners) and always return
// the same numbers as the original serial implementation.

#include <string>
#include <vector>

#include <opencv2/core/core.hpp>

#include "core/CalibrationData.h"
#include "core/Progress.h"
#include "core/StructuredLight.h"

namespace smcp::Calibration
{
	/// One capture sequence on disk.
	struct CaptureSet
	{
		std::string name;
		std::vector<std::string> images; ///< Image files in capture order.
		bool selected = true;            ///< Unselected sets are skipped by every stage.
	};

	struct ChessboardParams
	{
		cv::Size cornerCount{ 22, 15 };       ///< Interior corners (columns x rows).
		cv::Size2f squareSize{ 15.f, 15.f };  ///< Square width and height, in mm.
	};

	/// Corners detected in one set. Empty vectors mean "no chessboard" (or the set was not selected).
	struct SetCorners
	{
		std::vector<cv::Point2f> camera;
		std::vector<cv::Point3f> world;
	};

	/// How the calibration is solved. The default is the classic method of the program.
	struct SolveOptions
	{
		/// Refines camera intrinsics, projector intrinsics and the stereo pose together in the stereo
		/// step, starting from the separate calibrations. Off: the stereo step only estimates the
		/// pose and keeps the intrinsics of each device as calibrated on its own.
		///
		/// On the boards of four workspaces left out of the calibration it lowered the error of the
		/// reconstructed distances between corners by 1 to 32 % and never raised it. Estimating k3,
		/// the rational distortion model and a bent model of the board were tried the same way
		/// (SMCP_calibration_compare) and gave nothing or made those boards worse, so they are not
		/// offered.
		bool jointRefinement = false;

		bool operator==(const SolveOptions& other) const { return jointRefinement == other.jointRefinement; }
	};

	struct Params
	{
		ChessboardParams chessboard;
		StructuredLight::DecodeParams decode;
		cv::Size projectorSize;          ///< Resolution the Gray codes were generated for.
		unsigned homographyWindow = 60;  ///< Side, in camera pixels, of the window around each corner.
		unsigned shadowThreshold = 0;    ///< Minimum max-min intensity for a pixel to enter a homography.
		/// Locate the chessboard corners in the projector with the sub-pixel projector coordinates
		/// (StructuredLight::RefinePattern) instead of the whole decoded ones. The images of every set
		/// are read once more to compute them.
		bool subpixelCorners = false;
		SolveOptions solve;
	};

	/// Everything the pipeline produces, index-aligned with the input sets.
	struct Result
	{
		bool success = false;            ///< True when `calibration` holds a new valid calibration.
		bool canceled = false;
		bool cornersExtracted = false;   ///< The chessboard search finished, so empty corner lists mean "not found".
		CalibrationData calibration;
		std::vector<SetCorners> corners;
		std::vector<std::vector<cv::Point2f>> projectorCorners;
		std::vector<StructuredLight::DecodedSet> decoded;
		cv::Size imageSize;
	};

	/// Planar chessboard coordinates (Z = 0), row by row.
	std::vector<cv::Point3f> ChessboardWorldCoords(cv::Size cornerCount, cv::Size2f squareSize);

	/// Finds the interior corners of a chessboard in a gray image, refined to sub-pixel accuracy.
	/// Detection runs on a copy reduced by `imageScale` (1 = full size). Returns false and leaves
	/// `corners` empty when the board is not found.
	bool DetectChessboard(const cv::Mat& grayImage, cv::Size cornerCount, int imageScale, std::vector<cv::Point2f>& corners);

	/// Reduction factor used for detection: images wider than 1024 pixels are detected at about that width.
	int DetectionScale(cv::Size imageSize);

	/// Stage 1: detects the chessboard in image #1 of every selected set (sets run in parallel).
	/// Returns false on cancellation or when a selected image has a different size than the others.
	bool ExtractCorners(const std::vector<CaptureSet>& sets, const ChessboardParams& params,
		std::vector<SetCorners>& corners, cv::Size& imageSize, ProgressSink& progress);

	/// Stage 3: maps every camera corner to projector coordinates with a homography fitted to the
	/// decoded correspondences inside a window around it. Returns false and fills `error` when a
	/// corner is too close to the image border for the window or a homography cannot be estimated.
	bool ProjectorCorners(const StructuredLight::DecodedSet& decoded, const std::vector<cv::Point2f>& cameraCorners,
		unsigned homographyWindow, unsigned shadowThreshold, std::vector<cv::Point2f>& projectorCorners, std::string& error);

	/// Stage 3 with sub-pixel projector coordinates: reads the images of the set again, refines the
	/// decoded map and fits the homographies on it. `progress` may be null.
	bool SubpixelProjectorCorners(const std::vector<std::string>& images, const StructuredLight::DecodedSet& decoded,
		const std::vector<cv::Point2f>& cameraCorners, cv::Size projectorSize, unsigned homographyWindow,
		unsigned shadowThreshold, std::vector<cv::Point2f>& projectorCorners, std::string& error, ProgressSink* progress = nullptr);

	/// Stage 4: camera intrinsics, projector intrinsics and stereo extrinsics from the active sets.
	bool Solve(const std::vector<std::vector<cv::Point3f>>& world, const std::vector<std::vector<cv::Point2f>>& camera,
		const std::vector<std::vector<cv::Point2f>>& projector, cv::Size imageSize, cv::Size projectorSize,
		CalibrationData& calibration, std::string& error, ProgressSink& progress, const SolveOptions& options = SolveOptions());

	/// The whole pipeline. `cached[i]`, when it holds data, is used instead of decoding set i again
	/// (the caller guarantees it was decoded from the same files with the same parameters).
	Result Run(const std::vector<CaptureSet>& sets, const Params& params,
		const std::vector<StructuredLight::DecodedSet>& cached, ProgressSink& progress);

	/// Writes world_NN.txt, cam_NN.txt and proj_NN.txt (one line per corner) into `directory`.
	bool SaveCorners(const std::string& directory, const Result& result, std::string& error);
} // namespace smcp::Calibration
