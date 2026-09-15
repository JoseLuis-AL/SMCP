#pragma once

#include <QDialog>
#include "ui_PointcloudPreviewQDialog.h"

class PointcloudPreviewQDialog : public QDialog, public Ui::PointcloudPreviewQDialogClass
{
	Q_OBJECT

public:
	PointcloudPreviewQDialog(QWidget* parent = nullptr);
	~PointcloudPreviewQDialog();

public slots:
	void on_load_pointcloud_button_clicked(bool checked = false);

private:
};
