#include "core/Compensation.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace smcp::Pointcloud
{

namespace
{
	/// Largest difference (mm) accepted between this evaluator and the trainer's test vectors.
	constexpr double Test_Vector_Tolerance = 1e-6;

	/// Points per thread below which the correction runs on the calling thread only.
	constexpr std::size_t Points_Per_Thread = 20000;

	cv::Vec3d ReadVec3(const QJsonValue& value)
	{
		const QJsonArray array = value.toArray();
		return cv::Vec3d(array.at(0).toDouble(), array.at(1).toDouble(), array.at(2).toDouble());
	}

	/// Runs `work(first, last)` over [0, count) split across the hardware threads.
	template <typename Work>
	void ParallelFor(std::size_t count, Work work)
	{
		const std::size_t threads = std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1,
			std::max<std::size_t>(1, count / Points_Per_Thread));
		std::vector<std::thread> workers;
		for (std::size_t t = 1; t < threads; ++t)
		{
			workers.emplace_back(work, count * t / threads, count * (t + 1) / threads);
		}
		work(0, count / threads);
		for (auto& worker : workers)
		{
			worker.join();
		}
	}
} // namespace

cv::Vec3d CompensationModel::Correct(const cv::Vec3d& point, bool* outside) const
{
	if (outside)
	{
		*outside = false;
	}
	const double z = point[2];
	if (!(z > 0.0))
	{
		return point;
	}

	const double zn = (z - zCenter) / zScale;
	const double rho2 = (point[0] * point[0] + point[1] * point[1]) / (z * z);
	const double s = coefficients[0] + coefficients[1] * zn + coefficients[2] * zn * zn + coefficients[3] * rho2
		+ coefficients[4] * rho2 * rho2 + coefficients[5] * zn * rho2;
	cv::Vec3d corrected = point * (1.0 + s);
	if (layers.empty())
	{
		return corrected;
	}

	// Normalized input, clamped to the trained volume; `beyond` is how far outside it the point lies,
	// in the same normalized units (the volume spans 2 along each axis).
	double input[3];
	double beyond = 0.0;
	for (int k = 0; k < 3; ++k)
	{
		const double u = 2.0 * (point[k] - lower[k]) / (upper[k] - lower[k]) - 1.0;
		beyond = std::max(beyond, std::abs(u) - 1.0);
		input[k] = std::clamp(u, -1.0, 1.0);
	}
	const double weight = beyond <= 0.0 ? 1.0 : std::clamp(1.0 - beyond / (2.0 * extrapolationMargin), 0.0, 1.0);
	if (outside)
	{
		*outside = beyond > 0.0;
	}
	if (weight <= 0.0)
	{
		return corrected;
	}

	// Hidden layers use tanh; the last one is bounded with outputBound * tanh.
	std::vector<double> activation(input, input + 3);
	std::vector<double> next;
	for (std::size_t l = 0; l < layers.size(); ++l)
	{
		const Layer& layer = layers[l];
		next.assign(layer.outputs, 0.0);
		for (int o = 0; o < layer.outputs; ++o)
		{
			double sum = layer.bias[o];
			const double* row = layer.weight.data() + static_cast<std::size_t>(o) * layer.inputs;
			for (int i = 0; i < layer.inputs; ++i)
			{
				sum += row[i] * activation[i];
			}
			next[o] = std::tanh(sum);
		}
		activation.swap(next);
	}
	for (int k = 0; k < 3; ++k)
	{
		corrected[k] += weight * outputBound * activation[k];
	}
	return corrected;
}

