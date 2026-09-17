/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
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
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>

#include "core/PointcloudOps.h"
#include "ai/WslModelService.h"
#include "ui/preview/PointcloudPreviewWidget.h"
#include "ui_PointcloudEditorDialog.h"

class QTemporaryDir;

namespace smcp
{
class AiInferenceProgressDialog;

/// <summary>
/// Point cloud editor: adds multiple .xyz files from the working directory, removes outliers,
/// fits geometric models (plane, sphere) with RANSAC, compares all results in the 3D viewer
/// and exports the list.
/// </summary>
/// <remarks>
/// Fittable models are registered in the `fit_model_combo` combo box (enum FitModel); selecting
/// one opens its parameter dialog and starts the operation on the first cloud in the overlay.
/// Every command appends its result, leaving the existing clouds available for comparison.
/// Commands run on a worker thread (QtConcurrent) with the command bar disabled, so the
/// interface stays responsive with large clouds.
/// </remarks>
class PointcloudEditorDialog : public QDialog, public Ui::PointcloudEditorDialog
{
	Q_OBJECT

public:
	/// Creates, runs, and destroys the modal editor. The constructor is private so the dialog, which
	/// owns background work and a WSL process, is always used as a scoped modal window.
	static int Execute(const QString& rootDir, QWidget* parent = nullptr);
	~PointcloudEditorDialog() override;

	void reject() override;

public slots:
	// Point cloud.
	void on_load_pointcloud_button_clicked(bool checked = false);

	// Commands.
	void on_remove_outliers_button_clicked(bool checked = false);
	void on_fit_model_combo_currentIndexChanged(int index);
	void on_ai_model_combo_currentIndexChanged(int index);
	void on_save_button_clicked(bool checked = false);

private:
	explicit PointcloudEditorDialog(const QString& rootDir, QWidget* parent = nullptr,
		Qt::WindowFlags flags = Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);

	/// Geometric models available in `fit_model_combo` (stored as userData).
	enum class FitModel
	{
		None,
		Plane,
		Sphere
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
	void StartAiModelDiscovery();
	void SetAvailableAiModels(const QVector<AiModelInfo>& models, const QString& error = QString());
	void ConfigureAiModel(const QString& modelId);
	/// Enables the AI Model selector only when models are available and no command is running.
	void UpdateAiModelControls();
	/// Returns the selector to None, so choosing a model always opens its configuration again.
	void ResetAiModelSelection();
	void StartAiInference(const AiModelInfo& model, const QJsonObject& configuration);
	void FinishAiInference(bool success, const QString& diagnostics);
	void LoadAiResult();
	void CloseAiProgressDialog();
	void UpdatePointcloudCombo();
	Pointcloud::ColorCloudPtr FindCachedPointcloud(const QString& path, qint64 fileSize, const QDateTime& lastModified);
	void StoreCachedPointcloud(const QString& path, qint64 fileSize, const QDateTime& lastModified,
		const Pointcloud::ColorCloudPtr& cloud);
	void FitPlanes();
	void FitSpheres();
	void ResetFitModelSelection();
	void SetStatus(const QString& text);
	/// `waitCursor` is false for AI inference, which shows its own modal progress window instead.
	void SetBusy(bool busy, bool waitCursor = true);
	Pointcloud::ColorCloudPtr FirstPointcloud() const;

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

	/// Clouds already parsed, keyed by absolute file path, so reopening a file skips reading it.
	std::map<QString, CachedPointcloud> _cloudCache;
	std::uint64_t _cacheUseCounter = 0;
	WslModelService* _aiModelService = nullptr;
	QHash<QString, AiModelInfo> _aiModels;
	QHash<QString, QJsonObject> _aiConfigurations;
	std::unique_ptr<QTemporaryDir> _aiTemporaryDirectory;
	QString _aiResultPath;
	QString _aiResultName;
	bool _aiConfigurationLoading = false;
	bool _aiInferenceRunning = false;
	bool _aiCancelRequested = false;
	AiInferenceProgressDialog* _aiProgressDialog = nullptr;

	Pointcloud::OutlierRemovalParams _outlierRemovalParams;
	Pointcloud::PlaneFitParams _planeFitParams;
	Pointcloud::SphereFitParams _sphereFitParams;
	bool _busy = false;
	bool _waitCursor = false;
};
} // namespace smcp
