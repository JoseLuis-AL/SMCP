#include "core/Calibration.h"

#include <atomic>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include <opencv2/calib3d/calib3d.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "core/Parallel.h"

namespace smcp::Calibration
{

namespace
{
	/// Image of each set that shows the chessboard fully lit (the second capture).
	constexpr std::size_t Chessboard_Image_Index = 1;

	/// Detection is done at about this width; corners are then refined at full resolution.
	constexpr double Detection_Width = 1024.0;

	std::string SizeText(cv::Size size)
	{
		return std::to_string(size.width) + "x" + std::to_string(size.height);
	}

	/// Projector position of one camera corner, from a homography fitted to its neighborhood.
	enum class CornerStatus { Ok, NearBorder, NoHomography };

	CornerStatus ProjectCorner(const cv::Mat& patternImage, const cv::Mat& minMaxImage, const cv::Point2f& p,
		unsigned WINDOW_SIZE, unsigned threshold, cv::Point2f& q)
	{
		if (!(p.x > WINDOW_SIZE && p.y > WINDOW_SIZE && p.x + WINDOW_SIZE < patternImage.cols && p.y + WINDOW_SIZE < patternImage.rows))
		{
			return CornerStatus::NearBorder;
		}

		//collect the valid correspondences around p
		std::vector<cv::Point2f> imgPoints, projPoints;
		imgPoints.reserve(static_cast<std::size_t>(4) * WINDOW_SIZE * WINDOW_SIZE);
		projPoints.reserve(static_cast<std::size_t>(4) * WINDOW_SIZE * WINDOW_SIZE);
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
				{   //apply threshold and skip
					continue;
				}

				imgPoints.push_back(cv::Point2f(w, h));
				projPoints.push_back(cv::Point2f(pattern));
			}
		}
		if (imgPoints.size() < 4)
		{
			return CornerStatus::NoHomography;
		}

		const cv::Mat H = cv::findHomography(imgPoints, projPoints, CV_RANSAC);
		if (H.rows != 3 || H.cols != 3)
		{
			return CornerStatus::NoHomography;
		}
		const cv::Point3d Q = cv::Point3d(cv::Mat(H * cv::Mat(cv::Point3d(p.x, p.y, 1.0))));
		q = cv::Point2f(Q.x / Q.z, Q.y / Q.z);
		return CornerStatus::Ok;
	}

	FILE* OpenText(const std::filesystem::path& path)
	{
#ifdef _WIN32
		return _wfopen(path.c_str(), L"w");
#else
		return fopen(path.c_str(), "w");
#endif
	}
} // namespace

std::vector<cv::Point3f> ChessboardWorldCoords(cv::Size cornerCount, cv::Size2f squareSize)
{
	std::vector<cv::Point3f> worldCorners;
	worldCorners.reserve(static_cast<std::size_t>(std::max(0, cornerCount.width)) * std::max(0, cornerCount.height));
	for (int h = 0; h < cornerCount.height; h++)
	{
		for (int w = 0; w < cornerCount.width; w++)
		{
			worldCorners.push_back(cv::Point3f(squareSize.width * w, squareSize.height * h, 0.f));
		}
	}
	return worldCorners;
}

int DetectionScale(cv::Size imageSize)
{
	return imageSize.width > Detection_Width ? cvRound(imageSize.width / Detection_Width) : 1;
}

bool DetectChessboard(const cv::Mat& grayImage, cv::Size cornerCount, int imageScale, std::vector<cv::Point2f>& corners)
{
	corners.clear();
	if (grayImage.rows < 1 || grayImage.type() != CV_8UC1 || cornerCount.width < 3 || cornerCount.height < 3 || imageScale < 1)
	{
		return false;
	}

	try
	{
		cv::Mat smallImg = grayImage;
		if (imageScale > 1)
		{
			cv::resize(grayImage, smallImg, cv::Size(grayImage.cols / imageScale, grayImage.rows / imageScale));
		}

		if (!cv::findChessboardCorners(smallImg, cornerCount, corners, cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE))
		{   //OpenCV leaves the partial detection in the vector
			corners.clear();
			return false;
		}

		for (cv::Point2f& corner : corners)
		{
			corner = imageScale * corner;
		}
		cv::cornerSubPix(grayImage, corners, cv::Size(11, 11), cv::Size(-1, -1),
			cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER, 30, 0.1));
		return true;
	}
	catch (const cv::Exception&)
	{
		corners.clear();
		return false;
	}
}

