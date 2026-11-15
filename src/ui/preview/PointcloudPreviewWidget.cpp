// PointcloudPreviewWidget, part 1 of 3: scene transactions, layers, history, camera and OpenGL
// drawing. The overlays are in PointcloudPreviewOverlays.cpp and the input handling in
// PointcloudPreviewInput.cpp.

#include "ui/preview/PointcloudPreviewWidget.h"
#include "ui/preview/PointcloudPreviewInternal.h"
#include "core/AsyncTask.h"
#include "core/Parallel.h"
#include <atomic>
#include <QTimer>
#include <QVector4D>
#include <stdexcept>

#include <algorithm>
#include <limits>
#include <set>

#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QCursor>
#include <functional>
#include <QtMath>
#include <QOpenGLContext>
#include <QWheelEvent>

namespace smcp
{
	namespace
	{
		class PreviewOverlay : public QWidget
		{
		public:
			PreviewOverlay(QWidget* parent, std::function<void(QPainter&)> paint) :
				QWidget(parent), _paint(std::move(paint))
			{
				setAttribute(Qt::WA_TransparentForMouseEvents);
				setAttribute(Qt::WA_NoSystemBackground);
			}

		protected:
			void paintEvent(QPaintEvent*) override
			{
				QPainter painter(this);
				_paint(painter);
			}

		private:
			std::function<void(QPainter&)> _paint;
		};
	}


	// GLSL 120 (OpenGL 2.1), same as PointcloudWidget.
	// The color of a point is, in order of priority: the selection color when its `selected` flag
	// (a_color.a) is set, the layer's color override when it has one (u_override.a = 1), or the
	// color stored in the point.
	static const char* Vertex_Shader_Source = "#version 120\n"
											  "attribute vec3 a_position;\n"
											  "attribute vec4 a_color;\n"
											  "uniform mat4 u_mvp;\n"
											  "uniform vec4 u_override;\n"
											  "uniform vec3 u_selection;\n"
											  "varying vec3 v_color;\n"
											  "void main() {\n"
											  "    gl_Position  = u_mvp * vec4(a_position, 1.0);\n"
											  "    gl_PointSize = 2.0;\n"
											  "    vec3 base    = mix(a_color.rgb, u_override.rgb, u_override.a);\n"
											  "    v_color      = mix(base, u_selection, a_color.a);\n"
											  "}\n";

	static const char* Fragment_Shader_Source = "#version 120\n"
												"varying vec3 v_color;\n"
												"void main() {\n"
												"    gl_FragColor = vec4(v_color, 1.0);\n"
												"}\n";

	using namespace PreviewInternal;

	/* Construction ============================================================================ */

	PointcloudPreviewWidget::PointcloudPreviewWidget(QWidget* parent) : QOpenGLWidget(parent)
	{
		_history.push_back({ tr("Initial state"), SceneState() });
		setMinimumSize(320, 240);
		setFocusPolicy(Qt::StrongFocus);
		_overlay = new PreviewOverlay(this, [this](QPainter& painter) { PaintOverlays(painter); });
		_overlay->setObjectName("navigation_overlay");
		_navigationTimer = new QTimer(this);
		_navigationTimer->setSingleShot(true);
		_navigationTimer->setInterval(Navigation_Rest_Milliseconds);
		connect(_navigationTimer, &QTimer::timeout, this,
			[this]()
			{
				// The camera came to rest: draw every point again.
				_navigating = false;
				update();
			});
	}

	PointcloudPreviewWidget::~PointcloudPreviewWidget()
	{
		// During application shutdown Qt may already have released the widget's OpenGL context.
		// In that case there is no valid context in which to destroy GPU objects explicitly.
		if (!context() || !context()->isValid())
		{
			_clouds.clear();
			return;
		}

		makeCurrent();
		for (auto& entry : _clouds)
		{
			DestroyEntry(*entry);
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
		glDepthFunc(GL_LESS);
		BuildShaders();
	}

	void PointcloudPreviewWidget::BuildShaders()
	{
		if (!_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, Vertex_Shader_Source))
		{
			qWarning() << "PointcloudPreviewWidget: vertex shader error:" << _shaderProgram.log();
		}
		if (!_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, Fragment_Shader_Source))
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

	void PointcloudPreviewWidget::UpdateProjection(int w, int h)
	{
		const float aspect = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
		const float nearPlane = std::max(0.01f, _distance * 0.001f);
		const float farPlane = _distance * 10.0f;
		_projection.setToIdentity();
		_projection.perspective(Field_Of_View_Degrees, aspect, nearPlane, farPlane);
	}

	void PointcloudPreviewWidget::resizeGL(int w, int h)
	{
		CancelBrushStroke();
		glViewport(0, 0, w, std::max(1, h));
		UpdateProjection(w, std::max(1, h));
		_overlay->setGeometry(rect());
	}

	void PointcloudPreviewWidget::UpdateViewMatrix()
	{
		_view.setToIdentity();
		_view.translate(_pan.x(), _pan.y(), -_distance);
		_view.rotate(_rotationX, 1.0f, 0.0f, 0.0f);
		_view.rotate(_rotationY, 0.0f, 1.0f, 0.0f);
		_view.translate(-_center);
	}

