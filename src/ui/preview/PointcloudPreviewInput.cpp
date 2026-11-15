// PointcloudPreviewWidget, part 3 of 3: mouse, wheel and keyboard.
//
// mousePressEvent decides what a drag is, checking in this order:
//   1. the left button on the view selector    -> jump to an axis view, or orbit by dragging it
//   2. the left button with a selection tool   -> start a rectangle or a brush stroke
//   3. the left button on a gizmo handle       -> rotate or move the active layers (preview only)
// Anything else is navigation, handled in mouseMoveEvent: the middle button orbits, with Shift it
// pans and with Ctrl it zooms. Esc or the right button cancel a gizmo drag, a selection rectangle
// or a brush stroke. Selection and gizmo drags do not start while an edit is being published.

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

	void PointcloudPreviewWidget::mousePressEvent(QMouseEvent* event)
	{
		if (_brushStrokeActive && event->button() == Qt::RightButton)
		{
			CancelBrushStroke();
			event->accept();
			return;
		}
		if (event->button() == Qt::LeftButton && QRect(width() - 112, height() - 112, 108, 108).contains(event->pos()))
		{
			int pickedAxis = -1, pickedSign = 1;
			double nearest = 12.0;
			for (int sign : { -1, 1 })
				for (int axis = 0; axis < 3; ++axis)
				{
					const auto d = event->pos() - ViewAxisPosition(axis, sign);
					const double length = std::hypot(d.x(), d.y());
					if (length < nearest)
					{
						nearest = length;
						pickedAxis = axis;
						pickedSign = sign;
					}
				}
			if (pickedAxis >= 0)
			{
				const auto projected = ViewAxisPosition(pickedAxis, 1) - QPointF(width() - 58, height() - 58);
				if (std::hypot(projected.x(), projected.y()) < 2)
				{
					QMatrix4x4 rotation;
					rotation.rotate(_rotationX, 1, 0, 0);
					rotation.rotate(_rotationY, 0, 1, 0);
					pickedSign = rotation.mapVector(GizmoAxisVector(pickedAxis)).z() > 0 ? -1 : 1;
				}
				_rotationX = pickedAxis == 1 ? (pickedSign > 0 ? -90 : 90) : 0;
				_rotationY = pickedAxis == 0 ? (pickedSign > 0 ? -90 : 90)
					: pickedAxis == 2		 ? (pickedSign > 0 ? 180 : 0)
											 : 0;
				update();
				event->accept();
				return;
			}
			_orbitSelectorDrag = true;
			_lastMousePos = event->pos();
			event->accept();
			return;
		}
		_lastMousePos = event->pos();
		event->accept();
		if (_gizmoDragHandle >= 0 && event->button() == Qt::RightButton)
		{
			CancelGizmoDrag();
			update();
			return;
		}
		if (!_working && _selectionMode && event->button() == Qt::LeftButton)
		{
			_selectionBoxActive = true;
			_selectionBoxStart = event->pos();
			_selectionBoxEnd = event->pos();
			setFocus(Qt::MouseFocusReason); // Esc cancels
			return;
		}
		if (!_working && _brushSelectionMode && event->button() == Qt::LeftButton)
		{
			UpdateProjection(width(), height());
			UpdateViewMatrix();
			_brushMatrix = _projection * _view;
			_brushViewport = size();
			_brushStroke.clear();
			_brushStrokeActive = true;
			AppendBrushSample(event->localPos());
			setFocus(Qt::MouseFocusReason);
			update();
			return;
		}
		if (_working || event->button() != Qt::LeftButton || !GizmoVisible())
		{
			return;
		}

		UpdateProjection(width(), height());
		UpdateViewMatrix();
		const int handle = GizmoHandleAt(event->pos());
		if (handle < 0)
		{
			return; // not on a handle: the drag orbits the camera
		}

		if (_gizmoMode == GizmoMode::Rotate)
		{
			// Positive-rotation tangent of the ring at the grabbed point.
			const GizmoRing ring = ProjectGizmoRing(handle);
			std::size_t nearest = 1;
			double nearestDistance = std::numeric_limits<double>::max();
			for (std::size_t i = 1; i < ring.points.size(); ++i)
			{
				const double distance = DistanceToSegment(event->pos(), ring.points[i - 1], ring.points[i]);
				if (distance < nearestDistance)
				{
					nearestDistance = distance;
					nearest = i;
				}
			}
			const QPointF tangent = ring.points[nearest] - ring.points[nearest - 1];
			const double length = std::sqrt(QPointF::dotProduct(tangent, tangent));
			if (length < 1e-6)
			{
				return;
			}
			_gizmoDragDirection = tangent / length;
		}
		else if (handle == Gizmo_Center_Handle)
		{
			_gizmoDragScale = WorldPerPixel(_gizmoPivot);
		}
		else
		{
			// Axis direction on screen and how many world units one pixel along it is worth.
			QPointF start, tip;
			ProjectGizmoArrow(handle, start, tip);
			QPointF center;
			Project(_gizmoPivot, center);
			const QPointF d = tip - center;
			const double pixels = std::sqrt(QPointF::dotProduct(d, d));
			_gizmoDragDirection = d / pixels;
			_gizmoDragScale = Gizmo_Radius_Pixels * WorldPerPixel(_gizmoPivot) / pixels;
		}
		_gizmoDragHandle = handle;
		_gizmoDragStart = event->pos();
		_gizmoDragDegrees = 0.0;
		_gizmoDragOffset = QVector3D();
		setFocus(Qt::MouseFocusReason); // Esc cancels
		update();
	}

	void PointcloudPreviewWidget::mouseMoveEvent(QMouseEvent* event)
	{
		if (_brushSelectionMode)
		{
			_brushPosition = event->localPos();
			_brushCursorVisible = rect().contains(event->pos());
			if (_brushStrokeActive)
			{
				AppendBrushSample(event->localPos());
				update();
				event->accept();
				return;
			}
			update();
		}
		if (_selectionBoxActive)
		{
			_selectionBoxEnd = event->pos();
			update();
			event->accept();
			return;
		}
		if (_gizmoDragHandle >= 0)
		{
			const QPointF delta = event->pos() - _gizmoDragStart;
			const bool snap = event->modifiers() & Qt::ControlModifier;
			QMatrix4x4 transform;
			if (_gizmoMode == GizmoMode::Rotate)
			{
				// Moving along the ring by its on-screen radius turns one radian.
				double degrees =
					qRadiansToDegrees(QPointF::dotProduct(delta, _gizmoDragDirection) / Gizmo_Radius_Pixels);
				if (snap)
				{
					degrees = std::round(degrees / Gizmo_Snap_Degrees) * Gizmo_Snap_Degrees;
				}
				_gizmoDragDegrees = degrees;
				transform.translate(_gizmoPivot);
				transform.rotate(static_cast<float>(degrees), GizmoAxisVector(_gizmoDragHandle));
				transform.translate(-_gizmoPivot);
				emit LayerRotating(_gizmoDragHandle, degrees);
			}
			else
			{
				QVector3D offset;
				if (_gizmoDragHandle == Gizmo_Center_Handle)
				{
					// In the view plane; screen y grows downward.
					const QVector3D right = _view.inverted().mapVector(QVector3D(1, 0, 0)).normalized();
					const QVector3D up = _view.inverted().mapVector(QVector3D(0, 1, 0)).normalized();
					offset = static_cast<float>(_gizmoDragScale) *
						(static_cast<float>(delta.x()) * right - static_cast<float>(delta.y()) * up);
					if (snap)
					{
						offset = QVector3D(std::round(offset.x()), std::round(offset.y()), std::round(offset.z()));
					}
				}
				else
				{
					double distance = QPointF::dotProduct(delta, _gizmoDragDirection) * _gizmoDragScale;
					if (snap)
					{
						distance = std::round(distance);
					}
					offset = static_cast<float>(distance) * GizmoAxisVector(_gizmoDragHandle);
				}
				_gizmoDragOffset = offset;
				transform.translate(offset);
				emit LayerMoving(offset.x(), -offset.y(), -offset.z());
			}
			SetActiveLayersTransform(transform);
			event->accept();
			return;
		}
		if (event->buttons() == Qt::NoButton)
		{
			// Hover highlight (mouse tracking is on only while the gizmo is shown).
			if (GizmoVisible())
			{
				UpdateProjection(width(), height());
				UpdateViewMatrix();
				const int handle = GizmoHandleAt(event->pos());
				if (handle != _gizmoHoverHandle)
				{
					_gizmoHoverHandle = handle;
					if (handle >= 0)
						setCursor(Qt::PointingHandCursor);
					else
						unsetCursor();
					update();
				}
			}
			return;
		}

		const int dx = event->x() - _lastMousePos.x();
		const int dy = event->y() - _lastMousePos.y();

		const bool middle = event->buttons() & Qt::MiddleButton;
		if (_orbitSelectorDrag || (middle && !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))))
		{
			_rotationY += dx * 0.5f;
			_rotationX = std::clamp(_rotationX + dy * 0.5f, -90.0f, 90.0f);
		}
		else if (middle && (event->modifiers() & Qt::ControlModifier))
		{
			_distance = std::clamp(_distance * std::exp(dy * 0.01f), 0.01f, 100000.0f);
		}
		else if ((middle && (event->modifiers() & Qt::ShiftModifier)) || (event->buttons() & Qt::RightButton))
		{
			UpdateViewMatrix();
			const float speed = static_cast<float>(WorldPerPixel(_center));
			_pan += speed * QVector3D(dx, -dy, 0);
		}

		_lastMousePos = event->pos();
		MarkNavigating();
		update();
		event->accept();
	}

	void PointcloudPreviewWidget::mouseReleaseEvent(QMouseEvent* event)
	{
		event->accept();
		if (_brushStrokeActive && event->button() == Qt::LeftButton)
		{
			AppendBrushSample(event->localPos());
			_brushStrokeActive = false;
			SelectBrushStroke();
			update();
			return;
		}
		if (_orbitSelectorDrag)
		{
			_orbitSelectorDrag = false;
			return;
		}
		if (_selectionBoxActive && event->button() == Qt::LeftButton)
		{
			_selectionBoxActive = false;
			_selectionBoxEnd = event->pos();
			SelectInRectangle(QRect(_selectionBoxStart, _selectionBoxEnd).normalized(), event->modifiers());
			return;
		}
		if (_gizmoDragHandle < 0 || event->button() != Qt::LeftButton)
		{
			return;
		}
		const int handle = _gizmoDragHandle;
		const double degrees = _gizmoDragDegrees;
		const QVector3D offset = _gizmoDragOffset;
		const bool rotate = _gizmoMode == GizmoMode::Rotate;
		if (rotate ? std::abs(degrees) < 1e-9 : offset.isNull())
		{
			CancelGizmoDrag();
			update();
			return;
		}
		_gizmoDragHandle = -1;
		_gizmoDragDegrees = 0.0;
		_gizmoDragOffset = QVector3D();
		update();
		if (rotate)
		{
			emit LayerRotated(_gizmoLayerId, handle, degrees);
		}
		else
		{
			emit LayerMoved(_gizmoLayerId, offset.x(), -offset.y(), -offset.z());
		}
	}

	void PointcloudPreviewWidget::wheelEvent(QWheelEvent* event)
	{
		const float delta = event->angleDelta().y() / 120.0f;
		if (_brushSelectionMode && (event->modifiers() & Qt::ControlModifier))
		{
			_brushRadius = std::clamp(_brushRadius * std::exp(delta * 0.15), 4.0, 256.0);
			if (_brushStrokeActive)
				AppendBrushSample(_brushStroke.back().position);
			update();
			event->accept();
			return;
		}
		if (_brushStrokeActive)
		{
			event->accept();
			return;
		}
		_distance = std::clamp(_distance * std::exp(-delta * 0.1f), 0.01f, 100000.0f);
		MarkNavigating();
		update();
		event->accept();
	}

	void PointcloudPreviewWidget::keyPressEvent(QKeyEvent* event)
	{
		if (_brushStrokeActive)
		{
			if (event->key() == Qt::Key_Escape)
				CancelBrushStroke();
			event->accept();
			return;
		}
		if (event->key() == Qt::Key_F && !_activeLayers.empty())
		{
			FrameSelection();
			event->accept();
			return;
		}

		if (event->key() == Qt::Key_Escape && _selectionBoxActive)
		{
			_selectionBoxActive = false;
			update();
			event->accept();
			return;
		}
		if (event->key() == Qt::Key_Escape && _gizmoDragHandle >= 0)
		{
			CancelGizmoDrag();
			update();
			event->accept();
			return;
		}
		QOpenGLWidget::keyPressEvent(event);
	}

	void PointcloudPreviewWidget::enterEvent(QEvent* event)
	{
		if (_brushSelectionMode)
		{
			_brushPosition = mapFromGlobal(QCursor::pos());
			_brushCursorVisible = true;
			update();
		}
		QOpenGLWidget::enterEvent(event);
	}

	void PointcloudPreviewWidget::leaveEvent(QEvent* event)
	{
		_brushCursorVisible = false;
		update();
		QOpenGLWidget::leaveEvent(event);
	}
} // namespace smcp
