#pragma once

#include "ui/preview/PointcloudPreviewWidget.h"

namespace smcp::PointcloudProject
{
	/// Versioned, self-contained project with shared cloud/selection assets and a SHA-256 checksum.
	/// Both functions are worker-safe. Save atomically replaces the destination only after a complete write.
	void Save(const QString& path, const PointcloudPreviewWidget::ProjectState& project);
	PointcloudPreviewWidget::ProjectState Load(const QString& path);
}