	void PointcloudPreviewWidget::paintGL()
	{
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		// Send pending vertex data first, a bounded amount per frame so a large cloud does not
		// freeze the window; the frames in between keep drawing what is already complete.
		std::size_t uploadBudget = Upload_Bytes_Per_Frame;
		bool pending = false;
		for (auto& entry : _clouds)
		{
			if (entry->pendingUpload && uploadBudget)
				UploadEntry(*entry, uploadBudget);
			if (!entry->pendingUpload && entry->previous)
			{
				// The new buffers are complete: the replaced entry is no longer needed.
				DestroyEntry(*entry->previous);
				entry->previous.reset();
			}
			pending |= entry->pendingUpload;
		}
		if (pending)
			update();

		UpdateProjection(width(), height());
		UpdateViewMatrix();

		const QMatrix4x4 mvp = _projection * _view;
		// While the camera moves a very large scene is drawn decimated: the attribute stride is
		// widened so the GPU reads one vertex of every `step`, with no extra buffers.
		const int step = NavigationStep();

		_shaderProgram.bind();
		_shaderProgram.setUniformValue("u_selection", QVector3D(static_cast<float>(Selection_Color.redF()),
			static_cast<float>(Selection_Color.greenF()), static_cast<float>(Selection_Color.blueF())));

		// Last row first, so the first row of the panel ends on top.
		for (auto it = _clouds.rbegin(); it != _clouds.rend(); ++it)
		{
			auto& entry = *it;
			// While a replacement is still uploading, keep drawing the entry it replaces.
			CloudEntry* drawn = (entry->pendingUpload && entry->previous) ? entry->previous.get() : entry.get();
			if (!entry->visible || drawn->vertexCount <= 0 || drawn->pendingUpload || !drawn->positions ||
				!drawn->positions->isCreated() || !drawn->colors.isCreated())
			{
				continue;
			}
			// Each cloud keeps its depth; the top panel row wins overlapping pixels.
			glClear(GL_DEPTH_BUFFER_BIT);
			_shaderProgram.setUniformValue("u_mvp", mvp * entry->transform);
			const QColor& override = entry->colorOverride;
			_shaderProgram.setUniformValue("u_override",
				override.isValid() ? QVector4D(static_cast<float>(override.redF()), static_cast<float>(override.greenF()),
										 static_cast<float>(override.blueF()), 1.0f)
								   : QVector4D(0.0f, 0.0f, 0.0f, 0.0f));
			drawn->vao.bind();

			drawn->positions->bind();
			_shaderProgram.enableAttributeArray(0);
			_shaderProgram.setAttributeBuffer(0, GL_FLOAT, 0, 3, step * Position_Bytes);
			drawn->positions->release();

			drawn->colors.bind();
			_shaderProgram.enableAttributeArray(1);
			_shaderProgram.setAttributeBuffer(1, GL_UNSIGNED_BYTE, 0, 4, step * Color_Bytes);
			drawn->colors.release();

			glPointSize(2.0f);
			glDrawArrays(GL_POINTS, 0, (drawn->vertexCount - 1) / step + 1);

			_shaderProgram.disableAttributeArray(0);
			_shaderProgram.disableAttributeArray(1);
			drawn->vao.release();
		}

		_shaderProgram.release();
		glDisable(GL_DEPTH_TEST);
		_overlay->update();
	}

	void PointcloudPreviewWidget::MarkNavigating()
	{
		_navigating = true;
		_navigationTimer->start();
	}

	int PointcloudPreviewWidget::NavigationStep() const
	{
		if (!_navigating)
			return 1;
		long long visible = 0;
		for (const auto& entry : _clouds)
			if (entry->visible)
				visible += entry->vertexCount;
		if (visible <= Navigation_Full_Detail_Points)
			return 1;
		return static_cast<int>((visible + Navigation_Target_Points - 1) / Navigation_Target_Points);
	}

	/* Clouds ================================================================================== */

	PointcloudPreviewWidget::PreparedPointcloud PointcloudPreviewWidget::PreparePointcloud(
		const Pointcloud::ColorCloud& cloud, const std::vector<char>* selection, bool colorsOnly)
	{
		PreparedPointcloud prepared;
		prepared.reusePositions = colorsOnly;
		if (cloud.empty())
		{
			return prepared;
		}

		const bool useSelection = selection && selection->size() == cloud.size();

		float minX = std::numeric_limits<float>::max(), minY = minX, minZ = minX;
		float maxX = -std::numeric_limits<float>::max(), maxY = maxX, maxZ = maxX;
		cv::Vec3d sum(0, 0, 0);
		double colorSumR = 0.0;
		double colorSumG = 0.0;
		double colorSumB = 0.0;

		// Single pass writing through raw pointers: sized for every point, trimmed to the finite
		// ones. Both buffers skip the same points, so vertex i is the same point in each.
		if (!colorsOnly)
			prepared.positions.resize(cloud.size() * 3);
		prepared.colors.resize(cloud.size() * 4);
		float* position = prepared.positions.data();
		unsigned char* color = prepared.colors.data();
		int count = 0;
		for (std::size_t i = 0; i < cloud.size(); ++i)
		{
			const Pointcloud::ColorPoint& pt = cloud[i];
			if (!Pointcloud::IsFinite(pt))
			{
				continue;
			}
			if (!colorsOnly)
			{
				sum += cv::Vec3d(pt.x, pt.y, pt.z);
				// 180 degree rotation around X (OpenCV -> OpenGL): X, -Y, -Z.
				const float x = pt.x;
				const float y = -pt.y;
				const float z = -pt.z;
				minX = std::min(minX, x);
				maxX = std::max(maxX, x);
				minY = std::min(minY, y);
				maxY = std::max(maxY, y);
				minZ = std::min(minZ, z);
				maxZ = std::max(maxZ, z);
				position[0] = x;
				position[1] = y;
				position[2] = z;
				position += 3;
				colorSumR += pt.r / 255.0f;
				colorSumG += pt.g / 255.0f;
				colorSumB += pt.b / 255.0f;
			}
			color[0] = pt.r;
			color[1] = pt.g;
			color[2] = pt.b;
			color[3] = (useSelection && (*selection)[i]) ? 255 : 0;
			color += 4;
			++count;
		}

		prepared.vertexCount = count;
		prepared.colors.resize(static_cast<std::size_t>(count) * 4);
		if (colorsOnly)
		{
			return prepared;
		}
		prepared.positions.resize(static_cast<std::size_t>(count) * 3);
		if (count)
		{
			prepared.centroid = sum * (1.0 / count);
			prepared.pointColor = QColor::fromRgbF(colorSumR / count, colorSumG / count, colorSumB / count);
		}
		prepared.bbMin = QVector3D(minX, minY, minZ);
		prepared.bbMax = QVector3D(maxX, maxY, maxZ);
		return prepared;
	}

