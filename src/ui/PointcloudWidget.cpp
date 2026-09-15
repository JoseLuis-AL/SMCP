#include "ui/PointcloudWidget.h"

#include <cmath>
#include <vector>
#include <limits>

#include <QMouseEvent>
#include <QWheelEvent>
#include <QDebug>

#include "app/Application.h"
#include "core/StructuredLight.h"

namespace smcp
{

// ---------------------------------------------------------------------------
//  Shaders (GLSL 120 - compatible with OpenGL 2.1 / ES 2.0)
// ---------------------------------------------------------------------------

static const char* Vertex_Shader_Source =
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

static const char* Fragment_Shader_Source =
"#version 120\n"
"varying vec3 v_color;\n"
"void main() {\n"
"    gl_FragColor = vec4(v_color, 1.0);\n"
"}\n";

// ---------------------------------------------------------------------------
//  Constructor / Destructor
// ---------------------------------------------------------------------------

PointcloudWidget::PointcloudWidget(QWidget* parent)
	: QOpenGLWidget(parent)
	, _vbo(QOpenGLBuffer::VertexBuffer)
	, _vertexCount(0)
	, _center(0.0f, 0.0f, 0.0f)
	, _distance(500.0f)
	, _rotationX(0.0f)
	, _rotationY(0.0f)
{
	setMinimumSize(320, 240);
	setFocusPolicy(Qt::StrongFocus);
}

PointcloudWidget::~PointcloudWidget()
{
	makeCurrent();

	if (_vbo.isCreated())
	{
		_vbo.destroy();
	}

	doneCurrent();
}

// ---------------------------------------------------------------------------
//  OpenGL initialization
// ---------------------------------------------------------------------------

void PointcloudWidget::initializeGL()
{
	initializeOpenGLFunctions();

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnable(GL_DEPTH_TEST);

	BuildShaders();
	_vbo.create();
}

void PointcloudWidget::BuildShaders()
{
	if (!_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, Vertex_Shader_Source))
	{
		qWarning() << "PointcloudWidget: vertex shader compilation failed:" << _shaderProgram.log();
	}
	if (!_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, Fragment_Shader_Source))
	{
		qWarning() << "PointcloudWidget: fragment shader compilation failed:" << _shaderProgram.log();
	}

	_shaderProgram.bindAttributeLocation("a_position", 0);
	_shaderProgram.bindAttributeLocation("a_color", 1);

	if (!_shaderProgram.link())
	{
		qWarning() << "PointcloudWidget: shader program link failed:" << _shaderProgram.log();
	}
}

// ---------------------------------------------------------------------------
//  Load pointcloud from Application
// ---------------------------------------------------------------------------

void PointcloudWidget::loadPointcloud()
{
	Scan3d::Pointcloud const& pointcloud = APP->pointcloud;

	// Guard: need both points and colors
	if (!pointcloud.points.data || !pointcloud.colors.data)
	{
		_vertexCount = 0;
		update();
		return;
	}

	const int rows = pointcloud.points.rows;
	const int cols = pointcloud.points.cols;

	// --- First pass: count valid points and compute bounding box ---
	int validCount = 0;
	float bbMinX = std::numeric_limits<float>::max();
	float bbMinY = std::numeric_limits<float>::max();
	float bbMinZ = std::numeric_limits<float>::max();
	float bbMaxX = -std::numeric_limits<float>::max();
	float bbMaxY = -std::numeric_limits<float>::max();
	float bbMaxZ = -std::numeric_limits<float>::max();

	for (int r = 0; r < rows; ++r)
	{
		const cv::Vec3f* ptRow = pointcloud.points.ptr<cv::Vec3f>(r);
		for (int c = 0; c < cols; ++c)
		{
			const cv::Vec3f& pt = ptRow[c];
			if (StructuredLight::Invalid(pt))
			{
				continue;
			}
			++validCount;

			// Rotate 180° around X: X' = X, Y' = -Y, Z' = -Z (OpenCV → OpenGL)
			const float flippedY = -pt[1];
			const float flippedZ = -pt[2];
			if (pt[0] < bbMinX) bbMinX = pt[0];
			if (flippedY < bbMinY) bbMinY = flippedY;
			if (flippedZ < bbMinZ) bbMinZ = flippedZ;
			if (pt[0] > bbMaxX) bbMaxX = pt[0];
			if (flippedY > bbMaxY) bbMaxY = flippedY;
			if (flippedZ > bbMaxZ) bbMaxZ = flippedZ;
		}
	}

	if (validCount == 0)
	{
		_vertexCount = 0;
		update();
		return;
	}

	// --- Second pass: build interleaved buffer [x, y, z, r, g, b] ---
	std::vector<float> buffer;
	buffer.reserve(validCount * 6);

	for (int r = 0; r < rows; ++r)
	{
		const cv::Vec3f* ptRow = pointcloud.points.ptr<cv::Vec3f>(r);
		const cv::Vec3b* colRow = pointcloud.colors.ptr<cv::Vec3b>(r);

		for (int c = 0; c < cols; ++c)
		{
			const cv::Vec3f& pt = ptRow[c];
			if (StructuredLight::Invalid(pt))
			{
				continue;
			}

			// Position: rotate 180° around X (OpenCV → OpenGL)
			buffer.push_back(pt[0]);
			buffer.push_back(-pt[1]);
			buffer.push_back(-pt[2]);

			// Color: OpenCV stores BGR, shader expects RGB normalized
			const cv::Vec3b& color = colRow[c];
			buffer.push_back(color[2] / 255.0f);  // R
			buffer.push_back(color[1] / 255.0f);  // G
			buffer.push_back(color[0] / 255.0f);  // B
		}
	}

	_vertexCount = validCount;

	// --- Auto-center camera on the bounding box ---
	_center = QVector3D((bbMinX + bbMaxX) * 0.5f,
		(bbMinY + bbMaxY) * 0.5f,
		(bbMinZ + bbMaxZ) * 0.5f);

	float diagonal = QVector3D(bbMaxX - bbMinX, bbMaxY - bbMinY, bbMaxZ - bbMinZ).length();
	_distance = diagonal * 1.5f;
	if (_distance < 1.0f) _distance = 1.0f;

	_rotationX = 0.0f;
	_rotationY = 0.0f;

	// --- Upload to GPU ---
	makeCurrent();

	if (!_vbo.isCreated())
	{
		_vbo.create();
	}
	_vbo.bind();
	_vbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
	_vbo.allocate(buffer.data(), static_cast<int>(buffer.size() * sizeof(float)));
	_vbo.release();

	doneCurrent();

	update();
}

