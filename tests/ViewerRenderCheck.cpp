// Rendering check of the point cloud viewer on a real OpenGL context (the CTest suite runs on the
// offscreen platform, which has none). It only uses a synthetic cloud and shows a window briefly.
//
//   SMCP_viewer_render_check
//
// Frames are rendered one at a time with grabFramebuffer(), so the check can look at the frame
// right after an edit and tell what needed new vertex data: the cloud (5.3 million points, 63 MB
// of positions and 21 MB of colors) is larger than one frame's upload budget for positions but not
// for colors.

#include <cmath>
#include <iostream>

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>
#include <QThread>
#include <QWheelEvent>

#include "ui/preview/PointcloudPreviewWidget.h"

namespace
{
	int failures = 0;
	void Check(bool condition, const char* what)
	{
		std::cout << (condition ? "  ok    " : "  FAIL  ") << what << std::endl;
		failures += condition ? 0 : 1;
	}

	struct Pixels
	{
		int white = 0, orange = 0, red = 0;
		double whiteX = 0;  ///< Mean x of the white pixels.
		int drawn() const { return white + orange + red; }
	};

	/// Classifies the pixels of the 3D view (the overlay with the axis selector is not part of it).
	Pixels Count(const QImage& image)
	{
		Pixels pixels;
		double sumX = 0;
		for (int y = 0; y < image.height(); ++y)
		{
			for (int x = 0; x < image.width(); ++x)
			{
				const QColor color = image.pixelColor(x, y);
				if (color.red() > 240 && color.green() > 240 && color.blue() > 240)
				{
					++pixels.white;
					sumX += x;
				}
				else if (color.red() > 240 && color.green() > 120 && color.green() < 160 && color.blue() < 20)
					++pixels.orange;
				else if (color.red() > 240 && color.green() < 20 && color.blue() < 20)
					++pixels.red;
			}
		}
		pixels.whiteX = pixels.white ? sumX / pixels.white : 0;
		return pixels;
	}

	void Wait(smcp::PointcloudPreviewWidget& viewer)
	{
		QElapsedTimer timer;
		timer.start();
		while (viewer.IsWorking() && timer.elapsed() < 120000)
		{
			QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
			QThread::msleep(1);
		}
	}

