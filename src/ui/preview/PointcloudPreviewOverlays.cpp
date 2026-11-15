// PointcloudPreviewWidget, part 2 of 3: what is painted over the 3D view with QPainter.
//
//   - the transform gizmo (rotation rings or move arrows on the selection centroid): where its
//     handles are on screen, which one is under the cursor, and how it is drawn;
//   - the view selector in the bottom-right corner;
//   - the selection rectangle and the brush cursor.
//
// The gizmo only previews a change (SetActiveLayersTransform); releasing the mouse emits
// LayerRotated / LayerMoved and the editor dialog bakes the transform into the points.

#include "ui/preview/PointcloudPreviewWidget.h"
#include "ui/preview/PointcloudPreviewInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>

namespace smcp
{
	using namespace PreviewInternal;

	void PointcloudPreviewWidget::SetGizmo(GizmoMode mode, int layerId)
	{
		CancelGizmoDrag();
		_gizmoMode = layerId >= 0 ? mode : GizmoMode::None;
		_gizmoLayerId = _gizmoMode == GizmoMode::None ? -1 : layerId;
		_gizmoHoverHandle = -1;
		UpdateGizmoPivot();
		setMouseTracking(_gizmoLayerId >= 0 || _brushSelectionMode);
		unsetCursor();
		update();
	}

	void PointcloudPreviewWidget::UpdateGizmoPivot()
	{
		if (_gizmoLayerId < 0)
		{
			return;
		}
		const Pointcloud::ColorCloudPtr cloud = Cloud(IndexOfLayer(_gizmoLayerId));
		if (!cloud)
		{
			_gizmoMode = GizmoMode::None;
			_gizmoLayerId = -1;
			_gizmoDragHandle = -1;
			setMouseTracking(_brushSelectionMode);
			return;
		}
		const cv::Vec3d centroid = SelectionCentroid();
		_gizmoPivot = QVector3D(
			static_cast<float>(centroid[0]), static_cast<float>(-centroid[1]), static_cast<float>(-centroid[2]));
	}

	bool PointcloudPreviewWidget::GizmoVisible() const
	{
		const int index = IndexOfLayer(_gizmoLayerId);
		return _gizmoMode != GizmoMode::None && index >= 0 && _clouds[index]->visible && !_activeLayers.empty();
	}

	void PointcloudPreviewWidget::CancelGizmoDrag()
	{
		if (_gizmoDragHandle < 0)
		{
			return;
		}
		_gizmoDragHandle = -1;
		_gizmoDragDegrees = 0.0;
		_gizmoDragOffset = QVector3D();
		SetActiveLayersTransform(QMatrix4x4());
		emit GizmoDragCanceled();
	}

	bool PointcloudPreviewWidget::Project(const QVector3D& point, QPointF& screen) const
	{
		const QVector4D clip = _projection * _view * QVector4D(point, 1.0f);
		if (clip.w() <= 1e-6f)
		{
			return false;
		}
		screen = QPointF((clip.x() / clip.w() + 1.0) * 0.5 * width(), (1.0 - clip.y() / clip.w()) * 0.5 * height());
		return true;
	}

	double PointcloudPreviewWidget::WorldPerPixel(const QVector3D& point) const
	{
		const QVector3D eye = _view.inverted().map(QVector3D(0, 0, 0));
		return 2.0 * (eye - point).length() * std::tan(qDegreesToRadians(Field_Of_View_Degrees * 0.5)) /
			std::max(1, height());
	}

	PointcloudPreviewWidget::GizmoRing PointcloudPreviewWidget::ProjectGizmoRing(int axis) const
	{
		GizmoRing ring;
		const float radius = static_cast<float>(Gizmo_Radius_Pixels * WorldPerPixel(_gizmoPivot));
		// Front/back is judged along the camera's viewing direction, not toward the eye from the pivot:
		// with perspective the latter tilts off-center rings, so a ring facing the camera looked half hidden.
		const QVector3D towardViewer = _view.inverted().mapVector(QVector3D(0, 0, 1)).normalized();

		// u x v = axis, so increasing angles turn positively (right-hand rule) around the axis.
		const QVector3D a = GizmoAxisVector(axis);
		const QVector3D reference = std::abs(a.x()) < 0.9f ? QVector3D(1, 0, 0) : QVector3D(0, 1, 0);
		const QVector3D u = QVector3D::crossProduct(a, reference).normalized();
		const QVector3D v = QVector3D::crossProduct(a, u);
		for (int i = 0; i <= Gizmo_Ring_Samples; ++i)
		{
			const double angle = 2.0 * M_PI * i / Gizmo_Ring_Samples;
			const QVector3D offset =
				radius * (static_cast<float>(std::cos(angle)) * u + static_cast<float>(std::sin(angle)) * v);
			QPointF screen;
			if (!Project(_gizmoPivot + offset, screen))
			{
				return GizmoRing();
			}
			ring.points.push_back(screen);
			// Slightly favor the front so a ring seen edge-on stays grabbable.
			ring.front.push_back(QVector3D::dotProduct(offset, towardViewer) >= -0.05f * radius);
		}
		return ring;
	}

