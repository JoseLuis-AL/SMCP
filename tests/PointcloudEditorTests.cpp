#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include "export/IOExport.h"
#include <QLabel>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QTreeWidget>
#include <QMouseEvent>
#include <QComboBox>
#include <QToolButton>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QImage>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QRadioButton>
#include <QLineEdit>
#include <QWheelEvent>
#include <QPlainTextEdit>
#include <QStyleFactory>
#include <QElapsedTimer>
#include <QThread>
#include <stdexcept>
#include "core/AsyncTask.h"
#include <iostream>
#include <limits>
#include <algorithm>

#include "ui/pointcloud_editor/PointcloudLayersPanel.h"
#include "ui/preview/PointcloudPreviewWidget.h"
#include "ui/widgets/ComboBoxPopupStyle.h"
#include "ui/pointcloud_editor/IcpDialog.h"
#include "ui/pointcloud_editor/SphereMeasurementDialog.h"
#include "ui/widgets/EditorDialogStyle.h"
#include "ui/pointcloud_editor/AiModelConfigDialog.h"

namespace
{
	bool Check(bool condition, const char* message)
	{
		if (!condition)
			std::cerr << "FAIL: " << message << '\n';
		return condition;
	}
	void Wait(smcp::PointcloudPreviewWidget& viewer)
	{
		QElapsedTimer timer;
		timer.start();
		while (viewer.IsWorking() && timer.elapsed() < 10000)
		{
			QApplication::processEvents();
			QThread::msleep(1);
		}
		if (viewer.IsWorking())
			throw std::runtime_error("Editor task timeout");
	}
	class Viewer : public smcp::PointcloudPreviewWidget
	{
	public:
		void BeginBox()
		{
			SetSelectionMode(true);
			QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			mousePressEvent(&press);
		}
		void EndBox()
		{
			QMouseEvent release(QEvent::MouseButtonRelease, QPointF(width() - 1, height() - 1), Qt::LeftButton,
				Qt::NoButton, Qt::NoModifier);
			mouseReleaseEvent(&release);
		}
		void SelectBox()
		{
			BeginBox();
			EndBox();
		}
		void PanAndOrbit()
		{
			for (const auto button : { Qt::RightButton, Qt::MiddleButton })
			{
				QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), button, button, Qt::NoModifier);
				QMouseEvent move(QEvent::MouseMove, QPointF(40, 20), Qt::NoButton, button, Qt::NoModifier);
				QMouseEvent release(QEvent::MouseButtonRelease, QPointF(40, 20), button, Qt::NoButton, Qt::NoModifier);
				mousePressEvent(&press);
				mouseMoveEvent(&move);
				mouseReleaseEvent(&release);
			}
		}
		void BeginBrush(const QPointF& point)
		{
			SetBrushSelectionMode(true);
			QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			mousePressEvent(&press);
		}
		void MoveBrush(const QPointF& point)
		{
			QMouseEvent move(QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
			mouseMoveEvent(&move);
		}
		void EndBrush(const QPointF& point)
		{
			QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			mouseReleaseEvent(&release);
		}
		void Brush(const QPointF& from, const QPointF& to)
		{
			BeginBrush(from);
			MoveBrush(to);
			EndBrush(to);
		}
		void Scroll(int delta, Qt::KeyboardModifiers modifiers)
		{
			QWheelEvent wheel(QPointF(320, 240), QPointF(320, 240), QPoint(), QPoint(0, delta), Qt::NoButton, modifiers,
				Qt::NoScrollPhase, false);
			wheelEvent(&wheel);
		}
		void Escape()
		{
			QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
			keyPressEvent(&escape);
		}
	};
}

bool SphereResultsChecks();
bool PointcloudProjectChecks();
bool IoExportChecks();

/// Radial (brush) selection: strokes, undo/redo and cancellation.
static bool RadialSelectionChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	Viewer radial;
	radial.resize(640, 480);
	auto cloud = std::make_shared<smcp::Pointcloud::ColorCloud>();
	for (const auto& point : std::vector<cv::Vec3f>{ { 0, 0, 100 }, { 0, 0, 102 }, { 0, 0, 98 }, { 1.5f, 0, 100 },
			 { -1.5f, 0, 100 }, { 1.5f, 1.5f, 100 }, { -1.5f, -1.5f, 100 }, { 10, 10, 100 }, { -10, -10, 100 } })
		cloud->push_back({ point[0], point[1], point[2], 255, 255, 255 });
	radial.BeginCommand("Load brush clouds");
	for (const auto* name : { "Unselected", "A", "B", "Hidden" })
		radial.AddPointcloud(cloud, QColor(), name);
	radial.EndCommand();
	Wait(radial);
	radial.SetPointcloudVisible(3, false);
	Wait(radial);
	const auto layers = radial.Layers();
	radial.SetActiveLayers({ layers[1].id, layers[2].id, layers[3].id });
	Wait(radial);
	radial.FrameSelection();
	const auto position = radial.HistoryPosition();
	radial.Brush(QPointF(320, 240), QPointF(320, 240));
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 10 && radial.HistoryPosition() == position + 1,
		"radial click selects a disk through all depths in one history command");
	ok &= Check(radial.Selection(0).empty() && radial.Selection(3).empty() && !radial.Selection(1)[5] &&
			!radial.Selection(1)[6],
		"radial selection excludes hidden/unselected clouds and points outside the circle");
	radial.Brush(QPointF(320, 240), QPointF(320, 240));
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 10 && radial.HistoryPosition() == position + 1,
		"brushing existing selection does not count points again or add a history entry");
	radial.Brush(QPointF(260, 240), QPointF(380, 240));
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 14 && radial.HistoryPosition() == position + 2,
		"radial stroke covers gaps between mouse events and adds to the existing selection");
	radial.Undo();
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 10, "one undo removes the entire last radial stroke");
	radial.Redo();
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 14, "redo restores the radial selection");
	const auto beforeCancel = radial.HistoryPosition();
	radial.BeginBrush(QPointF(455, 375));
	radial.Escape();
	radial.EndBrush(QPointF(455, 375));
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 14 && radial.HistoryPosition() == beforeCancel,
		"Escape discards an unfinished radial stroke");
	radial.BeginBrush(QPointF(455, 375));
	radial.Undo();
	Wait(radial);
	radial.EndBrush(QPointF(455, 375));
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 10 && radial.HistoryPosition() == beforeCancel - 1,
		"undo cancels a pending radial gesture");
	const auto radius = radial.BrushRadius();
	radial.BeginBrush(QPointF(320, 240));
	radial.Scroll(240, Qt::ControlModifier);
	ok &= Check(radial.BrushRadius() > radius, "Ctrl plus wheel increases the radial brush radius");
	radial.EndBrush(QPointF(320, 240));
	Wait(radial);
	ok &= Check(radial.SelectedPointCount() == 14,
		"radius changes during a stroke enlarge selection without zooming the camera");
	radial.Scroll(-120000, Qt::ControlModifier);
	ok &= Check(radial.BrushRadius() == 4, "brush radius has a usable lower bound");
	radial.Scroll(120000, Qt::ControlModifier);
	ok &= Check(radial.BrushRadius() == 256, "brush radius has a bounded upper size");
	radial.SetSelectionMode(true);
	const auto boxRadius = radial.BrushRadius();
	radial.Scroll(-120, Qt::ControlModifier);
	ok &= Check(radial.BrushRadius() == boxRadius, "Ctrl plus wheel resizes only while radial selection is active");
	return ok;
}

