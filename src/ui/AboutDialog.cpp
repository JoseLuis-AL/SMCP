/*
Copyright (c) 2012-2015, Daniel Moreno and Gabriel Taubin
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

#include "ui/AboutDialog.h"

#include <QPixmap>

#include <opencv2/core/core.hpp>

// The version comes from project(SMCP VERSION ...) in CMakeLists.txt.
#ifndef SMCP_VERSION
#define SMCP_VERSION "0.0.0"
#endif

namespace smcp
{

AboutDialog::AboutDialog(QWidget * parent, Qt::WindowFlags flags):
    QDialog(parent, flags)
{
    Q_INIT_RESOURCE(resources);

    setupUi(this);

    // The logo is scaled once, smoothly, at the resolution of the screen.
    const qreal ratio = devicePixelRatioF();
    QPixmap logo(":/unison.png");
    logo = logo.scaled(logo_label->maximumSize() * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    logo.setDevicePixelRatio(ratio);
    logo_label->setPixmap(logo);

    version_label->setText(QStringLiteral("v%1").arg(QStringLiteral(SMCP_VERSION)));
    url_label->setText(QStringLiteral("<a href=\"https://github.com/JoseLuis-AL/SMCP\" "
        "style=\"color:#0067c0; text-decoration:none;\">github.com/JoseLuis-AL/SMCP</a>"));
    qt_version_label->setText(qVersion());
    opencv_version_label->setText(CV_VERSION);
    built_date_label->setText(QString("%1 %2").arg(__DATE__, __TIME__));
}

AboutDialog::~AboutDialog()
{
}

} // namespace smcp
