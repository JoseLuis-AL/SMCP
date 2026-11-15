// Renders the themed controls and some Point Cloud Editor dialogs to PNG files, to review the
// visual style (docs/STYLE.md) without opening the application or loading data.
//
//   SMCP_style_gallery <output directory>
//
// The pictures are taken from the widgets themselves (QWidget::grab), never from the screen.

#include <iostream>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFont>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStyleFactory>
#include <QToolButton>

#include "core/PointcloudOps.h"
#include "app/TaskRunner.h"
#include "ui/AboutDialog.h"
#include "ui/ProcessingDialog.h"
#include "ui/pointcloud_editor/AiInferenceProgressDialog.h"
#include "ui/pointcloud_editor/ExportPointcloudDialog.h"
#include "ui/pointcloud_editor/IcpDialog.h"
#include "ui/pointcloud_editor/PointcloudLayersPanel.h"
#include "ui/preview/PointcloudPreviewWidget.h"
#include "ui/widgets/EditorWindowIcon.h"
#include "ui/pointcloud_editor/PlaneFitDialog.h"
#include "ui/pointcloud_editor/RemoveOutliersDialog.h"
#include "ui/pointcloud_editor/SphereFitDialog.h"
#include "ui/pointcloud_editor/SphereMeasurementDialog.h"
#include "ui/pointcloud_editor/SphereResultsDialog.h"
#include "ui/widgets/ComboBoxPopupStyle.h"
#include "ui/widgets/EditorDialogStyle.h"

namespace
{
	void Settle()
	{
		for (int i = 0; i < 5; ++i)
		{
			QApplication::processEvents();
		}
	}

	bool Save(QWidget& widget, const QString& path)
	{
		Settle();
		const bool saved = widget.grab().save(path);
		std::cout << (saved ? "saved " : "FAILED ") << path.toStdString() << std::endl;
		return saved;
	}