/// AI model configuration dialog.
static bool AiConfigurationDialogChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	const QJsonObject configuration{ { "steps", 10 } };
	const QJsonObject schema{ { "steps", QJsonObject{ { "type", "integer" }, { "minimum", 1 } } } };
	smcp::AiModelConfigDialog dialog("Test", configuration, schema, &dialogRoot);
	dialog.show();
	app.processEvents();
	auto* description = dialog.findChild<QGroupBox*>("description_group");
	auto* parameters = dialog.findChild<QGroupBox*>("parameters_group");
	auto* editor = dialog.findChild<QPlainTextEdit*>("ai_model_configuration_editor");
	auto* buttons = dialog.findChild<QDialogButtonBox*>();
	ok &= Check(description && parameters && parameters->findChild<QPlainTextEdit*>() == editor &&
			parameters->geometry().top() > description->geometry().bottom(),
		"AI configuration groups Description above Parameters");
	ok &= Check(editor->font().family() == "Cascadia Mono", "AI parameters request the Cascadia Mono font");
	editor->clearFocus();
	editor->setAttribute(Qt::WA_UnderMouse, false);
	const auto normal = editor->viewport()->grab().toImage().pixelColor(
		editor->viewport()->width() - 2, editor->viewport()->height() - 2);
	editor->setAttribute(Qt::WA_UnderMouse, true);
	app.processEvents();
	ok &= Check(normal == QColor("#ffffff") &&
			editor->viewport()->grab().toImage().pixelColor(
				editor->viewport()->width() - 2, editor->viewport()->height() - 2) == normal,
		"AI parameter editor retains its normal background on hover");
	ok &= Check(buttons->centerButtons() &&
			buttons->button(QDialogButtonBox::Ok)->width() == buttons->button(QDialogButtonBox::Cancel)->width(),
		"AI accept and cancel buttons are centered and equally wide");
	editor->setPlainText("{\"steps\":0}");
	ok &= Check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(),
		"AI schema validation remains active after the layout change");
	editor->setPlainText("{\"steps\":12}");
	ok &= Check(
		buttons->button(QDialogButtonBox::Ok)->isEnabled() && dialog.Configuration().value("steps").toInt() == 12,
		"AI edited parameters are preserved for inference");
	dialog.reject();
	return ok;
}

/// Chooser of the Add point cloud command.
static bool AddCloudChooserChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	QInputDialog chooser(&dialogRoot);
	chooser.setComboBoxItems({ "sample.xyz" });
	chooser.setOkButtonText("Load");
	chooser.show();
	app.processEvents();
	auto* buttons = chooser.findChild<QDialogButtonBox*>();
	ok &= Check(buttons->button(QDialogButtonBox::Ok)->text() == "Load" &&
			buttons->button(QDialogButtonBox::Ok)->width() == buttons->button(QDialogButtonBox::Cancel)->width(),
		"editor input dialogs match Load and Cancel widths after relabeling");
	ok &= Check(buttons->button(QDialogButtonBox::Ok)->geometry().right() + 1 <
			buttons->button(QDialogButtonBox::Cancel)->geometry().left(),
		"equal button widths retain a gap between actions");
	chooser.reject();
	QDialog confirmation(&dialogRoot);
	QVBoxLayout confirmationLayout(&confirmation);
	QDialogButtonBox confirmationButtons(QDialogButtonBox::Yes | QDialogButtonBox::Cancel);
	confirmationLayout.addWidget(&confirmationButtons);
	confirmation.show();
	app.processEvents();
	ok &= Check(confirmationButtons.button(QDialogButtonBox::Yes)->width() ==
			confirmationButtons.button(QDialogButtonBox::Cancel)->width(),
		"nested confirmations also match accept and cancel widths");
	confirmation.reject();
	return ok;
}

/// Themed controls: sizes, spin boxes and combobox popup.
static bool ControlStyleChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	QWidget controls;
	QHBoxLayout layout(&controls);
	QToolButton button;
	button.setText("Action");
	QComboBox combo;
	QSpinBox integer;
	QDoubleSpinBox decimal;
	combo.addItems({ "First", "Second", "Third" });
	layout.addWidget(&button);
	layout.addWidget(&combo);
	layout.addWidget(&integer);
	layout.addWidget(&decimal);
	controls.show();
	app.processEvents();
	ok &= Check(button.height() == combo.height(), "combobox and action button heights match");
	ok &= Check(integer.height() == button.height() && decimal.height() == button.height(),
		"numeric fields share the button and combobox height");
	QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
	app.sendEvent(&integer, &up);
	app.sendEvent(&decimal, &up);
	ok &= Check(integer.value() == 1 && decimal.value() == 1.0, "styled spin boxes preserve keyboard stepping");
	combo.showPopup();
	app.processEvents();
	auto* popup = combo.view()->window();
	ok &= Check(popup->windowFlags().testFlag(Qt::NoDropShadowWindowHint) && !popup->mask().isEmpty() &&
			!popup->mask().contains(QPoint(0, 0)),
		"combobox popup has rounded corners and disables native shadows");
	ok &= Check(popup->width() >= combo.width(), "popup width accommodates its combobox");
	ok &= Check(popup->grab().toImage().pixelColor(0, popup->height() / 2) == QColor("#bdbdbd") &&
			combo.view()->sizeHintForRow(0) == button.height() - 2,
		"popup border and row sizing match the action controls");
	QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
	QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	app.sendEvent(combo.view(), &down);
	app.sendEvent(combo.view(), &enter);
	ok &= Check(combo.currentIndex() == 1 && !popup->isVisible(), "styled popup preserves keyboard selection");
	combo.hidePopup();
	return ok;
}

/// Editor menus.
static bool EditorMenuChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	QMenu menu(&dialogRoot);
	menu.setObjectName("project_file_menu");
	auto* open = menu.addAction("Open project");
	auto* save = menu.addAction("Save project");
	bool triggered = false;
	QObject::connect(save, &QAction::triggered, [&]() { triggered = true; });
	const auto rounded = [&]()
	{
		const auto region = menu.mask();
		return !region.isEmpty() && region.contains(menu.rect().center()) &&
			!region.contains(menu.rect().topLeft()) && !region.contains(menu.rect().topRight()) &&
			!region.contains(menu.rect().bottomLeft()) && !region.contains(menu.rect().bottomRight());
	};
	menu.popup(QPoint(10, 10));
	app.processEvents();
	ok &= Check(!menu.testAttribute(Qt::WA_TranslucentBackground) &&
			menu.windowFlags().testFlag(Qt::NoDropShadowWindowHint) && rounded(),
		"project menu is opaque and clips all four native window corners");
	menu.resize(menu.width() + 40, menu.height() + 10);
	app.processEvents();
	ok &= Check(rounded(), "menu clipping follows resized popup geometry");
	menu.hide();
	menu.popup(QPoint(20, 20));
	app.processEvents();
	ok &= Check(rounded(), "menu clipping survives closing and reopening");
	menu.setActiveAction(open);
	QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
	QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
	app.sendEvent(&menu, &down);
	app.sendEvent(&menu, &enter);
	ok &= Check(triggered && !menu.isVisible(), "opaque rounded menu preserves keyboard activation");
	return ok;
}

