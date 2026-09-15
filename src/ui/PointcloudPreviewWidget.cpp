#include "ui/PointcloudPreviewWidget.h"

#include <algorithm>
#include <limits>

#include <QDebug>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QWheelEvent>

namespace smcp
{

// GLSL 120 (OpenGL 2.1), igual que PointcloudWidget.
static const char* vertexShaderSource =
"#version 120\n"
"attribute vec3 a_position;\n"
"attribute vec3 a_color;\n"
"uniform mat4 u_mvp;\n"
"varying vec3 v_color;\n"
"void main() {\n"
"    gl_Position  = u_mvp * vec4(a_position, 1.0);\n"
"    gl_PointSize = 2.0;\n"
"    v_color      = a_color;\n"
"}\n";

static const char* fragmentShaderSource =
"#version 120\n"
"varying vec3 v_color;\n"
"void main() {\n"
"    gl_FragColor = vec4(v_color, 1.0);\n"
"}\n";

/* Construccion ============================================================================ */

PointcloudPreviewWidget::PointcloudPreviewWidget(QWidget* parent)
	: QOpenGLWidget(parent)
{
	setMinimumSize(320, 240);
	setFocusPolicy(Qt::StrongFocus);

	// Lista superpuesta de nubes (estilo en resources/theme/smcp.qss, #pointcloud_overlay_list).
	_cloudListWidget = new QListWidget(this);
	_cloudListWidget->setObjectName("pointcloud_overlay_list");
	_cloudListWidget->setDragDropMode(QAbstractItemView::InternalMove);
	_cloudListWidget->setDefaultDropAction(Qt::MoveAction);
	_cloudListWidget->setFixedWidth(230);
	_cloudListWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	_cloudListWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	_cloudListWidget->hide();
	_cloudListWidget->installEventFilter(this);
}

PointcloudPreviewWidget::~PointcloudPreviewWidget()
{
	makeCurrent();
	for (auto& entry : _clouds)
	{
		destroyEntry(*entry);
	}
	_clouds.clear();
	doneCurrent();
}

/* OpenGL ================================================================================== */

void PointcloudPreviewWidget::initializeGL()
{
	initializeOpenGLFunctions();
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnable(GL_DEPTH_TEST);
	buildShaders();
}

void PointcloudPreviewWidget::buildShaders()
{
	if (!_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShaderSource))
	{
		qWarning() << "PointcloudPreviewWidget: vertex shader error:" << _shaderProgram.log();
	}
	if (!_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShaderSource))
	{
		qWarning() << "PointcloudPreviewWidget: fragment shader error:" << _shaderProgram.log();
	}
	_shaderProgram.bindAttributeLocation("a_position", 0);
	_shaderProgram.bindAttributeLocation("a_color", 1);
	if (!_shaderProgram.link())
	{
		qWarning() << "PointcloudPreviewWidget: shader link error:" << _shaderProgram.log();
	}
}

void PointcloudPreviewWidget::updateProjection(int w, int h)
{
	const float aspect = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
	const float nearPlane = std::max(0.01f, _distance * 0.001f);
	const float farPlane = _distance * 10.0f;
	_projection.setToIdentity();
	_projection.perspective(45.0f, aspect, nearPlane, farPlane);
}

void PointcloudPreviewWidget::resizeGL(int w, int h)
{
	glViewport(0, 0, w, std::max(1, h));
	updateProjection(w, std::max(1, h));
}

void PointcloudPreviewWidget::updateViewMatrix()
{
	_view.setToIdentity();
	_view.translate(0.0f, 0.0f, -_distance);
	_view.rotate(_rotationX, 1.0f, 0.0f, 0.0f);
	_view.rotate(_rotationY, 0.0f, 1.0f, 0.0f);
	_view.translate(-_center);
}

void PointcloudPreviewWidget::paintGL()
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	if (_clouds.empty())
	{
		return;
	}

	for (auto& entry : _clouds)
	{
		if (entry->pendingUpload)
		{
			uploadEntry(*entry);
		}
	}

	updateProjection(width(), height());
	updateViewMatrix();

	const QMatrix4x4 mvp = _projection * _view;
	const int stride = 6 * static_cast<int>(sizeof(float));

	_shaderProgram.bind();
	_shaderProgram.setUniformValue("u_mvp", mvp);

	for (auto& entry : _clouds)
	{
		if (entry->vertexCount <= 0 || !entry->vao.isCreated())
		{
			continue;
		}
		entry->vao.bind();
		entry->vbo.bind();

		_shaderProgram.enableAttributeArray(0);
		_shaderProgram.setAttributeBuffer(0, GL_FLOAT, 0, 3, stride);
		_shaderProgram.enableAttributeArray(1);
		_shaderProgram.setAttributeBuffer(1, GL_FLOAT, 3 * static_cast<int>(sizeof(float)), 3, stride);

		glPointSize(2.0f);
		glDrawArrays(GL_POINTS, 0, entry->vertexCount);

		_shaderProgram.disableAttributeArray(0);
		_shaderProgram.disableAttributeArray(1);
		entry->vbo.release();
		entry->vao.release();
	}

	_shaderProgram.release();
}