	/// One row of the state table: the same controls enabled, checked/primary, focused and disabled.
	void AddRow(QGridLayout* grid, int row, const QString& title, bool enabled, bool checked, bool primary)
	{
		grid->addWidget(new QLabel(title), row, 0);
		auto* button = new QPushButton(primary ? "Primary" : "Button");
		button->setProperty("primary", primary);
		auto* toggle = new QToolButton;
		toggle->setText("Toggle");
		toggle->setCheckable(true);
		toggle->setChecked(checked);
		auto* check = new QCheckBox("Check box");
		check->setChecked(checked);
		auto* radio = new QRadioButton("Radio");
		radio->setAutoExclusive(false);
		radio->setChecked(checked);
		auto* line = new QLineEdit("Text");
		auto* spin = new QSpinBox;
		spin->setValue(50);
		auto* combo = new QComboBox;
		combo->addItems({ "Option", "Other" });
		int column = 1;
		for (QWidget* widget : std::initializer_list<QWidget*>{ button, toggle, check, radio, line, spin, combo })
		{
			widget->setEnabled(enabled);
			widget->setObjectName(QString("r%1c%2").arg(row).arg(column));
			grid->addWidget(widget, row, column++);
		}
	}
} // namespace

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	if (argc < 2)
	{
		std::cout << "usage: SMCP_style_gallery <output directory>" << std::endl;
		return 2;
	}
	const QDir output(QString::fromLocal8Bit(argv[1]));
	if (!output.exists() && !QDir().mkpath(output.path()))
	{
		std::cout << "cannot create " << output.path().toStdString() << std::endl;
		return 2;
	}

	// The application theme (Application::ApplyTheme).
	new smcp::ComboBoxPopupStyle(&app);
	app.setStyle(QStyleFactory::create("Fusion"));
	QFont font("Segoe UI");
	font.setPointSizeF(9);
	app.setFont(font);
	QFile qss(":/theme/smcp.qss");
	if (!qss.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		std::cout << "theme not found" << std::endl;
		return 2;
	}
	app.setStyleSheet(QString::fromUtf8(qss.readAll()));

	bool ok = true;

	// Stand-in for the editor window: dialogs parented to it get the editor dialog rules.
	QDialog editor;
	editor.setObjectName("PointcloudEditorDialog");
	smcp::EditorDialogStyle dialogStyle(&editor);

	{	// Control states.
		QDialog states(&editor);
		states.setWindowTitle("Control states");
		auto* grid = new QGridLayout(&states);
		grid->setContentsMargins(9, 9, 9, 9);
		grid->setSpacing(6);
		AddRow(grid, 0, "Normal", true, false, false);
		AddRow(grid, 1, "Checked / primary", true, true, true);
		AddRow(grid, 2, "Disabled", false, false, false);
		AddRow(grid, 3, "Disabled checked", false, true, true);
		states.show();
		ok &= Save(states, output.filePath("control_states.png"));

		// Keyboard focus on each kind of control in turn.
		states.activateWindow();
		Settle();
		int index = 0;
		for (const char* name : { "r0c1", "r0c2", "r0c3", "r0c4", "r0c5", "r0c6", "r0c7", "r1c1", "r1c3" })
		{
			if (auto* widget = states.findChild<QWidget*>(name))
			{
				widget->setFocus(Qt::TabFocusReason);
				ok &= Save(states, output.filePath(QString("focus_%1_%2.png").arg(index++).arg(name)));
			}
		}
		std::cout << "focus applied: " << (QApplication::focusWidget() ? "yes" : "no (window not active)") << std::endl;
		states.hide();
	}

	{	// About: opened from the main window, so it has no editor parent.
		smcp::AboutDialog dialog(nullptr, Qt::WindowCloseButtonHint);
		dialog.show();
		ok &= Save(dialog, output.filePath("about_dialog.png"));
		dialog.hide();
	}
	{
		smcp::IcpDialog dialog(smcp::Pointcloud::IcpParams(), true, &editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("icp_dialog.png"));
		dialog.hide();
	}
	{
		smcp::RemoveOutliersDialog dialog(smcp::Pointcloud::OutlierRemovalParams(), &editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("remove_outliers_dialog.png"));
		dialog.hide();
	}
	{
		smcp::SphereMeasurementDialog dialog(smcp::SphereMeasurementDialog::Settings(), "sample.xyz", &editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("sphere_measurement_dialog.png"));
		dialog.hide();
	}

	{
		smcp::PlaneFitDialog dialog(smcp::Pointcloud::PlaneFitParams(), &editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("plane_fit_dialog.png"));
		dialog.hide();
	}
	{
		smcp::SphereFitDialog dialog(smcp::Pointcloud::SphereFitParams(), &editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("sphere_fit_dialog.png"));
		dialog.hide();
	}
	{
		smcp::ExportPointcloudDialog dialog(&editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("export_dialog.png"));
		dialog.hide();
	}
	{
		smcp::AiInferenceProgressDialog dialog("Model", "sample.xyz", 5000, &editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("ai_progress_dialog.png"));
		dialog.hide();
	}
	{	// The processing dialog of the main window: running, and finished with a problem in the log.
		smcp::ProcessingDialog dialog;
		dialog.setWindowTitle("Corner detection");
		dialog.Reset();
		dialog.SetCurrentMessage("Extracting corners...");
		dialog.SetProgressTotal(16);
		dialog.SetProgressValue(9);
		dialog.Message(" * 2026-oct.-29_14.36.24.024: found 408 corners");
		dialog.show();
		ok &= Save(dialog, output.filePath("processing_running.png"));
		dialog.Message(" * 2026-oct.-29_14.38.09.319: chessboard not found!");
		dialog.SetProgressValue(16);
		dialog.SetCurrentMessage("Extract corners finished");
		dialog.Finish();
		ok &= Save(dialog, output.filePath("processing_finished.png"));
		dialog.hide();
	}
	{	// The layers panel with three clouds: one highlighted, one unchecked (hidden), one recolored.
		smcp::PointcloudPreviewWidget viewer;
		viewer.resize(320, 240);
		smcp::PointcloudLayersPanel panel(&editor);
		panel.setWindowFlag(Qt::Window);
		panel.SetPreview(&viewer);
		auto cloud = std::make_shared<smcp::Pointcloud::ColorCloud>();
		cloud->push_back({ 0, 0, 100, 255, 0, 0 });
		for (const char* name : { "Ball Bar P1 - 01.xyz", "icp_Ball Bar P1 - 02 (aligned).xyz", "Ball Bar P1 - 03_straightPCF.xyz" })
		{
			viewer.AddPointcloud(cloud, QColor(), name);
			for (int i = 0; i < 20 && viewer.IsWorking(); ++i)
				Settle();
		}
		const auto wait = [&]() { for (int i = 0; i < 20 && viewer.IsWorking(); ++i) Settle(); Settle(); };
		wait();
		viewer.SetPointcloudColor(1, QColor("#e43b44"));
		wait();
		viewer.SetPointcloudVisible(2, false);
		wait();
		if (!viewer.Layers().empty())
			panel.SelectLayer(viewer.Layers()[0].id);
		wait();
		panel.resize(280, 300);
		panel.show();
		ok &= Save(panel, output.filePath("layers_panel.png"));
		panel.hide();

		// The window icon of the editor at the sizes Windows uses, side by side and enlarged.
		const QIcon icon = smcp::EditorWindowIcon();
		QPixmap sheet(16 * 4 + 32 * 4 + 64 * 2 + 40, 140);
		sheet.fill(QColor("#f3f3f3"));
		QPainter painter(&sheet);
		painter.drawPixmap(8, 8, icon.pixmap(16, 16).scaled(64, 64, Qt::KeepAspectRatio, Qt::FastTransformation));
		painter.drawPixmap(84, 8, icon.pixmap(32, 32).scaled(128, 128, Qt::KeepAspectRatio, Qt::FastTransformation));
		painter.drawPixmap(224, 8, icon.pixmap(64, 64));
		painter.drawPixmap(300, 8, icon.pixmap(16, 16));
		painter.end();
		const bool saved = sheet.save(output.filePath("editor_window_icon.png"));
		std::cout << (saved ? "saved " : "FAILED ") << output.filePath("editor_window_icon.png").toStdString() << std::endl;
		ok &= saved;
	}
	{	// Results of a two-scan campaign against a two-sphere artifact, with the scale columns shown.
		smcp::Pointcloud::MeasuredSphere a, b;
		a.center = cv::Vec3d(1, 2, 3);
		a.radius = 5.05;
		a.label = 0;
		a.formRms = 0.002;
		a.formRange = 0.005;
		a.points = std::make_shared<smcp::Pointcloud::ColorCloud>();
		b = a;
		b.center[0] += 100.2;
		b.radius = 6.01;
		b.label = 1;
		smcp::SphereScanResult first{ "position_01.xyz", "position_01.xyz", { a, b }, true };
		smcp::SphereScanResult second = first;
		second.name = "position_02.xyz";
		for (auto& sphere : second.spheres)
		{
			sphere.radius *= 1.004;
			sphere.center *= 1.004;
		}
		smcp::Pointcloud::ArtifactReference reference{ { "E1", "E2" }, { 10, 12 }, { { 0, 100 }, { 100, 0 } } };
		smcp::SphereResultsDialog dialog({ first, second }, reference, QDir::tempPath(), QString(), &editor);
		dialog.after_scale_check->setChecked(true);
		dialog.resize(1160, 640);
		dialog.show();
		dialog.results_table->selectRow(2);
		ok &= Save(dialog, output.filePath("sphere_results_dialog.png"));
		dialog.hide();
	}
	{
		smcp::BusyProgressDialog dialog("Reconstruction in progress: triangulating", &editor);
		dialog.show();
		ok &= Save(dialog, output.filePath("busy_dialog.png"));
		dialog.Finish();
	}

	return ok ? 0 : 1;
}
