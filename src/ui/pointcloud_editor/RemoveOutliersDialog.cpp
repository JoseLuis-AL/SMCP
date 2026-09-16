#include "ui/pointcloud_editor/RemoveOutliersDialog.h"

namespace smcp
{

RemoveOutliersDialog::RemoveOutliersDialog(const Pointcloud::OutlierRemovalParams& params, QWidget* parent)
	: QDialog(parent)
{
	setupUi(this);
	mean_k_spin->setValue(params.meanK);
	stddev_mult_spin->setValue(params.stddevMult);
}

Pointcloud::OutlierRemovalParams RemoveOutliersDialog::Params() const
{
	Pointcloud::OutlierRemovalParams params;
	params.meanK = mean_k_spin->value();
	params.stddevMult = stddev_mult_spin->value();
	return params;
}

void RemoveOutliersDialog::on_done_button_clicked(bool)
{
	accept();
}

void RemoveOutliersDialog::on_cancel_button_clicked(bool)
{
	reject();
}

} // namespace smcp