	void Rest(int milliseconds)
	{
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < milliseconds)
		{
			QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
			QThread::msleep(5);
		}
	}

	class Viewer : public smcp::PointcloudPreviewWidget
	{
	public:
		/// Rectangle selection over the whole widget.
		void SelectAll()
		{
			SetSelectionMode(true);
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			mousePressEvent(&press);
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(width() - 1, height() - 1), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			mouseReleaseEvent(&release);
			SetSelectionMode(false);
		}
		/// A middle-button drag that does not move: the camera stays put but counts as navigating.
		void TouchNavigation()
		{
			const QPointF point(100, 100);
			QMouseEvent press(QEvent::MouseButtonPress, point, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
			mousePressEvent(&press);
			QMouseEvent move(QEvent::MouseMove, point, Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
			mouseMoveEvent(&move);
			QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
			mouseReleaseEvent(&release);
		}
		/// Zooms in with the wheel: every notch brings the camera about 10% closer.
		void ZoomIn(int notches)
		{
			QWheelEvent wheel(QPointF(320, 240), QPointF(320, 240), QPoint(), QPoint(0, 120 * notches), Qt::NoButton,
				Qt::NoModifier, Qt::NoScrollPhase, false);
			wheelEvent(&wheel);
		}
	};

	/// Renders frames until `done` accepts one (uploads take several frames) and returns it.
	template <typename Done> Pixels Settle(Viewer& viewer, Done done)
	{
		Pixels pixels = Count(viewer.grabFramebuffer());
		for (int frame = 0; frame < 40 && !done(pixels); ++frame)
			pixels = Count(viewer.grabFramebuffer());
		return pixels;
	}

	double FrameMilliseconds(Viewer& viewer, bool navigating)
	{
		QElapsedTimer timer;
		double total = 0;
		const int frames = 5;
		for (int i = 0; i < frames; ++i)
		{
			if (navigating)
				viewer.TouchNavigation();
			timer.restart();
			viewer.grabFramebuffer();
			total += timer.nsecsElapsed() / 1e6;
		}
		return total / frames;
	}
} // namespace

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	Viewer viewer;
	viewer.setUpdatesEnabled(false);  // frames are rendered only by grabFramebuffer(), one at a time
	viewer.resize(640, 480);
	viewer.show();
	QApplication::processEvents();
	if (!viewer.context() || !viewer.context()->isValid())
	{
		std::cout << "No OpenGL context: nothing checked." << std::endl;
		return 2;
	}

	auto cloud = std::make_shared<smcp::Pointcloud::ColorCloud>();
	const int side = 2300;
	cloud->reserve(static_cast<std::size_t>(side) * side);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x)
			cloud->push_back({ (x - side / 2) * 0.05f, (y - side / 2) * 0.05f, 300.0f, 255, 255, 255 });

	const int layer = viewer.AddPointcloud(cloud, QColor(), "Synthetic");
	Wait(viewer);
	viewer.FrameSelection();

	/* Loading ------------------------------------------------------------------------------- */
	const Pixels loading = Count(viewer.grabFramebuffer());
	Check(loading.drawn() == 0, "a new cloud is not drawn from partially uploaded buffers");
	const Pixels loaded = Settle(viewer, [](const Pixels& p) { return p.white > 0; });
	Check(loaded.white > 10000 && loaded.orange == 0 && loaded.red == 0, "the cloud is drawn with its own colors once uploaded");

	/* Layer color: a shader parameter, no vertex data ------------------------------------------ */
	viewer.SetLayerColor(layer, QColor(255, 0, 0));
	Wait(viewer);
	const Pixels recolored = Count(viewer.grabFramebuffer());
	Check(recolored.red == loaded.white && recolored.white == 0, "a layer color shows in the very next frame, on the same pixels");
	viewer.SetLayerColor(layer, QColor());
	Wait(viewer);
	const Pixels restored = Count(viewer.grabFramebuffer());
	Check(restored.white == loaded.white && restored.red == 0, "removing the layer color restores the point colors at once");

	/* Selection: only the color buffer is sent again -------------------------------------------- */
	viewer.SelectAll();
	Wait(viewer);
	Check(viewer.SelectedPointCount() == cloud->size(), "every point selected");
	const Pixels selected = Count(viewer.grabFramebuffer());
	Check(selected.orange == loaded.white && selected.white == 0,
		"a selection shows in the very next frame: positions are not sent again");
	viewer.SetLayerColor(layer, QColor(255, 0, 0));
	Wait(viewer);
	const Pixels selectedRecolored = Count(viewer.grabFramebuffer());
	Check(selectedRecolored.orange == loaded.white && selectedRecolored.red == 0, "selected points keep the selection color over a layer color");
	viewer.SetLayerColor(layer, QColor());
	viewer.ClearSelection();
	Wait(viewer);
	const Pixels cleared = Count(viewer.grabFramebuffer());
	Check(cleared.white == loaded.white && cleared.orange == 0, "clearing the selection restores the point colors at once");

	/* New points: positions take several frames; the previous buffers stay on screen ------------- */
	const double shift = 0.25 * side * 0.05;  // a quarter of the cloud width, to the right
	viewer.TransformSelected(cv::Matx44d(1, 0, 0, shift, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), "moved");
	Wait(viewer);
	const Pixels moving = Count(viewer.grabFramebuffer());
	Check(moving.white == loaded.white && std::abs(moving.whiteX - loaded.whiteX) < 1.0,
		"while moved points upload, the cloud is still drawn where it was (no blink)");
	const Pixels moved = Settle(viewer, [&](const Pixels& p) { return p.whiteX > loaded.whiteX + 20; });
	Check(moved.whiteX > loaded.whiteX + 20 && moved.white > 10000, "the moved cloud replaces it once uploaded");

	/* Edits one after another ------------------------------------------------------------------ */
	const int position = viewer.HistoryPosition();
	viewer.SelectAll();       // starts at once
	viewer.ClearSelection();  // waits behind it in the viewer's queue
	Wait(viewer);
	const Pixels chained = Settle(viewer, [](const Pixels& p) { return p.white > 0 && p.orange == 0; });
	Check(viewer.HistoryPosition() == position + 2 && chained.white == moved.white && chained.orange == 0,
		"an edit queued behind another runs after it, and the last state is the one drawn");
	viewer.Undo();
	Wait(viewer);
	const Pixels undone = Settle(viewer, [](const Pixels& p) { return p.orange > 0; });
	Check(undone.orange == moved.white && undone.white == 0, "undo draws the previous state (everything selected)");
	viewer.Undo();
	Wait(viewer);
	const Pixels undoneTwice = Settle(viewer, [](const Pixels& p) { return p.white > 0 && p.orange == 0; });
	Check(undoneTwice.white == moved.white && undoneTwice.orange == 0, "a second undo draws the state before the selection");

	/* Navigation: fewer points while the camera moves, all of them again at rest ------------------ */
	const double fullMs = FrameMilliseconds(viewer, false);
	const double movingMs = FrameMilliseconds(viewer, true);
	Rest(400);
	std::cout << "  frame time with " << cloud->size() << " points: " << fullMs << " ms at rest, " << movingMs
			  << " ms while navigating" << std::endl;

	// Close enough for single points to be told apart, so the number of points drawn can be counted.
	viewer.ZoomIn(37);
	Rest(400);
	const Pixels closeAtRest = Count(viewer.grabFramebuffer());
	viewer.TouchNavigation();
	const Pixels closeMoving = Count(viewer.grabFramebuffer());
	Check(closeAtRest.white > 500, "points are visible up close");
	Check(closeMoving.white > 0 && closeMoving.white < closeAtRest.white * 0.6,
		"while the camera moves a very large cloud is drawn with fewer points");
	Rest(400);
	const Pixels closeRested = Count(viewer.grabFramebuffer());
	Check(closeRested.white == closeAtRest.white, "every point is drawn again once the camera rests");

	viewer.hide();
	std::cout << (failures ? "RESULT: FAILED" : "RESULT: PASSED") << std::endl;
	return failures ? 1 : 0;
}
