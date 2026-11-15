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

#include <memory>
#include <functional>
#include <optional>
#include <vector>

#include <QColor>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <QVector3D>

#include "core/PointcloudOps.h"
#include "ui/preview/EditorScene.h"

class QPainter;
class QTimer;

namespace smcp
{
	/// <summary>
	/// OpenGL viewer for several point clouds with an orbital camera (middle drag: orbit,
	/// Shift+middle: pan, wheel or Ctrl+middle: zoom). It owns the list of clouds (layers); PointcloudLayersPanel
	/// lists them next to the viewer and edits them through the public layer API.
	/// Used by the point cloud editor (PointcloudEditorDialog).
	/// </summary>
	class PointcloudPreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions
	{
		Q_OBJECT

	public:
		/// Interleaved GPU buffer ready to upload, built without touching the widget.
		/// Vertex data of one cloud, built on a worker without touching the widget. Positions and
		/// colors go to separate GPU buffers because they change at different times: selecting points
		/// only changes colors, so positions (three quarters of the data) are neither rebuilt nor sent
		/// again. A layer's color override is not in either buffer; the shader applies it.
		struct PreparedPointcloud
		{
			std::vector<float> positions;       ///< [x, y, z] per finite point, in OpenGL coordinates (x, -y, -z).
			std::vector<unsigned char> colors;  ///< [r, g, b, selected] per finite point; `selected` is 0 or 255.
			bool reusePositions = false;        ///< Only `colors` was built: the points did not change.
			int vertexCount = 0;
			cv::Vec3d centroid{ 0, 0, 0 };
			QVector3D bbMin;
			QVector3D bbMax;
			QColor pointColor;  ///< Mean of the colors stored in the points.
		};

		struct ListedPointcloud
		{
			Pointcloud::ColorCloudPtr cloud;
			QString name;
			bool visible = true;
			int id = -1; ///< LayerInfo::id of the layer, or -1 when the cloud is not a listed layer.
		};

		/// What the layer panel shows for one entry; index-aligned with the drawing order.
		struct LayerInfo
		{
			QString name;
			QString details;
			QColor color;
			QColor colorOverride;
			int pointCount = 0;
			bool visible = true;
			int id = 0; ///< Stable while the layer exists, so the panel keeps its selection across changes.
		};

		// The scene itself is plain data defined in EditorScene.h, together with the operations on it.
		using CloudState = EditorScene::CloudState;
		using SceneState = EditorScene::SceneState;
		using HistoryEntry = EditorScene::HistoryEntry;
		struct ProjectState
		{
			SceneState scene;
			std::vector<HistoryEntry> history;
			int historyPosition = 0;
			QVector3D center, pan;
			bool centerInitialized = false;
			float distance = 500, rotationX = 0, rotationY = 0;
		};
		/* Scene, commands and history -------------------------------------------------------------
		   The scene (SceneState) is a value: the list of clouds with their immutable point data and
		   selection masks, plus the active layers. Nothing edits it in place. Every change is an
		   *edit*: a function from the current scene to the next one. An edit runs on a worker thread,
		   which also prepares the GPU buffers of the clouds it changed; the result is then installed
		   on the GUI thread and recorded as one history entry. See docs/EDITOR_ASYNC_HISTORY.md. */

		/// Copy of the current scene (the open command's draft, if any).
		SceneState CaptureScene() const;
		ProjectState CaptureProject() const;
		void RestoreProject(ProjectState project);

		/// Groups the changes made until EndCommand() into one history entry. Between the two calls
		/// the changes apply to a draft and nothing is published. Throws std::logic_error when a
		/// command is already open or an edit is still being published.
		void BeginCommand(const QString& label);
		/// Publishes the draft as one edit (nothing happens if it left the scene unchanged).
		void EndCommand();
		/// Discards the draft.
		void AbortCommand();

