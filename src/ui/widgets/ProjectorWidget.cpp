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


#include "ui/widgets/ProjectorWidget.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QPainter>

#include <stdio.h>
#include <iostream>
#include <assert.h>

#include "ui/AboutDialog.h"
#include "core/StructuredLight.h"

namespace smcp
{

ProjectorWidget::ProjectorWidget(QWidget * parent, Qt::WindowFlags flags) : 
    QWidget(parent, flags),
    _screen(0),
    _currentPattern(-1),
    _patternCount(4),
    _vbits(1),
    _hbits(1),
    _updated(false),
	_drawCross(false)
{
}

ProjectorWidget::~ProjectorWidget()
{
    Stop();
}

void ProjectorWidget::Reset(void)
{
    _currentPattern = -1;
    _updated = false;
    _pixmap = QPixmap();
    emit new_image(_pixmap);
}

void ProjectorWidget::Start(void)
{
    Stop();
    Reset();

    //validate screen
    const QList<QScreen*> screenList = QGuiApplication::screens();
    int screens = screenList.size();
    if (_screen<0 || _screen>=screens)
    {   //error, fix it
        _screen = screens;
    }

    //display (QScreen sustituye a QDesktopWidget, obsoleto en Qt 5.14)
    QRect screenResolution = (_screen < screens)
        ? screenList[_screen]->geometry()
        : QGuiApplication::primaryScreen()->geometry();
    move(QPoint(screenResolution.x(), screenResolution.y()));
    showFullScreen();

    //update bit count for the current resolution
    UpdatePatternBitCount();
}

void ProjectorWidget::Stop(void)
{
    hide();
    Reset();
}

void ProjectorWidget::Prev(void)
{
    if (_updated)
    {   //pattern not processed: wait
        return;
    }

    if (_currentPattern<1)
    {
        return;
    }

    _currentPattern--;
    _pixmap = QPixmap();
    update();
    QApplication::processEvents();
}

void ProjectorWidget::Next(void)
{
    if (_updated)
    {   //pattern not processed: wait
        return;
    }

    if (Finished())
    {
        return;
    }

    _currentPattern++;
    _pixmap = QPixmap();
    update();
    QApplication::processEvents();
}

bool ProjectorWidget::Finished(void) const
{
    return (_currentPattern+2 > 2+4*_patternCount);
}

void ProjectorWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    // Check for valid pattern.
    if (_currentPattern < 0)
    {
        QRectF rect = QRectF(QPointF(0, 0), QPointF(width(), height()));
        painter.drawText(rect, Qt::AlignCenter, "No image");
    }
    else
    {
        // Update pattern if needed;
        bool updated = false;
        if (_pixmap.isNull())
        {
            updated = true;
            MakePattern();
        }

        // Draw pattern.
        QRectF rect = QRectF(QPointF(0, 0), QPointF(width(), height()));
        painter.drawPixmap(rect, _pixmap, rect);

    	// Draw cross.
        if (_drawCross)
        {
            const QPen crossPen(QColor("#3e8948"), 10);
            painter.setPen(crossPen);
            const int w = width();
            const int h = height();
            const int cx = w / 2;
            const int cy = h / 2;
            painter.drawLine(0, cy, w, cy);
            painter.drawLine(cx, 0, cx, h);
        }

        // Emit the new image.
        if (updated)
        {
            _updated = true;
            emit new_image(_pixmap);
        }
    }
}

void ProjectorWidget::UpdatePatternBitCount(void)
{
    int cols = width();
    int rows = height();

    //search bit number
    _vbits = 1;
    _hbits = 1;
    for (int i=(1<<_vbits); i<cols; i=(1<<_vbits)) { _vbits++; }
    for (int i=(1<<_hbits); i<rows; i=(1<<_hbits)) { _hbits++; }
    _patternCount = std::min(std::min(_vbits, _hbits), _patternCount);
    std::cerr << " vbits " << _vbits << " / cols="<<cols<<", mvalue="<< ((1<<_vbits)-1) << std::endl;
    std::cerr << " hbits " << _hbits << " / rows="<<rows<<", mvalue="<< ((1<<_hbits)-1) << std::endl;
    std::cerr << " pattern_count="<< _patternCount << std::endl; 
}