bool ExtractCorners(const std::vector<CaptureSet>& sets, const ChessboardParams& params,
	std::vector<SetCorners>& corners, cv::Size& imageSize, ProgressSink& progress)
{
	const int count = static_cast<int>(sets.size());
	corners.assign(sets.size(), SetCorners());
	imageSize = cv::Size(0, 0);

	progress.Step(0, count);
	progress.Status("Extracting corners...");

	//load the chessboard image of every selected set
	std::vector<cv::Mat> grayImages(sets.size());
	Parallel::ParallelFor(count, 1, [&](long long first, long long last) {
		for (long long i = first; i < last; i++)
		{
			const CaptureSet& set = sets[i];
			if (set.selected && set.images.size() > Chessboard_Image_Index && !progress.Canceled())
			{
				grayImages[i] = StructuredLight::GetGrayImage(set.images[Chessboard_Image_Index]);
			}
		}
	});

	//every image must have the size of the first one
	for (int i = 0; i < count; i++)
	{
		if (grayImages[i].rows < 1)
		{
			continue;
		}
		if (imageSize.width == 0)
		{
			imageSize = grayImages[i].size();
		}
		else if (imageSize != grayImages[i].size())
		{
			progress.Message("ERROR: image of different size: set " + sets[i].name);
			return false;
		}
	}
	const int imageScale = DetectionScale(imageSize);

	//detect: sets are independent, each thread takes a contiguous block of them
	std::vector<char> found(sets.size(), 0);
	std::atomic<int> done{ 0 };
	Parallel::ParallelFor(count, 1, [&](long long first, long long last) {
		for (long long i = first; i < last; i++)
		{
			if (grayImages[i].rows > 0 && !progress.Canceled())
			{
				found[i] = DetectChessboard(grayImages[i], params.cornerCount, imageScale, corners[i].camera) ? 1 : 0;
				if (found[i])
				{
					corners[i].world = ChessboardWorldCoords(params.cornerCount, params.squareSize);
				}
			}
			progress.Step(++done, count);
		}
	});

	if (progress.Canceled())
	{
		corners.assign(sets.size(), SetCorners());
		progress.Status("Extract corners canceled");
		progress.Message("Extract corners canceled");
		return false;
	}

	//report in set order
	for (int i = 0; i < count; i++)
	{
		const std::string& name = sets[i].name;
		if (!sets[i].selected)
		{
			progress.Message(" * " + name + ": skip (not selected)");
		}
		else if (grayImages[i].rows < 1)
		{
			progress.Message(" * " + name + ": skip (failed to load image)");
		}
		else if (found[i])
		{
			progress.Message(" * " + name + ": found " + std::to_string(corners[i].camera.size()) + " corners");
		}
		else
		{
			progress.Message(" * " + name + ": chessboard not found!");
		}
	}

	progress.Status("Extract corners finished");
	progress.Step(count, count);
	return true;
}

bool ProjectorCorners(const StructuredLight::DecodedSet& decoded, const std::vector<cv::Point2f>& cameraCorners,
	unsigned homographyWindow, unsigned shadowThreshold, std::vector<cv::Point2f>& projectorCorners, std::string& error)
{
	projectorCorners.clear();
	error.clear();
	if (cameraCorners.empty())
	{
		return true;
	}
	if (!decoded.pattern.data || decoded.pattern.type() != CV_32FC2 || !decoded.minMax.data ||
		decoded.minMax.type() != CV_8UC2 || decoded.minMax.size() != decoded.pattern.size())
	{
		error = "ERROR: the set is not decoded";
		return false;
	}

	const unsigned WINDOW_SIZE = homographyWindow / 2;
	const long long count = static_cast<long long>(cameraCorners.size());
	std::vector<cv::Point2f> result(cameraCorners.size());
	std::vector<CornerStatus> status(cameraCorners.size(), CornerStatus::Ok);

	//each corner fits its own homography: independent work, same numbers in any order
	Parallel::ParallelFor(count, 1, [&](long long first, long long last) {
		for (long long i = first; i < last; i++)
		{
			try
			{
				status[i] = ProjectCorner(decoded.pattern, decoded.minMax, cameraCorners[i], WINDOW_SIZE, shadowThreshold, result[i]);
			}
			catch (const cv::Exception&)
			{
				status[i] = CornerStatus::NoHomography;
			}
		}
	});

	for (long long i = 0; i < count; i++)
	{
		if (status[i] == CornerStatus::NearBorder)
		{
			error = "ERROR: corner " + std::to_string(i) + " is closer to the image border than the homography window ("
				+ std::to_string(homographyWindow) + " px). Reduce the window or recapture the set.";
			return false;
		}
		if (status[i] == CornerStatus::NoHomography)
		{
			error = "ERROR: no homography for corner " + std::to_string(i)
				+ " (too few decoded pixels around it). Check the decode parameters or deselect the set.";
			return false;
		}
	}

	projectorCorners = std::move(result);
	return true;
}