/// ICP parameter dialog.
static bool IcpDialogChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	smcp::Pointcloud::IcpParams params;
	params.maxIterations = 75;
	smcp::IcpDialog dialog(params, true, &dialogRoot);
	dialog.show();
	app.processEvents();
	ok &= Check(
		dialog.windowTitle() == "ICP: Iterative Closest Point" && !dialog.findChild<QLabel*>("description_label"),
		"ICP dialog uses the requested title and omits the opening description");
	ok &=
		Check(dialog.done_button->width() == dialog.cancel_button->width(), "ICP action buttons have equal widths");
	const auto pointToPlane = dialog.Params().pointToPlane;
	dialog.point_to_plane_check->click();
	ok &= Check(dialog.Params().maxIterations == 75 && dialog.Params().pointToPlane != pointToPlane &&
			dialog.AlignToMerged(),
		"styled ICP fields retain parameters and checkbox toggling");
	dialog.cancel_button->click();
	ok &= Check(dialog.result() == QDialog::Rejected, "ICP Cancel does not accept settings");
	return ok;
}

/// Sphere measurement dialog.
static bool SphereMeasurementDialogChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	smcp::SphereMeasurementDialog::Settings settings;
	settings.folder = "scans";
	settings.params.nominalDiameter = 42.0;
	smcp::SphereMeasurementDialog dialog(settings, "Sample", &dialogRoot);
	dialog.show();
	app.processEvents();
	ok &= Check(dialog.description_group->title() == "Description" &&
			dialog.parameters_group->title() == "Parameters" &&
			dialog.parameters_group->findChild<QDoubleSpinBox*>("nominal_diameter_spin") ==
				dialog.nominal_diameter_spin &&
			dialog.parameters_group->geometry().top() > dialog.compensation_group->geometry().bottom(),
		"sphere measurement groups Description and Parameters after Compensation");
	const int left = dialog.done_button->geometry().left();
	const int right = dialog.cancel_button->geometry().right();
	ok &= Check(dialog.done_button->width() == dialog.cancel_button->width() &&
			dialog.done_button->geometry().right() + 1 < dialog.cancel_button->geometry().left() &&
			qAbs(left + right + 1 - dialog.width()) <= 2,
		"sphere measurement acceptance buttons have equal widths and are centered");
	ok &= Check(dialog.folder_edit->height() == dialog.folder_browse_button->height(),
		"text fields match action control heights");
	dialog.folder_radio->click();
	ok &= Check(dialog.Values().useFolder && !dialog.layer_radio->isChecked(),
		"styled source radio buttons stay exclusive");
	dialog.layer_radio->click();
	dialog.folder_edit->setText("new scans");
	ok &= Check(dialog.Values().addSphereLayers && dialog.add_layers_check->isChecked(),
		"sphere layers are added by default");
	dialog.add_layers_check->setChecked(false);
	ok &= Check(!dialog.Values().addSphereLayers, "the measurement can leave the layers as they are");
	ok &= Check(!dialog.Values().useFolder && dialog.Values().folder == "new scans" &&
			dialog.Values().params.nominalDiameter == 42.0,
		"regrouped measurement fields retain editable values and parameter units");
	dialog.cancel_button->click();
	return ok;
}

/// Merging point clouds.
static bool MergeChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	Viewer merging;
	merging.resize(640, 480);
	auto a = std::make_shared<smcp::Pointcloud::ColorCloud>();
	a->push_back({ -1, -1, 100, 255, 0, 0 });
	a->push_back({ 1, 1, 100, 0, 255, 0 });
	auto b = std::make_shared<smcp::Pointcloud::ColorCloud>();
	b->push_back({ 2, 0, 100, 0, 0, 255 });
	b->push_back({ 3, 0, 100, 30, 40, 50 });
	b->push_back({ 4, 0, 100, 60, 70, 80 });
	merging.BeginCommand("Load merge inputs");
	merging.AddPointcloud(a, QColor(), "Unselected");
	merging.AddPointcloud(a, QColor("#123456"), "A");
	merging.AddPointcloud(b, QColor(), "Hidden");
	merging.AddPointcloud(b, QColor(), "B");
	merging.AddPointcloud(a, QColor(), "Other");
	merging.EndCommand();
	Wait(merging);
	merging.SetPointcloudVisible(2, false);
	Wait(merging);
	const auto layers = merging.Layers();
	merging.SetActiveLayers({ layers[3].id, layers[1].id, layers[2].id });
	Wait(merging);
	merging.FrameSelection();
	merging.SelectBox();
	Wait(merging);
	const auto before = merging.CaptureScene();
	const auto position = merging.HistoryPosition();
	const auto center = merging.NavigationCenter();
	merging.MergeSelectedPointclouds();
	Wait(merging);
	const auto merged = merging.CaptureScene();
	ok &= Check(merged.clouds.size() == 4 && merged.clouds[1].name == "(Merged) A" &&
			merged.clouds[1].sourceCloud->size() == 5 &&
			merged.activeLayers == std::vector<int>{ merged.clouds[1].id },
		"merging replaces selected layers at their first position and selects the result");
	ok &= Check(merged.clouds[0].id == layers[0].id && merged.clouds[2].id == layers[2].id &&
			!merged.clouds[2].visible && merged.clouds[3].id == layers[4].id,
		"merging preserves hidden and unselected layers and their order");
	ok &= Check(merged.clouds[1].sourceCloud->at(0).r == 255 && merged.clouds[1].sourceCloud->at(2).b == 255 &&
			merged.clouds[1].sourceCloud->at(4).x == 4 && !merged.clouds[1].colorOverride.isValid(),
		"merging preserves original coordinates and RGB in panel order");
	ok &= Check(merging.HistoryPosition() == position + 1 && merging.NavigationCenter() == center,
		"merging commits one history command and preserves the navigation center");
	merging.Undo();
	Wait(merging);
	const auto restored = merging.CaptureScene();
	ok &= Check(restored.clouds.size() == 5 && restored.clouds[1].sourceCloud == a &&
			restored.clouds[3].sourceCloud == b && restored.clouds[1].selection == before.clouds[1].selection &&
			restored.activeLayers == before.activeLayers,
		"undo merge restores original clouds, selection masks, and selected identities");
	merging.Redo();
	Wait(merging);
	ok &=
		Check(merging.Cloud(1) == merged.clouds[1].sourceCloud, "redo merge restores the exact merged point data");
	merging.MergeSelectedPointclouds();
	Wait(merging);
	ok &=
		Check(merging.HistoryPosition() == position + 1, "one selected cloud cannot create another merge command");
	return ok;
}

/// Navigation center and framing.
static bool NavigationChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	Viewer navigation;
	auto first = std::make_shared<smcp::Pointcloud::ColorCloud>();
	first->push_back({ 0, 0, 100, 255, 0, 0 });
	first->push_back({ 0, 0, 100, 255, 0, 0 });
	first->push_back({ 9, 0, 100, 255, 0, 0 });
	navigation.AddPointcloud(first);
	Wait(navigation);
	const auto center = navigation.NavigationCenter();
	ok &= Check(cv::norm(center - cv::Vec3d(3, 0, 100)) < 1e-8,
		"navigation initializes from the centroid, not bounding-box midpoint");
	const auto beforeNavigation = navigation.HistoryPosition();
	navigation.PanAndOrbit();
	ok &= Check(navigation.NavigationCenter() == center && navigation.HistoryPosition() == beforeNavigation,
		"manual panning and orbit preserve the fixed center without changing command history");
	navigation.TransformSelected(cv::Matx44d(1, 0, 0, 1000, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), "Move");
	Wait(navigation);
	navigation.FrameSelection();
	navigation.SetActiveLayers({});
	Wait(navigation);
	ok &= Check(navigation.NavigationCenter() == center, "editing, framing, and selection keep the initial center");
	navigation.RemovePointcloud(0);
	Wait(navigation);
	auto other =
		smcp::Pointcloud::TransformCloud(*first, cv::Matx44d(1, 0, 0, 50, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1));
	navigation.AddPointcloud(other);
	Wait(navigation);
	navigation.Undo();
	Wait(navigation);
	navigation.Redo();
	Wait(navigation);
	ok &= Check(navigation.NavigationCenter() == center,
		"emptying the scene, reloading, and history keep the session's original center");
	return ok;
}