/* Nubes =================================================================================== */

int PointcloudPreviewWidget::addPointcloud(const pointcloud::ColorCloudPtr& cloud, const QColor& color_override,
	const QString& name)
{
	if (!cloud || cloud->empty())
	{
		return -1;
	}

	// Primera pasada: puntos validos y caja envolvente (en coordenadas OpenGL).
	int validCount = 0;
	QVector3D bbMin(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
	QVector3D bbMax(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());

	for (const auto& pt : *cloud)
	{
		if (!pointcloud::is_finite(pt))
		{
			continue;
		}
		++validCount;
		// Giro de 180 grados alrededor de X (OpenCV -> OpenGL): X, -Y, -Z.
		const QVector3D p(pt.x, -pt.y, -pt.z);
		bbMin = QVector3D(std::min(p.x(), bbMin.x()), std::min(p.y(), bbMin.y()), std::min(p.z(), bbMin.z()));
		bbMax = QVector3D(std::max(p.x(), bbMax.x()), std::max(p.y(), bbMax.y()), std::max(p.z(), bbMax.z()));
	}
	if (validCount == 0)
	{
		return -1;
	}

	// Segunda pasada: buffer intercalado [x, y, z, r, g, b].
	auto entry = std::make_unique<CloudEntry>();
	entry->pendingBuffer.reserve(static_cast<std::size_t>(validCount) * 6);

	const bool useOverride = color_override.isValid();
	const float cr = static_cast<float>(color_override.redF());
	const float cg = static_cast<float>(color_override.greenF());
	const float cb = static_cast<float>(color_override.blueF());

	for (const auto& pt : *cloud)
	{
		if (!pointcloud::is_finite(pt))
		{
			continue;
		}
		entry->pendingBuffer.push_back(pt.x);
		entry->pendingBuffer.push_back(-pt.y);
		entry->pendingBuffer.push_back(-pt.z);
		if (useOverride)
		{
			entry->pendingBuffer.push_back(cr);
			entry->pendingBuffer.push_back(cg);
			entry->pendingBuffer.push_back(cb);
		}
		else
		{
			entry->pendingBuffer.push_back(pt.r / 255.0f);
			entry->pendingBuffer.push_back(pt.g / 255.0f);
			entry->pendingBuffer.push_back(pt.b / 255.0f);
		}
	}
	entry->vertexCount = validCount;
	entry->pendingUpload = true;

	if (_clouds.empty())
	{
		_center = (bbMin + bbMax) * 0.5f;
		_distance = std::max(1.0f, (bbMax - bbMin).length() * 1.5f);
		_rotationX = 0.0f;
		_rotationY = 0.0f;
	}

	const int index = static_cast<int>(_clouds.size());
	entry->name = name.isEmpty() ? QString("Cloud %1").arg(_cloudCounter) : name;
	++_cloudCounter;
	_clouds.push_back(std::move(entry));

	rebuildCloudList();
	update();
	return index;
}

void PointcloudPreviewWidget::removePointcloud(int index)
{
	if (index < 0 || index >= static_cast<int>(_clouds.size()))
	{
		return;
	}
	makeCurrent();
	destroyEntry(*_clouds[index]);
	doneCurrent();
	_clouds.erase(_clouds.begin() + index);

	rebuildCloudList();
	update();
}

void PointcloudPreviewWidget::clearPointclouds()
{
	if (_clouds.empty())
	{
		return;
	}
	makeCurrent();
	for (auto& entry : _clouds)
	{
		destroyEntry(*entry);
	}
	doneCurrent();
	_clouds.clear();

	rebuildCloudList();
	update();
}

/* Lista superpuesta ======================================================================= */

void PointcloudPreviewWidget::rebuildCloudList()
{
	_cloudListWidget->clear();
	if (_clouds.empty())
	{
		_cloudListWidget->hide();
		return;
	}

	for (int i = 0; i < static_cast<int>(_clouds.size()); ++i)
	{
		auto* item = new QListWidgetItem();
		item->setData(Qt::UserRole, i);

		auto* row = new QWidget();
		auto* layout = new QHBoxLayout(row);
		layout->setContentsMargins(8, 2, 4, 2);
		layout->setSpacing(4);

		auto* label = new QLabel(QString("#%1  %2  (%3 pts)").arg(i + 1).arg(_clouds[i]->name).arg(_clouds[i]->vertexCount), row);
		label->setObjectName("pointcloud_overlay_label");

		auto* deleteButton = new QPushButton(QString::fromUtf8("\xC3\x97"), row);
		deleteButton->setObjectName("pointcloud_overlay_delete");
		deleteButton->setFixedSize(22, 22);
		deleteButton->setCursor(Qt::PointingHandCursor);
		connect(deleteButton, &QPushButton::clicked, this, [this, i]() { removePointcloud(i); });

		layout->addWidget(label, 1);
		layout->addWidget(deleteButton, 0);

		item->setSizeHint(QSize(0, 30));
		_cloudListWidget->addItem(item);
		_cloudListWidget->setItemWidget(item, row);
	}

	_cloudListWidget->show();
	repositionOverlay();
}

void PointcloudPreviewWidget::repositionOverlay()
{
	if (!_cloudListWidget || _cloudListWidget->isHidden())
	{
		return;
	}
	const int margin = 10;
	const int itemHeight = 30;
	const int idealHeight = _cloudListWidget->count() * itemHeight + 8;
	const int maxHeight = height() / 2;
	const int h = std::min(idealHeight, std::max(maxHeight, itemHeight + 8));
	const int w = _cloudListWidget->width();

	_cloudListWidget->setGeometry(width() - w - margin, margin, w, h);
	_cloudListWidget->raise();
}

void PointcloudPreviewWidget::onCloudListReordered()
{
	const int count = _cloudListWidget->count();
	std::vector<std::unique_ptr<CloudEntry>> reordered;
	reordered.reserve(count);

	for (int i = 0; i < count; ++i)
	{
		const int oldIndex = _cloudListWidget->item(i)->data(Qt::UserRole).toInt();
		if (oldIndex >= 0 && oldIndex < static_cast<int>(_clouds.size()) && _clouds[oldIndex])
		{
			reordered.push_back(std::move(_clouds[oldIndex]));
		}
	}
	_clouds = std::move(reordered);

	rebuildCloudList();
	update();
}

bool PointcloudPreviewWidget::eventFilter(QObject* obj, QEvent* event)
{
	if (obj == _cloudListWidget && event->type() == QEvent::Drop)
	{
		QTimer::singleShot(0, this, [this]() { onCloudListReordered(); });
	}
	return QOpenGLWidget::eventFilter(obj, event);
}

void PointcloudPreviewWidget::resizeEvent(QResizeEvent* event)
{
	QOpenGLWidget::resizeEvent(event);
	repositionOverlay();
}

/* GPU ===================================================================================== */

void PointcloudPreviewWidget::uploadEntry(CloudEntry& entry)
{
	if (!entry.vao.isCreated())
	{
		entry.vao.create();
	}
	if (!entry.vbo.isCreated())
	{
		entry.vbo.create();
	}
	entry.vbo.bind();
	entry.vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
	entry.vbo.allocate(entry.pendingBuffer.data(), static_cast<int>(entry.pendingBuffer.size() * sizeof(float)));
	entry.vbo.release();

	entry.pendingBuffer.clear();
	entry.pendingBuffer.shrink_to_fit();
	entry.pendingUpload = false;
}

void PointcloudPreviewWidget::destroyEntry(CloudEntry& entry)
{
	if (entry.vao.isCreated())
	{
		entry.vao.destroy();
	}
	if (entry.vbo.isCreated())
	{
		entry.vbo.destroy();
	}
}

/* Raton =================================================================================== */

void PointcloudPreviewWidget::mousePressEvent(QMouseEvent* event)
{
	_lastMousePos = event->pos();
	event->accept();
}

void PointcloudPreviewWidget::mouseMoveEvent(QMouseEvent* event)
{
	const int dx = event->x() - _lastMousePos.x();
	const int dy = event->y() - _lastMousePos.y();

	if (event->buttons() & Qt::LeftButton)
	{
		_rotationY += dx * 0.5f;
		_rotationX = std::clamp(_rotationX + dy * 0.5f, -90.0f, 90.0f);
	}
	else if (event->buttons() & Qt::MiddleButton)
	{
		const float panSpeed = _distance * 0.001f;
		QMatrix4x4 rot;
		rot.rotate(_rotationX, 1.0f, 0.0f, 0.0f);
		rot.rotate(_rotationY, 0.0f, 1.0f, 0.0f);
		const QVector3D right(rot(0, 0), rot(1, 0), rot(2, 0));
		const QVector3D up(rot(0, 1), rot(1, 1), rot(2, 1));
		_center -= right * (dx * panSpeed);
		_center += up * (dy * panSpeed);
	}

	_lastMousePos = event->pos();
	update();
	event->accept();
}

void PointcloudPreviewWidget::wheelEvent(QWheelEvent* event)
{
	const float delta = event->angleDelta().y() / 120.0f;
	_distance = std::clamp(_distance * (1.0f - delta * 0.1f), 1.0f, 100000.0f);
	update();
	event->accept();
}

} // namespace smcp
