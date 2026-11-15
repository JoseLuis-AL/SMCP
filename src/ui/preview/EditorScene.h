#pragma once

// The scene of the Point Cloud Editor as plain data, and the operations that turn one scene into
// the next. Nothing here draws, owns a widget or touches OpenGL, so every function can run on a
// worker thread and be tested on its own.
//
// A scene is a value. Point data and selection masks are shared and immutable: an operation that
// changes points or a selection builds new arrays and leaves the previous ones to the scenes (the
// displayed one, the history) that still refer to them.
//
// PointcloudPreviewWidget owns the current scene and its history, runs these operations as edits
// and draws the result. See docs/EDITOR_ASYNC_HISTORY.md.

#include <cstddef>
#include <memory>
#include <vector>

#include <QColor>
#include <QMatrix4x4>
#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector3D>

#include "core/PointcloudOps.h"

namespace smcp::EditorScene
{
	/// One layer of the scene.
	struct CloudState
	{
		Pointcloud::ColorCloudPtr sourceCloud;  ///< Point data, never modified in place.
		QString name, details;
		QColor color;          ///< Representative color shown in the layer list: the override, or `pointColor`.
		QColor colorOverride;  ///< Invalid means that the RGB stored in each point is used.
		QColor pointColor;     ///< Mean of the colors stored in the points (derived, like `centroid`).
		bool visible = true;
		int id = 0;            ///< Stable while the layer exists.
		// Derived from the point data when the scene is prepared for display.
		int vertexCount = 0;
		cv::Vec3d centroid{ 0, 0, 0 };
		QVector3D bbMin, bbMax;  ///< Bounding box in OpenGL coordinates (x, -y, -z).
		/// One flag per point of `sourceCloud`, or null when no point is selected. Never modified in place.
		std::shared_ptr<const std::vector<char>> selection;
		std::size_t selectedCount = 0;
	};

	struct SceneState
	{
		std::vector<CloudState> clouds;  ///< List order; index 0 has the highest drawing priority.
		std::vector<int> activeLayers;   ///< Ids of the layers selected in the panel.
	};

	struct HistoryEntry
	{
		QString label;
		SceneState state;
	};

	/// One position of a brush stroke, in widget pixels.
	struct BrushSample
	{
		QPointF position;
		double radius;
	};

	/// Selection rectangle in normalized device coordinates (x and y in [-1, 1], y up).
	struct NdcRect
	{
		float left, right, top, bottom;
	};

	/// A layer takes part in editing when it is visible and selected in the panel.
	bool IsActive(const SceneState& scene, const CloudState& cloud);

	/// Same layers in the same order with the same point data, selection, name, details, color and
	/// visibility. Point data and selections are compared by pointer (they are immutable).
	bool SameScene(const SceneState& a, const SceneState& b);

	/* Edits: each takes a scene by value and returns the next one ============================== */

	/// Selects (or with `remove`, unselects) the points of the active layers that `matrix`
	/// (projection * view, OpenGL coordinates) projects inside `rect`. Without `add` or `remove`
	/// the previous selection of every layer is replaced. Points behind others are included.
	SceneState SelectInRectangle(SceneState scene, const QMatrix4x4& matrix, const NdcRect& rect, bool add, bool remove);

	/// Adds to the selection the points of the active layers under the swept brush stroke.
	/// Throws std::runtime_error when the screen mask cannot be allocated.
	SceneState SelectBrushStroke(SceneState scene, const std::vector<BrushSample>& stroke, const QMatrix4x4& matrix,
		const QSize& viewport);

	SceneState ClearSelection(SceneState scene);

	/// Removes the selected points of the active layers; a layer left without points disappears.
	SceneState DeleteSelectedPoints(SceneState scene);

	/// Applies a rigid transform (cloud coordinates) to the points of the active layers.
	SceneState TransformActive(SceneState scene, const cv::Matx44d& transform, const QString& description);

	/// Translates every active layer but the first so that its centroid coincides with the first one's.
	SceneState AlignActiveCentroids(SceneState scene);

	/// Replaces the active layers by one layer, with id `mergedId`, holding all their points, at
	/// the position of the first of them. Does nothing with fewer than two active layers.
	SceneState MergeActive(SceneState scene, int mergedId);

	/// Removes the active layers.
	SceneState RemoveActive(SceneState scene);

	/* History ================================================================================== */

	/// Memory of the distinct point arrays and selection masks the entries keep alive.
	std::size_t HistoryBytes(const std::vector<HistoryEntry>& history);

	/// Drops the oldest entries beyond `maxUndoable` undoable commands or `byteBudget` bytes,
	/// always keeping the last two entries so the latest command can be undone.
	void TrimHistory(std::vector<HistoryEntry>& history, int maxUndoable, std::size_t byteBudget);
} // namespace smcp::EditorScene
