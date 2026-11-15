#include "ui/preview/EditorScene.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <stdexcept>

#include <QImage>
#include <QObject>
#include <QPainter>
#include <QPen>
#include <QStringList>
#include <QVector4D>

#include "core/Parallel.h"

namespace smcp::EditorScene
{
	namespace
	{
		/// Points projected per thread at least when a selection is computed.
		constexpr long long Min_Points_Per_Thread = 100000;

		void Unselect(CloudState& cloud)
		{
			cloud.selection.reset();
			cloud.selectedCount = 0;
		}
	} // namespace

	bool IsActive(const SceneState& scene, const CloudState& cloud)
	{
		return cloud.visible &&
			std::find(scene.activeLayers.begin(), scene.activeLayers.end(), cloud.id) != scene.activeLayers.end();
	}

	bool SameScene(const SceneState& a, const SceneState& b)
	{
		if (a.activeLayers != b.activeLayers || a.clouds.size() != b.clouds.size())
			return false;
		for (std::size_t i = 0; i < a.clouds.size(); ++i)
		{
			const auto& x = a.clouds[i];
			const auto& y = b.clouds[i];
			if (x.id != y.id || x.sourceCloud != y.sourceCloud || x.name != y.name || x.details != y.details ||
				x.colorOverride != y.colorOverride || x.visible != y.visible || x.selection != y.selection)
				return false;
		}
		return true;
	}

	/* Selection =============================================================================== */

	SceneState SelectInRectangle(SceneState scene, const QMatrix4x4& matrix, const NdcRect& rect, bool add, bool remove)
	{
		for (auto& c : scene.clouds)
		{
			const bool active = IsActive(scene, c);
			if (!add && !remove)
			{
				Unselect(c);
			}
			if (!active || !c.sourceCloud)
				continue;
			auto selection = std::make_shared<std::vector<char>>(
				c.selection ? *c.selection : std::vector<char>(c.sourceCloud->size(), 0));
			// Points are independent and each one owns its flag, so they are tested in parallel.
			const Pointcloud::ColorPoint* points = c.sourceCloud->data();
			char* flags = selection->data();
			Parallel::ParallelFor(static_cast<long long>(c.sourceCloud->size()), Min_Points_Per_Thread,
				[&](long long first, long long last)
				{
					for (long long i = first; i < last; ++i)
					{
						const auto& p = points[i];
						if (!Pointcloud::IsFinite(p))
							continue;
						const auto projected = matrix * QVector4D(p.x, -p.y, -p.z, 1);
						if (projected.w() <= 1e-6f)
							continue;
						const float x = projected.x() / projected.w(), y = projected.y() / projected.w();
						if (x >= rect.left && x <= rect.right && y >= rect.bottom && y <= rect.top)
							flags[i] = remove ? 0 : 1;
					}
				});
			c.selectedCount = std::count(selection->begin(), selection->end(), 1);
			c.selection = c.selectedCount ? selection : nullptr;
		}
		return scene;
	}

	SceneState SelectBrushStroke(SceneState scene, const std::vector<BrushSample>& stroke, const QMatrix4x4& matrix,
		const QSize& viewport)
	{
		// The stroke is rasterized once into a screen mask (discs joined by thick lines); each point
		// is then projected once and looked up in it, whatever the number of mouse samples.
		QImage mask(viewport, QImage::Format_ARGB32_Premultiplied);
		if (mask.isNull())
			throw std::runtime_error("Cannot allocate the radial selection mask");
		mask.fill(Qt::transparent);
		{
			QPainter painter(&mask);
			for (std::size_t i = 0; i < stroke.size(); ++i)
			{
				const auto& sample = stroke[i];
				painter.setPen(Qt::NoPen);
				painter.setBrush(Qt::white);
				painter.drawEllipse(sample.position, sample.radius, sample.radius);
				if (i)
				{
					painter.setPen(QPen(Qt::white, 2 * sample.radius, Qt::SolidLine, Qt::RoundCap));
					painter.drawLine(stroke[i - 1].position, sample.position);
				}
			}
		}
		for (auto& cloud : scene.clouds)
		{
			if (!IsActive(scene, cloud) || !cloud.sourceCloud)
				continue;
			auto selected = std::make_shared<std::vector<char>>(
				cloud.selection ? *cloud.selection : std::vector<char>(cloud.sourceCloud->size(), 0));
			// The mask is only read and each point owns its flag: points are tested in parallel.
			std::atomic<std::size_t> addedTotal{ 0 };
			const Pointcloud::ColorPoint* points = cloud.sourceCloud->data();
			char* flags = selected->data();
			const QImage& readMask = mask;
			Parallel::ParallelFor(static_cast<long long>(cloud.sourceCloud->size()), Min_Points_Per_Thread,
				[&](long long first, long long last)
				{
					std::size_t count = 0;
					for (long long i = first; i < last; ++i)
					{
						const auto& point = points[i];
						if (flags[i] || !Pointcloud::IsFinite(point))
							continue;
						const auto clip = matrix * QVector4D(point.x, -point.y, -point.z, 1);
						if (clip.w() <= 1e-6f)
							continue;
						const float x = (clip.x() / clip.w() + 1) * 0.5f * viewport.width();
						const float y = (1 - clip.y() / clip.w()) * 0.5f * viewport.height();
						if (x >= 0 && y >= 0 && x < viewport.width() && y < viewport.height() &&
							qAlpha(reinterpret_cast<const QRgb*>(
								readMask.constScanLine(static_cast<int>(y)))[static_cast<int>(x)]))
						{
							flags[i] = 1;
							++count;
						}
					}
					addedTotal.fetch_add(count, std::memory_order_relaxed);
				});
			const std::size_t added = addedTotal.load();
			if (added)
			{
				cloud.selection = selected;
				cloud.selectedCount += added;
			}
		}
		return scene;
	}