		/// Submits an edit. It starts at once when the viewer is idle; otherwise it waits in a queue
		/// and runs, in order, on the scene left by the edits before it. An edit submitted while a
		/// command is open (BeginCommand) is ignored: commands change the draft through the layer API.
		void EditScene(const QString& label, std::function<SceneState(SceneState)> work);

		void Undo();
		void Redo();
		void JumpToHistory(int position);
		QStringList HistoryLabels() const;
		int HistoryPosition() const { return _historyPosition; }
		/// True while an edit is running or waiting. BusyChanged reports the transitions.
		bool IsWorking() const { return _working; }

		/// Undo is limited both by count (Max_History_Entries) and by the memory of the point data
		/// and selection masks its entries keep alive. Older entries are dropped beyond the budget,
		/// always keeping at least one step to undo.
		void SetHistoryMemoryBudget(std::size_t bytes) { _historyByteBudget = bytes; }
		static constexpr int Max_History_Entries = 50;
		static constexpr std::size_t Default_History_Bytes = std::size_t(2) * 1024 * 1024 * 1024;
		void DeleteSelectedPoints();
		void TransformSelected(const cv::Matx44d& transform, const QString& description);
		void AlignSelectedCentroids();
		void MergeSelectedPointclouds();
		void RemoveSelectedPointclouds();
		void FrameSelection();
		cv::Vec3d NavigationCenter() const { return cv::Vec3d(_center.x(), -_center.y(), -_center.z()); }

		explicit PointcloudPreviewWidget(QWidget* parent = nullptr);
		~PointcloudPreviewWidget() override;

		/// Builds the vertex data of a cloud. Thread-safe, so large clouds are prepared on a worker.
		/// `selection` (one flag per point of `cloud`, or null) marks the points drawn in the selection
		/// color. With `colorsOnly` the positions and the derived geometry are not computed.
		static PreparedPointcloud PreparePointcloud(const Pointcloud::ColorCloud& cloud,
			const std::vector<char>* selection = nullptr, bool colorsOnly = false);

		/* Layers ----------------------------------------------------------------------------------
		   A layer is identified by its id (LayerInfo::id), which never changes while it exists.
		   Positions in the list change whenever a layer is added, removed or reordered, so code that
		   keeps a reference to a layer across edits must keep the id. The index-based calls below
		   serve the layer panel, whose rows are positions. */

		/// Adds a cloud at the end of the list. Returns the id of the new layer, or -1 when the
		/// cloud is null or empty.
		int AddPointcloud(const Pointcloud::ColorCloudPtr& cloud, const QColor& colorOverride = QColor(),
			const QString& name = QString(), const QString& details = QString());
		/// Adds a cloud at the beginning of the list (highest drawing priority). Returns its id or -1.
		int PrependPointcloud(const Pointcloud::ColorCloudPtr& cloud, const QColor& colorOverride = QColor(),
			const QString& name = QString(), const QString& details = QString());
		std::vector<ListedPointcloud> Pointclouds() const;
		std::vector<LayerInfo> Layers() const;
		/// Description of the layer with id `id`; false when it does not exist. `cloud`, if given,
		/// receives its point data.
		bool FindLayer(int id, LayerInfo& info, Pointcloud::ColorCloudPtr* cloud = nullptr) const;
		/// Changes the display color of the layer with id `id` (an invalid color restores the
		/// colors stored in the points).
		void SetLayerColor(int id, const QColor& colorOverride);
		void SetPointcloudColor(int index, const QColor& colorOverride);
		/// Changes the visibility of every point cloud currently in the list.
		void SetAllPointcloudsVisible(bool visible);
		void SetPointcloudVisible(int index, bool visible);
		/// Ignores empty names (after trimming).
		void RenamePointcloud(int index, const QString& name);
		/// Display-only transform of the active visible layers, in OpenGL coordinates (see
		/// PreparePointcloud): the preview of a gizmo drag. Point data is unchanged; installing a new
		/// scene resets it to the identity.
		void SetActiveLayersTransform(const QMatrix4x4& transform);
		/// Replaces the points of a layer, keeping its name, color, visibility, and position in the list.
		void ReplacePointcloud(int index, const Pointcloud::ColorCloudPtr& cloud, const QString& details);
		Pointcloud::ColorCloudPtr Cloud(int index) const;
		/// Index of the layer with LayerInfo::id `id`, or -1.
		int IndexOfLayer(int id) const;
		enum class GizmoMode
		{
			None,
			Rotate, ///< X red, Y green, Z blue rings.
			Move	///< Arrows along the same axes, plus a center handle that moves in the view plane.
		};
		/// Shows the transform gizmo on the centroid of the layer with LayerInfo::id `layerId`
		/// (GizmoMode::None or -1 hides it). Dragging a handle previews the change on that layer
		/// (Ctrl snaps to 5 degrees or to whole units; Esc or the right button cancels the drag), and
		/// releasing the button emits LayerRotated or LayerMoved. Dragging elsewhere orbits the camera.
		void SetGizmo(GizmoMode mode, int layerId);
		/// Restricts editing to visible selected IDs; navigation keeps the first loaded cloud's initial center.
		void SetActiveLayers(const std::vector<int>& ids);
		cv::Vec3d SelectionCentroid() const;

