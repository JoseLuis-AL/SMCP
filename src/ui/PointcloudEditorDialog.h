/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Brown University nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DANIEL MORENO AND GABRIEL TAUBIN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <QDateTime>
#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <QtConcurrent/QtConcurrentRun>

#include "core/PointcloudOps.h"
#include "ui/PointcloudPreviewWidget.h"
#include "ui_PointcloudEditorDialog.h"

namespace smcp
{
/// <summary>
/// Point cloud editor: loads the .xyz files of the working directory, removes outliers,
/// fits geometric models (plane, sphere) with RANSAC, compares the result against the
/// original cloud in the 3D viewer and saves the result.
/// </summary>
/// <remarks>
/// Fittable models are registered in the `fit_model_combo` combo box (enum FitModel); to add
/// a new one it is enough to extend the enum, `AddFitModels()` and `on_fit_button_clicked()`.
/// Commands run on a worker thread (QtConcurrent) with the command bar disabled, so the
/// interface stays responsive with large clouds.
/// </remarks>
class PointcloudEditorDialog : public QDialog, public Ui::PointcloudEditorDialog
{
	Q_OBJECT

public:
	explicit PointcloudEditorDialog(const QString& rootDir, QWidget* parent = nullptr,
		Qt::WindowFlags flags = Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
	~PointcloudEditorDialog() override;

	void reject() override;

public slots:
	// Point cloud.
	void on_refresh_button_clicked(bool checked = false);
	void on_pointcloud_combo_currentIndexChanged(int index);

	// Commands.
	void on_remove_outliers_button_clicked(bool checked = false);
	void on_fit_button_clicked(bool checked = false);
	void on_save_button_clicked(bool checked = false);
	void on_close_button_clicked(bool checked = false);

private:
	/// Geometric models available in `fit_model_combo` (stored as userData).
	enum class FitModel
	{
		Plane,
		Sphere
	};

	enum class LastCommand
	{
		None,
		FitPlanes,
		FitSpheres,
		Other
	};

	/// Result of loading a file on the worker thread: the parsed cloud plus its GPU buffer.
	struct LoadedPointcloud
	{
		Pointcloud::ColorCloudPtr cloud;
		PointcloudPreviewWidget::PreparedPointcloud prepared;
	};

	/// Parsed cloud kept in memory, valid while the file keeps the same size and modification time.
	struct CachedPointcloud
	{
		Pointcloud::ColorCloudPtr cloud;
		qint64 fileSize = 0;
		QDateTime lastModified;
		std::uint64_t lastUse = 0;
	};

	void AddFitModels();
	void UpdatePointcloudCombo();
	Pointcloud::ColorCloudPtr FindCachedPointcloud(const QString& path, qint64 fileSize, const QDateTime& lastModified);
	void StoreCachedPointcloud(const QString& path, qint64 fileSize, const QDateTime& lastModified,
		const Pointcloud::ColorCloudPtr& cloud);
	void FitPlanes();
	void FitSpheres();
	void SetStatus(const QString& text);
	void SetBusy(bool busy);
	void ResetResult();

	/// Runs `work` on a worker thread and, once finished, `done(result)` on the UI thread.
	/// Meanwhile the command bar stays disabled.
	template <typename Result>
	void RunAsync(const QString& status, std::function<Result()> work, std::function<void(const Result&)> done)
	{
		SetBusy(true);
		SetStatus(status);

		auto* watcher = new QFutureWatcher<Result>(this);
		connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, done]() {
			done(watcher->result());
			watcher->deleteLater();
			SetBusy(false);
		});
		watcher->setFuture(QtConcurrent::run(work));
	}

	/// Directory the .xyz files are looked up in.
	QString _rootDir;

	/// Current cloud (result of the last command) and the cloud as loaded, for comparison. Commands never
	/// modify a cloud in place, so both may share the same data.
	Pointcloud::ColorCloudPtr _currentCloud;
	Pointcloud::ColorCloudPtr _originalCloud;

	/// Clouds already parsed, keyed by absolute file path, so reopening a file skips reading it.
	std::map<QString, CachedPointcloud> _cloudCache;
	std::uint64_t _cacheUseCounter = 0;

	/// Individual planes from the last detection (so they can be saved separately).
	std::vector<Pointcloud::ColorCloudPtr> _planes;

	LastCommand _lastCommand = LastCommand::None;
	bool _busy = false;
};
} // namespace smcp
