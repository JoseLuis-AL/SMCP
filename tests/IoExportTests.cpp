// The XYZ writers format text in memory for speed. These checks require their output to be byte
// for byte what the original stream-based writers produced (reimplemented here as the reference).

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>

#include <QFile>
#include <QLocale>
#include <QTemporaryDir>
#include <QTextStream>

#include "core/PointcloudOps.h"
#include "core/Scan3d.h"
#include "export/IOExport.h"

namespace
{
	bool Check(bool condition, const char* message)
	{
		if (!condition)
			std::cerr << "FAIL: " << message << '\n';
		return condition;
	}

	QByteArray ReadAll(const QString& path)
	{
		QFile file(path);
		return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unreadable>");
	}

	/// Original editor writer: QTextStream, fixed notation, six decimals.
	bool ReferenceWriteColorCloud(const QString& path, const smcp::Pointcloud::ColorCloud& cloud)
	{
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly | QIODevice::Text))
			return false;
		QTextStream out(&file);
		out.setLocale(QLocale::c());
		out.setRealNumberNotation(QTextStream::FixedNotation);
		out.setRealNumberPrecision(6);
		for (const auto& pt : cloud)
		{
			out << pt.x << ' ' << pt.y << ' ' << pt.z << ' ' << int(pt.r) << ' ' << int(pt.g) << ' ' << int(pt.b) << '\n';
		}
		out.flush();
		return out.status() == QTextStream::Ok;
	}

	/// Original reconstruction writer: std::ofstream with eight significant digits.
	void ReferenceWriteScan(const QString& path, const smcp::Scan3d::Pointcloud& pointcloud, bool colors)
	{
		std::ofstream out(path.toStdString(), std::ios::out | std::ios::trunc);
		out << std::setprecision(8);
		const auto* points = pointcloud.points.ptr<cv::Vec3f>(0);
		const auto* colorData = colors ? pointcloud.colors.ptr<cv::Vec3b>(0) : nullptr;
		for (int i = 0; i < static_cast<int>(pointcloud.points.total()); ++i)
		{
			const cv::Vec3f& p = points[i];
			if (std::isnan(p[0]) || std::isnan(p[1]) || std::isnan(p[2]))
				continue;
			out << p[0] << ' ' << p[1] << ' ' << p[2];
			if (colorData)
			{
				const cv::Vec3b& c = colorData[i];
				out << ' ' << static_cast<int>(c[2]) << ' ' << static_cast<int>(c[1]) << ' ' << static_cast<int>(c[0]);
			}
			out << '\n';
		}
	}

	/// Values across magnitudes plus the awkward ones: zeros, rounding ties, tiny, huge, non-finite.
	std::vector<float> TestValues()
	{
		std::vector<float> values = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 0.0000005f, -0.0000005f, 0.0000004f, -0.0000004f, 1e-9f, -1e-9f,
			1e-30f, 123456.789f, -123456.789f, 1e9f, -1e9f, 3.4e38f, -3.4e38f, 0.1f, 0.2f, 0.3f, 2.5f, 1.0000005f, 999999.9375f,
			16777216.0f, 1e-5f, 1.5e-5f, 9.9999995e-5f, 12345678.0f, 0.000123456789f, std::numeric_limits<float>::min(),
			std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN() };
		std::mt19937 rng(7);
		std::uniform_real_distribution<float> small(-1.0f, 1.0f), medium(-500.0f, 500.0f), exponent(-12.0f, 12.0f);
		for (int i = 0; i < 20000; ++i)
		{
			values.push_back(small(rng));
			values.push_back(medium(rng));
			values.push_back(medium(rng) * std::pow(10.0f, exponent(rng)));
		}
		return values;
	}
} // namespace

bool IoExportChecks()
{
	bool ok = true;
	QTemporaryDir directory;
	ok &= Check(directory.isValid(), "temporary directory");
	const std::vector<float> values = TestValues();

	// Editor clouds (more than one write block).
	smcp::Pointcloud::ColorCloud cloud;
	for (std::size_t i = 0; i + 2 < values.size(); ++i)
	{
		smcp::Pointcloud::ColorPoint point;
		point.x = values[i];
		point.y = values[i + 1];
		point.z = values[i + 2];
		point.r = static_cast<std::uint8_t>(i);
		point.g = static_cast<std::uint8_t>(i * 7);
		point.b = static_cast<std::uint8_t>(255 - i % 256);
		cloud.push_back(point);
	}
	const QString referenceCloud = directory.filePath("reference_cloud.xyz");
	const QString writtenCloud = directory.filePath("written_cloud.xyz");
	ok &= Check(ReferenceWriteColorCloud(referenceCloud, cloud), "reference editor writer");
	ok &= Check(smcp::IOExport::WriteXyz(writtenCloud.toStdString(), cloud), "editor XYZ writer");
	ok &= Check(ReadAll(writtenCloud) == ReadAll(referenceCloud), "editor XYZ output is byte-identical to the original writer");
	{	// On a mismatch, show where the texts diverge.
		const QList<QByteArray> expected = ReadAll(referenceCloud).split('\n'), actual = ReadAll(writtenCloud).split('\n');
		int shown = 0;
		for (int i = 0; i < expected.size() && i < actual.size() && shown < 8; ++i)
		{
			if (expected[i] != actual[i])
			{
				std::cerr << "  line " << i << " (" << cloud[i].x << ' ' << cloud[i].y << ' ' << cloud[i].z << ")\n    expected: "
						  << expected[i].trimmed().constData() << "\n    written:  " << actual[i].trimmed().constData() << '\n';
				++shown;
			}
		}
	}
	ok &= Check(ReadAll(writtenCloud).size() > (2 << 20), "editor XYZ test spans several write blocks");
	ok &= Check(!smcp::IOExport::WriteXyz(writtenCloud.toStdString(), cloud), "editor XYZ writer never overwrites");

	smcp::Pointcloud::ColorCloud reread;
	ok &= Check(smcp::IOExport::ReadXyz(writtenCloud.toStdString(), reread) && !reread.empty(), "exported cloud can be read back");

	// Reconstructed clouds, with and without colors.
	smcp::Scan3d::Pointcloud scan;
	const int rows = 300, cols = static_cast<int>(values.size() / 3 / 300);
	scan.InitPoints(rows, cols);
	scan.InitColor(rows, cols);
	for (int i = 0; i < rows * cols; ++i)
	{
		if (i % 11 == 0)
			continue;  // left as NaN: not exported
		scan.points.ptr<cv::Vec3f>(0)[i] = cv::Vec3f(values[3 * i], values[3 * i + 1], values[3 * i + 2]);
		scan.colors.ptr<cv::Vec3b>(0)[i] = cv::Vec3b(static_cast<uchar>(i), static_cast<uchar>(i * 3), static_cast<uchar>(i * 5));
	}
	for (const bool colors : { false, true })
	{
		const QString reference = directory.filePath(colors ? "reference_rgb.xyz" : "reference.xyz");
		const QString written = directory.filePath(colors ? "written_rgb.xyz" : "written.xyz");
		ReferenceWriteScan(reference, scan, colors);
		ok &= Check(smcp::IOExport::WriteXyz(written.toStdString(), scan,
						colors ? smcp::IOExport::XyzFormat::XyzRgb : smcp::IOExport::XyzFormat::Xyz),
			"reconstruction XYZ writer");
		ok &= Check(ReadAll(written) == ReadAll(reference), "reconstruction XYZ output is byte-identical to the original writer");
	}

	std::cout << (ok ? "IoExport checks passed\n" : "IoExport checks FAILED\n");
	return ok;
}
