/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania
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

#pragma once
#include <QWidget>

namespace smcp
{

class ProjectorWidget final : public QWidget
{
	Q_OBJECT

public:
	explicit ProjectorWidget(QWidget* parent = nullptr, Qt::WindowFlags flags = nullptr);
	~ProjectorWidget() override;

	void Reset(void);
	void SetScreen(int screen) { _screen = screen; }
	void SetPatternCount(int count) { _patternCount = count; }
	int GetCurrentPattern(void) const { return _currentPattern; }
	int GetPatternCount() const { return _patternCount; }

	//projection cycle
	void Start(void);
	void Stop(void);
	void Prev(void);
	void Next(void);
	bool Finished(void) const;

	void Clear()
	{
		_pixmap = QPixmap();
		update();
	}

	const QPixmap* Pixmap() const { return &_pixmap; }

	void SetPixmap(const QPixmap& pixmap)
	{
		_pixmap = pixmap;
		update();
	}

	bool IsUpdated(void) const { return _updated; }
	void ClearUpdated(void) { _updated = false; }

	bool SaveInfo(const QString& filename, bool invert) const;

	// Alignment.
	void SetDrawCross(bool drawCross)
	{
		_drawCross = drawCross;
	}

signals:
	void new_image(QPixmap image);

protected:
	void paintEvent(QPaintEvent*) override;

	void MakePattern(void);
	void UpdatePatternBitCount(void);
	static QPixmap MakePattern(int rows, int cols, int vmask, int voffset, int hmask, int hoffset, int inverted);

private:
	int _screen;
	QPixmap _pixmap;
	int _currentPattern;
	int _patternCount;
	int _vbits;
	int _hbits;
	volatile bool _updated;

	// Alignment.
	bool _drawCross;
};

} // namespace smcp
