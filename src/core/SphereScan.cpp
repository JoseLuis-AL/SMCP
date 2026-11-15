#include "core/SphereScan.h"

#include <QStringList>

namespace smcp::Pointcloud
{
	namespace
	{
		/// The sphere carrying `label` in a labeled scan, or the `label`-th one of an unlabeled scan.
		const MeasuredSphere* SphereAt(const SphereScanResult& scan, int label)
		{
			if (scan.labeled)
			{
				for (const auto& sphere : scan.spheres)
					if (sphere.label == label)
						return &sphere;
				return nullptr;
			}
			return label < static_cast<int>(scan.spheres.size()) ? &scan.spheres[label] : nullptr;
		}
	} // namespace

	std::vector<ScanMeasurand> ScanMeasurands(const SphereScanResult& scan, const ArtifactReference& reference)
	{
		const bool identified = reference.Count() > 0 && scan.labeled;
		QStringList names;
		if (identified)
			for (const auto& name : reference.names)
				names << QString::fromStdString(name);
		else
			for (std::size_t i = 0; i < scan.spheres.size(); ++i)
				names << QString("S%1").arg(i + 1);

		std::vector<ScanMeasurand> rows;
		for (int i = 0; i < names.size(); ++i)
		{
			const auto* sphere = SphereAt(scan, i);
			ScanMeasurand row;
			row.target = names[i];
			row.diameter = true;
			if (sphere)
			{
				row.measured = sphere->Diameter();
				row.formRms = sphere->formRms;
			}
			row.reference = identified ? reference.diameters[i] : 0.0;
			rows.push_back(row);
		}
		for (int i = 0; i < names.size(); ++i)
			for (int j = i + 1; j < names.size(); ++j)
			{
				const auto* a = SphereAt(scan, i);
				const auto* b = SphereAt(scan, j);
				ScanMeasurand row;
				row.target = names[i] + " / " + names[j];
				if (a && b)
					row.measured = cv::norm(a->center - b->center);
				row.reference = identified ? reference.distances[i][j] : 0.0;
				rows.push_back(row);
			}
		return rows;
	}
} // namespace smcp::Pointcloud
