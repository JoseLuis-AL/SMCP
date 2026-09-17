#include "ui/pointcloud_editor/ExportPointcloudDialog.h"

namespace smcp
{

ExportPointcloudDialog::ExportPointcloudDialog(QWidget* parent)
	: QDialog(parent)
{
	setupUi(this);
}

ExportPointcloudDialog::Mode ExportPointcloudDialog::SelectedMode() const
{
	if (multiple_radio->isChecked())
	{
		return Mode::Multiple;
	}
	if (combine_radio->isChecked())
	{
		return Mode::Combine;
	}
	return Mode::First;
}

void ExportPointcloudDialog::on_export_button_clicked(bool)
{
	accept();
}

void ExportPointcloudDialog::on_cancel_button_clicked(bool)
{
	reject();
}

} // namespace smcp