	bool PointcloudPreviewWidget::ProjectGizmoArrow(int axis, QPointF& start, QPointF& tip) const
	{
		// While moving, the arrows follow the layer.
		const QVector3D pivot = _gizmoPivot + _gizmoDragOffset;
		const float length = static_cast<float>(Gizmo_Radius_Pixels * WorldPerPixel(pivot));
		const QVector3D a = GizmoAxisVector(axis);
		if (!Project(pivot + 0.2f * length * a, start) || !Project(pivot + length * a, tip))
		{
			return false;
		}
		const QPointF d = tip - start;
		return QPointF::dotProduct(d, d) > 12.0 * 12.0;
	}

	int PointcloudPreviewWidget::GizmoHandleAt(const QPoint& pos) const
	{
		int best = -1;
		double bestDistance = Gizmo_Grab_Pixels;
		if (_gizmoMode == GizmoMode::Move)
		{
			QPointF center;
			if (Project(_gizmoPivot, center))
			{
				const QPointF d = pos - center;
				if (std::sqrt(QPointF::dotProduct(d, d)) <= Gizmo_Center_Pixels)
				{
					return Gizmo_Center_Handle;
				}
			}
			for (int axis = 0; axis < 3; ++axis)
			{
				QPointF start, tip;
				if (ProjectGizmoArrow(axis, start, tip))
				{
					const double distance = DistanceToSegment(pos, start, tip);
					if (distance < bestDistance)
					{
						bestDistance = distance;
						best = axis;
					}
				}
			}
			return best;
		}

		for (int axis = 0; axis < 3; ++axis)
		{
			const GizmoRing ring = ProjectGizmoRing(axis);
			for (std::size_t i = 1; i < ring.points.size(); ++i)
			{
				if (!ring.front[i - 1] || !ring.front[i])
				{
					continue;
				}
				const double distance = DistanceToSegment(pos, ring.points[i - 1], ring.points[i]);
				if (distance < bestDistance)
				{
					bestDistance = distance;
					best = axis;
				}
			}
		}
		return best;
	}

	void PointcloudPreviewWidget::PaintOverlays(QPainter& painter)
	{
		painter.setRenderHint(QPainter::Antialiasing);
		if (_brushStrokeActive)
		{
			painter.setPen(QPen(Selection_Color, 2, Qt::SolidLine, Qt::RoundCap));
			for (std::size_t i = 1; i < _brushStroke.size(); ++i)
				painter.drawLine(_brushStroke[i - 1].position, _brushStroke[i].position);
		}
		if (_brushSelectionMode && _brushCursorVisible && !_working)
		{
			painter.setBrush(Qt::NoBrush);
			painter.setPen(QPen(Qt::white, 1, Qt::DashLine));
			painter.drawEllipse(_brushPosition, _brushRadius, _brushRadius);
		}
		if (GizmoVisible())
		{
			PaintGizmo(painter);
		}
		if (_selectionBoxActive)
		{
			PaintSelectionBox(painter);
		}
		PaintViewSelector(painter);
	}

	void PointcloudPreviewWidget::PaintSelectionBox(QPainter& painter)
	{
		const QRect box = QRect(_selectionBoxStart, _selectionBoxEnd).normalized();
		painter.setPen(QPen(Qt::white, 1.0, Qt::DashLine));
		painter.setBrush(QColor(255, 255, 255, 30));
		painter.drawRect(box);
	}

	QPointF PointcloudPreviewWidget::ViewAxisPosition(int axis, int sign) const
	{
		QMatrix4x4 rotation;
		rotation.rotate(_rotationX, 1, 0, 0);
		rotation.rotate(_rotationY, 0, 1, 0);
		const auto direction = rotation.mapVector(GizmoAxisVector(axis) * sign);
		return QPointF(width() - 58 + direction.x() * 34, height() - 58 - direction.y() * 34);
	}

