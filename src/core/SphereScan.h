#pragma once

// The spheres measured in one scan and the rows they give in the results table.

#include <optional>
#include <vector>

#include <QString>

#include "core/SphereMeasurement.h"

namespace smcp
{
	/// Spheres measured in one scan.
	struct SphereScanResult
	{
		QString name;									 ///< Scan or layer name.
		QString source;									 ///< File path, or empty for a layer.
		std::vector<Pointcloud::MeasuredSphere> spheres; ///< Sorted by label, or by X when unlabeled.
		bool labeled = false;
		QString details; ///< Layer history (filters, transforms), when measured from a layer.
	};
}

namespace smcp::Pointcloud
{
	/// One row of the results table: a sphere diameter or a distance between two sphere centers.
	struct ScanMeasurand
	{
		QString target; ///< Sphere name, or "A / B" for a distance.
		bool diameter = false;
		std::optional<double> measured; ///< Empty when a sphere was not detected.
		double reference = 0.0;			///< 0 without a reference value.
		std::optional<double> formRms;	///< Diameters only.
	};

	/// Rows of one scan in table order (diameters, then distances). Identified scans use the reference names
	/// and values; other scans are named S1..Sn in detection order and have no reference.
	std::vector<ScanMeasurand> ScanMeasurands(const SphereScanResult& scan, const ArtifactReference& reference);
} // namespace smcp::Pointcloud
