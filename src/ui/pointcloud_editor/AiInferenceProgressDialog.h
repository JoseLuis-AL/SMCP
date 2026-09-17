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
#include <QElapsedTimer>

#include "ui_AiInferenceProgressDialog.h"

class QKeyEvent;
class QTimer;

namespace smcp
{
/// <summary>
/// Modal "Thinking..." window shown while an AI model processes the first point cloud in WSL.
/// </summary>
/// <remarks>
/// It reports the elapsed time and the model's latest output line. Cancel, and closing the window,
/// emit CancelRequested(); the dialog stays open until the editor closes it after the job stops.
/// </remarks>
class AiInferenceProgressDialog : public QDialog, public Ui::AiInferenceProgressDialog
{
	Q_OBJECT

public:
	explicit AiInferenceProgressDialog(const QString& modelName, QWidget* parent = nullptr);

	void SetOutput(const QString& text);
	/// Inference finished; the result is being read into the editor and can no longer be cancelled.
	void SetLoadingResult();
	void SetCancelling();

	void reject() override;

signals:
	void CancelRequested();

public slots:
	void on_cancel_button_clicked(bool checked = false);

protected:
	void keyPressEvent(QKeyEvent* event) override;

private:
	void UpdateElapsedTime();

	QElapsedTimer _elapsed;
	QTimer* _elapsedTimer = nullptr;
};
} // namespace smcp