/// Centroid alignment.
static bool CentroidAlignmentChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	Viewer alignment;
	auto reference = std::make_shared<smcp::Pointcloud::ColorCloud>();
	reference->push_back({ -1, -1, 100, 255, 0, 0 });
	reference->push_back({ 1, 1, 100, 0, 255, 0 });
	const auto shifted = smcp::Pointcloud::TransformCloud(
		*reference, cv::Matx44d(1, 0, 0, 20, 0, 1, 0, -30, 0, 0, 1, 40, 0, 0, 0, 1));
	alignment.BeginCommand("Load alignment clouds");
	alignment.AddPointcloud(shifted, QColor(), "Unselected");
	alignment.AddPointcloud(reference, QColor(), "Reference");
	alignment.AddPointcloud(shifted, QColor(), "Moving A");
	alignment.AddPointcloud(shifted, QColor(), "Moving B");
	alignment.AddPointcloud(shifted, QColor(), "Hidden");
	alignment.EndCommand();
	Wait(alignment);
	alignment.SetPointcloudVisible(4, false);
	Wait(alignment);
	const auto layers = alignment.Layers();
	alignment.SetActiveLayers({ layers[3].id, layers[2].id, layers[1].id, layers[4].id });
	Wait(alignment);
	const auto position = alignment.HistoryPosition();
	alignment.AlignSelectedCentroids();
	Wait(alignment);
	ok &= Check(alignment.Cloud(1) == reference && alignment.Cloud(0) == shifted && alignment.Cloud(4) == shifted,
		"alignment leaves the reference, unselected, and hidden point data unchanged");
	ok &= Check(
		cv::norm(smcp::Pointcloud::Centroid(*alignment.Cloud(2)) - smcp::Pointcloud::Centroid(*reference)) < 1e-5 &&
			cv::norm(smcp::Pointcloud::Centroid(*alignment.Cloud(3)) - smcp::Pointcloud::Centroid(*reference)) <
				1e-5,
		"alignment uses the first selected cloud in panel order, independent of selection order");
	ok &= Check(alignment.Cloud(2)->at(0).r == shifted->at(0).r && alignment.HistoryPosition() == position + 1,
		"alignment preserves point colors and records one batch command");
	alignment.Undo();
	Wait(alignment);
	ok &= Check(
		alignment.Cloud(2) == shifted && alignment.Cloud(3) == shifted, "undo restores exact pre-alignment data");
	alignment.Redo();
	Wait(alignment);
	alignment.AlignSelectedCentroids();
	Wait(alignment);
	ok &= Check(
		alignment.HistoryPosition() == position + 1, "already aligned clouds add no redundant history command");
	return ok;
}