	PointcloudPreviewWidget::SceneState PointcloudPreviewWidget::CaptureScene() const
	{
		if (_draft)
			return *_draft;
		SceneState state;
		state.activeLayers = _activeLayers;
		for (const auto& cloud : _clouds)
			state.clouds.push_back(*cloud);
		return state;
	}

	PointcloudPreviewWidget::ProjectState PointcloudPreviewWidget::CaptureProject() const
	{
		if (_working || _draft)
			throw std::logic_error("Finish the current command before saving a project");
		return { CaptureScene(), _history, _historyPosition, _center, _pan, _centerInitialized, _distance, _rotationX,
			_rotationY };
	}

	void PointcloudPreviewWidget::RestoreProject(ProjectState project)
	{
		if (_working || _draft)
			return;
		CancelGizmoDrag();
		_selectionBoxActive = false;
		CancelBrushStroke();
		auto saved = std::make_shared<ProjectState>(std::move(project));
		_working = true;
		emit BusyChanged(true);
		RunBackground<PreparedScene>(
			this, [saved]() { return PrepareScene(saved->scene, {}); },
			[this, saved](PreparedScene result)
			{
				int nextId = 0;
				const auto inspect = [&](const SceneState& scene)
				{
					for (const auto& cloud : scene.clouds)
						nextId = std::max(nextId, cloud.id + 1);
				};
				inspect(saved->scene);
				for (const auto& entry : saved->history)
					inspect(entry.state);
				InstallScene(result);
				_history.swap(saved->history);
				_historyPosition = saved->historyPosition;
				_cloudCounter = nextId;
				_center = saved->center;
				_pan = saved->pan;
				_centerInitialized = saved->centerInitialized;
				_distance = saved->distance;
				_rotationX = saved->rotationX;
				_rotationY = saved->rotationY;
				UpdateViewMatrix();
				update();
				emit HistoryChanged();
				emit ProjectRestored();
			},
			[this](std::exception_ptr error) { ReportOperationFailure(error); },
			[this]() { FinishEdit(); });
	}

	/* Scene transactions ======================================================================
	   Read this block top to bottom to follow one change:

	     Change() / EditScene()        a function "scene -> next scene" is submitted
	       Submit()                    runs it now, or queues it behind the edit in progress
	         StartEdit()               worker: the function, then PrepareScene()
	                                   GUI:    InstallScene(), then the history entry
	         FinishEdit()              next queued edit, or idle (BusyChanged(false))

	   BeginCommand()/EndCommand() bracket several Change() calls into one edit: while a command is
	   open the changes go to a draft copy of the scene, and EndCommand() submits the draft. */

	void PointcloudPreviewWidget::BeginCommand(const QString& label)
	{
		if (_draft || _working)
			throw std::logic_error("Another editor command is pending");
		_draft = std::make_unique<SceneState>(CaptureScene());
		_commandLabel = label;
	}

	void PointcloudPreviewWidget::AbortCommand()
	{
		_draft.reset();
	}

	void PointcloudPreviewWidget::EndCommand()
	{
		if (!_draft)
			return;
		auto state = std::move(*_draft);
		_draft.reset();
		if (!EditorScene::SameScene(CaptureScene(), state))
			EditScene(_commandLabel, [state](SceneState) { return state; });
	}

	void PointcloudPreviewWidget::Change(const QString& label, const std::function<void(SceneState&)>& edit)
	{
		// InstallScene() notifies the panel, which answers by setting the selection it was just
		// given; that echo must not become a new edit.
		if (_installing)
			return;
		if (_draft)
		{
			edit(*_draft);
			return;
		}
		EditScene(label,
			[edit](SceneState state)
			{
				edit(state);
				return state;
			});
	}

