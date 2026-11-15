#include "ui/pointcloud_editor/PointcloudProject.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace smcp::PointcloudProject
{
	namespace
	{
		using Viewer = PointcloudPreviewWidget;
		constexpr int MaxManifest = 16 * 1024 * 1024;
		constexpr int PointsPerChunk = 16384;
		constexpr int PointBytes = 15;
		// QOpenGLBuffer uses an int byte count for the six-float vertex buffer.
		constexpr int MaxPoints = std::numeric_limits<int>::max() / (6 * sizeof(float));
		const QByteArray Magic("SMCPPCE\0", 8);

		void Require(bool valid)
		{
			if (!valid)
				throw std::runtime_error(QObject::tr("The project is damaged or contains invalid data.").toStdString());
		}
		int Integer(const QJsonValue& value, int minimum, int maximum)
		{
			Require(value.isDouble());
			const double number = value.toDouble();
			Require(std::isfinite(number) && number >= minimum && number <= maximum && std::floor(number) == number);
			return static_cast<int>(number);
		}
		double Number(const QJsonValue& value)
		{
			Require(value.isDouble() && std::isfinite(value.toDouble()));
			return value.toDouble();
		}
		QJsonArray Vector(const QVector3D& value)
		{
			return { value.x(), value.y(), value.z() };
		}
		QVector3D ReadVector(const QJsonValue& value)
		{
			Require(value.isArray() && value.toArray().size() == 3);
			const auto array = value.toArray();
			QVector3D vector;
			for (int i = 0; i < 3; ++i)
			{
				const double component = Number(array[i]);
				Require(std::abs(component) <= std::numeric_limits<float>::max());
				vector[i] = static_cast<float>(component);
			}
			return vector;
		}
		QString Color(const QColor& color)
		{
			return color.isValid() ? color.name(QColor::HexArgb) : QString();
		}
		QColor ReadColor(const QJsonValue& value)
		{
			Require(value.isString());
			const QString text = value.toString();
			QColor color(text);
			Require(text.isEmpty() || color.isValid());
			return color;
		}
		void PackFloat(char* destination, float value)
		{
			quint32 bits;
			std::memcpy(&bits, &value, sizeof(bits));
			qToLittleEndian(bits, destination);
		}
		float UnpackFloat(const char* source)
		{
			const quint32 bits = qFromLittleEndian<quint32>(source);
			float value;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}
	}

	void Save(const QString& path, const Viewer::ProjectState& project)
	{
		Require(!project.history.empty() && project.history.size() <= 51 && project.historyPosition >= 0 &&
			project.historyPosition < static_cast<int>(project.history.size()));
		std::vector<Pointcloud::ColorCloudPtr> clouds;
		std::vector<std::shared_ptr<const std::vector<char>>> masks;
		std::map<const Pointcloud::ColorCloud*, int> cloudIds;
		std::map<const std::vector<char>*, int> maskIds;
		const auto sceneJson = [&](const Viewer::SceneState& state)
		{
			QJsonArray layers, active;
			for (const auto& cloud : state.clouds)
			{
				Require(cloud.sourceCloud && cloud.sourceCloud->size() <= MaxPoints);
				const auto added = cloudIds.emplace(cloud.sourceCloud.get(), static_cast<int>(clouds.size()));
				if (added.second)
					clouds.push_back(cloud.sourceCloud);
				int mask = -1;
				if (cloud.selection)
				{
					Require(cloud.selection->size() == cloud.sourceCloud->size());
					const auto addedMask = maskIds.emplace(cloud.selection.get(), static_cast<int>(masks.size()));
					if (addedMask.second)
						masks.push_back(cloud.selection);
					mask = addedMask.first->second;
				}
				layers.append(QJsonObject{ { "asset", added.first->second }, { "mask", mask }, { "id", cloud.id },
					{ "name", cloud.name }, { "details", cloud.details }, { "visible", cloud.visible },
					{ "override", Color(cloud.colorOverride) } });
			}
			for (int id : state.activeLayers)
				active.append(id);
			return QJsonObject{ { "layers", layers }, { "active", active } };
		};
		const auto scene = sceneJson(project.scene);
		QJsonArray history, cloudSizes, maskSizes;
		for (const auto& command : project.history)
			history.append(QJsonObject{ { "label", command.label }, { "scene", sceneJson(command.state) } });
		for (const auto& cloud : clouds)
			cloudSizes.append(static_cast<double>(cloud->size()));
		for (const auto& mask : masks)
			maskSizes.append(static_cast<double>(mask->size()));
		const QJsonObject manifest{ { "version", 1 }, { "scene", scene }, { "history", history },
			{ "position", project.historyPosition }, { "clouds", cloudSizes }, { "masks", maskSizes },
			{ "view",
				QJsonObject{ { "center", Vector(project.center) }, { "pan", Vector(project.pan) },
					{ "initialized", project.centerInitialized }, { "distance", project.distance },
					{ "rotationX", project.rotationX }, { "rotationY", project.rotationY } } } };
		const QByteArray metadata = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
		if (metadata.size() > MaxManifest)
			throw std::runtime_error(QObject::tr("The project metadata exceeds the supported size.").toStdString());

		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly))
			throw std::runtime_error(QObject::tr("Cannot save the project: %1").arg(file.errorString()).toStdString());
		QCryptographicHash hash(QCryptographicHash::Sha256);
		const auto write = [&](const char* data, int size)
		{
			if (file.write(data, size) != size)
				throw std::runtime_error(
					QObject::tr("The project could not be written completely. Check available disk space.")
						.toStdString());
			hash.addData(data, size);
		};
		QByteArray header = Magic;
		header.resize(12);
		qToLittleEndian<quint32>(metadata.size(), header.data() + 8);
		write(header.constData(), header.size());
		write(metadata.constData(), metadata.size());
		QByteArray buffer(PointsPerChunk * PointBytes, Qt::Uninitialized);
		for (const auto& cloud : clouds)
		{
			for (std::size_t begin = 0; begin < cloud->size(); begin += PointsPerChunk)
			{
				const auto count = std::min<std::size_t>(PointsPerChunk, cloud->size() - begin);
				for (std::size_t i = 0; i < count; ++i)
				{
					const auto& point = (*cloud)[begin + i];
					char* packed = buffer.data() + i * PointBytes;
					PackFloat(packed, point.x);
					PackFloat(packed + 4, point.y);
					PackFloat(packed + 8, point.z);
					packed[12] = static_cast<char>(point.r);
					packed[13] = static_cast<char>(point.g);
					packed[14] = static_cast<char>(point.b);
				}
				write(buffer.constData(), static_cast<int>(count) * PointBytes);
			}
		}
		for (const auto& mask : masks)
			write(mask->data(), static_cast<int>(mask->size()));
		const auto checksum = hash.result();
		if (file.write(checksum) != checksum.size() || !file.commit())
			throw std::runtime_error(
				QObject::tr("The project could not be saved. The previous file was preserved.").toStdString());
	}

	Viewer::ProjectState Load(const QString& path)
	{
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
			throw std::runtime_error(QObject::tr("Cannot open the project: %1").arg(file.errorString()).toStdString());
		QCryptographicHash hash(QCryptographicHash::Sha256);
		const auto read = [&](int size)
		{
			QByteArray data = file.read(size);
			Require(data.size() == size);
			hash.addData(data);
			return data;
		};
		const auto header = read(12);
		if (header.left(8) != Magic)
			throw std::runtime_error(QObject::tr("This file is not an SMCP Point Cloud Editor project.").toStdString());
		const auto metadataSize = qFromLittleEndian<quint32>(header.constData() + 8);
		Require(metadataSize > 0 && metadataSize <= MaxManifest && metadataSize <= file.size() - 44);
		const auto document = QJsonDocument::fromJson(read(metadataSize));
		Require(document.isObject());
		const auto manifest = document.object();
		if (!manifest["version"].isDouble() || manifest["version"].toDouble() != 1.0)
			throw std::runtime_error(QObject::tr("This project version is not supported.").toStdString());
		Require(manifest["clouds"].isArray() && manifest["masks"].isArray());
		std::vector<int> cloudSizes, maskSizes;
		quint64 expectedSize = 12 + metadataSize + 32;
		for (const auto value : manifest["clouds"].toArray())
		{
			const int count = Integer(value, 0, MaxPoints);
			cloudSizes.push_back(count);
			expectedSize += static_cast<quint64>(count) * PointBytes;
		}
		for (const auto value : manifest["masks"].toArray())
		{
			const int count = Integer(value, 0, MaxPoints);
			maskSizes.push_back(count);
			expectedSize += count;
		}
		Require(expectedSize == static_cast<quint64>(file.size()));
		std::vector<Pointcloud::ColorCloudPtr> clouds;
		std::vector<std::shared_ptr<const std::vector<char>>> masks;
		std::vector<std::size_t> selectedCounts;
		for (const int count : cloudSizes)
		{
			auto cloud = std::make_shared<Pointcloud::ColorCloud>(count);
			for (int begin = 0; begin < count; begin += PointsPerChunk)
			{
				const int length = std::min(PointsPerChunk, count - begin);
				const auto data = read(length * PointBytes);
				for (int i = 0; i < length; ++i)
				{
					const char* packed = data.constData() + i * PointBytes;
					auto& point = (*cloud)[begin + i];
					point.x = UnpackFloat(packed);
					point.y = UnpackFloat(packed + 4);
					point.z = UnpackFloat(packed + 8);
					point.r = static_cast<std::uint8_t>(packed[12]);
					point.g = static_cast<std::uint8_t>(packed[13]);
					point.b = static_cast<std::uint8_t>(packed[14]);
				}
			}
			clouds.push_back(std::move(cloud));
		}
		for (int count : maskSizes)
		{
			const auto data = read(count);
			Require(std::all_of(data.begin(), data.end(), [](char flag) { return flag == 0 || flag == 1; }));
			selectedCounts.push_back(std::count(data.begin(), data.end(), char(1)));
			masks.push_back(std::make_shared<const std::vector<char>>(data.begin(), data.end()));
		}
		Require(file.read(32) == hash.result() && file.atEnd());
		const auto sceneFromJson = [&](const QJsonValue& value)
		{
			Require(value.isObject());
			const auto object = value.toObject();
			Require(object["layers"].isArray() && object["active"].isArray());
			Viewer::SceneState scene;
			QSet<int> ids, visible;
			for (const auto entry : object["layers"].toArray())
			{
				Require(entry.isObject());
				const auto layer = entry.toObject();
				Viewer::CloudState cloud;
				cloud.id = Integer(layer["id"], 0, std::numeric_limits<int>::max() - 1);
				Require(!ids.contains(cloud.id) && layer["name"].isString() && layer["details"].isString() &&
					layer["visible"].isBool());
				ids.insert(cloud.id);
				cloud.visible = layer["visible"].toBool();
				if (cloud.visible)
					visible.insert(cloud.id);
				const int asset = Integer(layer["asset"], 0, static_cast<int>(clouds.size()) - 1);
				cloud.sourceCloud = clouds[asset];
				cloud.name = layer["name"].toString();
				cloud.details = layer["details"].toString();
				cloud.colorOverride = ReadColor(layer["override"]);
				const int mask = Integer(layer["mask"], -1, static_cast<int>(masks.size()) - 1);
				if (mask >= 0)
				{
					Require(maskSizes[mask] == cloudSizes[asset]);
					cloud.selection = masks[mask];
					cloud.selectedCount = selectedCounts[mask];
				}
				scene.clouds.push_back(std::move(cloud));
			}
			QSet<int> selected;
			for (const auto value : object["active"].toArray())
			{
				const int id = Integer(value, 0, std::numeric_limits<int>::max() - 1);
				Require(visible.contains(id) && !selected.contains(id));
				selected.insert(id);
				scene.activeLayers.push_back(id);
			}
			return scene;
		};
		Viewer::ProjectState project;
		project.scene = sceneFromJson(manifest["scene"]);
		Require(manifest["history"].isArray());
		const auto history = manifest["history"].toArray();
		Require(!history.isEmpty() && history.size() <= 51);
		for (const auto value : history)
		{
			Require(value.isObject() && value.toObject()["label"].isString());
			const auto command = value.toObject();
			project.history.push_back({ command["label"].toString(), sceneFromJson(command["scene"]) });
		}
		project.historyPosition = Integer(manifest["position"], 0, history.size() - 1);
		Require(manifest["view"].isObject());
		const auto view = manifest["view"].toObject();
		project.center = ReadVector(view["center"]);
		project.pan = ReadVector(view["pan"]);
		Require(view["initialized"].isBool());
		project.centerInitialized = view["initialized"].toBool();
		const double distance = Number(view["distance"]), rotationX = Number(view["rotationX"]),
					 rotationY = Number(view["rotationY"]);
		Require(distance > 0 && distance <= std::numeric_limits<float>::max() && std::abs(rotationX) <= 90 &&
			std::abs(rotationY) <= std::numeric_limits<float>::max());
		project.distance = static_cast<float>(distance);
		project.rotationX = static_cast<float>(rotationX);
		project.rotationY = static_cast<float>(rotationY);
		return project;
	}
}