/// One session through the layer panel: selection, visibility, reordering, point editing, export, failures and history limits.
static bool LayerWorkflowChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	Viewer viewer;
	viewer.resize(640, 480);
	smcp::PointcloudLayersPanel panel;
	panel.SetPreview(&viewer);
	QObject::connect(&panel, &smcp::PointcloudLayersPanel::SelectedLayerChanged,
		[&]() { viewer.SetActiveLayers(panel.SelectedLayerIds()); });
	using namespace smcp::Pointcloud;
	auto cloud = std::make_shared<ColorCloud>();
	cloud->push_back({ -1, -1, 100, 255, 0, 0 });
	cloud->push_back({ 1, 1, 100, 255, 0, 0 });
	viewer.AddPointcloud(cloud, QColor(), "A");
	Wait(viewer);
	viewer.AddPointcloud(cloud, QColor(), "B");
	Wait(viewer);
	auto distant = std::make_shared<ColorCloud>();
	distant->push_back({ 0, 0, 1000, 0, 255, 0 });
	viewer.AddPointcloud(distant, QColor(), "Hidden");
	Wait(viewer);
	viewer.SetPointcloudVisible(2, false);
	Wait(viewer);
	// The tree is rebuilt after every change of the viewer, so rows are looked up again each time.
	const auto tree = [&]() { return panel.findChild<QTreeWidget*>("layer_tree"); };
	tree()->topLevelItem(0)->setSelected(true);
	Wait(viewer);
	tree()->topLevelItem(1)->setSelected(true);
	Wait(viewer);
	const auto selected = panel.SelectedLayerIds();
	ok &= Check(selected.size() == 2, "Ctrl/Shift selection can contain multiple clouds");
	ok &= Check(tree()->topLevelItem(0)->checkState(0) == Qt::Checked && tree()->topLevelItem(2)->checkState(0) == Qt::Unchecked,
		"the check of a row shows whether its cloud is visible");
	ok &= Check(!tree()->topLevelItem(0)->icon(0).isNull() && tree()->topLevelItemCount() == 3 &&
			!tree()->rootIsDecorated() && !(tree()->topLevelItem(0)->flags() & Qt::ItemIsDropEnabled),
		"rows carry a color square and cannot be nested");
	// A hidden row can be highlighted, but it is not a target of the commands.
	tree()->topLevelItem(2)->setSelected(true);
	Wait(viewer);
	ok &= Check(panel.HighlightedLayerIds().size() == 3 && panel.SelectedLayerIds() == selected,
		"hidden clouds can be highlighted but are not targets");
	ok &= Check(panel.LayersToDelete().size() == 3, "Delete removes the highlighted rows, hidden ones included");
	tree()->topLevelItem(2)->setSelected(false);
	Wait(viewer);
	ok &= Check(cv::norm(viewer.SelectionCentroid() - cv::Vec3d(0, 0, 100)) < 1e-8, "pivot is the selection centroid");
	viewer.SelectBox();
	Wait(viewer);
	ok &= Check(viewer.SelectedPointCount() == 4, "point selection covers selected visible clouds");
	viewer.SetPointcloudVisible(1, false);
	Wait(viewer);
	ok &= Check(panel.SelectedLayerIds().size() == 1 && viewer.SelectedPointCount() == 2,
		"hiding removes cloud and point selection");
	viewer.ReorderPointclouds({ 2, 1, 0 });
	Wait(viewer);
	ok &=
		Check(panel.SelectedLayerIds() == std::vector<int>{ selected[0] }, "reordering preserves selected identities");
	ok &= Check(viewer.IndexOfLayer(selected[0]) == 2, "panel order matches viewer order");
	viewer.SetPointcloudVisible(0, true);
	Wait(viewer);
	viewer.SelectBox();
	Wait(viewer);
	ok &= Check(viewer.Selection(0).empty(), "visible unselected clouds are ignored");
	viewer.SetActiveLayers({ viewer.Layers()[0].id, selected[0] });
	Wait(viewer);
	viewer.SelectBox();
	Wait(viewer);
	ok &= Check(!viewer.Selection(0).empty(), "box selection includes projected points regardless of depth");
	panel.SelectLayer(selected[0]);
	Wait(viewer);
	viewer.ReplacePointcloud(
		2, TransformCloud(*cloud, cv::Matx44d(1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)), "moved");
	Wait(viewer);
	ok &= Check(cloud->at(0).x == -1 && viewer.Cloud(2)->at(0).x == 4, "editing a copy preserves other clouds");
	ok &= Check(panel.findChild<QLabel*>("layer_title_label")->text() == "Point Clouds", "panel title has no count");

	// Checks, scope and the header buttons.
	{
		const auto before = viewer.CaptureScene();
		const auto beforeHighlight = panel.HighlightedLayerIds();
		const auto beforeTargets = panel.SelectedLayerIds();
		int deletions = 0;
		QObject::connect(&panel, &smcp::PointcloudLayersPanel::DeleteRequested, [&]() { ++deletions; });
		// A check box applies its change from the event loop, so events are processed before waiting.
		const auto settle = [&]()
		{
			for (int i = 0; i < 5; ++i)
			{
				QApplication::processEvents();
				Wait(viewer);
			}
		};

		// Unchecking a row hides its cloud; checking it shows it again.
		const int hiddenRow = 2;
		const bool wasVisible = viewer.Layers()[hiddenRow].visible;
		const int togglePosition = viewer.HistoryPosition();
		tree()->topLevelItem(hiddenRow)->setCheckState(0, wasVisible ? Qt::Unchecked : Qt::Checked);
		settle();
		ok &= Check(viewer.Layers()[hiddenRow].visible != wasVisible && viewer.HistoryPosition() == togglePosition + 1,
			"the check box of a row shows or hides its cloud as one command");
		tree()->topLevelItem(hiddenRow)->setCheckState(0, wasVisible ? Qt::Checked : Qt::Unchecked);
		settle();

		// Uncheck all and check all are one history entry each.
		const int position = viewer.HistoryPosition();
		panel.findChild<QPushButton*>("uncheck_all_button")->click();
		settle();
		bool anyVisible = false;
		for (const auto& layer : viewer.Layers())
			anyVisible |= layer.visible;
		ok &= Check(!anyVisible && viewer.HistoryPosition() == position + 1 && panel.SelectedLayerIds().empty(),
			"Uncheck all hides every cloud as one command and leaves no targets");
		panel.findChild<QPushButton*>("check_all_button")->click();
		settle();
		bool allVisible = true;
		for (const auto& layer : viewer.Layers())
			allVisible &= layer.visible;
		ok &= Check(allVisible && viewer.HistoryPosition() == position + 2, "Check all shows every cloud as one command");

		// The scope check box switches the targets between the highlighted rows and every checked row.
		panel.SelectLayers({ viewer.Layers()[0].id });
		Wait(viewer);
		ok &= Check(!panel.AppliesToAllChecked() && panel.SelectedLayerIds().size() == 1,
			"by default the targets are the highlighted rows");
		panel.findChild<QCheckBox*>("apply_checked_check")->setChecked(true);
		settle();
		ok &= Check(panel.SelectedLayerIds().size() == viewer.Layers().size() &&
				viewer.CaptureScene().activeLayers.size() == viewer.Layers().size() && panel.HighlightedLayerIds().size() == 1,
			"Apply to all checked makes every checked cloud a target without touching the highlight");
		tree()->topLevelItem(1)->setCheckState(0, Qt::Unchecked);
		settle();
		ok &= Check(panel.SelectedLayerIds().size() == viewer.Layers().size() - 1 && panel.LayersToDelete() == panel.SelectedLayerIds(),
			"an unchecked cloud leaves the targets, and Delete follows the scope");
		panel.findChild<QCheckBox*>("apply_checked_check")->setChecked(false);
		tree()->topLevelItem(1)->setCheckState(0, Qt::Checked);
		settle();

		// The Delete key only asks; the editor confirms and removes.
		tree()->setFocus();
		QKeyEvent press(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
		QApplication::sendEvent(tree(), &press);
		ok &= Check(deletions <= 1, "Delete never removes a cloud by itself");

		// Highlighting a row keeps the rows where they are: same items, same scroll, and Shift+click
		// extends the highlight from the row clicked before.
		{
			const int rows = tree()->topLevelItemCount();
			QTreeWidgetItem* const firstBefore = tree()->topLevelItem(0);
			const auto click = [&](int row, Qt::KeyboardModifiers modifiers)
			{
				const QPoint position = tree()->visualItemRect(tree()->topLevelItem(row)).center() + QPoint(40, 0);
				QMouseEvent press(QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton, modifiers);
				QMouseEvent release(QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton, modifiers);
				QApplication::sendEvent(tree()->viewport(), &press);
				QApplication::sendEvent(tree()->viewport(), &release);
				settle();
			};
			panel.resize(260, 400);
			panel.show();
			settle();
			click(0, Qt::NoModifier);
			ok &= Check(panel.HighlightedLayerIds() == std::vector<int>{ viewer.Layers()[0].id }, "a click highlights one row");
			ok &= Check(tree()->topLevelItem(0) == firstBefore && tree()->topLevelItemCount() == rows,
				"highlighting a row does not recreate the rows");
			click(rows - 1, Qt::ShiftModifier);
			ok &= Check(static_cast<int>(panel.HighlightedLayerIds().size()) == rows,
				"Shift+click highlights from the row clicked before to this one");
			click(1, Qt::NoModifier);
			ok &= Check(panel.HighlightedLayerIds() == std::vector<int>{ viewer.Layers()[1].id },
				"a plain click goes back to one highlighted row");

			// Several rows highlighted together get their check; a single hidden row does not.
			panel.findChild<QPushButton*>("uncheck_all_button")->click();
			settle();
			click(0, Qt::NoModifier);
			ok &= Check(!viewer.Layers()[0].visible && panel.HighlightedLayerIds().size() == 1,
				"highlighting one hidden row does not show it");
			const int groupPosition = viewer.HistoryPosition();
			click(rows - 1, Qt::ShiftModifier);
			bool everyVisible = true;
			for (const auto& layer : viewer.Layers())
				everyVisible &= layer.visible;
			ok &= Check(everyVisible && static_cast<int>(panel.SelectedLayerIds().size()) == rows &&
					viewer.HistoryPosition() == groupPosition + 1,
				"highlighting several rows checks them as one command and makes them the targets");
			ok &= Check(panel.findChild<QHBoxLayout*>("layer_controls")->indexOf(panel.findChild<QCheckBox*>("apply_checked_check")) == 2,
				"All, None and Apply to all share one row under the tree");
			panel.hide();
		}

		// Back to the state the rest of this session expects.
		while (viewer.HistoryPosition() > position)
		{
			viewer.Undo();
			Wait(viewer);
		}
		panel.SelectLayers(beforeHighlight);
		Wait(viewer);
		ok &= Check(viewer.CaptureScene().clouds.size() == before.clouds.size() && panel.SelectedLayerIds() == beforeTargets,
			"undo brings back the checks and the targets");
	}
	QTemporaryDir directory;
	const auto path = directory.filePath("sample.xyz");
	QString error;
	ok &= Check(smcp::IOExport::ValidateNewFiles({ path }, error), "new export destination accepted");
	ok &= Check(smcp::IOExport::WriteXyz(path.toStdString(), *cloud), "XYZ export uses a new file");
	QFile saved(path);
	saved.open(QIODevice::ReadOnly);
	const auto original = saved.readAll();
	saved.close();
	ok &= Check(
		original.startsWith("-1.000000 -1.000000 100.000000 255 0 0"), "PolyWorks XYZ uses spaces and decimal dots");
	ok &= Check(!smcp::IOExport::ValidateNewFiles({ directory.filePath("new.xyz"), path }, error),
		"whole batch rejected for an existing destination");
	ok &= Check(
		!smcp::IOExport::WriteXyz(path.toStdString(), *cloud), "writer refuses overwriting even without preflight");
	saved.open(QIODevice::ReadOnly);
	ok &= Check(saved.readAll() == original, "existing contents are unchanged");
	saved.close();
	ok &= Check(!smcp::IOExport::ValidateNewFiles({ directory.filePath("a.xyz"), directory.filePath("A.xyz") }, error),
		"duplicate Windows filenames rejected");
	const auto emptyPath = directory.filePath("empty.xyz");
	{
		QFile empty(emptyPath);
		empty.open(QIODevice::WriteOnly);
	}
	ColorCloud empty;
	ok &= Check(smcp::IOExport::ReadXyz(emptyPath.toStdString(), empty) && empty.empty(),
		"empty XYZ is recognized without inventing points");

	const auto previousSelection = viewer.CaptureScene().activeLayers;
	const auto deletionLayers = viewer.Layers();
	viewer.SetActiveLayers({ deletionLayers[0].id, deletionLayers[1].id, deletionLayers[2].id });
	Wait(viewer);
	const auto beforeCloudDeletion = viewer.CaptureScene();
	const auto deletionPosition = viewer.HistoryPosition();
	viewer.RemovePointcloud(2);
	Wait(viewer);
	ok &= Check(
		viewer.Layers().size() == 2 && !viewer.Layers()[1].visible && viewer.HistoryPosition() == deletionPosition + 1,
		"layer removal preserves other clouds and records one command");
	viewer.Undo();
	Wait(viewer);
	const auto restoredClouds = viewer.CaptureScene();
	ok &= Check(restoredClouds.clouds.size() == beforeCloudDeletion.clouds.size() &&
			restoredClouds.activeLayers == beforeCloudDeletion.activeLayers &&
			restoredClouds.clouds[2].sourceCloud == beforeCloudDeletion.clouds[2].sourceCloud,
		"undo cloud deletion restores data, order, and selected identities");
	viewer.SetActiveLayers(previousSelection);
	Wait(viewer);

	const auto beforeAbort = viewer.Layers().size();
	const auto positionBeforeAbort = viewer.HistoryPosition();
	viewer.BeginCommand("Cancelled draft");
	viewer.PrependPointcloud(cloud, QColor(), "never published");
	viewer.AbortCommand();
	ok &= Check(viewer.Layers().size() == beforeAbort && viewer.HistoryPosition() == positionBeforeAbort,
		"cancelled draft publishes no cloud and no history entry");

	const auto beforeHistory = viewer.CaptureScene();
	viewer.BeginCommand("Batch processing");
	viewer.PrependPointcloud(cloud, QColor(), "Filtered A");
	viewer.PrependPointcloud(cloud, QColor(), "AI result B");
	viewer.EndCommand();
	Wait(viewer);
	const int batchPosition = viewer.HistoryPosition();
	viewer.Undo();
	Wait(viewer);
	ok &= Check(viewer.Layers().size() == beforeHistory.clouds.size(), "one undo removes a whole processing batch");
	viewer.Redo();
	Wait(viewer);
	ok &= Check(viewer.HistoryPosition() == batchPosition && viewer.Layers().size() == beforeHistory.clouds.size() + 2,
		"redo restores processing outputs");
	viewer.SetActiveLayers({ viewer.Layers()[0].id });
	Wait(viewer);
	const auto originalPoints = viewer.Cloud(0);
	viewer.TransformSelected(cv::Matx44d(1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), "Move");
	Wait(viewer);
	viewer.Undo();
	Wait(viewer);
	ok &= Check(viewer.Cloud(0) == originalPoints, "undo restores exact shared point data");
	viewer.SelectBox();
	Wait(viewer);
	const auto selectionCount = viewer.SelectedPointCount();
	viewer.DeleteSelectedPoints();
	Wait(viewer);
	viewer.Undo();
	Wait(viewer);
	ok &= Check(viewer.SelectedPointCount() == selectionCount && viewer.Cloud(0) == originalPoints,
		"undo deletion restores points and point selection");
	viewer.BeginBox();
	viewer.Undo();
	Wait(viewer);
	const auto gestureUndoPosition = viewer.HistoryPosition();
	viewer.EndBox();
	Wait(viewer);
	ok &= Check(viewer.HistoryPosition() == gestureUndoPosition && viewer.SelectedPointCount() == 0,
		"undo cancels an unfinished selection gesture before restoring history");
	const auto failedPosition = viewer.HistoryPosition();
	bool failed = false;
	QObject::connect(&viewer, &smcp::PointcloudPreviewWidget::OperationFailed,
		[&](const QString& message) { failed = message == "injected failure"; });
	viewer.EditScene("Fail", [](smcp::PointcloudPreviewWidget::SceneState) -> smcp::PointcloudPreviewWidget::SceneState
		{ throw std::runtime_error("injected failure"); });
	Wait(viewer);
	ok &= Check(failed && viewer.HistoryPosition() == failedPosition,
		"failed worker preserves message, model, and history position");
	bool callbackFailure = false, finished = false;
	smcp::RunBackground<int>(
		&app, [] { return 1; }, [](int) { throw std::runtime_error("commit failed"); },
		[&](std::exception_ptr e)
		{
			try
			{
				std::rethrow_exception(e);
			}
			catch (const std::runtime_error&)
			{
				callbackFailure = true;
			}
		},
		[&]() { finished = true; });
	QElapsedTimer deadline;
	deadline.start();
	while (!finished && deadline.elapsed() < 10000)
	{
		QApplication::processEvents();
		QThread::msleep(1);
	}
	ok &= Check(callbackFailure && finished, "completion exception still executes final cleanup");
	for (int i = 0; i < 55; ++i)
	{
		viewer.RenamePointcloud(0, QString("history %1").arg(i));
		Wait(viewer);
	}
	ok &= Check(viewer.HistoryLabels().size() == 51 && viewer.HistoryPosition() == 50,
		"history retains exactly 50 undoable commands");
	viewer.Undo();
	Wait(viewer);
	viewer.RenamePointcloud(0, "new branch");
	Wait(viewer);
	ok &= Check(
		viewer.HistoryPosition() + 1 == viewer.HistoryLabels().size(), "new command after undo discards redo branch");
	return ok;
}

