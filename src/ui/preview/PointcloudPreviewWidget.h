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

class QListWidget;
class QResizeEvent;

namespace smcp
{
/// <summary>
/// OpenGL viewer for several point clouds with an orbital camera (left drag: rotate,
/// middle: pan, wheel: zoom) and an overlay list to reorder or hide clouds.
/// Used by the point cloud editor (PointcloudEditorDialog).
/// </summary>
class PointcloudPreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
	Q_OBJECT

public:
	/// Interleaved GPU buffer ready to upload, built without touching the widget.
	struct PreparedPointcloud
	{
		std::vector<float> buffer;  ///< [x, y, z, r, g, b] per finite point, already in OpenGL coordinates.
		Pointcloud::ColorCloudPtr sourceCloud;  ///< Original coordinates and colors used when exporting.
		int vertexCount = 0;
		QVector3D bbMin;
		QVector3D bbMax;
		QColor color;  ///< Representative color shown in the overlay.
		QColor colorOverride;  ///< Invalid means that the RGB stored in each point is used.
		QString name;
	};

	struct ListedPointcloud
	{
		Pointcloud::ColorCloudPtr cloud;
		QString name;
		bool visible = true;
	};

	explicit PointcloudPreviewWidget(QWidget* parent = nullptr);
	~PointcloudPreviewWidget() override;

	/// Builds the GPU buffer of a cloud. Thread-safe, so large clouds can be prepared on a worker thread.
	/// With a valid `colorOverride` every point is painted in that color; otherwise its own RGB is used.
	static PreparedPointcloud PreparePointcloud(const Pointcloud::ColorCloud& cloud, const QColor& colorOverride = QColor(),
		const QString& name = QString());

	/// Adds a prepared cloud (taking ownership of its buffer) and returns its index, or -1 if it has no
	/// finite points. The first cloud centers the camera on its bounding box.
	int AddPreparedPointcloud(PreparedPointcloud&& prepared);

	/// Prepares and adds a cloud on the calling thread; returns its index (-1 if empty or with no finite points).
	int AddPointcloud(const Pointcloud::ColorCloudPtr& cloud, const QColor& colorOverride = QColor(),
		const QString& name = QString());
	/// Adds a cloud at the beginning of the overlay and rendering order.
	int PrependPointcloud(const Pointcloud::ColorCloudPtr& cloud, const QColor& colorOverride = QColor(),
		const QString& name = QString());
	std::vector<ListedPointcloud> Pointclouds() const;
	/// Changes the display color of the first list entry backed by `cloud`.
	void SetPointcloudColor(const Pointcloud::ColorCloudPtr& cloud, const QColor& colorOverride);
	/// Changes the visibility of every point cloud currently in the list.
	void SetAllPointcloudsVisible(bool visible);
	void RemovePointcloud(int index);

protected:
	void initializeGL() override;
	void resizeGL(int w, int h) override;
	void paintGL() override;

	void mousePressEvent(QMouseEvent* event) override;
	void mouseMoveEvent(QMouseEvent* event) override;
	void wheelEvent(QWheelEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	bool eventFilter(QObject* obj, QEvent* event) override;

private:
	struct CloudEntry
	{
		QOpenGLVertexArrayObject vao;
		QOpenGLBuffer vbo;
		int vertexCount = 0;
		std::vector<float> pendingBuffer;  ///< [x, y, z, r, g, b] per vertex, pending upload to the GPU.
		Pointcloud::ColorCloudPtr sourceCloud;
		bool pendingUpload = false;
		bool visible = true;
		QColor color;  ///< Representative color shown in the overlay.
		QColor colorOverride;  ///< Invalid means that the RGB stored in each point is used.
		QString name;

		CloudEntry() : vbo(QOpenGLBuffer::VertexBuffer) {}
	};

	void BuildShaders();
	void UpdateViewMatrix();
	void UpdateProjection(int w, int h);

	void RebuildCloudList();
	void RepositionOverlay();
	void OnCloudListReordered();
	void ChoosePointcloudColor(int index);
	void RenamePointcloud(int index, const QString& name);
	void SetPointcloudColor(int index, const QColor& colorOverride);
	void SetPointcloudVisible(int index, bool visible);

	void UploadEntry(CloudEntry& entry);
	void DestroyEntry(CloudEntry& entry);

	QOpenGLShaderProgram _shaderProgram;
	std::vector<std::unique_ptr<CloudEntry>> _clouds;  ///< Drawn in order: index 0 first.
	QListWidget* _cloudListWidget = nullptr;
	int _cloudCounter = 0;

	QMatrix4x4 _projection;
	QMatrix4x4 _view;
	QVector3D _center;
	float _distance = 500.0f;
	float _rotationX = 0.0f;
	float _rotationY = 0.0f;
	QPoint _lastMousePos;
};
} // namespace smcp
