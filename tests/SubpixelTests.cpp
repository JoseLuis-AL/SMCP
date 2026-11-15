// Sub-pixel decoding (StructuredLight::RefinePattern) and reconstruction
// (Scan3d::ReconstructModelSubpixel) on synthetic data with a known answer.

#include <cmath>
#include <iostream>
#include <vector>

#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "core/Calibration.h"
#include "core/CalibrationData.h"
#include "core/Scan3d.h"
#include "core/StructuredLight.h"

namespace
{
	bool Check(bool condition, const char* message)
	{
		if (!condition)
			std::cerr << "FAIL: " << message << '\n';
		return condition;
	}

	/// A camera looking at a projector whose pixels span about 3.5 camera pixels, slightly rotated.
	struct Scene
	{
		static constexpr int Cols = 320, Rows = 240, Bits = 7;
		cv::Size projector{ 128, 96 };

		double Column(double x, double y) const { return 0.283 * x + 0.011 * y + 5.3; }
		double Row(double x, double y) const { return 0.271 * y - 0.008 * x + 14.1; }
	};

	/// The capture set of `scene`: white, black, then the pairs (stripes, inverse) of every bit of the
	/// columns and of the rows, most significant bit first. Each camera pixel integrates the stripe
	/// over its area, and the image is slightly blurred, like a real lens.
	std::vector<cv::Mat> Capture(const Scene& scene)
	{
		const int offset[2] = { ((1 << Scene::Bits) - scene.projector.width) / 2,
			((1 << Scene::Bits) - scene.projector.height) / 2 };
		std::vector<cv::Mat> images;
		images.push_back(cv::Mat(Scene::Rows, Scene::Cols, CV_8UC1, cv::Scalar(220)));
		images.push_back(cv::Mat(Scene::Rows, Scene::Cols, CV_8UC1, cv::Scalar(20)));
		constexpr int Samples = 6;
		for (int channel = 0; channel < 2; channel++)
			for (int bit = Scene::Bits - 1; bit >= 0; bit--)
			{
				cv::Mat stripes(Scene::Rows, Scene::Cols, CV_32FC1);
				for (int y = 0; y < Scene::Rows; y++)
					for (int x = 0; x < Scene::Cols; x++)
					{
						int lit = 0;
						for (int sy = 0; sy < Samples; sy++)
							for (int sx = 0; sx < Samples; sx++)
							{
								const double px = x - 0.5 + (sx + 0.5) / Samples, py = y - 0.5 + (sy + 0.5) / Samples;
								const double coordinate = channel == 0 ? scene.Column(px, py) : scene.Row(px, py);
								// A whole coordinate is the centre of a projector pixel.
								const unsigned n = static_cast<unsigned>(static_cast<int>(std::floor(coordinate + 0.5)) + offset[channel]);
								lit += ((n ^ (n >> 1)) >> bit) & 1;
							}
						stripes.at<float>(y, x) = static_cast<float>(lit) / (Samples * Samples);
					}
				cv::GaussianBlur(stripes, stripes, cv::Size(0, 0), 0.6);
				cv::Mat normal, inverse;
				stripes.convertTo(normal, CV_8UC1, 200.0, 20.0);
				cv::Mat(1.0 - stripes).convertTo(inverse, CV_8UC1, 200.0, 20.0);
				images.push_back(normal);
				images.push_back(inverse);
			}
		return images;
	}

	/// RMS error of a projector-coordinate image against the scene, and how many pixels have a value.
	double Error(const Scene& scene, const cv::Mat& coordinates, int& valid)
	{
		double sum = 0.0;
		valid = 0;
		for (int y = 8; y < Scene::Rows - 8; y++)
			for (int x = 8; x < Scene::Cols - 8; x++)
			{
				const cv::Vec2f& value = coordinates.at<cv::Vec2f>(y, x);
				if (smcp::StructuredLight::Invalid(value))
					continue;
				const double du = value[0] - scene.Column(x, y), dv = value[1] - scene.Row(x, y);
				sum += du * du + dv * dv;
				valid++;
			}
		return valid ? std::sqrt(sum / (2.0 * valid)) : 1e9;
	}

