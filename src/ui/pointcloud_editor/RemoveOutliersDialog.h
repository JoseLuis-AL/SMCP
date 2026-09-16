/*
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
*/

#pragma once

#include <QDialog>

#include "core/PointcloudOps.h"
#include "ui_RemoveOutliersDialog.h"

namespace smcp
{
/// Lets the user configure the statistical outlier removal operation.
class RemoveOutliersDialog : public QDialog, public Ui::RemoveOutliersDialog
{
	Q_OBJECT

public:
	explicit RemoveOutliersDialog(const Pointcloud::OutlierRemovalParams& params, QWidget* parent = nullptr);

	Pointcloud::OutlierRemovalParams Params() const;

public slots:
	void on_done_button_clicked(bool checked = false);
	void on_cancel_button_clicked(bool checked = false);
};
} // namespace smcp