bool SubpixelProjectorCorners(const std::vector<std::string>& images, const StructuredLight::DecodedSet& decoded,
	const std::vector<cv::Point2f>& cameraCorners, cv::Size projectorSize, unsigned homographyWindow,
	unsigned shadowThreshold, std::vector<cv::Point2f>& projectorCorners, std::string& error, ProgressSink* progress)
{
	projectorCorners.clear();
	if (cameraCorners.empty())
	{
		error.clear();
		return true;
	}
	std::vector<cv::Mat> gray;
	if (!StructuredLight::LoadGraySet(images, gray, error, progress))
	{
		return false;
	}
	StructuredLight::DecodedSet refined;
	refined.pattern = StructuredLight::RefinePattern(gray, decoded.pattern, decoded.minMax, projectorSize,
		static_cast<int>(shadowThreshold), progress);
	refined.minMax = decoded.minMax;
	if (!refined.pattern.data)
	{
		error = "ERROR: the images of the set do not match its decoded pattern";
		return false;
	}
	return ProjectorCorners(refined, cameraCorners, homographyWindow, shadowThreshold, projectorCorners, error);
}

bool Solve(const std::vector<std::vector<cv::Point3f>>& world, const std::vector<std::vector<cv::Point2f>>& camera,
	const std::vector<std::vector<cv::Point2f>>& projector, cv::Size imageSize, cv::Size projectorSize,
	CalibrationData& calibration, std::string& error, ProgressSink& progress, const SolveOptions& options)
{
	calibration.Clear();
	error.clear();

	if (world.size() < 3 || camera.size() != world.size() || projector.size() != world.size())
	{
		error = "ERROR: use at least 3 sets";
		return false;
	}

	const int calFlags = cv::CALIB_FIX_K3;
	// Joint refinement: the stereo step starts from the two separate calibrations and optimizes
	// every parameter over all the views at once, instead of keeping the intrinsics fixed.
	const int stereoFlags = (options.jointRefinement ? cv::CALIB_USE_INTRINSIC_GUESS : cv::CALIB_FIX_INTRINSIC) + calFlags;

	try
	{
		//calibrate the camera
		progress.Message(" * Calibrate camera [" + SizeText(imageSize) + "]");
		std::vector<cv::Mat> camRvecs, camTvecs;
		calibration.camError = cv::calibrateCamera(world, camera, imageSize, calibration.camK, calibration.camKc, camRvecs, camTvecs, calFlags,
			cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 50, DBL_EPSILON));

		//calibrate the projector
		progress.Message(" * Calibrate projector [" + SizeText(projectorSize) + "]");
		std::vector<cv::Mat> projRvecs, projTvecs;
		calibration.projError = cv::calibrateCamera(world, projector, projectorSize, calibration.projK, calibration.projKc, projRvecs, projTvecs, calFlags,
			cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 50, DBL_EPSILON));

		//stereo calibration
		progress.Message(" * Calibrate stereo");
		cv::Mat E, F;
		calibration.stereoError = cv::stereoCalibrate(world, camera, projector, calibration.camK, calibration.camKc, calibration.projK, calibration.projKc,
			imageSize /*ignored*/, calibration.R, calibration.T, E, F,
			cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 150, DBL_EPSILON),
			stereoFlags);
	}
	catch (const cv::Exception& e)
	{
		calibration.Clear();
		error = std::string("ERROR: the calibration could not be solved: ") + e.what();
		return false;
	}
	return true;
}

