// Golden regression of the scanning pipeline against the original serial implementation.
//
//   SMCP_pipeline_regress <workspace> [--cols N --rows N --square W H --b B --m M --hwin N
//                                      --shadow N --threshold N --maxdist D --skip i,j,...]
//
// The reference below is the code SMCP shipped before the restructuring: tests/legacy holds the
// untouched StructuredLight/Scan3d sources, and the Legacy namespace in this file transcribes the
// orchestration that lived in Application (corner extraction, per-set decoding, the homography
// loop and the solver calls). The tool runs both pipelines on a real workspace and requires every
// intermediate and final result to match bit for bit. It never writes inside the workspace.

#include <chrono>
#include <cfloat>
#include <cstring>
#include <iostream>
#include <set>

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "core/Calibration.h"
#include "core/Scan3d.h"
#include "core/StructuredLight.h"
#include "legacy/LegacyScan3d.h"
#include "legacy/LegacyStructuredLight.h"

namespace
{
	struct Options
	{
		int cols = 22, rows = 15;
		double squareW = 15, squareH = 15;
		float b = 0.5f;
		unsigned m = 5, hwin = 60, shadow = 0;
		int threshold = 20;
		double maxDist = 100.0;
		std::set<int> skip;
	};

	struct Workspace
	{
		std::vector<smcp::Calibration::CaptureSet> sets;
		cv::Size projectorSize{ 1024, 768 };
	};

	double Seconds(std::chrono::steady_clock::time_point since)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
	}

	/// Same discovery as Application::SetRootDir.
	Workspace ReadWorkspace(const QString& root, const Options& options)
	{
		Workspace workspace;
		QDir rootDir(root);
		bool first = true;
		for (const QString& item : rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
		{
			QDir dir(rootDir.filePath(item));
			const QStringList files = dir.entryList(QStringList() << "*.jpg" << "*.bmp" << "*.png", QDir::Files, QDir::Name);
			if (files.isEmpty())
			{
				continue;
			}
			smcp::Calibration::CaptureSet set;
			set.name = item.toStdString();
			set.selected = !options.skip.count(static_cast<int>(workspace.sets.size()));
			for (const QString& file : files)
			{
				set.images.push_back((dir.path() + "/" + file).toStdString());
			}
			if (first)
			{
				first = false;
				FILE* fp = fopen(qPrintable(root + "/" + item + "/projector_info.txt"), "r");
				if (fp)
				{
					int width, height;
					if (fscanf(fp, "%u %u", &width, &height) == 2 && width > 0 && height)
					{
						workspace.projectorSize = cv::Size(width, height);
					}
					fclose(fp);
				}
			}
			workspace.sets.push_back(set);
		}
		return workspace;
	}

	bool SameBits(const cv::Mat& a, const cv::Mat& b)
	{
		if (a.size() != b.size() || a.type() != b.type())
		{
			return false;
		}
		if (a.empty())
		{
			return true;
		}
		const std::size_t rowBytes = static_cast<std::size_t>(a.cols) * a.elemSize();
		for (int r = 0; r < a.rows; r++)
		{
			if (std::memcmp(a.ptr(r), b.ptr(r), rowBytes) != 0)
			{
				return false;
			}
		}
		return true;
	}

	template <typename Point> bool SameBits(const std::vector<Point>& a, const std::vector<Point>& b)
	{
		return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Point)) == 0);
	}

	int failures = 0;
	void Check(bool condition, const std::string& what)
	{
		if (!condition)
		{
			++failures;
			std::cout << "  MISMATCH: " << what << std::endl;
		}
	}
} // namespace

/* Original orchestration (Application.cpp before the restructuring) ============================ */
namespace Legacy
{
	using namespace smcp_legacy;

	cv::Mat GetImage(const std::string& filename, bool gray)
	{
		cv::Mat rgbImage = cv::imread(filename);
		if (rgbImage.rows > 0 && rgbImage.cols > 0)
		{
			if (!gray)
			{
				return rgbImage;
			}
			cv::Mat grayImage;
			cvtColor(rgbImage, grayImage, CV_BGR2GRAY);
			return grayImage;
		}
		return cv::Mat();
	}

