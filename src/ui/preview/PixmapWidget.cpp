#include "ui/preview/PixmapWidget.h"

// Qt.
#include <QPainter>
#include <QResizeEvent>

// OpenCV.
#include <opencv2/imgproc/imgproc.hpp>

namespace smcp
{

/* ============================================================================================= */
/* CONSTRUCTOR                                                                                   */
/* ============================================================================================= */

/// <summary>
/// Constructs a PixmapWidget with an empty display and an invalidated scale cache.
/// </summary>
/// <param name="parent">Parent widget (passed to QWidget).</param>
/// <param name="flags">Window flags (passed to QWidget).</param>
PixmapWidget::PixmapWidget(QWidget* parent, Qt::WindowFlags flags)
	: QWidget(parent, flags)
	, currentPixmap()
	, cachedScaled()
	, lastCachedSize()
	, cacheValid(false)
	, mutex()
{
}

/* ============================================================================================= */
/* PUBLIC API                                                                                    */
/* ============================================================================================= */

void PixmapWidget::setImage(const cv::Mat& image)
{
	// Convert the cv::Mat to QPixmap outside of paintEvent to keep the GUI thread free.
	QPixmap converted;

	if (!image.empty())
	{
		switch (image.type())
		{
		case CV_8UC3:
			converted = pixmapFromBGR(image);
			break;

		case CV_8UC1:
			converted = pixmapFromGray(image);
			break;

		default:
			// Unsupported format; leave converted as null pixmap.
			break;
		}
	}

	// Store the converted pixmap and schedule a repaint.
	{
		QMutexLocker locker(&mutex);
		currentPixmap = converted;
		invalidateCache();
	}
	update();
}

void PixmapWidget::setImage(const QPixmap& pixmap)
{
	{
		QMutexLocker locker(&mutex);
		currentPixmap = pixmap;
		invalidateCache();
	}
	update();
}

void PixmapWidget::setPixmap(const QPixmap& pixmap)
{
	setImage(pixmap);
}

const QPixmap* PixmapWidget::pixmap() const
{
	return &currentPixmap;
}

void PixmapWidget::Clear()
{
	{
		QMutexLocker locker(&mutex);
		currentPixmap = QPixmap();
		cachedScaled = QPixmap();
		cacheValid = false;
	}
	update();
}

/* ============================================================================================= */
/* EVENTS                                                                                        */
/* ============================================================================================= */

void PixmapWidget::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event);
	QPainter painter(this);

	if (currentPixmap.isNull())
	{
		// No image available; display a centred placeholder text.
		painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, tr("No image"));
		return;
	}

	// Rebuild the scaled pixmap only when the cache is stale.
	if (!cacheValid || lastCachedSize != size())
	{
		cachedScaled = currentPixmap.scaled(size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
		lastCachedSize = size();
		cacheValid = true;
	}

	// Draw the cached scaled pixmap centred within the widget.
	const int x = (width() - cachedScaled.width()) / 2;
	const int y = (height() - cachedScaled.height()) / 2;
	painter.drawPixmap(x, y, cachedScaled);
}

void PixmapWidget::resizeEvent(QResizeEvent* event)
{
	QWidget::resizeEvent(event);

	// Invalidate the cache so paintEvent recomputes at the new size.
	cacheValid = false;
}

/* ============================================================================================= */
/* CONVERSION HELPERS                                                                            */
/* ============================================================================================= */

QPixmap PixmapWidget::pixmapFromBGR(const cv::Mat& bgr)
{
	if (bgr.type() != CV_8UC3)
	{
		return {};
	}

	// Vectorised BGR → BGRA conversion (adds opaque alpha channel).
	cv::Mat bgra;
	cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);

	// Construct QImage directly over the cv::Mat buffer (zero-copy).
	// QImage::Format_ARGB32 on little-endian is stored as B-G-R-A in memory,
	// which matches the BGRA layout produced by cv::cvtColor.
	const QImage qimg(bgra.data, bgra.cols, bgra.rows,
		static_cast<int>(bgra.step), QImage::Format_ARGB32);

	// QPixmap::fromImage performs a deep copy, so the cv::Mat can be freed safely.
	return QPixmap::fromImage(qimg);
}

QPixmap PixmapWidget::pixmapFromGray(const cv::Mat& gray)
{
	if (gray.type() != CV_8UC1)
	{
		return {};
	}

	// Vectorised GRAY → BGRA conversion (replicates intensity to all channels).
	cv::Mat bgra;
	cv::cvtColor(gray, bgra, cv::COLOR_GRAY2BGRA);

	// Construct QImage directly over the cv::Mat buffer (zero-copy).
	const QImage qimg(bgra.data, bgra.cols, bgra.rows,
		static_cast<int>(bgra.step), QImage::Format_ARGB32);

	// QPixmap::fromImage performs a deep copy, so the cv::Mat can be freed safely.
	return QPixmap::fromImage(qimg);
}

/* ============================================================================================= */
/* CACHE                                                                                         */
/* ============================================================================================= */

void PixmapWidget::invalidateCache()
{
	cacheValid = false;
}

} // namespace smcp