	bool RefineChecks()
	{
		using namespace smcp;
		bool ok = true;
		const Scene scene;
		const std::vector<cv::Mat> images = Capture(scene);
		cv::Mat pattern, minMax;
		std::string error;
		ok &= Check(StructuredLight::DecodePattern(images, pattern, minMax, scene.projector,
						StructuredLight::GrayPatternDecode, cv::Mat(), 0, error),
			"the synthetic set decodes");
		if (!ok)
			return false;

		int wholeCount = 0, refinedCount = 0, halfwayCount = 0;
		const double whole = Error(scene, pattern, wholeCount);
		const cv::Mat refined = StructuredLight::RefinePattern(images, pattern, minMax, scene.projector, 20);
		const cv::Mat halfway = StructuredLight::RefinePattern({}, pattern, minMax, scene.projector, 20);
		ok &= Check(refined.size() == pattern.size() && refined.type() == CV_32FC2, "refined image has the camera size");
		const double fine = Error(scene, refined, refinedCount);
		const double coarse = Error(scene, halfway, halfwayCount);
		std::cout << "projector coordinate RMS error: whole codes " << whole << ", borders halfway " << coarse
				  << ", borders from intensities " << fine << " (pixels " << wholeCount << " / " << halfwayCount << " / "
				  << refinedCount << ")\n";
		ok &= Check(whole > 0.25, "whole codes carry the quantization error of one projector pixel");
		ok &= Check(fine < 0.05, "intensity borders locate the projector coordinate to a few hundredths of a pixel");
		ok &= Check(coarse < 0.5 * whole && fine < coarse, "even without images the interpolation halves the error");
		ok &= Check(refinedCount > 0.97 * wholeCount, "nearly every decoded pixel gets a refined value");

		// A pixel that cannot be used breaks the interpolation only where a border is missing.
		cv::Mat holed = pattern.clone();
		holed.at<cv::Vec2f>(120, 160) = cv::Vec2f(StructuredLight::Pixel_Uncertain, StructuredLight::Pixel_Uncertain);
		const cv::Mat around = StructuredLight::RefinePattern(images, holed, minMax, scene.projector, 20);
		ok &= Check(StructuredLight::Invalid(around.at<cv::Vec2f>(120, 160)), "an uncertain pixel stays without value");
		int near = 0;
		Error(scene, around, near);
		ok &= Check(near >= refinedCount - 12, "an uncertain pixel costs at most its own two runs");

		// A jump of several projector pixels (a depth edge) is never interpolated across.
		cv::Mat jump = pattern.clone();
		for (int y = 0; y < Scene::Rows; y++)
			for (int x = 200; x < Scene::Cols; x++)
				jump.at<cv::Vec2f>(y, x)[0] += 9.f;
		const cv::Mat split = StructuredLight::RefinePattern({}, jump, minMax, scene.projector, 20);
		bool bounded = true;
		for (int y = 8; y < Scene::Rows - 8; y++)
			for (int x = 190; x < 210; x++)
			{
				const float value = split.at<cv::Vec2f>(y, x)[0];
				bounded &= std::isnan(value) || std::abs(value - jump.at<cv::Vec2f>(y, x)[0]) <= 0.5f;
			}
		ok &= Check(bounded, "values beside a jump stay inside their own projector pixel");

		ok &= Check(StructuredLight::RefinePattern(images, cv::Mat(), minMax, scene.projector, 20).empty(),
			"invalid input gives an empty result");
		return ok;
	}