	SceneState ClearSelection(SceneState scene)
	{
		for (auto& c : scene.clouds)
			Unselect(c);
		return scene;
	}

	/* Point data ============================================================================== */

	SceneState DeleteSelectedPoints(SceneState scene)
	{
		for (auto& c : scene.clouds)
			if (c.selection && c.selectedCount && IsActive(scene, c))
			{
				auto kept = std::make_shared<Pointcloud::ColorCloud>();
				kept->reserve(c.sourceCloud->size() - c.selectedCount);
				for (std::size_t i = 0; i < c.sourceCloud->size(); ++i)
					if (!(*c.selection)[i])
						kept->push_back((*c.sourceCloud)[i]);
				c.sourceCloud = kept;
				Unselect(c);
				c.details += QObject::tr("; points deleted");
			}
		scene.clouds.erase(std::remove_if(scene.clouds.begin(), scene.clouds.end(),
							   [](const CloudState& c) { return !c.sourceCloud || c.sourceCloud->empty(); }),
			scene.clouds.end());
		return scene;
	}

	SceneState TransformActive(SceneState scene, const cv::Matx44d& transform, const QString& description)
	{
		for (auto& c : scene.clouds)
			if (IsActive(scene, c) && c.sourceCloud)
			{
				c.sourceCloud = Pointcloud::TransformCloud(*c.sourceCloud, transform);
				Unselect(c);
				c.details += "; " + description;
			}
		return scene;
	}

	SceneState AlignActiveCentroids(SceneState scene)
	{
		const CloudState* reference = nullptr;
		for (auto& cloud : scene.clouds)
		{
			if (cloud.vertexCount <= 0 || !IsActive(scene, cloud))
				continue;
			if (!reference)
			{
				reference = &cloud;
				continue;
			}
			const auto delta = reference->centroid - cloud.centroid;
			if (cv::norm(delta) < 1e-12)
				continue;
			cloud.sourceCloud = Pointcloud::TransformCloud(*cloud.sourceCloud,
				cv::Matx44d(1, 0, 0, delta[0], 0, 1, 0, delta[1], 0, 0, 1, delta[2], 0, 0, 0, 1));
			Unselect(cloud);
			cloud.details += QObject::tr("; centroid aligned to %1").arg(reference->name);
		}
		return scene;
	}

	/* Layers ================================================================================== */

	SceneState MergeActive(SceneState scene, int mergedId)
	{
		const auto selected = [&](const CloudState& cloud) { return IsActive(scene, cloud); };
		if (std::count_if(scene.clouds.begin(), scene.clouds.end(), selected) < 2)
			return scene;
		const auto first = std::find_if(scene.clouds.begin(), scene.clouds.end(), selected);
		const auto index = first - scene.clouds.begin();
		auto merged = *first;
		auto points = std::make_shared<Pointcloud::ColorCloud>();
		std::size_t count = 0;
		QStringList names;
		for (const auto& cloud : scene.clouds)
			if (selected(cloud))
			{
				count += cloud.sourceCloud->size();
				names << cloud.name;
			}
		points->reserve(count);
		for (const auto& cloud : scene.clouds)
			if (selected(cloud))
				points->insert(points->end(), cloud.sourceCloud->begin(), cloud.sourceCloud->end());
		merged.id = mergedId;
		merged.name = QObject::tr("(Merged) %1").arg(first->name);
		merged.details = QObject::tr("Merged point clouds: %1").arg(names.join(", "));
		merged.sourceCloud = points;
		// The merged cloud shows the colors stored in its points, not the display overrides.
		merged.colorOverride = QColor();
		Unselect(merged);
		scene.clouds.erase(std::remove_if(scene.clouds.begin(), scene.clouds.end(), selected), scene.clouds.end());
		scene.clouds.insert(scene.clouds.begin() + index, std::move(merged));
		scene.activeLayers = { mergedId };
		return scene;
	}

	SceneState RemoveActive(SceneState scene)
	{
		scene.clouds.erase(std::remove_if(scene.clouds.begin(), scene.clouds.end(),
							   [&](const CloudState& cloud) { return IsActive(scene, cloud); }),
			scene.clouds.end());
		return scene;
	}

	/* History ================================================================================= */

	std::size_t HistoryBytes(const std::vector<HistoryEntry>& history)
	{
		// Snapshots share unchanged point data, so memory is counted per distinct array.
		std::set<const void*> counted;
		std::size_t total = 0;
		for (const HistoryEntry& entry : history)
			for (const CloudState& cloud : entry.state.clouds)
			{
				if (cloud.sourceCloud && counted.insert(cloud.sourceCloud.get()).second)
					total += cloud.sourceCloud->size() * sizeof(Pointcloud::ColorPoint);
				if (cloud.selection && counted.insert(cloud.selection.get()).second)
					total += cloud.selection->size();
			}
		return total;
	}

	void TrimHistory(std::vector<HistoryEntry>& history, int maxUndoable, std::size_t byteBudget)
	{
		// Entry 0 is the state before the oldest undoable command, hence the + 1.
		while (history.size() > static_cast<std::size_t>(std::max(1, maxUndoable)) + 1)
			history.erase(history.begin());
		// Keep the new state and the one before it, so the last command can always be undone.
		while (history.size() > 2 && HistoryBytes(history) > byteBudget)
			history.erase(history.begin());
	}
} // namespace smcp::EditorScene
