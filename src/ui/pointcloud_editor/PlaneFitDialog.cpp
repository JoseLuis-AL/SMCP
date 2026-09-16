#include "ui/pointcloud_editor/PlaneFitDialog.h"

namespace smcp
{

PlaneFitDialog::PlaneFitDialog(const Pointcloud::PlaneFitParams& params, QWidget* parent)
	: QDialog(parent)
{
	setupUi(this);
	max_planes_spin->setValue(params.maxPlanes);
	max_iterations_spin->setValue(params.maxIterations);
	distance_threshold_spin->setValue(params.distanceThreshold);
	min_inliers_spin->setValue(static_cast<int>(params.minInliers));
	optimize_coefficients_check->setChecked(params.optimizeCoefficients);
}

Pointcloud::PlaneFitParams PlaneFitDialog::Params() const
{
	Pointcloud::PlaneFitParams params;
	params.maxPlanes = max_planes_spin->value();
	params.maxIterations = max_iterations_spin->value();
	params.distanceThreshold = distance_threshold_spin->value();
	params.minInliers = static_cast<std::size_t>(min_inliers_spin->value());
	params.optimizeCoefficients = optimize_coefficients_check->isChecked();
	return params;
}

void PlaneFitDialog::on_done_button_clicked(bool)
{
	accept();
}

void PlaneFitDialog::on_cancel_button_clicked(bool)
{
	reject();
}

} // namespace smcp