bool LoadCompensationModel(const QString& path, CompensationModel& model, QString& error)
{
	model = CompensationModel();
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		error = QString("Cannot open %1.").arg(path);
		return false;
	}
	QJsonParseError parseError;
	const QJsonObject root = QJsonDocument::fromJson(file.readAll(), &parseError).object();
	if (root.isEmpty())
	{
		error = QString("Invalid JSON: %1.").arg(parseError.errorString());
		return false;
	}
	if (root.value("format").toString() != "smcp-compensation" || root.value("version").toInt() != 1)
	{
		error = "Not an SMCP compensation model (format smcp-compensation, version 1).";
		return false;
	}

	const QJsonObject physics = root.value("physics").toObject();
	const QJsonArray coefficients = physics.value("coefficients").toArray();
	if (coefficients.size() != 6)
	{
		error = "The model needs six physical coefficients.";
		return false;
	}
	for (int i = 0; i < 6; ++i)
	{
		model.coefficients[i] = coefficients.at(i).toDouble();
	}
	model.zCenter = physics.value("z_center").toDouble();
	model.zScale = physics.value("z_scale").toDouble(1.0);

	const QJsonObject network = root.value("network").toObject();
	model.lower = ReadVec3(network.value("lower"));
	model.upper = ReadVec3(network.value("upper"));
	model.outputBound = network.value("output_bound_mm").toDouble();
	model.extrapolationMargin = network.value("extrapolation_margin").toDouble(0.1);
	int inputs = 3;
	for (const QJsonValue& value : network.value("layers").toArray())
	{
		const QJsonObject layerObject = value.toObject();
		CompensationModel::Layer layer;
		const QJsonArray weight = layerObject.value("weight").toArray();
		const QJsonArray bias = layerObject.value("bias").toArray();
		layer.inputs = inputs;
		layer.outputs = weight.size();
		if (layer.outputs == 0 || bias.size() != layer.outputs)
		{
			error = "A network layer has inconsistent weights and biases.";
			return false;
		}
		for (const QJsonValue& row : weight)
		{
			const QJsonArray values = row.toArray();
			if (values.size() != inputs)
			{
				error = "A network layer does not match the size of the previous one.";
				return false;
			}
			for (const QJsonValue& v : values) layer.weight.push_back(v.toDouble());
		}
		for (const QJsonValue& v : bias) layer.bias.push_back(v.toDouble());
		inputs = layer.outputs;
		model.layers.push_back(std::move(layer));
	}
	if (!model.layers.empty() && model.layers.back().outputs != 3)
	{
		error = "The network must output three values.";
		return false;
	}
	for (int k = 0; k < 3; ++k)
	{
		if (!model.layers.empty() && !(model.upper[k] > model.lower[k]))
		{
			error = "The model has an empty trained volume.";
			return false;
		}
	}

	model.fixedScale = root.value("fixed_scale").toDouble(1.0);
	model.calibrationSha256 = root.value("calibration_sha256").toString().toStdString();
	const QJsonObject training = root.value("training").toObject();
	model.trainingScans = training.value("scans").toArray().size();
	const QJsonObject validation = training.value("cross_validation").toObject();
	if (!validation.isEmpty())
	{
		model.validationFixedScale = validation.value("fixed_scale").toObject().value("rms_um").toDouble(-1.0);
		model.validationPhysicsOnly = validation.value("physics_only").toObject().value("rms_um").toDouble(-1.0);
		model.validationPinn = validation.value("pinn").toObject().value("rms_um").toDouble(-1.0);
	}

	// The test vectors come from the trainer itself: they must be reproduced to load the model.
	const QJsonObject vectors = root.value("test_vectors").toObject();
	const QJsonArray input = vectors.value("input").toArray();
	const QJsonArray output = vectors.value("output").toArray();
	if (input.isEmpty() || input.size() != output.size())
	{
		error = "The model has no test vectors to verify it.";
		return false;
	}
	for (int i = 0; i < input.size(); ++i)
	{
		const double difference = cv::norm(model.Correct(ReadVec3(input.at(i))) - ReadVec3(output.at(i)));
		if (!(difference <= Test_Vector_Tolerance))
		{
			error = QString("The model does not reproduce its test vector %1 (difference %2 mm).").arg(i + 1).arg(difference);
			return false;
		}
	}
	return true;
}

std::string FileSha256(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		return std::string();
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!hash.addData(&file))
	{
		return std::string();
	}
	return hash.result().toHex().toStdString();
}

