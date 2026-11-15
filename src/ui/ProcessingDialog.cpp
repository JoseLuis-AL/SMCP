/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Brown University nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DANIEL MORENO AND GABRIEL TAUBIN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "ui/ProcessingDialog.h"

#include <QStyle>

namespace smcp
{


ProcessingDialog::ProcessingDialog(QWidget * parent, Qt::WindowFlags flags): 
    QDialog(parent, flags),
    _total(0),
    _cancel(false),
    _attention(false)
{
    setupUi(this);
}

ProcessingDialog::~ProcessingDialog()
{
}

void ProcessingDialog::Reset(void)
{
    progress_bar->setMaximum(0);
    current_message_label->clear();
    message_text->clear();
    percent_label->clear();
    SetRunning(true);
    current_message_label->setStyleSheet(QString());
    _cancel = false;
    _attention = false;
}

namespace
{
    // Semantic colors of the theme (docs/STYLE.md): `delete` for errors, a dark amber for warnings.
    const char * Error_Color = "#e43b44";
    const char * Warning_Color = "#9a6700";

    bool IsError(const QString & text)
    {
        return text.contains("ERROR") || text.contains("failed") || text.contains("not found");
    }

    bool IsWarning(const QString & text)
    {
        return text.contains("WARNING") || text.contains("CANCEL") || text.contains("canceled");
    }
}

void ProcessingDialog::SetCurrentMessage(const QString & text)
{
    current_message_label->setStyleSheet(IsError(text) ? QString("color: %1;").arg(Error_Color) : QString());
    current_message_label->setText(text);
    _attention = _attention || IsError(text) || IsWarning(text);
}

void ProcessingDialog::Finish(void)
{
    //progress_bar->setValue(_total);
    SetRunning(false);
}

void ProcessingDialog::SetRunning(bool running)
{
    close_cancel_button->setText(running ? "Cancel" : "Close");
    close_cancel_button->setEnabled(true);
    close_cancel_button->setProperty("danger", running);
    close_cancel_button->setProperty("primary", !running);
    // Dynamic properties change the style rule that applies: ask the style to look again.
    close_cancel_button->style()->unpolish(close_cancel_button);
    close_cancel_button->style()->polish(close_cancel_button);
}

void ProcessingDialog::UpdatePercent(void)
{
    const int maximum = progress_bar->maximum();
    percent_label->setText(maximum > 0 ? QString("%1%").arg(100 * progress_bar->value() / maximum) : QString());
}

void ProcessingDialog::on_close_cancel_button_clicked(bool checked)
{
    if (close_cancel_button->text()=="Close")
    {
        accept();
    }
    else if (!_cancel)
    {
        _cancel = true;
        Message("CANCEL: waiting the current operation to finish (might take a little)");
        emit CancelRequested();
    }
}

void ProcessingDialog::reject()
{
    on_close_cancel_button_clicked();
}

void ProcessingDialog::Message(const QString & text)
{
    const bool error = IsError(text), warning = IsWarning(text);
    _attention = _attention || error || warning;

    const QColor normal = message_text->palette().color(QPalette::Text);
    message_text->setTextColor(error ? QColor(Error_Color) : warning ? QColor(Warning_Color) : normal);
    message_text->append(text);
    message_text->setTextColor(normal);
}

} // namespace smcp