	void PointcloudPreviewWidget::PaintViewSelector(QPainter& painter)
	{
		const QPointF center(width() - 58, height() - 58);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(35, 38, 43, 200));
		painter.drawEllipse(center, 49, 49);
		struct AxisEnd
		{
			int axis;
			int sign;
			float depth;
		};
		std::vector<AxisEnd> ends;
		QMatrix4x4 rotation;
		rotation.rotate(_rotationX, 1, 0, 0);
		rotation.rotate(_rotationY, 0, 1, 0);
		for (int sign : { -1, 1 })
			for (int axis = 0; axis < 3; ++axis)
				ends.push_back({ axis, sign, rotation.mapVector(GizmoAxisVector(axis) * sign).z() });
		std::sort(ends.begin(), ends.end(), [](const AxisEnd& a, const AxisEnd& b) { return a.depth < b.depth; });
		for (const auto& end : ends)
		{
			const auto point = ViewAxisPosition(end.axis, end.sign);
			const QColor color = GizmoAxisColor(end.axis);
			painter.setPen(QPen(color, 2));
			painter.drawLine(center, point);
			painter.setBrush(end.sign > 0 ? color : color.darker(190));
			painter.drawEllipse(point, 9, 9);
			painter.setPen(end.sign > 0 ? Qt::black : Qt::white);
			painter.drawText(QRectF(point - QPointF(9, 9), QSizeF(18, 18)), Qt::AlignCenter,
				end.sign > 0 ? GizmoAxisName(end.axis) : QString());
		}
	}

	void PointcloudPreviewWidget::PaintGizmo(QPainter& painter)
	{
		QPointF center;
		if (!Project(_gizmoPivot + _gizmoDragOffset, center))
		{
			return;
		}
		const bool dragging = _gizmoDragHandle >= 0;
		const auto isActive = [this, dragging](int handle)
		{ return handle == _gizmoDragHandle || (!dragging && handle == _gizmoHoverHandle); };

		if (_gizmoMode == GizmoMode::Rotate)
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				if (dragging && axis != _gizmoDragHandle)
				{
					continue; // while dragging, only the active ring is drawn
				}
				const GizmoRing ring = ProjectGizmoRing(axis);
				const bool active = isActive(axis);
				const QColor color = active ? GizmoAxisColor(axis).lighter(130) : GizmoAxisColor(axis);
				for (std::size_t i = 1; i < ring.points.size(); ++i)
				{
					const bool front = ring.front[i - 1] && ring.front[i];
					QColor segmentColor = color;
					segmentColor.setAlpha(front || dragging ? 255 : 60);
					painter.setPen(QPen(segmentColor, active ? 4.0 : (front ? 2.5 : 1.5), Qt::SolidLine, Qt::RoundCap));
					painter.drawLine(ring.points[i - 1], ring.points[i]);
				}
			}
			painter.setPen(Qt::NoPen);
			painter.setBrush(Qt::white);
			painter.drawEllipse(center, 3.0, 3.0);
		}
		else
		{
			for (int axis = 0; axis < 3; ++axis)
			{
				QPointF start, tip;
				if ((dragging && axis != _gizmoDragHandle) || !ProjectGizmoArrow(axis, start, tip))
				{
					continue;
				}
				const bool active = isActive(axis);
				const QColor color = active ? GizmoAxisColor(axis).lighter(130) : GizmoAxisColor(axis);
				painter.setPen(QPen(color, active ? 4.0 : 2.5, Qt::SolidLine, Qt::RoundCap));
				painter.drawLine(start, tip);
				// Arrowhead: a triangle pointing along the projected axis.
				const QPointF d = tip - start;
				const QPointF unit = d / std::sqrt(QPointF::dotProduct(d, d));
				const QPointF normal(-unit.y(), unit.x());
				const double size = active ? 12.0 : 10.0;
				const QPointF head[3] = { tip + unit * size, tip + normal * (size * 0.5), tip - normal * (size * 0.5) };
				painter.setPen(Qt::NoPen);
				painter.setBrush(color);
				painter.drawPolygon(head, 3);
			}
			if (!dragging || _gizmoDragHandle == Gizmo_Center_Handle)
			{
				const bool active = isActive(Gizmo_Center_Handle);
				painter.setPen(QPen(Qt::white, active ? 2.5 : 1.5));
				painter.setBrush(active ? QColor(255, 255, 255, 90) : Qt::NoBrush);
				painter.drawEllipse(center, Gizmo_Center_Pixels - 3.0, Gizmo_Center_Pixels - 3.0);
			}
		}

		if (!dragging)
		{
			return;
		}
		QString text;
		QColor color = Qt::white;
		if (_gizmoMode == GizmoMode::Rotate)
		{
			text = QString("%1  %2\u00b0").arg(GizmoAxisName(_gizmoDragHandle)).arg(_gizmoDragDegrees, 0, 'f', 1);
			color = GizmoAxisColor(_gizmoDragHandle).lighter(130);
		}
		else if (_gizmoDragHandle == Gizmo_Center_Handle)
		{
			// Cloud coordinates are (x, -y, -z) of the OpenGL offset.
			text = QString("(%1, %2, %3)")
					   .arg(_gizmoDragOffset.x(), 0, 'f', 2)
					   .arg(-_gizmoDragOffset.y(), 0, 'f', 2)
					   .arg(-_gizmoDragOffset.z(), 0, 'f', 2);
		}
		else
		{
			const double distance = QVector3D::dotProduct(_gizmoDragOffset, GizmoAxisVector(_gizmoDragHandle));
			text = QString("%1  %2").arg(GizmoAxisName(_gizmoDragHandle)).arg(distance, 0, 'f', 2);
			color = GizmoAxisColor(_gizmoDragHandle).lighter(130);
		}
		QFont font = painter.font();
		font.setPointSizeF(15);
		font.setWeight(QFont::DemiBold);
		font.setBold(true);
		painter.setFont(font);
		QRectF box = painter.fontMetrics().boundingRect(text);
		box.adjust(-8, -4, 8, 4);
		box.moveBottomLeft(center + QPointF(Gizmo_Radius_Pixels * 0.75, -Gizmo_Radius_Pixels * 0.75));
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(20, 20, 20, 200));
		painter.drawRoundedRect(box, 4, 4);
		painter.setPen(color);
		painter.drawText(box, Qt::AlignCenter, text);
	}
} // namespace smcp
