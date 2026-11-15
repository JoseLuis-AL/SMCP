#include "ui/pointcloud_editor/IcpDialog.h"

namespace smcp
{

	IcpDialog::IcpDialog(const Pointcloud::IcpParams& params, bool alignToMerged, QWidget* parent) : QDialog(parent)
	{
		setupUi(this);
		max_iterations_spin->setValue(params.maxIterations);
		max_distance_spin->setValue(params.maxCorrespondenceDistance);
		overlap_ratio_spin->setValue(params.overlapRatio);
		convergence_spin->setValue(params.convergenceThreshold);
		max_source_points_spin->setValue(static_cast<int>(params.maxSourcePoints));
		point_to_plane_check->setChecked(params.pointToPlane);
		align_centroids_check->setChecked(params.alignCentroids);
		align_to_merged_check->setChecked(alignToMerged);
	}

	Pointcloud::IcpParams IcpDialog::Params() const
	{
		Pointcloud::IcpParams params;
		params.maxIterations = max_iterations_spin->value();
		params.maxCorrespondenceDistance = max_distance_spin->value();
		params.overlapRatio = overlap_ratio_spin->value();
		params.convergenceThreshold = convergence_spin->value();
		params.maxSourcePoints = static_cast<std::size_t>(max_source_points_spin->value());
		params.pointToPlane = point_to_plane_check->isChecked();
		params.alignCentroids = align_centroids_check->isChecked();
		return params;
	}

	bool IcpDialog::AlignToMerged() const
	{
		return align_to_merged_check->isChecked();
	}

	void IcpDialog::on_done_button_clicked(bool)
	{
		accept();
	}

	void IcpDialog::on_cancel_button_clicked(bool)
	{
		reject();
	}

} // namespace smcp