/// Selections computed by several threads on a large cloud.
static bool LargeCloudChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	// Large cloud: selections are computed by several threads and must not lose or repeat points.
	Viewer large;
	large.resize(640, 480);
	auto cloud = std::make_shared<smcp::Pointcloud::ColorCloud>();
	const int side = 700;
	cloud->reserve(static_cast<std::size_t>(side) * side + 2);
	for (int y = 0; y < side; ++y)
		for (int x = 0; x < side; ++x)
			cloud->push_back({ (x - side / 2) * 0.01f, (y - side / 2) * 0.01f, 100.0f, 255, 255, 255 });
	const std::size_t finite = cloud->size();
	cloud->push_back({ std::numeric_limits<float>::quiet_NaN(), 0.0f, 100.0f, 255, 255, 255 });
	cloud->push_back({ 0.0f, std::numeric_limits<float>::infinity(), 100.0f, 255, 255, 255 });
	large.AddPointcloud(cloud, QColor(), "Large");
	Wait(large);
	large.FrameSelection();
	large.SelectBox();
	Wait(large);
	ok &= Check(large.SelectedPointCount() == finite, "box selection of a large cloud selects every finite point once");
	const auto flags = large.Selection(0);
	ok &= Check(flags.size() == cloud->size() && !flags[finite] && !flags[finite + 1], "non-finite points are never selected");
	large.ClearSelection();
	Wait(large);
	large.Brush(QPointF(100, 240), QPointF(540, 240));
	Wait(large);
	const auto brushed = large.SelectedPointCount();
	const auto brushedFlags = large.Selection(0);
	ok &= Check(brushed > 0 && brushed < finite, "brush selects part of a large cloud");
	ok &= Check(static_cast<std::size_t>(std::count(brushedFlags.begin(), brushedFlags.end(), 1)) == brushed,
		"brush selection count matches its flags");
	large.Undo();
	Wait(large);
	large.Brush(QPointF(100, 240), QPointF(540, 240));
	Wait(large);
	ok &= Check(large.Selection(0) == brushedFlags, "the same brush stroke always selects the same points");
	large.DeleteSelectedPoints();
	Wait(large);
	ok &= Check(large.Cloud(0) && large.Cloud(0)->size() == cloud->size() - brushed, "deleting a large selection keeps the rest");
	large.Undo();
	Wait(large);
	ok &= Check(large.Cloud(0) == cloud, "undo restores the large cloud without copying it");
	return ok;
}

