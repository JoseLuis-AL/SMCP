#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QMatrix4x4>
#include <QVector3D>
#include <QPoint>

namespace SMCP
{
	class PointcloudWidget : public QOpenGLWidget, protected QOpenGLFunctions
	{
		Q_OBJECT

	public:
		explicit PointcloudWidget(QWidget* parent = nullptr);
		~PointcloudWidget() override;

		/// Reads APP->pointcloud and uploads valid points to the GPU.
		void loadPointcloud();

	protected:
		void initializeGL() override;
		void resizeGL(int w, int h) override;
		void paintGL() override;

		void mousePressEvent(QMouseEvent* event) override;
		void mouseMoveEvent(QMouseEvent* event) override;
		void wheelEvent(QWheelEvent* event) override;

	private:
		void buildShaders();
		void updateViewMatrix();

		// Shader program
		QOpenGLShaderProgram _shaderProgram;

		// Vertex buffer: interleaved [x, y, z, r, g, b] per vertex
		QOpenGLBuffer _vbo;
		int _vertexCount;

		// Matrices
		QMatrix4x4 _projection;
		QMatrix4x4 _view;

		// Orbital camera
		QVector3D _center;     // look-at target (centroid of the pointcloud)
		float _distance;       // distance from target
		float _rotationX;      // pitch in degrees
		float _rotationY;      // yaw in degrees

		// Mouse tracking
		QPoint _lastMousePos;
	};
}
