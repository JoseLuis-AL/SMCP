#include "PointcloudPreviewQDialog.h"

PointcloudPreviewQDialog::PointcloudPreviewQDialog(QWidget* parent)
	: QDialog(parent)
{
	setupUi(this);
}

PointcloudPreviewQDialog::~PointcloudPreviewQDialog()
{}

void PointcloudPreviewQDialog::on_load_pointcloud_button_clicked(bool checked)
{
	pointcloud_preview->loadPointcloud();
}