// ---------------------------------------------------------------------------
//  Resize
// ---------------------------------------------------------------------------

void PointcloudWidget::resizeGL(int w, int h)
{
	if (h == 0) h = 1;

	glViewport(0, 0, w, h);

	float aspect = static_cast<float>(w) / static_cast<float>(h);
	float nearPlane = _distance * 0.001f;
	float farPlane = _distance * 10.0f;
	if (nearPlane < 0.01f) nearPlane = 0.01f;

	_projection.setToIdentity();
	_projection.perspective(45.0f, aspect, nearPlane, farPlane);
}

// ---------------------------------------------------------------------------
//  Render
// ---------------------------------------------------------------------------

void PointcloudWidget::UpdateViewMatrix()
{
	_view.setToIdentity();
	_view.translate(0.0f, 0.0f, -_distance);
	_view.rotate(_rotationX, 1.0f, 0.0f, 0.0f);
	_view.rotate(_rotationY, 0.0f, 1.0f, 0.0f);
	_view.translate(-_center);
}

void PointcloudWidget::paintGL()
{
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	if (_vertexCount <= 0)
	{
		return;
	}

	// Recalculate near/far based on current distance
	float aspect = (height() > 0)
		? static_cast<float>(width()) / static_cast<float>(height())
		: 1.0f;
	float nearPlane = _distance * 0.001f;
	float farPlane = _distance * 10.0f;
	if (nearPlane < 0.01f) nearPlane = 0.01f;

	_projection.setToIdentity();
	_projection.perspective(45.0f, aspect, nearPlane, farPlane);

	UpdateViewMatrix();

	QMatrix4x4 mvp = _projection * _view;

	_shaderProgram.bind();
	_shaderProgram.setUniformValue("u_mvp", mvp);

	_vbo.bind();

	const int stride = 6 * static_cast<int>(sizeof(float));

	// Attribute 0: position (vec3) at offset 0
	_shaderProgram.enableAttributeArray(0);
	_shaderProgram.setAttributeBuffer(0, GL_FLOAT, 0, 3, stride);

	// Attribute 1: color (vec3) at offset 3 floats
	_shaderProgram.enableAttributeArray(1);
	_shaderProgram.setAttributeBuffer(1, GL_FLOAT, 3 * static_cast<int>(sizeof(float)), 3, stride);

	glPointSize(2.0f);
	glDrawArrays(GL_POINTS, 0, _vertexCount);

	_shaderProgram.disableAttributeArray(0);
	_shaderProgram.disableAttributeArray(1);

	_vbo.release();
	_shaderProgram.release();
}

// ---------------------------------------------------------------------------
//  Mouse interaction: orbital camera
// ---------------------------------------------------------------------------

void PointcloudWidget::mousePressEvent(QMouseEvent* event)
{
	_lastMousePos = event->pos();
	event->accept();
}

void PointcloudWidget::mouseMoveEvent(QMouseEvent* event)
{
	int dx = event->x() - _lastMousePos.x();
	int dy = event->y() - _lastMousePos.y();

	if (event->buttons() & Qt::LeftButton)
	{
		// Rotate
		_rotationY += dx * 0.5f;
		_rotationX += dy * 0.5f;

		// Clamp pitch
		if (_rotationX > 90.0f) _rotationX = 90.0f;
		if (_rotationX < -90.0f) _rotationX = -90.0f;
	}
	else if (event->buttons() & Qt::MiddleButton)
	{
		// Pan: translate the look-at center along the camera's local axes
		float panSpeed = _distance * 0.001f;

		QMatrix4x4 rot;
		rot.rotate(_rotationX, 1.0f, 0.0f, 0.0f);
		rot.rotate(_rotationY, 0.0f, 1.0f, 0.0f);

		QVector3D right(rot(0, 0), rot(1, 0), rot(2, 0));
		QVector3D up(rot(0, 1), rot(1, 1), rot(2, 1));

		_center -= right * (dx * panSpeed);
		_center += up * (dy * panSpeed);
	}

	_lastMousePos = event->pos();
	update();
	event->accept();
}

void PointcloudWidget::wheelEvent(QWheelEvent* event)
{
	float delta = event->angleDelta().y() / 120.0f;
	float factor = 1.0f - delta * 0.1f;

	_distance *= factor;

	if (_distance < 1.0f)      _distance = 1.0f;
	if (_distance > 100000.0f) _distance = 100000.0f;

	update();
	event->accept();
}
} // namespace smcp