Result Run(const std::vector<CaptureSet>& sets, const Params& params,
	const std::vector<StructuredLight::DecodedSet>& cached, ProgressSink& progress)
{
	Result result;
	const int count = static_cast<int>(sets.size());
	result.projectorCorners.resize(sets.size());
	result.decoded.resize(sets.size());

	const auto canceled = [&]() {
		if (!progress.Canceled())
		{
			return false;
		}
		result.canceled = true;
		progress.Status("Calibration canceled");
		progress.Message("Calibration canceled");
		return true;
	};

	//detect corners
	progress.Message("Extracting corners:");
	cv::Size cornerImageSize;
	if (!ExtractCorners(sets, params.chessboard, result.corners, cornerImageSize, progress))
	{
		result.canceled = progress.Canceled();
		return result;
	}
	result.cornersExtracted = true;
	progress.Message("");

	//collect projector correspondences
	progress.Step(0, count);
	progress.Status("Decoding and computing homographies...");

	for (int i = 0; i < count; i++)
	{
		const std::string& name = sets[i].name;
		if (!sets[i].selected)
		{
			progress.Message(" * " + name + ": skip (not selected)");
			progress.Step(i + 1, count);
			continue;
		}
		if (canceled())
		{
			return result;
		}

		progress.Status("Decoding... " + name);
		if (static_cast<std::size_t>(i) < cached.size() && cached[i].pattern.data && cached[i].minMax.data)
		{
			result.decoded[i] = cached[i];
			progress.Message(" * " + name + ": decoded data reused");
		}
		else
		{
			std::string error;
			if (!StructuredLight::DecodeGraySet(sets[i].images, params.projectorSize, params.decode, result.decoded[i], error, &progress))
			{
				if (!canceled())
				{
					progress.Message(error);
					progress.Message("ERROR: decode of set " + name + " failed");
					progress.Status("ERROR: decode of set " + name + " failed");
				}
				return result;
			}
		}

		const cv::Size patternSize = result.decoded[i].pattern.size();
		if (result.imageSize.width == 0)
		{
			result.imageSize = patternSize;
		}
		else if (result.imageSize != patternSize)
		{
			progress.Message("ERROR: pattern image of different size: set " + name);
			progress.Status("ERROR: pattern image of different size: set " + name);
			return result;
		}

		progress.Status("Computing homographies... " + name);
		std::string error;
		const bool mapped = params.subpixelCorners
			? SubpixelProjectorCorners(sets[i].images, result.decoded[i], result.corners[i].camera, params.projectorSize,
				params.homographyWindow, params.shadowThreshold, result.projectorCorners[i], error, &progress)
			: ProjectorCorners(result.decoded[i], result.corners[i].camera, params.homographyWindow, params.shadowThreshold,
				result.projectorCorners[i], error);
		if (!mapped)
		{
			progress.Message(" * " + name + ": " + error);
			progress.Status("ERROR: set " + name + " cannot be used (see the log)");
			return result;
		}
		if (canceled())
		{
			return result;
		}

		progress.Message(" * " + name + ": finished");
		progress.Step(i + 1, count);
	}
	progress.Message("");

	//sets with corners in the three spaces
	std::vector<std::vector<cv::Point3f>> worldActive;
	std::vector<std::vector<cv::Point2f>> cameraActive, projectorActive;
	for (int i = 0; i < count; i++)
	{
		if (result.corners[i].world.size() && result.corners[i].camera.size() && result.projectorCorners[i].size())
		{
			worldActive.push_back(result.corners[i].world);
			cameraActive.push_back(result.corners[i].camera);
			projectorActive.push_back(result.projectorCorners[i]);
		}
	}

	if (worldActive.size() < 3)
	{
		progress.Status("ERROR: use at least 3 sets");
		progress.Message("ERROR: use at least 3 sets");
		return result;
	}

	std::string error;
	if (!Solve(worldActive, cameraActive, projectorActive, result.imageSize, params.projectorSize, result.calibration, error, progress, params.solve))
	{
		progress.Status("ERROR: calibration failed");
		progress.Message(error);
		return result;
	}

	std::stringstream stream;
	result.calibration.Display(stream);
	progress.Message("\n **** Calibration results ****\n");
	progress.Message(stream.str());

	result.success = true;
	return result;
}

bool SaveCorners(const std::string& directory, const Result& result, std::string& error)
{
	const std::filesystem::path root = std::filesystem::u8path(directory);
	for (std::size_t i = 0; i < result.corners.size(); i++)
	{
		const std::vector<cv::Point3f>& worldCorners = result.corners[i].world;
		const std::vector<cv::Point2f>& camCorners = result.corners[i].camera;
		const std::vector<cv::Point2f>& projCorners = result.projectorCorners[i];

		char suffix[32];
		std::snprintf(suffix, sizeof(suffix), "_%02u.txt", static_cast<unsigned>(i));
		FILE* files[3] = { nullptr, nullptr, nullptr };
		const char* prefixes[3] = { "world", "cam", "proj" };
		bool opened = true;
		for (int f = 0; f < 3; f++)
		{
			files[f] = OpenText(root / (std::string(prefixes[f]) + suffix));
			opened = opened && files[f];
		}
		if (opened)
		{
			//a set is written only when it took part: the three lists then have one entry per corner
			const std::size_t lines = (camCorners.size() >= worldCorners.size() && projCorners.size() >= worldCorners.size())
				? worldCorners.size() : 0;
			for (std::size_t j = 0; j < lines; j++)
			{
				fprintf(files[0], "%lf %lf %lf\n", worldCorners[j].x, worldCorners[j].y, worldCorners[j].z);
				fprintf(files[1], "%lf %lf\n", camCorners[j].x, camCorners[j].y);
				fprintf(files[2], "%lf %lf\n", projCorners[j].x, projCorners[j].y);
			}
		}
		for (FILE* file : files)
		{
			if (file)
			{
				fclose(file);
			}
		}
		if (!opened)
		{
			error = std::string("ERROR: could not write the corner files of set ") + std::to_string(i);
			return false;
		}
	}
	return true;
}

} // namespace smcp::Calibration