		/// Box selection: while enabled, a left-button drag draws a rectangle and selects the points of the
		/// selected visible layers inside it, hidden ones behind others included (Shift adds, Ctrl removes, a click
		/// without dragging clears; Esc cancels the rectangle). Selected points are drawn in orange. The
		/// selection is kept when the mode is turned off; replacing a layer's points, hiding it, or removing
		/// it clears its part.
		void SetSelectionMode(bool enabled);
		void SetBrushSelectionMode(bool enabled);
		double BrushRadius() const { return _brushRadius; }
		void ClearSelection();
		std::size_t SelectedPointCount() const;
		/// Number of layers with at least one selected point.
		int SelectedLayerCount() const;
		/// One flag per point of the layer's cloud, or an empty vector when none of its points is selected.
		std::vector<char> Selection(int index) const;
		/// Reorders the layers: `order[i]` is the current index of the layer that moves to position i.
		void ReorderPointclouds(const std::vector<int>& order);
		void RemovePointcloud(int index);

	signals:
		/// Emitted after any change to the layers (added, removed, reordered, renamed, recolored, shown or hidden).
		void PointcloudsChanged();
		void BusyChanged(bool busy);
		void HistoryChanged();
		void ActiveLayersRestored();
		void ProjectRestored();
		void OperationFailed(const QString& error);
		/// While a gizmo ring is dragged: axis 0, 1, or 2 (X, Y, Z in cloud coordinates) and the angle so far.
		void LayerRotating(int axis, double degrees);
		/// The drag ended with a non-zero angle. The layer keeps the previewed transform until its cloud is
		/// replaced (ReplacePointcloud), so the receiver bakes the rotation around Centroid() of its points.
		void LayerRotated(int layerId, int axis, double degrees);
		/// While a move handle is dragged: the offset so far, in cloud coordinates.
		void LayerMoving(double dx, double dy, double dz);
		/// The drag ended with a non-zero offset (cloud coordinates); the receiver bakes the translation.
		void LayerMoved(int layerId, double dx, double dy, double dz);
		/// The drag was cancelled (Esc or right button) or ended without change; the preview was reset.
		void GizmoDragCanceled();
		/// The selected points changed through a box selection or ClearSelection().
		void SelectionChanged();

	protected:
		void initializeGL() override;
		void resizeGL(int w, int h) override;
		void paintGL() override;

		void mousePressEvent(QMouseEvent* event) override;
		void mouseMoveEvent(QMouseEvent* event) override;
		void mouseReleaseEvent(QMouseEvent* event) override;
		void wheelEvent(QWheelEvent* event) override;
		void keyPressEvent(QKeyEvent* event) override;
		void enterEvent(QEvent* event) override;
		void leaveEvent(QEvent* event) override;

