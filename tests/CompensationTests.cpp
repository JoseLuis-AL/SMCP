#include <cmath>
#include <iostream>
#include <limits>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "core/Compensation.h"

namespace
{
	bool Check(bool condition, const char* message)
	{
		if (!condition) std::cerr << "FAIL: " << message << '\n';
		return condition;
	}

	/// The physical block alone, as documented in the model.
	cv::Vec3d Physics(const smcp::Pointcloud::CompensationModel& m, const cv::Vec3d& p)
	{
		const double zn = (p[2] - m.zCenter) / m.zScale;
		const double rho2 = (p[0] * p[0] + p[1] * p[1]) / (p[2] * p[2]);
		const auto& a = m.coefficients;
		return p * (1.0 + a[0] + a[1] * zn + a[2] * zn * zn + a[3] * rho2 + a[4] * rho2 * rho2 + a[5] * zn * rho2);
	}
}

int main()
{
	bool success = true;
	const QString fixture = QStringLiteral(SMCP_SOURCE_DIR "/tests/fixtures/compensation_model.json");

	// The fixture was written by the pinn-smcp trainer; loading verifies its PyTorch test vectors.
	smcp::Pointcloud::CompensationModel model;
	QString error;
	success &= Check(smcp::Pointcloud::LoadCompensationModel(fixture, model, error), "the trainer's model loads");
	if (!error.isEmpty()) std::cerr << error.toStdString() << '\n';
	std::cout << "model: " << model.layers.size() << " layers, a0 " << model.coefficients[0] << ", fixed scale "
			  << model.fixedScale << ", trained on " << model.trainingScans << " scans\n";

	// Outside the trained volume the network fades out: far away only the physical block remains, and
	// within the margin the correction lies between the two.
	const cv::Vec3d center = 0.5 * (model.lower + model.upper);
	const cv::Vec3d size = model.upper - model.lower;
	const cv::Vec3d far = center + cv::Vec3d(0.0, 0.0, size[2] * 0.5 + size[2] * 0.5);  // half a volume beyond
	bool outside = false;
	const cv::Vec3d farCorrected = model.Correct(far, &outside);
	success &= Check(outside, "a point beyond the volume is reported as outside");
	success &= Check(cv::norm(farCorrected - Physics(model, far)) < 1e-9, "far outside, only the physical block applies");
	const cv::Vec3d inside = center;
	success &= Check(!(model.Correct(inside, &outside), outside), "the center is inside");

	// Points behind the camera are left alone; NaN points are skipped in a reconstruction matrix.
	success &= Check(model.Correct(cv::Vec3d(1, 2, -5)) == cv::Vec3d(1, 2, -5), "z <= 0 is unchanged");
	cv::Mat points(1, 2, CV_32FC3);
	points.at<cv::Vec3f>(0, 0) = cv::Vec3f(static_cast<float>(center[0]), static_cast<float>(center[1]), static_cast<float>(center[2]));
	points.at<cv::Vec3f>(0, 1) = cv::Vec3f(std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f);
	smcp::Pointcloud::CompensationStats stats;
	smcp::Pointcloud::Compensate(points, model, &stats);
	const cv::Vec3d expected = model.Correct(center);
	success &= Check(stats.points == 1 && stats.outside == 0, "one finite point corrected");
	success &= Check(cv::norm(cv::Vec3d(points.at<cv::Vec3f>(0, 0)) - expected) < 1e-3, "matrix points are corrected");
	success &= Check(std::isnan(points.at<cv::Vec3f>(0, 1)[0]), "NaN points stay NaN");

	smcp::Pointcloud::ColorCloud cloud(1);
	cloud[0].x = static_cast<float>(center[0]);
	cloud[0].y = static_cast<float>(center[1]);
	cloud[0].z = static_cast<float>(center[2]);
	cloud[0].r = 12;
	const auto compensated = smcp::Pointcloud::Compensate(cloud, model, &stats);
	success &= Check(stats.points == 1 && (*compensated)[0].r == 12, "colored clouds keep their colors");

	// A model whose weights were altered no longer reproduces its test vectors and is rejected.
	{
		QFile file(fixture);
		file.open(QIODevice::ReadOnly);
		QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
		QJsonObject network = root.value("network").toObject();
		QJsonArray layers = network.value("layers").toArray();
		QJsonObject first = layers.at(0).toObject();
		QJsonArray bias = first.value("bias").toArray();
		bias.replace(0, bias.at(0).toDouble() + 0.5);
		first.insert("bias", bias);
		layers.replace(0, first);
		network.insert("layers", layers);
		root.insert("network", network);
		QTemporaryDir directory;
		const QString tampered = directory.filePath("tampered.json");
		QFile out(tampered);
		out.open(QIODevice::WriteOnly);
		out.write(QJsonDocument(root).toJson());
		out.close();
		smcp::Pointcloud::CompensationModel rejected;
		QString reason;
		success &= Check(!smcp::Pointcloud::LoadCompensationModel(tampered, rejected, reason), "a tampered model is rejected");
		std::cout << "tampered model: " << reason.toStdString() << '\n';
	}

	std::cout << (success ? "All compensation tests passed.\n" : "Compensation tests failed.\n");
	return success ? 0 : 1;
}
