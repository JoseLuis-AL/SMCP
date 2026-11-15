#include "ui/pointcloud_editor/PointcloudProject.h"
#include "ui/pointcloud_editor/AiInferenceProgressDialog.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLocale>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

bool PointcloudProjectChecks()
{
	using namespace smcp;
	using Viewer = PointcloudPreviewWidget;
	bool ok = true;
	const auto check = [&](bool condition, const char* message)
	{
		if (!condition)
			std::cerr << "FAIL: " << message << '\n';
		ok &= condition;
	};
	const auto wait = [](Viewer& viewer)
	{
		QElapsedTimer deadline;
		deadline.start();
		while (viewer.IsWorking() && deadline.elapsed() < 10000)
		{
			QApplication::processEvents();
			QThread::msleep(1);
		}
		if (viewer.IsWorking())
			throw std::runtime_error("Project preparation timeout");
	};
	QTemporaryDir folder;
	Viewer original;
	auto cloud = std::make_shared<Pointcloud::ColorCloud>();
	cloud->push_back({ -1.25f, 2.5f, 3.75f, 11, 22, 33 });
	cloud->push_back({ 4, 5, 6, 44, 55, 66 });
	cloud->push_back({ 7, 8, 9, 77, 88, 99 });
	cloud->push_back({ std::numeric_limits<float>::quiet_NaN(), 0, 0, 1, 2, 3 });
	for (const QString& name : { "A", "B", "C", "Hidden" })
	{
		original.AddPointcloud(cloud, QColor(), name, "source & details");
		wait(original);
	}
	original.SetPointcloudVisible(3, false);
	wait(original);
	original.SetPointcloudColor(1, QColor("#123456"));
	wait(original);
	const auto layers = original.Layers();
	original.SetActiveLayers({ layers[0].id, layers[1].id, layers[2].id });
	wait(original);
	original.EditScene("Select points",
		[](Viewer::SceneState state)
		{
			state.clouds[0].selection = std::make_shared<const std::vector<char>>(std::vector<char>{ 1, 0, 0, 0 });
			state.clouds[0].selectedCount = 1;
			return state;
		});
	wait(original);
	original.TransformSelected(cv::Matx44d(1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1), "Move");
	wait(original);
	original.Undo();
	wait(original);
	auto project = original.CaptureProject();
	project.center = { 1.25f, -2.5f, -3.75f };
	project.pan = { 4, -5, 6 };
	project.distance = 123.25f;
	project.rotationX = 30;
	project.rotationY = -45;
	const QString path = folder.filePath("session.smcppce");
	PointcloudProject::Save(path, project);
	auto restored = PointcloudProject::Load(path);
	check(restored.history.size() == project.history.size() && restored.historyPosition == project.historyPosition &&
			restored.historyPosition + 1 < static_cast<int>(restored.history.size()),
		"project retains the complete history and redo position");
	check(restored.scene.clouds.size() == 4 && restored.scene.clouds[3].visible == false &&
			restored.scene.clouds[1].colorOverride == QColor("#123456") &&
			restored.scene.activeLayers == project.scene.activeLayers,
		"project preserves order, visibility, colors and selected layers");
	check(restored.scene.clouds[0].sourceCloud == restored.scene.clouds[1].sourceCloud &&
			restored.scene.clouds[0].sourceCloud ==
				restored.history[restored.historyPosition].state.clouds[0].sourceCloud,
		"shared point data is restored once across layers and history");
	const auto& point = restored.scene.clouds[0].sourceCloud->front();
	check(point.x == -1.25f && point.y == 2.5f && point.z == 3.75f && point.r == 11 && point.g == 22 && point.b == 33 &&
			std::isnan((*restored.scene.clouds[0].sourceCloud)[3].x),
		"project preserves float coordinates and RGB bytes, including nonfinite source points");
	Viewer loaded;
	loaded.RestoreProject(restored);
	wait(loaded);
	const auto loadedLabels = loaded.HistoryLabels(), originalLabels = original.HistoryLabels();
	check(std::equal(loadedLabels.begin(), loadedLabels.end(), originalLabels.begin(), originalLabels.end()) &&
			loaded.HistoryPosition() == original.HistoryPosition() && loaded.SelectedPointCount() == 1 &&
			loaded.Selection(0) == std::vector<char>({ 1, 0, 0, 0 }),
		"installed project restores undo labels, position and point selection");
	const auto view = loaded.CaptureProject();
	check(view.center == project.center && view.pan == project.pan && view.distance == project.distance &&
			view.rotationX == project.rotationX && view.rotationY == project.rotationY,
		"project restores the fixed center, pan, zoom and orbit");
	loaded.Redo();
	wait(loaded);
	check(loaded.Cloud(0)->front().x == 3.75f, "redo works after reopening without rerunning the original command");
	loaded.Undo();
	wait(loaded);
	check(
		loaded.Cloud(0)->front().x == -1.25f && loaded.SelectedPointCount() == 1 && loaded.Layers()[0].pointCount == 3,
		"undo after reopening restores exact points, selection and render metadata");
	loaded.Undo();
	wait(loaded);
	check(loaded.SelectedPointCount() == 0 && loaded.Layers()[1].pointCount == 3,
		"restored metadata-only history keeps valid rendering caches for unchanged clouds");
	loaded.Redo();
	wait(loaded);
	const int position = loaded.HistoryPosition();
	loaded.RemoveSelectedPointclouds();
	wait(loaded);
	check(
		loaded.Layers().size() == 1 && loaded.Layers()[0].name == "Hidden" && loaded.HistoryPosition() == position + 1,
		"batch deletion removes selected visible layers in one command");
	loaded.Undo();
	wait(loaded);
	check(loaded.Layers().size() == 4 && loaded.SelectedPointCount() == 1 &&
			loaded.CaptureScene().activeLayers == project.scene.activeLayers,
		"one undo restores a whole batch deletion, layer selection and point selection");
	loaded.AddPointcloud(cloud, QColor(), "New");
	wait(loaded);
	std::set<int> ids;
	for (const auto& layer : loaded.Layers())
		ids.insert(layer.id);
	check(ids.size() == 5, "new layers cannot collide with restored layer IDs");
	const auto good = [&]()
	{
		QFile file(path);
		file.open(QIODevice::ReadOnly);
		return file.readAll();
	}();
	const QString brokenPath = folder.filePath("broken.smcppce");
	const auto rejects = [&](QByteArray bytes)
	{
		QFile file(brokenPath);
		file.open(QIODevice::WriteOnly);
		file.write(bytes);
		file.close();
		try
		{
			PointcloudProject::Load(brokenPath);
			return false;
		}
		catch (const std::exception&)
		{
			return true;
		}
	};
	QByteArray changed = good;
	changed[changed.size() - 33] = static_cast<char>(changed.at(changed.size() - 33) ^ 1);
	check(rejects(changed) && rejects(good.left(32)) && rejects("not a project"),
		"damaged, truncated and foreign project files are rejected");
	const auto mutate = [&](const std::function<void(QJsonObject&)>& change)
	{
		const int length = qFromLittleEndian<quint32>(good.constData() + 8);
		auto object = QJsonDocument::fromJson(good.mid(12, length)).object();
		change(object);
		const auto metadata = QJsonDocument(object).toJson(QJsonDocument::Compact);
		QByteArray bytes = good.left(12);
		qToLittleEndian<quint32>(metadata.size(), bytes.data() + 8);
		bytes += metadata;
		bytes += good.mid(12 + length, good.size() - (12 + length) - 32);
		bytes += QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
		return bytes;
	};
	check(rejects(mutate([](QJsonObject& object) { object["version"] = 2; })) &&
			rejects(mutate([](QJsonObject& object) { object["position"] = 999; })) &&
			rejects(mutate([](QJsonObject& object) { object["clouds"] = QJsonArray{ 1000000000000.0 }; })),
		"unsupported versions, invalid history and malicious counts are rejected before installation");
	check(loaded.Layers().size() == 5, "failed project reads leave the live session intact");
	PointcloudProject::Save(path, loaded.CaptureProject());
	check(
		PointcloudProject::Load(path).scene.clouds.size() == 5, "saving a project again atomically updates its state");
	Viewer empty;
	PointcloudProject::Save(path, empty.CaptureProject());
	check(PointcloudProject::Load(path).scene.clouds.empty(), "empty projects can be saved and reopened");
	Viewer large;
	auto many = std::make_shared<Pointcloud::ColorCloud>(100001, Pointcloud::ColorPoint{ 1, 2, 3, 4, 5, 6 });
	large.AddPointcloud(many, QColor(), "Large");
	wait(large);
	for (int i = 0; i < 55; ++i)
	{
		large.RenamePointcloud(0, QString("Large %1").arg(i));
		wait(large);
	}
	PointcloudProject::Save(path, large.CaptureProject());
	const auto largeProject = PointcloudProject::Load(path);
	check(largeProject.history.size() == 51 && largeProject.scene.clouds[0].sourceCloud->size() == 100001 &&
			QFileInfo(path).size() < 2000000,
		"50-command history stores a shared large cloud once and streams across chunk boundaries");

	AiInferenceProgressDialog ai("PointCleanNet", "Selected cloud", 123456);
	ai.ensurePolished();
	check(ai.title_label->text() == "Thinking" && ai.title_label->font().pointSize() == 21 && ai.icon_label->pixmap() &&
			!ai.icon_label->pixmap()->isNull(),
		"AI progress uses the supplied icon and Fluent title size");
	check(ai.message_label->text().contains("Selected cloud") &&
			ai.message_label->text().contains(QLocale().toString(123456)) &&
			ai.progress_group->isAncestorOf(ai.activity_bar) && ai.progress_group->isAncestorOf(ai.output_label),
		"AI progress identifies the actual cloud and point count in its grouped layout");
	AiInferenceProgressDialog single("Model", "Only cloud", 10);
	single.SetBatchPosition(1, 1);
	check(!single.message_label->text().contains("Cloud 1 of 1"), "a single cloud shows no batch position");
	ai.SetBatchPosition(2, 5);
	check(ai.message_label->text().startsWith("Cloud 2 of 5.") && ai.message_label->text().contains("Selected cloud"),
		"a batch shows which cloud is being processed");
	ai.SetOutput("<model output>");
	check(ai.output_label->text() == "<model output>" && ai.output_label->textFormat() == Qt::PlainText,
		"model output remains literal text below the progress bar");
	int cancellations = 0;
	QObject::connect(&ai, &AiInferenceProgressDialog::CancelRequested, [&]() { ++cancellations; });
	QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	QApplication::sendEvent(&ai, &escape);
	check(cancellations == 0, "Escape does not accidentally cancel AI inference");
	ai.cancel_button->click();
	ai.SetCancelling();
	ai.reject();
	check(
		cancellations == 1 && !ai.cancel_button->isEnabled(), "AI cancellation remains guarded while the model stops");
	return ok;
}
