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

#include "core/pointcloud_ops.h"

class QListWidget;
class QResizeEvent;

namespace smcp
{
/// <summary>
/// Visor OpenGL de varias nubes de puntos con camara orbital (arrastre izquierdo: rotar,
/// central: desplazar, rueda: zoom) y una lista superpuesta para reordenar o quitar nubes.
/// Lo usa el editor de nubes de puntos (PointcloudEditorDialog).
/// </summary>
class PointcloudPreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
	Q_OBJECT

public:
	explicit PointcloudPreviewWidget(QWidget* parent = nullptr);
	~PointcloudPreviewWidget() override;

	/// Anade una nube y devuelve su indice (-1 si esta vacia o no tiene puntos finitos).
	/// Con `color_override` valido todos los puntos se pintan de ese color; si no, con su RGB.
	/// La primera nube centra la camara en su caja envolvente.
	int addPointcloud(const pointcloud::ColorCloudPtr& cloud, const QColor& color_override = QColor(),
		const QString& name = QString());
	void removePointcloud(int index);
	void clearPointclouds();

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
		std::vector<float> pendingBuffer;  ///< [x, y, z, r, g, b] por vertice, pendiente de subir a la GPU.
		bool pendingUpload = false;
		QString name;

		CloudEntry() : vbo(QOpenGLBuffer::VertexBuffer) {}
	};

	void buildShaders();
	void updateViewMatrix();
	void updateProjection(int w, int h);

	void rebuildCloudList();
	void repositionOverlay();
	void onCloudListReordered();

	void uploadEntry(CloudEntry& entry);
	void destroyEntry(CloudEntry& entry);

	QOpenGLShaderProgram _shaderProgram;
	std::vector<std::unique_ptr<CloudEntry>> _clouds;  ///< Se dibujan en orden: indice 0 primero.
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
