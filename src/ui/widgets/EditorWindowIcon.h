#pragma once

#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace smcp
{
	/// Window icon of the Point Cloud Editor.
	///
	/// The artwork of the button that opens the editor (pointcloud-editor-icon.svg) is a cloud of
	/// small colored points on a transparent background with a wide margin: at the 16 px of a title
	/// bar almost nothing of it is left. Here the same points are enlarged to fill a white rounded
	/// square with a gray border, which stays readable on light and dark title bars.
	inline QIcon EditorWindowIcon()
	{
		// The points occupy this part of the 128 x 128 artwork.
		const QRectF content(22.0, 40.0, 84.0, 61.0);
		const QIcon artwork(":/pointcloud-editor-icon.svg");
		QIcon icon;
		for (const int side : { 16, 20, 24, 32, 48, 64 })
		{
			QPixmap pixmap(side, side);
			pixmap.fill(Qt::transparent);
			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
			painter.setPen(QPen(QColor("#bdbdbd"), 1));
			painter.setBrush(Qt::white);
			const qreal radius = side * 0.19;
			painter.drawRoundedRect(QRectF(0.5, 0.5, side - 1.0, side - 1.0), radius, radius);

			// Scale the artwork so its points span 84 % of the square, and center them.
			const qreal scale = 0.84 * side / content.width();
			const int rendered = qRound(128.0 * scale);
			const QPixmap points = artwork.pixmap(QSize(rendered, rendered));
			const QPointF origin(side / 2.0 - content.center().x() * scale, side / 2.0 - content.center().y() * scale);
			painter.drawPixmap(QRectF(origin, QSizeF(rendered, rendered)), points, QRectF(points.rect()));
			painter.end();
			icon.addPixmap(pixmap);
		}
		return icon;
	}
} // namespace smcp
