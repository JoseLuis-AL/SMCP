// Takes the documentation screenshots (docs/screenshots) from the real application.
//
// It starts the application on a workspace, drives the main window and the point-cloud editor
// through a fixed script and saves a screen capture of each window, title bar included. The
// windows are shown on the desktop while it runs (about one minute); do not use the mouse or the
// keyboard meanwhile.
//
// Usage (set SMCP_SETTINGS_DIR first, so the user's settings are not touched):
//   SMCP_doc_screenshots <workspace> <output directory>
//
// The workspace needs: calibration sets (chessboard captures), calibration.yml, a full scan
// "Piece - Down - Front - 01.xyz", the scans cropped to the reference spheres
// "Spheres - Down - {Front,Left,Right} - 01.xyz" and three_sphere_artifact.json.

#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>

#include <deque>
#include <functional>
#include <iostream>

#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QInputDialog>
#include <QMessageBox>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QTreeView>
#include <QWheelEvent>

#include "app/Application.h"
#include "ui/AboutDialog.h"
#include "ui/CalibrationDialog.h"
#include "ui/ProcessingDialog.h"
#include "ui/pointcloud_editor/PlaneFitDialog.h"
#include "ui/pointcloud_editor/PointcloudEditorDialog.h"
#include "ui/pointcloud_editor/RemoveOutliersDialog.h"
#include "ui/pointcloud_editor/SphereFitDialog.h"
#include "ui/pointcloud_editor/SphereMeasurementDialog.h"
#include "ui/pointcloud_editor/SphereResultsDialog.h"

using namespace smcp;

namespace
{
	struct Step
	{
		QString name;
		std::function<bool()> ready;
		std::function<void()> run;
	};

	std::deque<Step> steps;
	QDir output;
	QString workspace;

	template <typename T>
	T* Modal()
	{
		return qobject_cast<T*>(QApplication::activeModalWidget());
	}

	PointcloudEditorDialog* Editor()
	{
		for (QWidget* widget : QApplication::topLevelWidgets())
		{
			auto* editor = qobject_cast<PointcloudEditorDialog*>(widget);
			if (editor && editor->isVisible())
			{
				return editor;
			}
		}
		return nullptr;
	}

	/// The editor is open, no dialog is over it and no command is running.
	bool EditorIdle()
	{
		PointcloudEditorDialog* editor = Editor();
		QWidget* modal = QApplication::activeModalWidget();
		return editor && (modal == editor || modal == nullptr) && editor->layers_panel->isEnabled() &&
			   !editor->preview_widget->IsWorking();
	}

	/// Keeps the window above every other one, with square corners, so the capture shows only it.
	void Front(QWidget* widget)
	{
		const HWND handle = reinterpret_cast<HWND>(widget->window()->winId());
		const DWORD square = 1; // DWMWCP_DONOTROUND
		DwmSetWindowAttribute(handle, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &square, sizeof(square));
		SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
		SetForegroundWindow(handle);
		widget->window()->activateWindow();
	}