	PointcloudPreviewWidget::PreparedScene PointcloudPreviewWidget::PrepareScene(
		SceneState state, const SceneState& before)
	{
		// Runs on the worker. What must be rebuilt depends on what changed in each cloud:
		//   - new point data      -> positions, colors and the derived geometry;
		//   - new selection only  -> colors only (the selection flag travels in the color buffer);
		//   - anything else (color override, name, visibility, order) -> nothing: the override is a
		//     shader uniform and the rest is not vertex data.
		PreparedScene prepared;
		prepared.state = std::move(state);
		prepared.buffers.resize(prepared.state.clouds.size());
		for (std::size_t i = 0; i < prepared.state.clouds.size(); ++i)
		{
			auto& cloud = prepared.state.clouds[i];
			const auto old = std::find_if(
				before.clouds.begin(), before.clouds.end(), [&](const CloudState& c) { return c.id == cloud.id; });
			const bool known = old != before.clouds.end();
			const bool newPoints = !known || old->sourceCloud != cloud.sourceCloud;
			if (newPoints && cloud.sourceCloud)
			{
				auto buffer = PreparePointcloud(*cloud.sourceCloud, cloud.selection.get());
				cloud.vertexCount = buffer.vertexCount;
				cloud.centroid = buffer.centroid;
				cloud.pointColor = buffer.pointColor;
				cloud.bbMin = buffer.bbMin;
				cloud.bbMax = buffer.bbMax;
				prepared.buffers[i] = std::move(buffer);
			}
			else if (known)
			{
				// Derived geometry follows immutable points, including restored history snapshots.
				cloud.vertexCount = old->vertexCount;
				cloud.centroid = old->centroid;
				cloud.pointColor = old->pointColor;
				cloud.bbMin = old->bbMin;
				cloud.bbMax = old->bbMax;
				if (old->selection != cloud.selection && cloud.sourceCloud)
					prepared.buffers[i] = PreparePointcloud(*cloud.sourceCloud, cloud.selection.get(), true);
			}
			// The swatch of the layer list: the override, or the mean color of the points.
			cloud.color = cloud.colorOverride.isValid() ? cloud.colorOverride : cloud.pointColor;
		}
		// Layers that no longer exist or are hidden cannot stay active.
		auto& ids = prepared.state.activeLayers;
		ids.erase(std::remove_if(ids.begin(), ids.end(),
					  [&](int id)
					  {
						  return std::none_of(prepared.state.clouds.begin(), prepared.state.clouds.end(),
							  [&](const CloudState& c) { return c.id == id && c.visible; });
					  }),
			ids.end());
		return prepared;
	}

	void PointcloudPreviewWidget::InstallScene(PreparedScene& prepared)
	{
		// Runs on the GUI thread. Builds the new list of GPU entries: a fresh entry (uploaded over
		// the next frames) for each cloud PrepareScene() rebuilt, the existing entry for the rest.
		auto activeLayers = prepared.state.activeLayers;
		std::vector<int> oldIds;
		for (const auto& c : _clouds)
			oldIds.push_back(c->id);
		std::vector<std::unique_ptr<CloudEntry>> next(prepared.state.clouds.size());
		// Allocate everything before moving live GPU entries, so allocation failures leave the scene intact.
		for (std::size_t i = 0; i < next.size(); ++i)
		{
			if (prepared.buffers[i])
			{
				next[i] = std::make_unique<CloudEntry>();
				static_cast<CloudState&>(*next[i]) = prepared.state.clouds[i];
				next[i]->pendingPositions = std::move(prepared.buffers[i]->positions);
				next[i]->pendingColors = std::move(prepared.buffers[i]->colors);
				next[i]->pendingUpload = true;
			}
		}
		std::vector<std::unique_ptr<CloudEntry>> retired;
		for (std::size_t i = 0; i < next.size(); ++i)
		{
			const std::size_t old = static_cast<std::size_t>(
				std::find(oldIds.begin(), oldIds.end(), prepared.state.clouds[i].id) - oldIds.begin());
			const bool known = old < _clouds.size() && _clouds[old];
			if (!next[i])
			{
				// Unchanged cloud: keep its GPU entry. A cloud that is neither prepared nor known (a
				// state without points) gets an empty entry instead of indexing past the old list.
				next[i] = known ? std::move(_clouds[old]) : std::make_unique<CloudEntry>();
				static_cast<CloudState&>(*next[i]) = prepared.state.clouds[i];
				continue;
			}

			if (prepared.buffers[i]->reusePositions)
			{
				// Only the colors were rebuilt. The positions come from the entry being replaced:
				// its GPU buffer when it is complete, otherwise the data it had not finished sending.
				CloudEntry* source = known ? _clouds[old].get() : nullptr;
				const bool complete = source && source->positions && source->positions->isCreated() &&
					source->pendingPositions.empty();
				if (complete)
					next[i]->positions = source->positions;
				else if (source && !source->pendingPositions.empty())
					next[i]->pendingPositions = std::move(source->pendingPositions);
				else if (next[i]->sourceCloud)
					next[i]->pendingPositions = PreparePointcloud(*next[i]->sourceCloud).positions;  // not expected
			}
			if (known)
			{
				// The last complete entry of this cloud stays on screen until the new one is uploaded.
				std::unique_ptr<CloudEntry> shown = std::move(_clouds[old]);
				if (shown->pendingUpload)
				{
					std::unique_ptr<CloudEntry> older = std::move(shown->previous);
					retired.push_back(std::move(shown));
					shown = std::move(older);
				}
				if (shown && shown->vertexCount > 0 && shown->positions && shown->positions->isCreated() &&
					shown->colors.isCreated())
					next[i]->previous = std::move(shown);
				else if (shown)
					retired.push_back(std::move(shown));
			}
		}
		const bool valid = context() && context()->isValid();
		if (valid)
			makeCurrent();
		for (auto& old : _clouds)
			if (old && valid)
				DestroyEntry(*old);
		for (auto& old : retired)
			if (valid)
				DestroyEntry(*old);
		if (valid)
			doneCurrent();
		_clouds = std::move(next);
		_activeLayers = std::move(activeLayers);
		if (!_centerInitialized)
		{
			const auto first =
				std::find_if(_clouds.begin(), _clouds.end(), [](const auto& c) { return c->vertexCount > 0; });
			if (first != _clouds.end())
			{
				const auto c = (*first)->centroid;
				_center = QVector3D(c[0], -c[1], -c[2]);
				_centerInitialized = true;
				FrameSelection();
			}
		}
		// Listeners refresh from the new scene; _installing ignores the edits they request back.
		_installing = true;
		LayersChanged();
		emit ActiveLayersRestored();
		emit SelectionChanged();
		_installing = false;
	}

