#pragma once

// Constants and small helpers shared by the translation units that implement
// PointcloudPreviewWidget:
//
//   PointcloudPreviewWidget.cpp    scene transactions, layers, history, camera and OpenGL drawing
//   PointcloudPreviewOverlays.cpp  transform gizmo, view selector and the other painted overlays
//   PointcloudPreviewInput.cpp     mouse, wheel and keyboard: navigation, selection and gizmo drags
//
// Not part of the widget's public interface: include it only from those files.

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <QColor>
#include <QPointF>
#include <QVector3D>

namespace smcp::PreviewInternal
{
	/// Gizmo ring radius on screen, in pixels.
	constexpr double Gizmo_Radius_Pixels = 90.0;
	/// Distance from a ring, in pixels, that still grabs it.
	constexpr double Gizmo_Grab_Pixels = 8.0;
	constexpr int Gizmo_Ring_Samples = 96;
	constexpr double Gizmo_Snap_Degrees = 5.0;
	/// Pixels around the move center that grab it.
	constexpr double Gizmo_Center_Pixels = 10.0;
	/// Move handle index of the center (axes are 0..2).
	constexpr int Gizmo_Center_Handle = 3;
	/// Drags shorter than this, in pixels, count as a click (which clears the selection).
	constexpr int Selection_Click_Pixels = 3;
	/// Color of selected points.
	inline const QColor Selection_Color(255, 140, 0);
	constexpr float Field_Of_View_Degrees = 45.0f;
	/// Vertex data sent to the GPU per frame; larger clouds are uploaded over several frames.
	constexpr std::size_t Upload_Bytes_Per_Frame = 32u * 1024u * 1024u;
	/// Bytes per vertex in the position buffer (three floats) and in the color buffer (r, g, b, selected).
	constexpr int Position_Bytes = 3 * static_cast<int>(sizeof(float));
	constexpr int Color_Bytes = 4;
	/// While the camera moves, scenes with more visible points than this are drawn decimated...
	constexpr long long Navigation_Full_Detail_Points = 4000000;
	/// ...to about this many points; the full scene is drawn again once the camera rests this long.
	constexpr long long Navigation_Target_Points = 2000000;
	constexpr int Navigation_Rest_Milliseconds = 200;

	/// Cloud axes X, Y, Z in the viewer's OpenGL coordinates (x, -y, -z).
	inline QVector3D GizmoAxisVector(int axis)
	{
		return axis == 0 ? QVector3D(1, 0, 0) : axis == 1 ? QVector3D(0, -1, 0) : QVector3D(0, 0, -1);
	}

	/// Blender-like axis colors.
	inline QColor GizmoAxisColor(int axis)
	{
		return axis == 0 ? QColor(255, 51, 82) : axis == 1 ? QColor(139, 220, 0) : QColor(40, 144, 255);
	}

	inline const char* GizmoAxisName(int axis)
	{
		return axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
	}

	inline double DistanceToSegment(const QPointF& p, const QPointF& a, const QPointF& b)
	{
		const QPointF ab = b - a;
		const double lengthSquared = QPointF::dotProduct(ab, ab);
		const double t =
			lengthSquared > 0.0 ? std::clamp(QPointF::dotProduct(p - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
		const QPointF d = p - (a + t * ab);
		return std::sqrt(QPointF::dotProduct(d, d));
	}
} // namespace smcp::PreviewInternal