/// Edits submitted while the viewer is busy, access to layers by id, and the memory limit of the history.
static bool EditQueueAndHistoryChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	auto cloud = std::make_shared<smcp::Pointcloud::ColorCloud>();
	for (const float x : { -1.0f, 0.0f, 1.0f })
		cloud->push_back({ x, 0.0f, 100.0f, 255, 255, 255 });

	{
		// Three edits in a row, without waiting: the second and third wait for the first.
		Viewer queued;
		queued.resize(640, 480);
		const int first = queued.AddPointcloud(cloud, QColor(), "A");
		const int second = queued.AddPointcloud(cloud, QColor(), "B");
		queued.RenamePointcloud(1, "B renamed");
		ok &= Check(queued.IsWorking(), "the viewer reports work while edits are queued");
		Wait(queued);
		const auto layers = queued.Layers();
		ok &= Check(layers.size() == 2 && layers[0].name == "A" && layers[1].name == "B renamed",
			"edits submitted while the viewer is busy run in order instead of being dropped");
		ok &= Check(queued.HistoryPosition() == 3, "each queued edit is its own history entry");

		smcp::PointcloudPreviewWidget::LayerInfo info;
		smcp::Pointcloud::ColorCloudPtr found;
		ok &= Check(first >= 0 && second >= 0 && first != second, "adding a cloud returns the id of its layer");
		ok &= Check(queued.FindLayer(second, info, &found) && info.name == "B renamed" && found == cloud &&
				queued.IndexOfLayer(second) == 1,
			"a layer is found by its id");
		ok &= Check(!queued.FindLayer(-5, info) && !queued.FindLayer(1000, info), "unknown ids are not found");
		ok &= Check(queued.AddPointcloud(nullptr) == -1 && queued.AddPointcloud(std::make_shared<smcp::Pointcloud::ColorCloud>()) == -1,
			"null and empty clouds are rejected");

		queued.SetLayerColor(first, QColor("#123456"));
		queued.ReorderPointclouds({ 1, 0 });
		Wait(queued);
		ok &= Check(queued.FindLayer(first, info) && info.colorOverride == QColor("#123456") && queued.IndexOfLayer(first) == 1,
			"an id keeps naming the same layer after the list is reordered");

		// A failing edit drops the edits queued behind it: they assumed it would succeed.
		bool failed = false;
		QObject::connect(&queued, &smcp::PointcloudPreviewWidget::OperationFailed, [&](const QString&) { failed = true; });
		const int position = queued.HistoryPosition();
		queued.EditScene("Fail", [](smcp::PointcloudPreviewWidget::SceneState) -> smcp::PointcloudPreviewWidget::SceneState
			{ throw std::runtime_error("injected failure"); });
		queued.AddPointcloud(cloud, QColor(), "Never added");
		Wait(queued);
		ok &= Check(failed && queued.Layers().size() == 2 && queued.HistoryPosition() == position,
			"a failed edit leaves the scene and history unchanged and cancels the edits queued behind it");
		queued.AddPointcloud(cloud, QColor(), "C");
		Wait(queued);
		ok &= Check(queued.Layers().size() == 3 && !queued.IsWorking(), "the viewer accepts edits again after a failure");
	}

	const auto move = [](Viewer& viewer, int steps)
	{
		for (int i = 0; i < steps; ++i)
		{
			viewer.TransformSelected(cv::Matx44d(1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), "moved");
			Wait(viewer);
		}
	};
	{
		// Every move makes a new copy of the points. With no room for them, only the last step stays undoable.
		Viewer limited;
		limited.resize(640, 480);
		limited.SetHistoryMemoryBudget(1);
		limited.AddPointcloud(cloud, QColor(), "A");
		Wait(limited);
		move(limited, 5);
		ok &= Check(limited.HistoryLabels().size() == 2 && limited.HistoryPosition() == 1,
			"history beyond the memory budget is dropped, keeping one step to undo");
		ok &= Check(limited.Cloud(0)->at(0).x == 4.0f, "the scene itself is not affected by trimming the history");
		limited.Undo();
		Wait(limited);
		ok &= Check(limited.HistoryPosition() == 0 && limited.Cloud(0)->at(0).x == 3.0f, "the remaining step can be undone");
	}
	{
		Viewer roomy;
		roomy.resize(640, 480);
		roomy.AddPointcloud(cloud, QColor(), "A");
		Wait(roomy);
		move(roomy, 5);
		ok &= Check(roomy.HistoryLabels().size() == 7 && roomy.HistoryPosition() == 6,
			"within the memory budget every command stays undoable");
	}
	return ok;
}