void ProjectorWidget::MakePattern(void)
{
    int cols = width();
    int rows = height();

    /*
    if (_currentPattern<1)
    {   //search bit number
        _vbits = 1;
        _hbits = 1;
        for (int i=(1<<_vbits); i<cols; i=(1<<_vbits)) { _vbits++; }
        for (int i=(1<<_hbits); i<rows; i=(1<<_hbits)) { _hbits++; }
        _patternCount = std::min(std::min(_vbits, _hbits), _patternCount);
        std::cerr << " vbits " << _vbits << " / cols="<<cols<<", mvalue="<< ((1<<_vbits)-1) << std::endl;
        std::cerr << " hbits " << _hbits << " / rows="<<rows<<", mvalue="<< ((1<<_hbits)-1) << std::endl;
        std::cerr << " pattern_count="<< _patternCount << std::endl; 
    }
    */

    int vmask = 0, voffset = ((1<<_vbits)-cols)/2, hmask = 0, hoffset = ((1<<_hbits)-rows)/2, inverted = (_currentPattern%2)==0;

    // patterns
    // -----------
    // 00 white
    // 01 black
    // -----------
    // 02 vertical, bit N-0, normal
    // 03 vertical, bit N-0, inverted
    // 04 vertical, bit N-1, normal
    // 04 vertical, bit N-2, inverted
    // ..
    // XX =  (2*_patternCount + 2) - 2 vertical, bit N, normal
    // XX =  (2*_patternCount + 2) - 1 vertical, bit N, inverted
    // -----------
    // 2+N+00 = 2*(_patternCount + 2) horizontal, bit N-0, normal
    // 2+N+01 horizontal, bit N-0, inverted
    // ..
    // YY =  (4*_patternCount + 2) - 2 horizontal, bit N, normal
    // YY =  (4*_patternCount + 2) - 1 horizontal, bit N, inverted

    if (_currentPattern<2)
    {   //white or black
        _pixmap = MakePattern(rows, cols, vmask, voffset, hmask, hoffset, inverted);
    }
    else if (_currentPattern<2*_patternCount+2)
    {   //vertical
        int bit = _vbits - _currentPattern/2;
        vmask = 1<<bit;
        //std::cerr << "v# cp: " << _currentPattern << " bit:" << bit << " mask:" << vmask << std::endl;
        _pixmap = MakePattern(rows, cols, vmask, voffset, hmask, hoffset, !inverted);
    }
    else if (_currentPattern<4*_patternCount+2)
    {   //horizontal
        int bit = _hbits + _patternCount - _currentPattern/2;
        hmask = 1<<bit;
        //std::cerr << "h# cp: " << _currentPattern << " bit:" << bit << " mask:" << hmask << std::endl;
        _pixmap = MakePattern(rows, cols, vmask, voffset, hmask, hoffset, !inverted);
    }
    else
    {   //error
        assert(false);
        Stop();
        return;
    }

    //_pixmap.save(QString("pat_%1.png").arg(_currentPattern, 2, 10, QLatin1Char('0')));
}

QPixmap ProjectorWidget::MakePattern(int rows, int cols, int vmask, int voffset, int hmask, int hoffset, int inverted)
{
    QImage image(cols, rows, QImage::Format_ARGB32);

    int tvalue = (inverted ? 0 : 255);
    int fvalue = (inverted ? 255 : 0);

    for (int h=0; h<rows; h++)
    {
        uchar * row = image.scanLine(h);
        for (int w=0; w<cols; w++)
        {
            uchar * px = row + (4*w);
            int test = (StructuredLight::BinaryToGray(h+hoffset) & hmask) + (StructuredLight::BinaryToGray(w+voffset) & vmask);
            int value = (test ? tvalue : fvalue);

            px[0] = value; //B
            px[1] = value; //G
            px[2] = value; //R
            px[3] = 0xff;  //A
        }
    }

    return QPixmap::fromImage(image);
}

bool ProjectorWidget::SaveInfo(QString const& filename, bool invert) const
{
    FILE * fp = fopen(qPrintable(filename), "w");
    if (!fp)
    {   //failed
        std::cerr << "Projector SaveInfo failed, file: " << qPrintable(filename) << std::endl;
        return false;
    }

    int cols = width();
    int rows = height();

    if (invert)
    { //rotated image
      rows = width();
      cols  = height();
    }

    int effectiveWidth = cols;
    int effectiveHeight = rows;

    int maxVertValue = (1<<std::min(_vbits,_patternCount));
    while (effectiveWidth>maxVertValue )
    {
        effectiveWidth >>= 1;
    }
    int maxHorzValue = (1<<std::min(_hbits,_patternCount));
    while (effectiveHeight>maxHorzValue)
    {
        effectiveHeight >>= 1;
    }

    fprintf(fp, "%u %u\n", effectiveWidth, effectiveHeight);

    fprintf(fp, "\n# width height\n"); //help

    std::cerr << "Saved projetor info: " << qPrintable(filename) << std::endl
              << " - Effective resolution: " << effectiveWidth << "x" << effectiveHeight << std::endl;

    //close
    fclose(fp);
    return true;
}
} // namespace smcp