	void PointcloudPreviewWidget::EditScene(const QString& label, std::function<SceneState(SceneState)> work)
	{
		Submit(label, std::move(work));
	}

	void PointcloudPreviewWidget::Submit(
		const QString& label, std::function<SceneState(SceneState)> work, int restorePosition)
	{
		if (_draft)
		{
			// An open command owns the scene: its changes go to the draft through Change().
			return;
		}
		PendingEdit edit{ label, std::move(work), restorePosition };
		if (_working)
		{
			// One edit at a time. This one will start from the scene the current one leaves.
			_pendingEdits.push_back(std::move(edit));
			return;
		}
		_working = true;
		emit BusyChanged(true);
		StartEdit(std::move(edit));
	}

	void PointcloudPreviewWidget::StartEdit(PendingEdit edit)
	{
		// The worker receives copies: `before` to compute the edit from, and to tell which clouds
		// changed and need a new GPU buffer.
		const auto before = CaptureScene();
		const QString label = edit.label;
		const int restorePosition = edit.restorePosition;
		RunBackground<PreparedScene>(
			this, [before, work = std::move(edit.work)]() { return PrepareScene(work(before), before); },
			[this, before, label, restorePosition](PreparedScene result)
			{
				// Back on the GUI thread. An edit that changed nothing leaves no history entry.
				if (restorePosition < 0 && EditorScene::SameScene(before, result.state))
					return;
				// The new history is built aside and swapped in only after the scene is installed,
				// so a failure in between leaves scene and history consistent.
				auto history = _history;
				int position = restorePosition;
				if (restorePosition < 0)
				{
					history.resize(_historyPosition + 1);  // a new edit discards the redo branch
					history.push_back({ label, result.state });
					EditorScene::TrimHistory(history, Max_History_Entries, _historyByteBudget);
					position = static_cast<int>(history.size()) - 1;
				}
				InstallScene(result);
				_history.swap(history);
				_historyPosition = position;
				emit HistoryChanged();
			},
			[this](std::exception_ptr error)
			{
				// The edits that were waiting assumed this one would succeed.
				_pendingEdits.clear();
				ReportOperationFailure(error);
			},
			[this]() { FinishEdit(); });
	}

	void PointcloudPreviewWidget::FinishEdit()
	{
		if (!_pendingEdits.empty())
		{
			PendingEdit next = std::move(_pendingEdits.front());
			_pendingEdits.erase(_pendingEdits.begin());
			StartEdit(std::move(next));
			return;
		}
		_working = false;
		emit BusyChanged(false);
	}