/// The scene operations on their own: plain data in, plain data out, no viewer involved.
static bool SceneOperationChecks(QApplication& app, QDialog& dialogRoot)
{
	(void)app;
	(void)dialogRoot;
	bool ok = true;
	namespace Scene = smcp::EditorScene;

	const auto makeCloud = [](float offset)
	{
		auto cloud = std::make_shared<smcp::Pointcloud::ColorCloud>();
		for (const float x : { -0.5f, 0.0f, 0.5f })
			cloud->push_back({ x + offset, 0.0f, 1.0f, 10, 20, 30 });
		cloud->push_back({ std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 0, 0, 0 });
		return cloud;
	};
	const auto layer = [](int id, const smcp::Pointcloud::ColorCloudPtr& cloud, const char* name)
	{
		Scene::CloudState state;
		state.id = id;
		state.sourceCloud = cloud;
		state.name = name;
		state.vertexCount = 3;
		state.centroid = smcp::Pointcloud::Centroid(*cloud);
		return state;
	};
	const auto a = makeCloud(0.0f), b = makeCloud(10.0f);
	Scene::SceneState scene;
	scene.clouds = { layer(1, a, "A"), layer(2, b, "B"), layer(3, a, "Hidden") };
	scene.clouds[2].visible = false;
	scene.activeLayers = { 1, 3 };

	ok &= Check(Scene::IsActive(scene, scene.clouds[0]) && !Scene::IsActive(scene, scene.clouds[1]) &&
			!Scene::IsActive(scene, scene.clouds[2]),
		"a layer is active when it is selected and visible");
	ok &= Check(Scene::SameScene(scene, scene), "a scene equals itself");

	// With the identity as camera, cloud x is screen x: the rectangle x in [-0.1, 1] holds two points of A.
	const Scene::NdcRect right{ -0.1f, 1.0f, 1.0f, -1.0f };
	const auto selected = Scene::SelectInRectangle(scene, QMatrix4x4(), right, false, false);
	ok &= Check(selected.clouds[0].selectedCount == 2 && selected.clouds[0].selection &&
			*selected.clouds[0].selection == std::vector<char>({ 0, 1, 1, 0 }),
		"rectangle selection flags the finite points inside it");
	ok &= Check(!selected.clouds[1].selection && !selected.clouds[2].selection, "inactive layers are never selected");
	ok &= Check(!scene.clouds[0].selection && !Scene::SameScene(scene, selected), "the input scene is not modified");

	const Scene::NdcRect left{ -1.0f, -0.4f, 1.0f, -1.0f };
	const auto added = Scene::SelectInRectangle(selected, QMatrix4x4(), left, true, false);
	ok &= Check(added.clouds[0].selectedCount == 3, "Shift adds to the selection");
	ok &= Check(*selected.clouds[0].selection == std::vector<char>({ 0, 1, 1, 0 }), "adding builds a new mask");
	const auto removed = Scene::SelectInRectangle(added, QMatrix4x4(), left, false, true);
	ok &= Check(removed.clouds[0].selectedCount == 2, "Ctrl removes from the selection");
	ok &= Check(!Scene::ClearSelection(added).clouds[0].selection, "clearing removes every mask");

	const auto deleted = Scene::DeleteSelectedPoints(selected);
	ok &= Check(deleted.clouds[0].sourceCloud->size() == 2 && deleted.clouds[0].sourceCloud != a && a->size() == 4 &&
			!deleted.clouds[0].selection,
		"deleting points builds a new cloud and leaves the shared one intact");
	ok &= Check(deleted.clouds[2].sourceCloud == a, "other layers keep sharing the original points");
	auto all = Scene::SelectInRectangle(scene, QMatrix4x4(), Scene::NdcRect{ -1.0f, 1.0f, 1.0f, -1.0f }, false, false);
	all.clouds[0].selection = std::make_shared<std::vector<char>>(4, 1);
	all.clouds[0].selectedCount = 4;
	ok &= Check(Scene::DeleteSelectedPoints(all).clouds.size() == 2, "a layer left without points is removed");

	const auto moved = Scene::TransformActive(scene, cv::Matx44d(1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), "moved");
	ok &= Check(moved.clouds[0].sourceCloud->at(0).x == 4.5f && moved.clouds[1].sourceCloud == b &&
			moved.clouds[0].details.contains("moved"),
		"a transform changes only the active layers");

	scene.activeLayers = { 1, 2 };
	const auto aligned = Scene::AlignActiveCentroids(scene);
	ok &= Check(aligned.clouds[0].sourceCloud == a &&
			cv::norm(smcp::Pointcloud::Centroid(*aligned.clouds[1].sourceCloud) - smcp::Pointcloud::Centroid(*a)) < 1e-5,
		"centroid alignment moves the other layers onto the first");

	const auto merged = Scene::MergeActive(scene, 99);
	ok &= Check(merged.clouds.size() == 2 && merged.clouds[0].id == 99 && merged.clouds[0].sourceCloud->size() == 8 &&
			merged.activeLayers == std::vector<int>{ 99 } && merged.clouds[1].id == 3,
		"merging replaces the active layers by one layer at the first position");
	scene.activeLayers = { 1 };
	ok &= Check(Scene::SameScene(Scene::MergeActive(scene, 100), scene), "merging needs two active layers");
	ok &= Check(Scene::RemoveActive(scene).clouds.size() == 2 && Scene::RemoveActive(scene).clouds[0].id == 2,
		"removing takes out the active layers");

	// History: memory is counted once per distinct array; trimming keeps the last two entries.
	std::vector<Scene::HistoryEntry> history = { { "initial", {} }, { "load", scene }, { "move", moved } };
	const std::size_t point = sizeof(smcp::Pointcloud::ColorPoint);
	ok &= Check(Scene::HistoryBytes(history) == 3 * 4 * point, "shared point data is counted once");
	Scene::TrimHistory(history, 50, 1);
	ok &= Check(history.size() == 2 && history.back().label == "move", "the memory budget drops the oldest entries first");
	std::vector<Scene::HistoryEntry> many(10, Scene::HistoryEntry{ "x", {} });
	Scene::TrimHistory(many, 3, std::numeric_limits<std::size_t>::max());
	ok &= Check(many.size() == 4, "the count limit keeps the undoable commands plus their baseline");
	return ok;
}

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	bool ok = SphereResultsChecks();
	smcp::ComboBoxPopupStyle popupStyle(&app);
	app.setStyle(QStyleFactory::create("Fusion"));
	QFile stylesheet(":/theme/smcp.qss");
	stylesheet.open(QIODevice::ReadOnly);
	app.setStyleSheet(QString::fromUtf8(stylesheet.readAll()));
	ok &= PointcloudProjectChecks();
	ok &= IoExportChecks();
	QDialog dialogRoot;
	smcp::EditorDialogStyle dialogStyle(&dialogRoot);
	// Each group builds its own viewer and data, so a failure in one does not affect the others.
	const struct
	{
		const char* name;
		bool (*run)(QApplication&, QDialog&);
	} groups[] = {
		{ "RadialSelectionChecks", RadialSelectionChecks },
		{ "AiConfigurationDialogChecks", AiConfigurationDialogChecks },
		{ "AddCloudChooserChecks", AddCloudChooserChecks },
		{ "ControlStyleChecks", ControlStyleChecks },
		{ "EditorMenuChecks", EditorMenuChecks },
		{ "IcpDialogChecks", IcpDialogChecks },
		{ "SphereMeasurementDialogChecks", SphereMeasurementDialogChecks },
		{ "MergeChecks", MergeChecks },
		{ "NavigationChecks", NavigationChecks },
		{ "CentroidAlignmentChecks", CentroidAlignmentChecks },
		{ "LayerWorkflowChecks", LayerWorkflowChecks },
		{ "LargeCloudChecks", LargeCloudChecks },
		{ "EditQueueAndHistoryChecks", EditQueueAndHistoryChecks },
		{ "SceneOperationChecks", SceneOperationChecks },
	};
	for (const auto& group : groups)
	{
		bool passed = false;
		try
		{
			passed = group.run(app, dialogRoot);
		}
		catch (const std::exception& e)
		{
			std::cerr << "FAIL: " << group.name << " threw: " << e.what() << '\n';
		}
		if (!passed)
			std::cerr << "FAILED GROUP: " << group.name << '\n';
		ok &= passed;
	}

	if (ok)
		std::cout << "Point cloud editor selection tests passed.\n";
	return ok ? 0 : 1;
}