	private:
		/// A layer as it is drawn: its state plus its GPU buffers.
		struct CloudEntry : CloudState
		{
			QOpenGLVertexArrayObject vao;
			/// Positions are shared: an entry that only differs in colors (a new selection) points to
			/// the same buffer as the entry it replaces. The buffer is destroyed with its last owner.
			std::shared_ptr<QOpenGLBuffer> positions;
			QOpenGLBuffer colors;
			// Data still to be sent to the GPU, a few megabytes per frame (see UploadEntry).
			std::vector<float> pendingPositions;
			std::vector<unsigned char> pendingColors;
			std::size_t uploadedPositionBytes = 0;
			std::size_t uploadedColorBytes = 0;
			bool pendingUpload = false;
			QMatrix4x4 transform;
			/// The entry this one replaces, drawn until this one's buffers are complete so an edit
			/// (selection, new points) never makes the cloud blink.
			std::unique_ptr<CloudEntry> previous;
			CloudEntry() : colors(QOpenGLBuffer::VertexBuffer) {}
		};
		struct PreparedScene
		{
			SceneState state;
			std::vector<std::optional<PreparedPointcloud>> buffers;
		};
		/// An edit waiting for the one in progress. `restorePosition` >= 0 marks a history jump,
		/// which installs a stored scene without adding an entry.
		struct PendingEdit
		{
			QString label;
			std::function<SceneState(SceneState)> work;
			int restorePosition = -1;
		};

		std::vector<HistoryEntry> _history;
		int _historyPosition = 0;
		std::size_t _historyByteBudget = Default_History_Bytes;
		std::unique_ptr<SceneState> _draft;  ///< Scene of the command opened with BeginCommand().
		QString _commandLabel;
		bool _working = false;     ///< An edit is running on the worker or waiting in _pendingEdits.
		bool _installing = false;  ///< InstallScene() is notifying; edits its listeners request are ignored.
		std::vector<PendingEdit> _pendingEdits;

		/// Applies `edit` to the open command's draft, or submits it as an edit of its own.
		void Change(const QString& label, const std::function<void(SceneState&)>& edit);
		void Submit(const QString& label, std::function<SceneState(SceneState)> work, int restorePosition = -1);
		/// Runs one edit on the worker and installs its result (GUI thread on both ends).
		void StartEdit(PendingEdit edit);
		/// Called when an edit ends, successfully or not: starts the next queued edit or goes idle.
		void FinishEdit();
		/// Worker side: computes the derived data of `state` and the GPU buffers of the clouds that
		/// differ from `before`. Touches no widget state.
		static PreparedScene PrepareScene(SceneState state, const SceneState& before);
		/// GUI side: replaces the displayed scene, reusing the GPU entries of unchanged clouds.
		void InstallScene(PreparedScene& prepared);
		void ReportOperationFailure(std::exception_ptr error);
		/// Clouds of the current scene (the draft of an open command, otherwise the displayed one),
		/// visited in list order without copying the scene.
		template <typename Visit> void VisitClouds(Visit visit) const
		{
			if (_draft)
			{
				for (const CloudState& cloud : _draft->clouds)
					visit(cloud);
				return;
			}
			for (const auto& cloud : _clouds)
				visit(static_cast<const CloudState&>(*cloud));
		}

		void BuildShaders();
		void UpdateViewMatrix();
		void UpdateProjection(int w, int h);
		/// Repaints and notifies the layer panel.
		void LayersChanged();
		/// Applies the selection rectangle (widget pixels) to the visible layers.
		void SelectInRectangle(const QRect& rect, Qt::KeyboardModifiers modifiers);