	void PointcloudPreviewWidget::ReportOperationFailure(std::exception_ptr error)
	{
		_installing = false;
		try
		{
			std::rethrow_exception(error);
		}
		catch (const std::exception& e)
		{
			emit OperationFailed(QString::fromUtf8(e.what()));
		}
		catch (...)
		{
			emit OperationFailed(tr("Unknown editor error."));
		}
		for (auto& cloud : _clouds)
			cloud->transform.setToIdentity();
		update();
	}
	QStringList PointcloudPreviewWidget::HistoryLabels() const
	{
		QStringList labels;
		for (const auto& item : _history)
			labels << item.label;
		return labels;
	}
	void PointcloudPreviewWidget::Undo()
	{
		JumpToHistory(_historyPosition - 1);
	}
	void PointcloudPreviewWidget::Redo()
	{
		JumpToHistory(_historyPosition + 1);
	}
	void PointcloudPreviewWidget::JumpToHistory(int position)
	{
		if (position < 0 || position >= static_cast<int>(_history.size()) || position == _historyPosition || _working ||
			_draft)
			return;
		CancelGizmoDrag();
		_selectionBoxActive = false;
		CancelBrushStroke();
		const auto state = _history[position].state;
		Submit(tr("Restore history"), [state](SceneState) { return state; }, position);
	}
	int PointcloudPreviewWidget::AddPointcloud(
		const Pointcloud::ColorCloudPtr& cloud, const QColor& color, const QString& name, const QString& details)
	{
		if (!cloud || cloud->empty())
			return -1;
		CloudState entry;
		entry.sourceCloud = cloud;
		entry.name = name;
		entry.details = details;
		entry.colorOverride = color;
		entry.id = _cloudCounter++;
		Change(tr("Add point cloud"),
			[entry](SceneState& state)
			{
				if (state.clouds.empty())
					state.activeLayers = { entry.id };
				state.clouds.push_back(entry);
			});
		return entry.id;
	}
	int PointcloudPreviewWidget::PrependPointcloud(
		const Pointcloud::ColorCloudPtr& cloud, const QColor& color, const QString& name, const QString& details)
	{
		if (!cloud || cloud->empty())
			return -1;
		CloudState entry;
		entry.sourceCloud = cloud;
		entry.name = name;
		entry.details = details;
		entry.colorOverride = color;
		entry.id = _cloudCounter++;
		Change(
			tr("Add result cloud"), [entry](SceneState& state) { state.clouds.insert(state.clouds.begin(), entry); });
		return entry.id;
	}
	std::vector<PointcloudPreviewWidget::ListedPointcloud> PointcloudPreviewWidget::Pointclouds() const
	{
		std::vector<ListedPointcloud> result;
		VisitClouds([&](const CloudState& c) { result.push_back({ c.sourceCloud, c.name, c.visible, c.id }); });
		return result;
	}
	std::vector<PointcloudPreviewWidget::LayerInfo> PointcloudPreviewWidget::Layers() const
	{
		std::vector<LayerInfo> result;
		VisitClouds([&](const CloudState& c)
			{ result.push_back({ c.name, c.details, c.color, c.colorOverride, c.vertexCount, c.visible, c.id }); });
		return result;
	}
	Pointcloud::ColorCloudPtr PointcloudPreviewWidget::Cloud(int index) const
	{
		Pointcloud::ColorCloudPtr found;
		int position = 0;
		VisitClouds([&](const CloudState& c)
			{
				if (position++ == index)
					found = c.sourceCloud;
			});
		return found;
	}
	int PointcloudPreviewWidget::IndexOfLayer(int id) const
	{
		int found = -1, position = 0;
		VisitClouds([&](const CloudState& c)
			{
				if (c.id == id && found < 0)
					found = position;
				++position;
			});
		return found;
	}
	bool PointcloudPreviewWidget::FindLayer(int id, LayerInfo& info, Pointcloud::ColorCloudPtr* cloud) const
	{
		bool found = false;
		VisitClouds([&](const CloudState& c)
			{
				if (c.id != id || found)
					return;
				found = true;
				info = { c.name, c.details, c.color, c.colorOverride, c.vertexCount, c.visible, c.id };
				if (cloud)
					*cloud = c.sourceCloud;
			});
		return found;
	}
	void PointcloudPreviewWidget::SetLayerColor(int id, const QColor& color)
	{
		Change(tr("Change color"),
			[=](SceneState& state)
			{
				for (auto& cloud : state.clouds)
					if (cloud.id == id)
						cloud.colorOverride = color;
			});
	}
	void PointcloudPreviewWidget::SetPointcloudColor(int index, const QColor& color)
	{
		Change(tr("Change color"),
			[=](SceneState& state)
			{
				if (index >= 0 && index < static_cast<int>(state.clouds.size()))
					state.clouds[index].colorOverride = color;
			});
	}
	void PointcloudPreviewWidget::RenamePointcloud(int index, const QString& name)
	{
		Change(tr("Rename point cloud"),
			[=](SceneState& state)
			{
				if (index >= 0 && index < static_cast<int>(state.clouds.size()) && !name.trimmed().isEmpty())
					state.clouds[index].name = name.trimmed();
			});
	}
	void PointcloudPreviewWidget::RemovePointcloud(int index)
	{
		Change(tr("Remove point cloud"),
			[=](SceneState& state)
			{
				if (index >= 0 && index < static_cast<int>(state.clouds.size()))
					state.clouds.erase(state.clouds.begin() + index);
			});
	}
	void PointcloudPreviewWidget::RemoveSelectedPointclouds()
	{
		EditScene(tr("Delete selected point clouds"), [](SceneState scene) { return EditorScene::RemoveActive(std::move(scene)); });
	}
	void PointcloudPreviewWidget::SetPointcloudVisible(int index, bool visible)
	{
		Change(tr("Change visibility"),
			[=](SceneState& state)
			{
				if (index >= 0 && index < static_cast<int>(state.clouds.size()))
				{
					auto& c = state.clouds[index];
					c.visible = visible;
					if (!visible)
					{
						c.selection.reset();
						c.selectedCount = 0;
					}
				}
			});
	}
	void PointcloudPreviewWidget::SetAllPointcloudsVisible(bool visible)
	{
		Change(tr("Change visibility"),
			[=](SceneState& state)
			{
				for (auto& c : state.clouds)
				{
					c.visible = visible;
					if (!visible)
					{
						c.selection.reset();
						c.selectedCount = 0;
					}
				}
			});
	}
	void PointcloudPreviewWidget::ReplacePointcloud(
		int index, const Pointcloud::ColorCloudPtr& cloud, const QString& details)
	{
		Change(tr("Replace point cloud"),
			[=](SceneState& state)
			{
				if (index >= 0 && index < static_cast<int>(state.clouds.size()) && cloud)
				{
					auto& c = state.clouds[index];
					c.sourceCloud = cloud;
					c.details = details;
					c.selection.reset();
					c.selectedCount = 0;
				}
			});
	}
	void PointcloudPreviewWidget::ReorderPointclouds(const std::vector<int>& order)
	{
		Change(tr("Reorder point clouds"),
			[=](SceneState& state)
			{
				if (order.size() != state.clouds.size())
					return;
				auto clouds = state.clouds;
				std::vector<bool> seen(order.size());
				for (std::size_t i = 0; i < order.size(); ++i)
				{
					if (order[i] < 0 || order[i] >= static_cast<int>(order.size()) || seen[order[i]])
						return;
					seen[order[i]] = true;
					clouds[i] = state.clouds[order[i]];
				}
				state.clouds = std::move(clouds);
			});
	}
	void PointcloudPreviewWidget::SetActiveLayersTransform(const QMatrix4x4& transform)
	{
		for (auto& c : _clouds)
			if (c->visible && std::find(_activeLayers.begin(), _activeLayers.end(), c->id) != _activeLayers.end())
				c->transform = transform;
		update();
	}