QString DescribeCompensationModel(const CompensationModel& model, const QString& calibrationPath)
{
	QStringList lines;
	lines << QString("Trained on %1 scan(s); fixed scale %2.").arg(model.trainingScans).arg(model.fixedScale, 0, 'f', 6);
	if (model.validationPinn >= 0.0)
	{
		lines << QString("Cross-validated RMS error on unseen scans: fixed scale %1 \u00b5m, physical block %2 \u00b5m, "
						 "PINN %3 \u00b5m.")
					 .arg(model.validationFixedScale, 0, 'f', 1).arg(model.validationPhysicsOnly, 0, 'f', 1)
					 .arg(model.validationPinn, 0, 'f', 1);
		if (model.validationPinn >= model.validationFixedScale)
		{
			lines << "Warning: in validation the PINN was not better than the fixed scale; train with more scans "
					 "that cover the volume.";
		}
	}
	else
	{
		lines << "The model was not cross-validated (fewer than two training scans).";
	}
	if (model.calibrationSha256.empty())
	{
		lines << "The model does not record its calibration.";
	}
	else if (calibrationPath.isEmpty() || FileSha256(calibrationPath).empty())
	{
		lines << "No current calibration file to compare with the model's.";
	}
	else
	{
		lines << (CompensationCalibrationDiffers(model, calibrationPath)
				? QString("Warning: the model was trained with a different calibration than %1.").arg(calibrationPath)
				: QString("The model was trained with the current calibration (%1).").arg(calibrationPath));
	}
	return lines.join("\n");
}

bool CompensationCalibrationDiffers(const CompensationModel& model, const QString& calibrationPath)
{
	if (model.calibrationSha256.empty() || calibrationPath.isEmpty())
	{
		return false;
	}
	const std::string current = FileSha256(calibrationPath);
	return !current.empty() && current != model.calibrationSha256;
}

ColorCloudPtr Compensate(const ColorCloud& cloud, const CompensationModel& model, CompensationStats* stats)
{
	auto out = std::make_shared<ColorCloud>(cloud);
	std::atomic<std::size_t> corrected{ 0 }, outside{ 0 };
	ParallelFor(out->size(), [&](std::size_t first, std::size_t last) {
		std::size_t localCorrected = 0, localOutside = 0;
		for (std::size_t i = first; i < last; ++i)
		{
			ColorPoint& p = (*out)[i];
			if (!IsFinite(p)) continue;
			bool beyond = false;
			const cv::Vec3d q = model.Correct(cv::Vec3d(p.x, p.y, p.z), &beyond);
			p.x = static_cast<float>(q[0]);
			p.y = static_cast<float>(q[1]);
			p.z = static_cast<float>(q[2]);
			++localCorrected;
			localOutside += beyond ? 1 : 0;
		}
		corrected += localCorrected;
		outside += localOutside;
	});
	if (stats)
	{
		stats->points = corrected;
		stats->outside = outside;
	}
	return out;
}

void Compensate(cv::Mat& points, const CompensationModel& model, CompensationStats* stats)
{
	CV_Assert(points.type() == CV_32FC3 && points.isContinuous());
	float* data = points.ptr<float>(0);
	std::atomic<std::size_t> corrected{ 0 }, outside{ 0 };
	ParallelFor(points.total(), [&](std::size_t first, std::size_t last) {
		std::size_t localCorrected = 0, localOutside = 0;
		for (std::size_t i = first; i < last; ++i)
		{
			float* p = data + 3 * i;
			if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
			bool beyond = false;
			const cv::Vec3d q = model.Correct(cv::Vec3d(p[0], p[1], p[2]), &beyond);
			p[0] = static_cast<float>(q[0]);
			p[1] = static_cast<float>(q[1]);
			p[2] = static_cast<float>(q[2]);
			++localCorrected;
			localOutside += beyond ? 1 : 0;
		}
		corrected += localCorrected;
		outside += localOutside;
	});
	if (stats)
	{
		stats->points = corrected;
		stats->outside = outside;
	}
}

} // namespace smcp::Pointcloud