	void GetChessboardWorldCoords(std::vector<cv::Point3f>& worldCorners, cv::Size cornerCount, cv::Size cornerSize)
	{
		for (int h = 0; h < cornerCount.height; h++)
		{
			for (int w = 0; w < cornerCount.width; w++)
			{
				worldCorners.push_back(cv::Point3f(cornerSize.width * w, cornerSize.height * h, 0.f));
			}
		}
	}

	bool ExtractChessboardCorners(const Workspace& workspace, cv::Size cornerCount, cv::Size2f cornerSize,
		std::vector<std::vector<cv::Point3f>>& cornersWorld, std::vector<std::vector<cv::Point2f>>& cornersCamera)
	{
		unsigned count = static_cast<unsigned>(workspace.sets.size());
		cornersWorld.clear();
		cornersCamera.clear();
		cornersWorld.resize(count);
		cornersCamera.resize(count);

		cv::Size imageSize(0, 0);
		int imageScale = 1;

		for (unsigned i = 0; i < count; i++)
		{
			if (!workspace.sets[i].selected)
			{
				continue;
			}
			cv::Mat grayImage = GetImage(workspace.sets[i].images.at(1), true);
			if (grayImage.rows < 1)
			{
				continue;
			}

			if (imageSize.width == 0)
			{
				imageSize = grayImage.size();
				if (imageSize.width > 1024)
				{
					imageScale = cvRound(imageSize.width / 1024.0);
				}
			}
			else if (imageSize != grayImage.size())
			{
				return false;
			}

			cv::Mat smallImg;
			if (imageScale > 1)
			{
				cv::resize(grayImage, smallImg, cv::Size(grayImage.cols / imageScale, grayImage.rows / imageScale));
			}
			else
			{
				grayImage.copyTo(smallImg);
			}

			std::vector<cv::Point2f>& camCorners = cornersCamera[i];
			std::vector<cv::Point3f>& worldCorners = cornersWorld[i];
			if (cv::findChessboardCorners(smallImg, cornerCount, camCorners,
					cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE))
			{
				GetChessboardWorldCoords(worldCorners, cornerCount, cornerSize);
			}

			for (std::vector<cv::Point2f>::iterator iter = camCorners.begin(); iter != camCorners.end(); iter++)
			{
				*iter = imageScale * (*iter);
			}
			if (camCorners.size())
			{
				cv::cornerSubPix(grayImage, camCorners, cv::Size(11, 11), cv::Size(-1, -1),
					cv::TermCriteria(CV_TERMCRIT_EPS + CV_TERMCRIT_ITER, 30, 0.1));
			}
		}
		return true;
	}

	bool DecodeGraySet(const smcp::Calibration::CaptureSet& set, cv::Size projectorSize, float b, unsigned m,
		cv::Mat& patternImage, cv::Mat& minMaxImage)
	{
		patternImage = cv::Mat();
		minMaxImage = cv::Mat();

		std::vector<cv::Mat> images;
		int totalImages = static_cast<int>(set.images.size());
		int totalPatterns = totalImages / 2 - 1;
		const int directLightCount = 4;
		const int directLightOffset = 4;
		if (totalPatterns < directLightCount + directLightOffset)
		{
			return false;
		}

		QList<unsigned> directComponentImages;
		for (unsigned i = 0; i < directLightCount; i++)
		{
			int index = totalImages - totalPatterns - directLightCount - directLightOffset + i + 1;
			directComponentImages.append(index);
			directComponentImages.append(index + totalPatterns);
		}
		foreach(unsigned i, directComponentImages)
		{
			images.push_back(GetImage(set.images.at(i - 1), true));
		}
		cv::Mat directLight = StructuredLight::EstimateDirectLight(images, b);

		return StructuredLight::DecodePattern(set.images, patternImage, minMaxImage, projectorSize,
			StructuredLight::RobustDecode | StructuredLight::GrayPatternDecode, directLight, m);
	}