	bool ReconstructionChecks()
	{
		using namespace smcp;
		bool ok = true;
		const int cols = 160, rows = 120;
		const cv::Size projector(128, 96);
		CalibrationData calib;
		calib.camK = (cv::Mat_<double>(3, 3) << 400, 0, 80, 0, 400, 60, 0, 0, 1);
		calib.projK = (cv::Mat_<double>(3, 3) << 150, 0, 64, 0, 150, 90, 0, 0, 1);
		calib.camKc = (cv::Mat_<double>(1, 5) << -0.05, 0.02, 0.001, -0.001, 0);
		calib.projKc = (cv::Mat_<double>(1, 5) << 0.03, -0.01, 0, 0.0005, 0);
		cv::Mat rotation;
		cv::Rodrigues(cv::Mat(cv::Vec3d(0.02, -0.25, 0.01)), rotation);
		calib.R = rotation;
		calib.T = (cv::Mat_<double>(3, 1) << 180.0, -15.0, 40.0);

		// A tilted plane in front of the camera: the point of every camera pixel, and where the
		// projector sees it (X_projector = R X_camera + T).
		cv::Mat pixels(rows * cols, 1, CV_64FC2), rays;
		for (int y = 0; y < rows; y++)
			for (int x = 0; x < cols; x++)
				pixels.at<cv::Vec2d>(y * cols + x, 0) = cv::Vec2d(x, y);
		cv::undistortPoints(pixels, rays, calib.camK, calib.camKc);
		std::vector<cv::Point3d> truth(rows * cols);
		for (int i = 0; i < rows * cols; i++)
		{
			const cv::Vec2d& ray = rays.at<cv::Vec2d>(i, 0);
			// Plane z = 600 + 0.3 x - 0.2 y along the ray (x, y, 1) * z.
			const double z = 600.0 / (1.0 - 0.3 * ray[0] + 0.2 * ray[1]);
			truth[i] = cv::Point3d(ray[0] * z, ray[1] * z, z);
		}
		std::vector<cv::Point2d> projected;
		cv::Mat rvec;
		cv::Rodrigues(calib.R, rvec);
		cv::projectPoints(truth, rvec, calib.T, calib.projK, calib.projKc, projected);

		cv::Mat subpixel(rows, cols, CV_32FC2), minMax(rows, cols, CV_8UC2, cv::Scalar(10, 200));
		cv::Mat colors(rows, cols, CV_8UC3, cv::Scalar(10, 20, 30));
		int inside = 0;
		for (int i = 0; i < rows * cols; i++)
		{
			const bool visible = projected[i].x >= 0 && projected[i].x < projector.width && projected[i].y >= 0 &&
				projected[i].y < projector.height;
			inside += visible;
			subpixel.at<cv::Vec2f>(i / cols, i % cols) = visible
				? cv::Vec2f(static_cast<float>(projected[i].x), static_cast<float>(projected[i].y))
				: cv::Vec2f(StructuredLight::Pixel_Uncertain, StructuredLight::Pixel_Uncertain);
		}
		ok &= Check(inside > rows * cols / 4, "the synthetic plane is seen by the projector");
		minMax.at<cv::Vec2b>(5, 5) = cv::Vec2b(100, 105);  // a pixel below the contrast threshold

		Scan3d::Pointcloud cloud;
		Scan3d::ReconstructModelSubpixel(cloud, calib, subpixel, minMax, colors, projector, 20, 1.0);
		ok &= Check(cloud.points.rows == rows && cloud.points.cols == cols, "the cloud is organized like the camera image");
		int reconstructed = 0;
		double worst = 0.0;
		for (int i = 0; i < rows * cols && cloud.points.data; i++)
		{
			const cv::Vec3f& p = cloud.points.at<cv::Vec3f>(i / cols, i % cols);
			if (StructuredLight::Invalid(p))
				continue;
			reconstructed++;
			worst = std::max(worst, cv::norm(cv::Point3d(p[0], p[1], p[2]) - truth[i]));
		}
		std::cout << "sub-pixel reconstruction: " << reconstructed << " of " << inside << " points, worst error " << worst
				  << " mm\n";
		ok &= Check(reconstructed >= inside - 2 && reconstructed <= inside, "every visible pixel gives one point");
		ok &= Check(worst < 0.02, "exact coordinates reconstruct the plane (float precision)");
		ok &= Check(StructuredLight::Invalid(cloud.points.at<cv::Vec3f>(5, 5)) || !inside, "low-contrast pixels are left out");
		ok &= Check(cloud.colors.at<cv::Vec3b>(rows / 2, cols / 2) == cv::Vec3b(10, 20, 30) ||
				StructuredLight::Invalid(cloud.points.at<cv::Vec3f>(rows / 2, cols / 2)),
			"points take the color of their camera pixel");

		// Projector-plane triangulation gives the same plane from exact coordinates...
		Scan3d::Pointcloud planeCloud;
		Scan3d::ReconstructModelSubpixel(planeCloud, calib, subpixel, minMax, colors, projector, 20, 1.0, nullptr,
			Scan3d::Triangulation::ProjectorPlane);
		// ...and, unlike the midpoint, barely moves when the projector row (which carries no depth
		// here: the projector is beside the camera) is wrong by a quarter of a pixel.
		cv::Mat shifted = subpixel.clone();
		for (int i = 0; i < rows * cols; i++)
			shifted.at<cv::Vec2f>(i / cols, i % cols)[1] += 0.25f;
		Scan3d::Pointcloud planeShifted, midpointShifted;
		Scan3d::ReconstructModelSubpixel(planeShifted, calib, shifted, minMax, colors, projector, 20, 5.0, nullptr,
			Scan3d::Triangulation::ProjectorPlane);
		Scan3d::ReconstructModelSubpixel(midpointShifted, calib, shifted, minMax, colors, projector, 20, 5.0);
		double planeWorst = 0.0, planeShift = 0.0, midpointShift = 0.0;
		int planePoints = 0;
		for (int i = 0; i < rows * cols && planeCloud.points.data && planeShifted.points.data && midpointShifted.points.data; i++)
		{
			const cv::Vec3f& p = planeCloud.points.at<cv::Vec3f>(i / cols, i % cols);
			const cv::Vec3f& a = planeShifted.points.at<cv::Vec3f>(i / cols, i % cols);
			const cv::Vec3f& b = midpointShifted.points.at<cv::Vec3f>(i / cols, i % cols);
			if (StructuredLight::Invalid(p) || StructuredLight::Invalid(a) || StructuredLight::Invalid(b))
				continue;
			planePoints++;
			planeWorst = std::max(planeWorst, cv::norm(cv::Point3d(p[0], p[1], p[2]) - truth[i]));
			planeShift = std::max(planeShift, cv::norm(cv::Point3d(a[0], a[1], a[2]) - truth[i]));
			midpointShift = std::max(midpointShift, cv::norm(cv::Point3d(b[0], b[1], b[2]) - truth[i]));
		}
		std::cout << "projector-plane triangulation: worst error " << planeWorst << " mm; with the row off by 0.25 px, plane "
				  << planeShift << " mm, midpoint " << midpointShift << " mm\n";
		ok &= Check(planePoints > inside / 2 && planeWorst < 0.02, "the projector plane reconstructs the same plane");
		ok &= Check(planeShift < 0.2 * midpointShift && midpointShift > 0.05,
			"a wrong projector row moves the midpoint but hardly the projector-plane point");

		Scan3d::Pointcloud empty;
		Scan3d::ReconstructModelSubpixel(empty, CalibrationData(), subpixel, minMax, colors, projector, 20, 1.0);
		ok &= Check(!empty.points.data, "no cloud without a calibration");
		return ok;
	}
	/// Both ways of solving the calibration recover a synthetic camera-projector pair from exact
	/// corners, and the joint refinement fits them at least as well as the classic method.
	bool CalibrationChecks()
	{
		using namespace smcp;
		bool ok = true;
		const cv::Size imageSize(1200, 900), projectorSize(800, 600);
		const cv::Mat camK = (cv::Mat_<double>(3, 3) << 1800, 0, 610, 0, 1795, 440, 0, 0, 1);
		const cv::Mat projK = (cv::Mat_<double>(3, 3) << 1100, 0, 395, 0, 1105, 480, 0, 0, 1);
		const cv::Mat camKc = (cv::Mat_<double>(1, 5) << -0.08, 0.12, 0.0005, -0.0004, 0);
		const cv::Mat projKc = (cv::Mat_<double>(1, 5) << 0.05, -0.06, -0.0006, 0.0003, 0);
		cv::Mat stereoR;
		cv::Rodrigues(cv::Mat(cv::Vec3d(0.03, -0.3, 0.02)), stereoR);
		const cv::Mat stereoT = (cv::Mat_<double>(3, 1) << -250.0, -30.0, 20.0);

		const std::vector<cv::Point3f> board = Calibration::ChessboardWorldCoords(cv::Size(10, 8), cv::Size2f(15.f, 15.f));
		std::vector<std::vector<cv::Point3f>> world;
		std::vector<std::vector<cv::Point2f>> camera, projector;
		for (int pose = 0; pose < 9; pose++)
		{
			// Boards in front of the camera with different tilts, positions and depths.
			const cv::Vec3d rvec(0.35 * ((pose % 3) - 1), 0.3 * ((pose / 3) - 1), 0.1 * pose);
			const cv::Vec3d tvec(-70.0 + 25.0 * (pose % 3), -50.0 + 20.0 * (pose / 3), 560.0 + 15.0 * pose);
			cv::Mat poseR;
			cv::Rodrigues(cv::Mat(rvec), poseR);
			// The projector sees the board through X_projector = R X_camera + T.
			cv::Mat projectorR = stereoR * poseR, projectorRvec;
			const cv::Mat projectorT = stereoR * cv::Mat(tvec) + stereoT;
			cv::Rodrigues(projectorR, projectorRvec);
			std::vector<cv::Point2f> inCamera, inProjector;
			cv::projectPoints(board, rvec, tvec, camK, camKc, inCamera);
			cv::projectPoints(board, projectorRvec, projectorT, projK, projKc, inProjector);
			world.push_back(board);
			camera.push_back(inCamera);
			projector.push_back(inProjector);
		}

		class Silent : public ProgressSink
		{
		public:
			void Message(const std::string&) override {}
			void Status(const std::string&) override {}
			void Step(int, int) override {}
			bool Canceled() const override { return false; }
		} progress;
		Calibration::SolveOptions joint;
		joint.jointRefinement = true;
		CalibrationData classicResult, jointResult;
		std::string error;
		ok &= Check(Calibration::Solve(world, camera, projector, imageSize, projectorSize, classicResult, error, progress),
			"the classic calibration solves the synthetic sets");
		ok &= Check(Calibration::Solve(world, camera, projector, imageSize, projectorSize, jointResult, error, progress, joint),
			"the joint refinement solves the synthetic sets");
		if (!ok)
			return false;
		const double classicT = cv::norm(classicResult.T, stereoT), jointT = cv::norm(jointResult.T, stereoT);
		std::cout << "calibration of a synthetic pair: stereo error classic " << classicResult.stereoError << " px, joint "
				  << jointResult.stereoError << " px; error of T " << classicT << " mm and " << jointT << " mm\n";
		// The classic method stops a few millimetres away even with exact corners: it calibrates each
		// device on its own with a limited number of iterations and then keeps those intrinsics.
		ok &= Check(classicT < 5.0, "the classic method places the projector within millimetres");
		ok &= Check(jointT < 0.01, "the joint refinement recovers the position of the projector");
		ok &= Check(std::abs(jointResult.camK.at<double>(0, 0) - 1800.0) < 2.0 &&
				std::abs(jointResult.projK.at<double>(0, 0) - 1100.0) < 2.0,
			"the joint refinement recovers both focal lengths");
		ok &= Check(jointResult.stereoError <= classicResult.stereoError + 1e-6,
			"the joint refinement fits the sets at least as well as the classic method");
		ok &= Check(Calibration::SolveOptions() == Calibration::SolveOptions() && !(joint == Calibration::SolveOptions()),
			"solve options compare by value");
		return ok;
	}
} // namespace

int main()
{
	bool ok = true;
	ok &= RefineChecks();
	ok &= ReconstructionChecks();
	ok &= CalibrationChecks();
	std::cout << (ok ? "sub-pixel tests passed" : "sub-pixel tests FAILED") << std::endl;
	return ok ? 0 : 1;
}
