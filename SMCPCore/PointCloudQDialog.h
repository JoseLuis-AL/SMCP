#pragma once

#include <QDialog>
#include "ui_PointCloudQDialog.h"

class PointCloudQDialog : public QDialog, public Ui::PointCloudQDialogClass
{
	Q_OBJECT

public:
	PointCloudQDialog(QWidget *parent = nullptr);
	~PointCloudQDialog();

private:
	 
};