	/// Screen capture of the visible bounds of the window (frame and title bar included).
	void Grab(QWidget* widget, const QString& name)
	{
		const HWND handle = reinterpret_cast<HWND>(widget->window()->winId());
		RECT bounds{};
		DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds));
		QScreen* screen = QGuiApplication::primaryScreen();
		const QImage desktop = screen->grabWindow(0).toImage();
		const QImage image = desktop.copy(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top);
		const QString path = output.filePath(name + ".png");
		std::cout << (image.save(path) ? "saved " : "FAILED ") << path.toStdString() << " " << image.width() << "x"
				  << image.height() << std::endl;
	}

	void Add(const QString& name, std::function<bool()> ready, std::function<void()> run)
	{
		steps.push_back({ name, std::move(ready), std::move(run) });
	}

	/// Two steps: bring the window to the front, then (one settle delay later) capture it.
	void AddShot(const QString& name, std::function<QWidget*()> window, std::function<bool()> ready)
	{
		Add("front " + name, ready, [window]() { Front(window()); });
		Add("grab " + name, ready, [window, name]() { Grab(window(), name); });
	}

	/// Starts a blocking call (a modal dialog) from the event loop, so the script keeps running.
	template <typename Object, typename Function>
	void Later(Object* object, Function function)
	{
		QTimer::singleShot(0, object, function);
	}

	void SelectLayers(const QStringList& names)
	{
		PointcloudEditorDialog* editor = Editor();
		std::vector<int> ids;
		for (const auto& layer : editor->preview_widget->Layers())
		{
			if (names.contains(layer.name))
			{
				ids.push_back(layer.id);
			}
		}
		editor->layers_panel->SelectLayers(ids);
		editor->preview_widget->FrameSelection();
	}

	/// Moves the camera closer, like turning the mouse wheel over the viewer.
	void Zoom(int notches)
	{
		PointcloudPreviewWidget* viewer = Editor()->preview_widget;
		const QPoint position = viewer->rect().center();
		for (int i = 0; i < notches; ++i)
		{
			QWheelEvent wheel(position, viewer->mapToGlobal(position), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
				Qt::NoScrollPhase, false);
			QApplication::sendEvent(viewer, &wheel);
		}
	}

	void AddLoad(const QString& file, const QString& shot)
	{
		Add("open chooser " + file, EditorIdle, []() { Later(Editor(), []() { Editor()->on_load_pointcloud_button_clicked(); }); });
		Add("choose " + file, []() { return Modal<QInputDialog>() != nullptr; },
			[file]() { Modal<QInputDialog>()->setTextValue(file); });
		if (!shot.isEmpty())
		{
			AddShot(shot, []() -> QWidget* { return Editor(); }, []() { return Modal<QInputDialog>() != nullptr; });
		}
		Add("load " + file, []() { return Modal<QInputDialog>() != nullptr; }, []() { Modal<QInputDialog>()->accept(); });
		Add("frame " + file, EditorIdle,
			[file]()
			{
				SelectLayers({ file });
				Zoom(file.startsWith("Piece") ? 6 : 2);
			});
	}

	/// Runs an editor command that asks for its parameters in a dialog of type Dialog, accepting the defaults.
	template <typename Dialog>
	void AddCommand(const QString& name, const QStringList& layers, void (PointcloudEditorDialog::*slot)(bool), const QString& shot)
	{
		Add("select for " + name, EditorIdle, [layers]() { SelectLayers(layers); });
		Add("start " + name, EditorIdle, [slot]() { Later(Editor(), [slot]() { (Editor()->*slot)(false); }); });
		Add("accept " + name, []() { return Modal<Dialog>() != nullptr; }, []() { Modal<Dialog>()->accept(); });
		AddShot(shot, []() -> QWidget* { return Editor(); }, EditorIdle);
	}
} // namespace

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::cout << "usage: SMCP_doc_screenshots <workspace> <output directory>" << std::endl;
		return 2;
	}
	if (qEnvironmentVariableIsEmpty("SMCP_SETTINGS_DIR"))
	{
		std::cout << "set SMCP_SETTINGS_DIR, so the user's settings are not modified" << std::endl;
		return 2;
	}

	Application app(argc, argv);
	workspace = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
	output.setPath(QString::fromLocal8Bit(argv[2]));
	QDir().mkpath(output.path());

	MainWindow& window = app.mainWin;
	const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
	const QSize windowSize(qMin(1600, available.width() - 80), qMin(900, available.height() - 80));
	window.showNormal();
	window.resize(windowSize);
	window.move(available.topLeft() + QPoint(30, 20));
	if (app.GetRootDir() != workspace)
	{
		app.SetRootDir(workspace);
	}
	app.calib.LoadCalibration(workspace + "/calibration.yml");

	static bool cornersDone = false;
	const auto mainIdle = [&window]() { return QApplication::activeModalWidget() == nullptr && window.isVisible(); };

	/* Main window ------------------------------------------------------------------------------ */
	Add("select image", mainIdle,
		[&app, &window]()
		{
			const QModelIndex set = app.model.index(0, 0);
			window.image_tree->expand(set);
			window.image_tree->setCurrentIndex(app.model.index(0, 0, set));
		});
	Add("extract corners", mainIdle,
		[&window]()
		{
			Later(&window,
				[&window]()
				{
					window.on_extract_corners_action_button_clicked();
					cornersDone = true;
				});
		});
	AddShot("MainWindow", [&window]() -> QWidget* { return &window; }, [mainIdle]() { return cornersDone && mainIdle(); });

	Add("open calibration", mainIdle, [&window]() { Later(&window, [&window]() { window.on_display_calibration_action_triggered(); }); });
	AddShot("CalibrationDialog", []() -> QWidget* { return Modal<CalibrationDialog>(); },
		[]() { return Modal<CalibrationDialog>() != nullptr; });
	Add("close calibration", []() { return Modal<CalibrationDialog>() != nullptr; }, []() { Modal<CalibrationDialog>()->reject(); });

	Add("open about", mainIdle, [&window]() { Later(&window, [&window]() { window.on_about_action_triggered(); }); });
	AddShot("About", []() -> QWidget* { return Modal<AboutDialog>(); }, []() { return Modal<AboutDialog>() != nullptr; });
	Add("close about", []() { return Modal<AboutDialog>() != nullptr; }, []() { Modal<AboutDialog>()->reject(); });

	/* Point cloud editor ----------------------------------------------------------------------- */
	const auto openEditor = [&window, windowSize, mainIdle]()
	{
		Add("open editor", mainIdle, [&window]() { Later(&window, [&window]() { window.on_pointcloud_action_button_clicked(); }); });
		Add("size editor", []() { return Editor() != nullptr; },
			[&window, windowSize]()
			{
				Editor()->resize(windowSize);
				Editor()->move(window.pos());
			});
	};
	const auto closeEditor = []()
	{
		Add("close editor", EditorIdle, []() { Later(Editor(), []() { Editor()->reject(); }); });
		Add("discard session", []() { return Modal<QMessageBox>() != nullptr; },
			[]() { Modal<QMessageBox>()->button(QMessageBox::Discard)->click(); });
	};
	const QString scan = "Piece - Down - Front - 01.xyz";
	const QStringList spheres{ "Spheres - Down - Front - 01.xyz", "Spheres - Down - Left - 01.xyz",
		"Spheres - Down - Right - 01.xyz" };

	openEditor();
	AddLoad(scan, "PointcloudEditorLoad");
	AddShot("PointcloudEditor", []() -> QWidget* { return Editor(); }, EditorIdle);
	AddCommand<RemoveOutliersDialog>("remove outliers", { scan }, &PointcloudEditorDialog::on_remove_outliers_action_triggered,
		"PointcloudEditorRemoveOutliers");
	AddCommand<PlaneFitDialog>("fit plane", { scan }, &PointcloudEditorDialog::on_fit_plane_action_triggered,
		"PointcloudEditorFitPlanes");
	closeEditor();

	openEditor();
	AddLoad(spheres.front(), QString());
	AddCommand<SphereFitDialog>("fit sphere", { spheres.front() }, &PointcloudEditorDialog::on_fit_sphere_action_triggered,
		"PointcloudEditorFitSpheres");
	closeEditor();

	/* Sphere measurement workflow -------------------------------------------------------------- */
	openEditor();
	for (const QString& file : spheres)
	{
		AddLoad(file, QString());
	}
	Add("select scans", EditorIdle, [spheres]() { SelectLayers(spheres); });
	AddShot("MeasureSpheresScans", []() -> QWidget* { return Editor(); }, EditorIdle);
	Add("open measure spheres", EditorIdle, []() { Later(Editor(), []() { Editor()->on_measure_spheres_action_triggered(); }); });
	Add("fill measure spheres", []() { return Modal<SphereMeasurementDialog>() != nullptr; },
		[]()
		{
			auto* dialog = Modal<SphereMeasurementDialog>();
			dialog->artifact_edit->setText(QDir::toNativeSeparators(workspace + "/three_sphere_artifact.json"));
			QMetaObject::invokeMethod(dialog, "on_artifact_edit_editingFinished");
			dialog->nominal_diameter_spin->setValue(16.0);
			dialog->artifact_edit->setCursorPosition(0);
			dialog->done_button->setFocus();
		});
	AddShot("MeasureSpheresDialog", []() -> QWidget* { return Modal<SphereMeasurementDialog>(); },
		[]() { return Modal<SphereMeasurementDialog>() != nullptr; });
	Add("run measure spheres", []() { return Modal<SphereMeasurementDialog>() != nullptr; },
		[]() { Modal<SphereMeasurementDialog>()->done_button->click(); });
	AddShot("MeasureSpheresResults", []() -> QWidget* { return Modal<SphereResultsDialog>(); },
		[]() { return Modal<SphereResultsDialog>() != nullptr; });
	Add("close results", []() { return Modal<SphereResultsDialog>() != nullptr; },
		[]() { Modal<SphereResultsDialog>()->close_button->click(); });
	AddShot("MeasureSpheresLayers", []() -> QWidget* { return Editor(); }, EditorIdle);

	/* Script runner ---------------------------------------------------------------------------- */
	QElapsedTimer sinceLast;
	sinceLast.start();
	bool running = false;
	QTimer timer;
	QObject::connect(&timer, &QTimer::timeout,
		[&]()
		{
			if (running)
			{
				return;
			}
			if (steps.empty())
			{
				std::cout << "done" << std::endl;
				TerminateProcess(GetCurrentProcess(), 0); // the modal editor is still open
			}
			// A processing window that waits for the user would stop the script.
			if (auto* processing = Modal<ProcessingDialog>())
			{
				if (sinceLast.elapsed() > 5000)
				{
					processing->accept();
				}
			}
			if (sinceLast.elapsed() > 60000)
			{
				std::cout << "TIMEOUT waiting for: " << steps.front().name.toStdString() << std::endl;
				QWidget* modal = QApplication::activeModalWidget();
				std::cout << "  modal: " << (modal ? modal->metaObject()->className() : "none") << std::endl;
				if (PointcloudEditorDialog* editor = Editor())
				{
					std::cout << "  editor panel enabled: " << editor->layers_panel->isEnabled()
							  << ", viewer working: " << editor->preview_widget->IsWorking() << std::endl;
				}
				TerminateProcess(GetCurrentProcess(), 3);
			}
			if (sinceLast.elapsed() < 900 || !steps.front().ready())
			{
				return;
			}
			running = true;
			const Step step = steps.front();
			steps.pop_front();
			std::cout << "step: " << step.name.toStdString() << std::endl;
			step.run();
			sinceLast.restart();
			running = false;
		});
	timer.start(150);
	return app.exec();
}
