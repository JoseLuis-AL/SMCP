/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
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

// Qt.
#include <QWidget>
#include <QPixmap>
#include <QMutex>

// OpenCV.
#include <opencv2/core/core.hpp>

namespace smcp
{
/// <summary>
/// High-performance image display widget. Renders cv::Mat or QPixmap images with automatic aspect-ratio scaling and an internal scale cache that
/// avoids redundant resampling on every repaint.
/// </summary>
/// <remarks>
/// Designed as a drop-in replacement for ImageLabel. All cv::Mat to QPixmap conversions are self-contained (no external IoUtil dependency) and use
/// vectorised cv::cvtColor instead of manual pixel loops. The scaled pixmap is cached and only recomputed when the source image or the widget size
/// changes, eliminating the main bottleneck of the original implementation. Thread safety: setImage(cv::Mat) and setImage(QPixmap) are safe to call
/// from any thread; the actual pixel data is converted immediately under a mutex and the GUI is updated asynchronously via QWidget::update().
/// </remarks>
class PixmapWidget final : public QWidget
{
	Q_OBJECT

public:
	explicit PixmapWidget(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());

	/* PUBLIC API =============================================================================== */

	/// <summary>
	/// Sets the display image from an OpenCV matrix. Accepts CV_8UC1 (grayscale) and CV_8UC3 (BGR) formats.
	/// </summary>
	/// <param name="image">Source cv::Mat; an empty Mat clears the display.</param>
	void setImage(const cv::Mat& image);

	/// <summary>
	/// Sets the display image from an already-built QPixmap.
	/// </summary>
	/// <param name="pixmap">Source pixmap; a null pixmap clears the display.</param>
	void setImage(const QPixmap& pixmap);

	/// <summary>
	/// Alias for setImage(QPixmap) to maintain compatibility with code that calls setPixmap() (e.g. ImageLabel, CaptureDialog).
	/// </summary>
	/// <param name="pixmap">Source pixmap.</param>
	void setPixmap(const QPixmap& pixmap);

	/// <summary>
	/// Returns a read-only pointer to the current (unscaled) source pixmap.
	/// </summary>
	const QPixmap* pixmap() const;

	/// <summary>
	/// Clears the display and invalidates the scale cache.
	/// </summary>
	void Clear();

protected:
	/// <summary>
	/// Paints the cached scaled pixmap centred within the widget, or a "No image" placeholder when no image has been set.
	/// </summary>
	void paintEvent(QPaintEvent* event) override;

	/// <summary>
	/// Invalidates the scale cache so the next paintEvent recomputes the scaled pixmap at the new widget dimensions.
	/// </summary>
	void resizeEvent(QResizeEvent* event) override;

private:

	/* CONVERSION HELPERS ====================================================================== */

	/// <summary>
	/// Converts a CV_8UC3 (BGR) matrix to a QPixmap using vectorised cv::cvtColor (BGR to BGRA) and zero-copy QImage construction.
	/// </summary>
	/// <param name="bgr">Source 3-channel BGR image.</param>
	/// <returns>QPixmap ready for display, or a null QPixmap if the type is unsupported.</returns>
	static QPixmap pixmapFromBGR(const cv::Mat& bgr);

	/// <summary>
	/// Converts a CV_8UC1 (grayscale) matrix to a QPixmap using vectorised cv::cvtColor (GRAY to BGRA) and zero-copy QImage construction.
	/// </summary>
	/// <param name="gray">Source single-channel grayscale image.</param>
	/// <returns>QPixmap ready for display, or a null QPixmap if the type is unsupported.</returns>
	static QPixmap pixmapFromGray(const cv::Mat& gray);

	/// <summary>
	/// Invalidates the scale cache, forcing the next paintEvent to recompute the scaled version from the source pixmap.
	/// </summary>
	void invalidateCache();

	/* ATTRIBUTES ============================================================================== */

	QPixmap currentPixmap;		///< Unscaled source pixmap (full resolution).
	QPixmap cachedScaled;		///< Scaled pixmap at lastCachedSize dimensions.
	QSize   lastCachedSize;	///< Widget size when cachedScaled was computed.
	bool    cacheValid;		///< True when cachedScaled is usable for the current frame.
	QMutex  mutex;				///< Guards currentPixmap for thread-safe setImage calls.
};
}