	/* Selection =============================================================================== */

	void PointcloudPreviewWidget::SetSelectionMode(bool enabled)
	{
		_selectionMode = enabled;
		_selectionBoxActive = false;
		SetBrushSelectionMode(false);
		if (enabled)
			setCursor(Qt::CrossCursor);
		else
			unsetCursor();
		update();
	}
	void PointcloudPreviewWidget::SetBrushSelectionMode(bool enabled)
	{
		CancelBrushStroke();
		_brushSelectionMode = enabled;
		if (enabled)
		{
			_selectionMode = false;
			_selectionBoxActive = false;
			_brushPosition = mapFromGlobal(QCursor::pos());
			_brushCursorVisible = rect().contains(_brushPosition.toPoint());
			setCursor(Qt::BlankCursor);
		}
		else if (!_selectionMode)
			unsetCursor();
		setMouseTracking(enabled || _gizmoLayerId >= 0);
		update();
	}
	void PointcloudPreviewWidget::CancelBrushStroke()
	{
		_brushStrokeActive = false;
		_brushStroke.clear();
		update();
	}
	void PointcloudPreviewWidget::AppendBrushSample(const QPointF& position)
	{
		_brushPosition = position;
		if (_brushStroke.empty() || _brushStroke.back().position != position ||
			_brushStroke.back().radius != _brushRadius)
			_brushStroke.push_back({ position, _brushRadius });
	}
	void PointcloudPreviewWidget::SelectBrushStroke()
	{
		// The stroke and the camera it was painted with travel to the worker by value.
		auto samples = std::move(_brushStroke);
		const auto matrix = _brushMatrix;
		const auto viewport = _brushViewport;
		EditScene(tr("Brush select points"),
			[samples, matrix, viewport](SceneState scene)
			{ return EditorScene::SelectBrushStroke(std::move(scene), samples, matrix, viewport); });
	}

	void PointcloudPreviewWidget::ClearSelection()
	{
		Change(tr("Clear point selection"), [](SceneState& scene) { scene = EditorScene::ClearSelection(std::move(scene)); });
	}
	std::size_t PointcloudPreviewWidget::SelectedPointCount() const
	{
		std::size_t count = 0;
		VisitClouds([&](const CloudState& c) { count += c.selectedCount; });
		return count;
	}
	int PointcloudPreviewWidget::SelectedLayerCount() const
	{
		int count = 0;
		VisitClouds([&](const CloudState& c) { count += c.selectedCount > 0; });
		return count;
	}
	std::vector<char> PointcloudPreviewWidget::Selection(int index) const
	{
		const auto state = CaptureScene();
		return index >= 0 && index < static_cast<int>(state.clouds.size()) && state.clouds[index].selection
			? *state.clouds[index].selection
			: std::vector<char>();
	}
	void PointcloudPreviewWidget::SelectInRectangle(const QRect& rect, Qt::KeyboardModifiers modifiers)
	{
		if (_working)
			return;
		const bool add = modifiers & Qt::ShiftModifier, remove = modifiers & Qt::ControlModifier;
		if (rect.width() < Selection_Click_Pixels && rect.height() < Selection_Click_Pixels)
		{
			// A click without dragging clears the selection.
			if (!add && !remove)
				ClearSelection();
			return;
		}
		UpdateProjection(width(), height());
		UpdateViewMatrix();
		const auto matrix = _projection * _view;
		// Widget pixels to normalized device coordinates (y grows upward there).
		EditorScene::NdcRect bounds;
		bounds.left = 2.0f * rect.left() / std::max(1, width()) - 1;
		bounds.right = 2.0f * (rect.right() + 1) / std::max(1, width()) - 1;
		bounds.top = 1 - 2.0f * rect.top() / std::max(1, height());
		bounds.bottom = 1 - 2.0f * (rect.bottom() + 1) / std::max(1, height());
		EditScene(tr("Select points"),
			[=](SceneState scene) { return EditorScene::SelectInRectangle(std::move(scene), matrix, bounds, add, remove); });
	}
	void PointcloudPreviewWidget::DeleteSelectedPoints()
	{
		EditScene(tr("Delete selected points"), [](SceneState scene) { return EditorScene::DeleteSelectedPoints(std::move(scene)); });
	}
	void PointcloudPreviewWidget::TransformSelected(const cv::Matx44d& transform, const QString& description)
	{
		EditScene(description,
			[=](SceneState scene) { return EditorScene::TransformActive(std::move(scene), transform, description); });
	}