		/// One gizmo ring projected to the widget: points, and whether each one faces the camera.
		struct GizmoRing
		{
			std::vector<QPointF> points;
			std::vector<bool> front;
		};
		/// Recomputes the pivot (layer centroid, OpenGL coordinates); hides the gizmo if the layer is gone.
		void UpdateGizmoPivot();
		bool GizmoVisible() const;
		/// Projects an OpenGL-space point to widget pixels; false if it is behind the camera.
		bool Project(const QVector3D& point, QPointF& screen) const;
		/// World units per pixel at `point`'s depth, so overlays keep a constant size on screen.
		double WorldPerPixel(const QVector3D& point) const;
		/// Ring of axis 0..2 sampled counterclockwise around its axis (the positive rotation direction).
		GizmoRing ProjectGizmoRing(int axis) const;
		/// Move arrow of axis 0..2 on screen; false when it points at the camera (too short to grab).
		bool ProjectGizmoArrow(int axis, QPointF& start, QPointF& tip) const;
		/// Handle under `pos`: 0..2 for an axis (ring or arrow), 3 for the move center, -1 for none.
		int GizmoHandleAt(const QPoint& pos) const;
		/// View selector, transform gizmo, and selection rectangle.
		void PaintOverlays(QPainter& painter);
		void PaintViewSelector(QPainter& painter);
		QPointF ViewAxisPosition(int axis, int sign) const;
		void PaintGizmo(QPainter& painter);
		void PaintSelectionBox(QPainter& painter);
		void CancelGizmoDrag();

		void UploadEntry(CloudEntry& entry, std::size_t& budget);
		void DestroyEntry(CloudEntry& entry);
		/// Called by orbit, pan and zoom: very large scenes are drawn with fewer points until the
		/// camera has been still for a moment (see NavigationStep).
		void MarkNavigating();
		/// 1 to draw every point; N to draw one point of every N while the camera moves.
		int NavigationStep() const;
		bool _navigating = false;
		QTimer* _navigationTimer = nullptr;

		QWidget* _overlay = nullptr;
		QOpenGLShaderProgram _shaderProgram;
		std::vector<std::unique_ptr<CloudEntry>> _clouds; ///< Panel order; index 0 has highest drawing priority.
		int _cloudCounter = 0;

		QMatrix4x4 _projection;
		QMatrix4x4 _view;
		QVector3D _center;
		bool _centerInitialized = false;
		QVector3D _pan;
		float _distance = 500.0f;
		float _rotationX = 0.0f;
		float _rotationY = 0.0f;
		QPoint _lastMousePos;

		std::vector<int> _activeLayers;
		bool _orbitSelectorDrag = false;
		bool _selectionMode = false;
		using BrushSample = EditorScene::BrushSample;
		void CancelBrushStroke();
		void AppendBrushSample(const QPointF& position);
		void SelectBrushStroke();
		bool _brushSelectionMode = false;
		bool _brushStrokeActive = false;
		bool _brushCursorVisible = false;
		double _brushRadius = 24.0;
		QPointF _brushPosition;
		std::vector<BrushSample> _brushStroke;
		QMatrix4x4 _brushMatrix;
		QSize _brushViewport;
		bool _selectionBoxActive = false;
		QPoint _selectionBoxStart;
		QPoint _selectionBoxEnd;
		GizmoMode _gizmoMode = GizmoMode::None;
		int _gizmoLayerId = -1;
		QVector3D _gizmoPivot; ///< OpenGL coordinates.
		int _gizmoHoverHandle = -1;
		int _gizmoDragHandle = -1;
		QPoint _gizmoDragStart;
		/// Rotate: unit screen direction of a positive rotation at the grabbed point.
		/// Move along an axis: unit screen direction of the axis.
		QPointF _gizmoDragDirection;
		/// Move: world units per pixel (along the axis, or in the view plane for the center handle).
		double _gizmoDragScale = 0.0;
		double _gizmoDragDegrees = 0.0;
		QVector3D _gizmoDragOffset; ///< Move offset so far, OpenGL coordinates.
	};
} // namespace smcp
