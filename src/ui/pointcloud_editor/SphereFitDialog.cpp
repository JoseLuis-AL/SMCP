#include "ui/pointcloud_editor/SphereFitDialog.h"

#include <QMessageBox>

namespace smcp
{

SphereFitDialog::SphereFitDialog(const Pointcloud::SphereFitParams& params, QWidget* parent)
	: QDialog(parent)
{
	setupUi(this);
	max_spheres_spin->setValue(params.maxSpheres);
	max_iterations_spin->setValue(params.maxIterations);
	distance_threshold_spin->setValue(params.distanceThreshold);
	min_radius_spin->setValue(params.minRadius);
	max_radius_spin->setValue(params.maxRadius);
	min_inliers_spin->setValue(static_cast<int>(params.minInliers));
	optimize_coefficients_check->setChecked(params.optimizeCoefficients);
}

Pointcloud::SphereFitParams SphereFitDialog::Params() const
{
	Pointcloud::SphereFitParams params;
	params.maxSpheres = max_spheres_spin->value();
	params.maxIterations = max_iterations_spin->value();
	params.distanceThreshold = distance_threshold_spin->value();
	params.minRadius = min_radius_spin->value();
	params.maxRadius = max_radius_spin->value();
	params.minInliers = static_cast<std::size_t>(min_inliers_spin->value());
	params.optimizeCoefficients = optimize_coefficients_check->isChecked();
	return params;
}

void SphereFitDialog::on_done_button_clicked(bool)
{
	if (min_radius_spin->value() > max_radius_spin->value())
	{
		QMessageBox::warning(this, tr("Invalid radius range"),
			tr("Minimum radius must be less than or equal to maximum radius."));
		return;
	}
	accept();
}

void SphereFitDialog::on_cancel_button_clicked(bool)
{
	reject();
}

} // namespace smcp