	void PointcloudPreviewWidget::AlignSelectedCentroids()
	{
		EditScene(tr("Align centroids"), [](SceneState scene) { return EditorScene::AlignActiveCentroids(std::move(scene)); });
	}

	void PointcloudPreviewWidget::MergeSelectedPointclouds()
	{
		// The id is taken here, on the GUI thread, so it is never reused even if the edit fails.
		const int mergedId = _cloudCounter++;
		EditScene(tr("Merge point clouds"),
			[mergedId](SceneState scene) { return EditorScene::MergeActive(std::move(scene), mergedId); });
	}

	void PointcloudPreviewWidget::LayersChanged()
	{
		UpdateGizmoPivot();
		update();
		emit PointcloudsChanged();
	}

	/* Active layers and framing =============================================================== */

	void PointcloudPreviewWidget::SetActiveLayers(const std::vector<int>& ids)
	{
		if (_installing)
			return;
		CancelGizmoDrag();
		CancelBrushStroke();
		Change(tr("Select point clouds"),
			[=](SceneState& state)
			{
				state.activeLayers = ids;
				for (auto& c : state.clouds)
					if (std::find(ids.begin(), ids.end(), c.id) == ids.end())
					{
						c.selection.reset();
						c.selectedCount = 0;
					}
			});
	}
	cv::Vec3d PointcloudPreviewWidget::SelectionCentroid() const
	{
		cv::Vec3d center(0, 0, 0);
		std::size_t count = 0;
		for (const auto& c : CaptureScene().clouds)
			if (c.visible && std::find(_activeLayers.begin(), _activeLayers.end(), c.id) != _activeLayers.end())
			{
				center += c.centroid * c.vertexCount;
				count += c.vertexCount;
			}
		return count ? center * (1.0 / count) : center;
	}
	void PointcloudPreviewWidget::FrameSelection()
	{
		_pan = QVector3D();
		float radius = 1;
		for (const auto& cloud : _clouds)
			if (cloud->visible &&
				std::find(_activeLayers.begin(), _activeLayers.end(), cloud->id) != _activeLayers.end())
				for (int corner = 0; corner < 8; ++corner)
				{
					QVector3D point((corner & 1) ? cloud->bbMax.x() : cloud->bbMin.x(),
						(corner & 2) ? cloud->bbMax.y() : cloud->bbMin.y(),
						(corner & 4) ? cloud->bbMax.z() : cloud->bbMin.z());
					radius = std::max(radius, (point - _center).length());
				}
		_distance = radius * 3;
		update();
	}

	/* GPU ===================================================================================== */

	void PointcloudPreviewWidget::UploadEntry(CloudEntry& entry, std::size_t& budget)
	{
		const std::size_t positionBytes = entry.pendingPositions.size() * sizeof(float);
		const std::size_t colorBytes = entry.pendingColors.size();
		const std::size_t limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
		if (positionBytes > limit || colorBytes > limit)
		{
			// QOpenGLBuffer sizes are int: a cloud this large cannot be drawn. Keep the scene usable.
			qWarning() << "PointcloudPreviewWidget: cloud" << entry.name << "is too large to display";
			entry.pendingPositions = std::vector<float>();
			entry.pendingColors = std::vector<unsigned char>();
			entry.pendingUpload = false;
			entry.vertexCount = 0;
			return;
		}
		if (!entry.vao.isCreated())
			entry.vao.create();

		// Sends up to `budget` bytes of `data` to `buffer`, allocating it on the first call.
		const auto send = [&budget](QOpenGLBuffer& buffer, const void* data, std::size_t bytes, std::size_t& uploaded)
		{
			if (!buffer.isCreated())
				buffer.create();
			buffer.bind();
			if (uploaded == 0)
			{
				buffer.setUsagePattern(QOpenGLBuffer::StaticDraw);
				buffer.allocate(nullptr, static_cast<int>(bytes));
			}
			const std::size_t count = std::min(budget, bytes - uploaded);
			buffer.write(static_cast<int>(uploaded), static_cast<const char*>(data) + uploaded, static_cast<int>(count));
			uploaded += count;
			budget -= count;
			buffer.release();
			return uploaded == bytes;
		};

		// Positions first (when this entry has its own), then colors.
		if (positionBytes > 0)
		{
			if (!entry.positions)
				entry.positions = std::make_shared<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
			if (!send(*entry.positions, entry.pendingPositions.data(), positionBytes, entry.uploadedPositionBytes))
				return;
			entry.pendingPositions = std::vector<float>();
		}
		if (colorBytes > 0)
		{
			if (budget == 0)
				return;
			if (!send(entry.colors, entry.pendingColors.data(), colorBytes, entry.uploadedColorBytes))
				return;
			entry.pendingColors = std::vector<unsigned char>();
		}
		entry.pendingUpload = false;
	}

	void PointcloudPreviewWidget::DestroyEntry(CloudEntry& entry)
	{
		if (entry.previous)
		{
			DestroyEntry(*entry.previous);
			entry.previous.reset();
		}
		if (entry.vao.isCreated())
		{
			entry.vao.destroy();
		}
		if (entry.colors.isCreated())
		{
			entry.colors.destroy();
		}
		// The position buffer may still be drawn by the entry that replaces this one.
		if (entry.positions && entry.positions.use_count() == 1 && entry.positions->isCreated())
		{
			entry.positions->destroy();
		}
		entry.positions.reset();
	}
} // namespace smcp