	/// The homography loop of Application::Calibrate for one set. False = the original aborted here.
	bool ProjectorCorners(const cv::Mat& patternImage, const cv::Mat& minMaxImage, const std::vector<cv::Point2f>& camCorners,
		unsigned hwin, unsigned threshold, std::vector<cv::Point2f>& projCorners)
	{
		projCorners.clear();
		for (std::vector<cv::Point2f>::const_iterator iter = camCorners.cbegin(); iter != camCorners.cend(); iter++)
		{
			const cv::Point2f& p = *iter;
			cv::Point2f q;

			unsigned WINDOW_SIZE = hwin / 2;
			std::vector<cv::Point2f> imgPoints, projPoints;
			if (p.x > WINDOW_SIZE && p.y > WINDOW_SIZE && p.x + WINDOW_SIZE < patternImage.cols && p.y + WINDOW_SIZE < patternImage.rows)
			{
				for (unsigned h = p.y - WINDOW_SIZE; h < p.y + WINDOW_SIZE; h++)
				{
					const cv::Vec2f* row = patternImage.ptr<cv::Vec2f>(h);
					const cv::Vec2b* minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);
					for (unsigned w = p.x - WINDOW_SIZE; w < p.x + WINDOW_SIZE; w++)
					{
						const cv::Vec2f& pattern = row[w];
						const cv::Vec2b& minMax = minMaxRow[w];
						if (StructuredLight::Invalid(pattern))
						{
							continue;
						}
						if ((minMax[1] - minMax[0]) < static_cast<int>(threshold))
						{
							continue;
						}

						imgPoints.push_back(cv::Point2f(w, h));
						projPoints.push_back(cv::Point2f(pattern));
					}
				}
				cv::Mat H = cv::findHomography(imgPoints, projPoints, CV_RANSAC);
				cv::Point3d Q = cv::Point3d(cv::Mat(H * cv::Mat(cv::Point3d(p.x, p.y, 1.0))));
				q = cv::Point2f(Q.x / Q.z, Q.y / Q.z);
			}
			else
			{
				return false;
			}
			projCorners.push_back(q);
		}
		return true;
	}

	bool Solve(const std::vector<std::vector<cv::Point3f>>& worldCornersActive,
		const std::vector<std::vector<cv::Point2f>>& cameraCornersActive,
		const std::vector<std::vector<cv::Point2f>>& projectorCornersActive, cv::Size imageSize, cv::Size projectorSize,
		smcp::CalibrationData& calib)
	{
		if (worldCornersActive.size() < 3)
		{
			return false;
		}
		int calFlags = 0 + cv::CALIB_FIX_K3;

		std::vector<cv::Mat> camRvecs, camTvecs;
		int camFlags = calFlags;
		calib.camError = cv::calibrateCamera(worldCornersActive, cameraCornersActive, imageSize, calib.camK, calib.camKc, camRvecs, camTvecs, camFlags,
			cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 50, DBL_EPSILON));

		std::vector<cv::Mat> projRvecs, projTvecs;
		int projFlags = calFlags;
		calib.projError = cv::calibrateCamera(worldCornersActive, projectorCornersActive, projectorSize, calib.projK, calib.projKc, projRvecs, projTvecs, projFlags,
			cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 50, DBL_EPSILON));

		cv::Mat E, F;
		calib.stereoError = cv::stereoCalibrate(worldCornersActive, cameraCornersActive, projectorCornersActive, calib.camK, calib.camKc, calib.projK, calib.projKc,
			imageSize /*ignored*/, calib.R, calib.T, E, F,
			cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 150, DBL_EPSILON),
			cv::CALIB_FIX_INTRINSIC + calFlags);
		return true;
	}
} // namespace Legacy

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cout << "usage: SMCP_pipeline_regress <workspace> [options]" << std::endl;
		return 2;
	}
	Options options;
	for (int i = 2; i < argc; i++)
	{
		const std::string arg = argv[i];
		const auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string("0"); };
		if (arg == "--cols") options.cols = std::stoi(next());
		else if (arg == "--rows") options.rows = std::stoi(next());
		else if (arg == "--square") { options.squareW = std::stod(next()); options.squareH = std::stod(next()); }
		else if (arg == "--b") options.b = std::stof(next());
		else if (arg == "--m") options.m = static_cast<unsigned>(std::stoi(next()));
		else if (arg == "--hwin") options.hwin = static_cast<unsigned>(std::stoi(next()));
		else if (arg == "--shadow") options.shadow = static_cast<unsigned>(std::stoi(next()));
		else if (arg == "--threshold") options.threshold = std::stoi(next());
		else if (arg == "--maxdist") options.maxDist = std::stod(next());
		else if (arg == "--skip")
		{
			for (const QString& item : QString::fromStdString(next()).split(',', QString::SkipEmptyParts))
			{
				options.skip.insert(item.toInt());
			}
		}
	}

	const QString root = QString::fromLocal8Bit(argv[1]);
	const Workspace workspace = ReadWorkspace(root, options);
	const std::size_t count = workspace.sets.size();
	std::cout << "Workspace: " << root.toStdString() << " (" << count << " sets, projector "
			  << workspace.projectorSize.width << "x" << workspace.projectorSize.height << ")" << std::endl;
	if (count == 0)
	{
		return 2;
	}

	const cv::Size cornerCount(options.cols, options.rows);
	const cv::Size2f cornerSize(static_cast<float>(options.squareW), static_cast<float>(options.squareH));

	/* Reference ----------------------------------------------------------------------------- */
	std::cout << "Running the original pipeline..." << std::endl;
	auto start = std::chrono::steady_clock::now();
	std::vector<std::vector<cv::Point3f>> legacyWorld;
	std::vector<std::vector<cv::Point2f>> legacyCamera, legacyProjector(count);
	std::vector<cv::Mat> legacyPattern(count), legacyMinMax(count);
	bool legacyOk = Legacy::ExtractChessboardCorners(workspace, cornerCount, cornerSize, legacyWorld, legacyCamera);
	const double legacyCornerTime = Seconds(start);
	cv::Size legacyImageSize(0, 0);
	double legacyDecodeTime = 0, legacyHomographyTime = 0;
	for (std::size_t i = 0; legacyOk && i < count; i++)
	{
		if (!workspace.sets[i].selected)
		{
			continue;
		}
		auto t = std::chrono::steady_clock::now();
		legacyOk = Legacy::DecodeGraySet(workspace.sets[i], workspace.projectorSize, options.b, options.m, legacyPattern[i], legacyMinMax[i]);
		legacyDecodeTime += Seconds(t);
		if (!legacyOk)
		{
			break;
		}
		if (legacyImageSize.width == 0)
		{
			legacyImageSize = legacyPattern[i].size();
		}
		t = std::chrono::steady_clock::now();
		// Partial detections never reached the solver; the original still ran their homographies.
		if (!legacyWorld[i].empty())
		{
			legacyOk = Legacy::ProjectorCorners(legacyPattern[i], legacyMinMax[i], legacyCamera[i], options.hwin, options.shadow, legacyProjector[i]);
		}
		legacyHomographyTime += Seconds(t);
	}
	smcp::CalibrationData legacyCalib;
	auto solveStart = std::chrono::steady_clock::now();
	if (legacyOk)
	{
		std::vector<std::vector<cv::Point3f>> world;
		std::vector<std::vector<cv::Point2f>> camera, projector;
		for (std::size_t i = 0; i < count; i++)
		{
			if (legacyWorld[i].size() && legacyCamera[i].size() && legacyProjector[i].size())
			{
				world.push_back(legacyWorld[i]);
				camera.push_back(legacyCamera[i]);
				projector.push_back(legacyProjector[i]);
			}
		}
		std::cout << "  active sets: " << world.size() << std::endl;
		legacyOk = Legacy::Solve(world, camera, projector, legacyImageSize, workspace.projectorSize, legacyCalib);
	}
	const double legacySolveTime = Seconds(solveStart);
	const double legacyTotal = Seconds(start);

	/* New pipeline --------------------------------------------------------------------------- */
	std::cout << "Running the restructured pipeline..." << std::endl;
	smcp::Calibration::Params params;
	params.chessboard.cornerCount = cornerCount;
	params.chessboard.squareSize = cornerSize;
	params.decode.b = options.b;
	params.decode.m = options.m;
	params.projectorSize = workspace.projectorSize;
	params.homographyWindow = options.hwin;
	params.shadowThreshold = options.shadow;
	smcp::NullProgress progress;
	start = std::chrono::steady_clock::now();
	const smcp::Calibration::Result result = smcp::Calibration::Run(workspace.sets, params, {}, progress);
	const double newTotal = Seconds(start);

	{	// Stage timings of the new pipeline (informative).
		auto t = std::chrono::steady_clock::now();
		std::vector<smcp::Calibration::SetCorners> corners;
		cv::Size size;
		smcp::Calibration::ExtractCorners(workspace.sets, params.chessboard, corners, size, progress);
		const double cornerTime = Seconds(t);
		t = std::chrono::steady_clock::now();
		std::vector<cv::Mat> loaded(workspace.sets[0].images.size());
		for (std::size_t i = 0; i < loaded.size(); i++)
		{
			loaded[i] = smcp::StructuredLight::GetGrayImage(workspace.sets[0].images[i]);
		}
		const double serialLoad = Seconds(t);
		t = std::chrono::steady_clock::now();
		smcp::StructuredLight::DecodedSet decoded;
		std::string error;
		smcp::StructuredLight::DecodeGraySet(workspace.sets[0].images, workspace.projectorSize, params.decode, decoded, error);
		std::cout << "  new stages: corners " << cornerTime << " s, one set decode " << Seconds(t) << " s (serial image load alone "
				  << serialLoad << " s, " << loaded[0].cols << "x" << loaded[0].rows << ")" << std::endl;
	}

	/* Comparison ----------------------------------------------------------------------------- */
	Check(result.success == legacyOk, "pipeline outcome (original " + std::to_string(legacyOk) + ", new " + std::to_string(result.success) + ")");
	for (std::size_t i = 0; i < count && i < result.corners.size(); i++)
	{
		const std::string tag = "set " + std::to_string(i) + " ";
		if (legacyWorld[i].empty())
		{
			// The original kept OpenCV's partial detection for display only.
			Check(result.corners[i].camera.empty() && result.corners[i].world.empty(), tag + "corners of a set without chessboard");
		}
		else
		{
			Check(SameBits(result.corners[i].camera, legacyCamera[i]), tag + "camera corners");
			Check(SameBits(result.corners[i].world, legacyWorld[i]), tag + "world corners");
		}
		Check(SameBits(result.decoded[i].pattern, legacyPattern[i]), tag + "decoded pattern");
		Check(SameBits(result.decoded[i].minMax, legacyMinMax[i]), tag + "min/max image");
		Check(SameBits(result.projectorCorners[i], legacyProjector[i]), tag + "projector corners");
	}
	if (legacyOk && result.success)
	{
		Check(SameBits(result.calibration.camK, legacyCalib.camK), "cam_K");
		Check(SameBits(result.calibration.camKc, legacyCalib.camKc), "cam_kc");
		Check(SameBits(result.calibration.projK, legacyCalib.projK), "proj_K");
		Check(SameBits(result.calibration.projKc, legacyCalib.projKc), "proj_kc");
		Check(SameBits(result.calibration.R, legacyCalib.R), "R");
		Check(SameBits(result.calibration.T, legacyCalib.T), "T");
		Check(result.calibration.camError == legacyCalib.camError, "cam_error");
		Check(result.calibration.projError == legacyCalib.projError, "proj_error");
		Check(result.calibration.stereoError == legacyCalib.stereoError, "stereo_error");
		std::cout.precision(17);
		std::cout << "  errors: cam " << legacyCalib.camError << "  proj " << legacyCalib.projError << "  stereo "
				  << legacyCalib.stereoError << std::endl;

		// Informative: the calibration stored in the workspace by the application, if any.
		smcp::CalibrationData stored;
		if (stored.LoadCalibration(root + "/calibration.yml") && stored.IsValid())
		{
			const bool same = cv::norm(stored.camK, legacyCalib.camK, cv::NORM_INF) < 1e-9 && cv::norm(stored.projK, legacyCalib.projK, cv::NORM_INF) < 1e-9
				&& cv::norm(stored.R, legacyCalib.R, cv::NORM_INF) < 1e-12 && cv::norm(stored.T, legacyCalib.T, cv::NORM_INF) < 1e-9;
			std::cout << "  calibration.yml in the workspace " << (same ? "matches" : "differs from") << " this run"
					  << (same ? "" : " (it may have been computed with other parameters or sets)") << std::endl;
		}
	}

	/* Reconstruction --------------------------------------------------------------------------- */
	double legacyReconstructTime = 0, newReconstructTime = 0;
	const smcp::CalibrationData& calib = legacyCalib;
	int reconstructed = 0;
	for (std::size_t i = 0; legacyOk && i < count && reconstructed < 2; i++)
	{
		if (!legacyPattern[i].data)
		{
			continue;
		}
		++reconstructed;
		const std::string tag = "set " + std::to_string(i) + " ";
		const cv::Mat colorImage = Legacy::GetImage(workspace.sets[i].images.at(0), false);
		Check(SameBits(colorImage, smcp::StructuredLight::GetColorImage(workspace.sets[i].images.at(0))), tag + "color image loader");

		smcp_legacy::Scan3d::Pointcloud legacyCloud;
		auto t = std::chrono::steady_clock::now();
		smcp_legacy::Scan3d::ReconstructModel(legacyCloud, calib, legacyPattern[i], legacyMinMax[i], colorImage, workspace.projectorSize,
			options.threshold, options.maxDist);
		smcp_legacy::Scan3d::ComputeNormals(legacyCloud);
		legacyReconstructTime += Seconds(t);

		smcp::Scan3d::Pointcloud cloud;
		t = std::chrono::steady_clock::now();
		smcp::Scan3d::ReconstructModel(cloud, calib, legacyPattern[i], legacyMinMax[i], colorImage, workspace.projectorSize,
			options.threshold, options.maxDist);
		smcp::Scan3d::ComputeNormals(cloud);
		newReconstructTime += Seconds(t);

		Check(SameBits(cloud.points, legacyCloud.points), tag + "reconstructed points");
		Check(SameBits(cloud.colors, legacyCloud.colors), tag + "reconstructed colors");
		Check(SameBits(cloud.normals, legacyCloud.normals), tag + "normals");
		Check(SameBits(smcp::Scan3d::MakeProjectorView(legacyPattern[i], legacyMinMax[i], colorImage, workspace.projectorSize, options.threshold),
				  smcp_legacy::Scan3d::MakeProjectorView(legacyPattern[i], legacyMinMax[i], colorImage, workspace.projectorSize, options.threshold)),
			tag + "projector view");
		int valid = 0;
		for (int r = 0; r < cloud.points.rows; r++)
		{
			for (int c = 0; c < cloud.points.cols; c++)
			{
				valid += !smcp::StructuredLight::Invalid(cloud.points.at<cv::Vec3f>(r, c));
			}
		}
		std::cout << "  " << tag << "reconstructed points: " << valid << std::endl;
	}

	std::cout.precision(3);
	std::cout << std::fixed << "Timing (s)            original      new\n"
			  << "  corners             " << legacyCornerTime << "\n"
			  << "  decode              " << legacyDecodeTime << "\n"
			  << "  homographies        " << legacyHomographyTime << "\n"
			  << "  solver              " << legacySolveTime << "\n"
			  << "  calibration total   " << legacyTotal << "        " << newTotal << "   (x" << legacyTotal / std::max(1e-9, newTotal) << ")\n"
			  << "  reconstruction      " << legacyReconstructTime << "        " << newReconstructTime << "   (x"
			  << legacyReconstructTime / std::max(1e-9, newReconstructTime) << ")" << std::endl;

	std::cout << (failures ? "RESULT: MISMATCH" : "RESULT: IDENTICAL") << std::endl;
	return failures ? 1 : 0;
}
