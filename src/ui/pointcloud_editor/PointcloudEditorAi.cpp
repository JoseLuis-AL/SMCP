// PointcloudEditorDialog, part 3 of 4: AI models that run in WSL (see ai/WslModelService).
//
// Sequence of one batch (the chosen model applied to each selected cloud, one after another):
//
//   ConfigureAiModel       asks WSL for the model's parameters and shows the configuration dialog
//   StartAiInference       remembers the inputs and the configuration of the batch
//   StartNextAiInference   writes the first pending cloud to a temporary folder and starts the model
//   FinishAiInference      the model ended: on success LoadAiResult, on failure SkipAiCloud
//   LoadAiResult           reads the output cloud; when clouds remain, ContinueAi starts the next
//                          one once the viewer is idle; after the last one, every result is added
//                          to the viewer together, as one command
//
// The clouds run one at a time: the WSL service runs a single job, and the models share one GPU
// whose memory two of them at once can exceed. A cloud that fails is skipped and the batch goes
// on; cancelling stops the batch. In both cases the results already obtained are kept and added
// to the viewer, because each one can take minutes.

#include "ui/pointcloud_editor/PointcloudEditorDialog.h"

#include <algorithm>
#include <iterator>

#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QLayout>
#include <QDialogButtonBox>
#include <QToolButton>
#include <QShortcut>
#include <QFileInfo>
#include <QLocale>
#include <QMatrix4x4>
#include <QMessageBox>
#include <QtMath>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QTemporaryDir>
#include <QTimer>

#include "core/Compensation.h"
#include "export/IOExport.h"
#include "ui/pointcloud_editor/AiInferenceProgressDialog.h"
#include "ui/pointcloud_editor/PointcloudProject.h"
#include "ui/pointcloud_editor/AiModelConfigDialog.h"
#include "ui/pointcloud_editor/ExportPointcloudDialog.h"
#include "ui/pointcloud_editor/IcpDialog.h"
#include "ui/pointcloud_editor/PlaneFitDialog.h"
#include "ui/pointcloud_editor/RemoveOutliersDialog.h"
#include "ui/pointcloud_editor/SphereFitDialog.h"
#include "ui/pointcloud_editor/SphereResultsDialog.h"
#include "ui/widgets/EditorDialogStyle.h"

namespace smcp
{
	/// Discovery and configuration use the existing WSL service; menu actions carry stable model IDs.
	void PointcloudEditorDialog::StartAiModelDiscovery()
	{
		ai_models_menu->clear();
		ai_models_menu->addAction(tr("Searching..."))->setEnabled(false);
		ai_models_menu->menuAction()->setEnabled(false);
		ai_models_menu->menuAction()->setToolTip(tr("Searching for AI models in WSL..."));
		_aiModelService->ListModels(
			[this](const QVector<AiModelInfo>& models, const QString& error) { SetAvailableAiModels(models, error); });
	}

	void PointcloudEditorDialog::SetAvailableAiModels(const QVector<AiModelInfo>& models, const QString& error)
	{
		_aiModels.clear();
		ai_models_menu->clear();
		for (const auto& model : models)
		{
			_aiModels.insert(model.id, model);
			auto* action = ai_models_menu->addAction(QString(model.displayName).replace("&", "&&"));
			action->setData(model.id);
			connect(action, &QAction::triggered, this, [this, id = model.id]() { ConfigureAiModel(id); });
		}
		if (models.isEmpty())
		{
			ai_models_menu->addAction(tr("No models available"))->setEnabled(false);
			ai_models_menu->menuAction()->setToolTip(error.isEmpty()
					? tr("No AI models were found in ~/smcp-models inside WSL.")
					: tr("AI models are unavailable: %1").arg(error));
		}
		else
			ai_models_menu->menuAction()->setToolTip(error.isEmpty()
					? tr("Select an AI model to configure and run it on every selected point cloud, one after another.")
					: error);
		UpdateAiModelControls();
	}

	void PointcloudEditorDialog::UpdateAiModelControls()
	{
		const bool available = !IsWorking() && !_aiConfigurationLoading && !_aiModels.isEmpty();
		ai_models_menu->menuAction()->setEnabled(available);
		for (auto* action : ai_models_menu->actions())
			action->setEnabled(available);
	}

	void PointcloudEditorDialog::ConfigureAiModel(const QString& modelId)
	{
		if (IsWorking() || _aiConfigurationLoading || !_aiModels.contains(modelId))
		{
			return;
		}
		if (!FirstSelectedCloud())
		{
			SetStatus(tr("Load at least one point cloud before running an AI model."));
			return;
		}

		_aiConfigurationLoading = true;
		SetBusy(true, false);
		UpdateAiModelControls();
		SetStatus(tr("Loading %1 configuration from WSL...").arg(_aiModels.value(modelId).displayName));
		_aiModelService->DescribeModel(modelId,
			[this, modelId](const AiModelInfo& described, const QString& error)
			{
				_aiConfigurationLoading = false;
				SetBusy(false);
				UpdateAiModelControls();
				if (!error.isEmpty())
				{
					SetStatus(tr("Could not load AI model configuration: %1").arg(error));
					return;
				}
				if (described.id != modelId)
				{
					SetStatus(tr("WSL returned configuration for an unexpected AI model."));
					return;
				}

				_aiModels.insert(modelId, described);
				const QJsonObject configuration =
					_aiConfigurations.contains(modelId) ? _aiConfigurations.value(modelId) : described.configuration;
				AiModelConfigDialog dialog(described.displayName, configuration, described.configurationSchema, this);
				if (dialog.exec() != QDialog::Accepted)
				{
					SetStatus(tr("AI inference was not started."));
					return;
				}
				_aiConfigurations.insert(modelId, dialog.Configuration());
				StartAiInference(described, dialog.Configuration());
			});
	}

	void PointcloudEditorDialog::StartAiInference(const AiModelInfo& model, const QJsonObject& configuration)
	{
		_aiResults.clear();
		_aiFailures.clear();
		_aiInputs = SelectedClouds();
		_aiBatchTotal = static_cast<int>(_aiInputs.size());
		_batchAiModel = model;
		_batchAiConfiguration = configuration;
		StartNextAiInference();
	}

	void PointcloudEditorDialog::ContinueAi()
	{
		if (!_continueAi || _viewerBusy)
			return;
		_continueAi = false;
		try
		{
			StartNextAiInference();
		}
		catch (...)
		{
			FailTask(std::current_exception());
			SetBusy(false);
		}
	}

	void PointcloudEditorDialog::StartNextAiInference()
	{
		const auto model = _batchAiModel;
		const auto configuration = _batchAiConfiguration;

		const auto pointclouds = _aiInputs;
		if (pointclouds.empty() || !pointclouds.front().cloud || pointclouds.front().cloud->empty())
		{
			_aiInputs.clear();
			FinishAiBatch(tr("Select visible point clouds."));
			return;
		}

		_aiTemporaryDirectory =
			std::make_unique<QTemporaryDir>(QDir::tempPath() + QDir::separator() + "smcp-ai-XXXXXX");
		if (!_aiTemporaryDirectory->isValid())
		{
			_aiTemporaryDirectory.reset();
			_aiInputs.clear();
			FinishAiBatch(tr("Could not create temporary files for AI inference."));
			return;
		}

		const QString inputPath = QDir(_aiTemporaryDirectory->path()).filePath("input.xyz");
		_aiResultName = WslModelService::ResultFilename(pointclouds.front().name, model.outputSuffix);
		_aiResultPath = QDir(_aiTemporaryDirectory->path()).filePath(_aiResultName);
		const QString configurationPath = QDir(_aiTemporaryDirectory->path()).filePath("configuration.json");


		_aiInferenceRunning = true;
		_aiCancelRequested = false;
		SetBusy(true, false);
		const int position = _aiBatchTotal - static_cast<int>(pointclouds.size()) + 1;
		_aiProgressDialog = new AiInferenceProgressDialog(
			model.displayName, pointclouds.front().name, pointclouds.front().cloud->size(), this);
		_aiProgressDialog->SetBatchPosition(position, _aiBatchTotal);
		connect(_aiProgressDialog, &AiInferenceProgressDialog::CancelRequested, this,
			[this]()
			{
				if (!_aiInferenceRunning)
					return;
				_aiCancelRequested = true;
				_aiProgressDialog->SetCancelling();
				SetStatus(tr("Cancelling AI inference..."));
				_aiModelService->Cancel();
			});
		// Window-modal and non-blocking: the inference callbacks keep arriving through the event loop.
		_aiProgressDialog->open();
		SetStatus(_aiBatchTotal > 1
				? tr("Running %1 on \"%2\" (cloud %3 of %4)...")
					  .arg(model.displayName, pointclouds.front().name)
					  .arg(position)
					  .arg(_aiBatchTotal)
				: tr("Running %1 on the selected point cloud...").arg(model.displayName));
		const auto input = pointclouds.front().cloud;
		RunAsync<bool>(
			tr("Prepare AI input"),
			[input, inputPath, configurationPath, configuration]()
			{
				if (!IOExport::WriteXyz(inputPath.toStdString(), *input))
					throw std::runtime_error("Cannot write temporary AI point cloud");
				QFile file(configurationPath);
				const auto json = QJsonDocument(configuration).toJson();
				if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(json) != json.size() ||
					!file.flush())
					throw std::runtime_error("Cannot write AI configuration");
				return true;
			},
			[this, model, inputPath, configurationPath](bool)
			{
				if (_aiCancelRequested)
				{
					// From the event loop: this callback runs inside a viewer command, and ending the
					// batch opens its own command to publish the results obtained so far.
					QTimer::singleShot(0, this, [this]() { FinishAiInference(false, tr("AI inference was cancelled.")); });
					return;
				}
				_aiModelService->RunInference(model.id, inputPath, _aiResultPath, configurationPath,
					[this](bool success, const QString& diagnostics) { FinishAiInference(success, diagnostics); });
			});
	}

	void PointcloudEditorDialog::FinishAiInference(bool success, const QString& diagnostics)
	{
		_aiInferenceRunning = false;
		if (_aiCancelRequested)
		{
			// Cancelling stops the whole batch; what was already processed is kept.
			_aiInputs.clear();
			_aiTemporaryDirectory.reset();
			CloseAiProgressDialog();
			FinishAiBatch(tr("AI inference was cancelled."));
			return;
		}
		if (!success)
		{
			SkipAiCloud(diagnostics.isEmpty() ? tr("the model failed") : diagnostics);
			return;
		}
		if (!QFileInfo::exists(_aiResultPath))
		{
			SkipAiCloud(tr("the model finished without creating its output file"));
			return;
		}
		if (_aiProgressDialog)
			_aiProgressDialog->SetLoadingResult();
		LoadAiResult();
	}

	void PointcloudEditorDialog::SkipAiCloud(const QString& reason)
	{
		// Only the last line of a model's diagnostics fits in a status line.
		const QString shortReason = reason.trimmed().section('\n', -1).trimmed().left(200);
		if (!_aiInputs.empty())
		{
			_aiFailures << QString("%1: %2").arg(_aiInputs.front().name, shortReason);
			_aiInputs.erase(_aiInputs.begin());
		}
		_aiTemporaryDirectory.reset();
		CloseAiProgressDialog();
		if (_aiInputs.empty())
		{
			FinishAiBatch();
			return;
		}
		// The editor stays busy and the next cloud starts from the event loop.
		_continueAi = true;
		SetBusy(true, false);
		QTimer::singleShot(0, this, &PointcloudEditorDialog::ContinueAi);
	}

	void PointcloudEditorDialog::AddAiResults()
	{
		for (const auto& output : _aiResults)
			preview_widget->AddPointcloud(output.cloud, QColor(), output.name, tr("AI inference result"));
		_aiResults.clear();
	}

	QString PointcloudEditorDialog::AiBatchSummary(const QString& interruption) const
	{
		const int processed = static_cast<int>(_aiResults.size());
		QString summary = _aiBatchTotal > 1 || processed != 1 || !interruption.isEmpty() || !_aiFailures.isEmpty()
			? tr("%1: %2 of %3 point cloud(s) processed.").arg(_batchAiModel.displayName).arg(processed).arg(_aiBatchTotal)
			: tr("AI result %1 added (%2 points).").arg(_aiResults.front().name).arg(_aiResults.front().cloud->size());
		if (!interruption.isEmpty())
			summary += " " + interruption;
		if (!_aiFailures.isEmpty())
			summary += " " + tr("Not processed: %1").arg(_aiFailures.join("; "));
		return summary;
	}

	void PointcloudEditorDialog::FinishAiBatch(const QString& interruption)
	{
		_continueAi = false;
		const QString summary = AiBatchSummary(interruption);
		if (!_aiResults.empty())
		{
			// One command, so a single undo removes every result of the batch.
			preview_widget->BeginCommand(tr("AI inference"));
			try
			{
				AddAiResults();
				preview_widget->EndCommand();
			}
			catch (...)
			{
				preview_widget->AbortCommand();
				_aiResults.clear();
			}
		}
		_aiFailures.clear();
		SetBusy(false);
		SetStatus(summary);
	}

	void PointcloudEditorDialog::LoadAiResult()
	{
		const QString path = _aiResultPath;
		const QString name = _aiResultName;
		RunAsync<std::shared_ptr<LoadedPointcloud>>(
			tr("AI inference"),
			[path]()
			{
				auto result = std::make_shared<LoadedPointcloud>();
				result->cloud = std::make_shared<Pointcloud::ColorCloud>();
				if (!IOExport::ReadXyz(path.toStdString(), *result->cloud) || result->cloud->empty())
				{
					result->cloud.reset();
					return result;
				}
				return result;
			},
			[this, name](const std::shared_ptr<LoadedPointcloud>& result)
			{
				_aiTemporaryDirectory.reset();
				CloseAiProgressDialog();
				if (result->cloud)
					_aiResults.push_back({ result->cloud, name, true });
				else if (!_aiInputs.empty())
					_aiFailures << QString("%1: %2").arg(_aiInputs.front().name, tr("the result could not be read as an XYZ point cloud"));
				if (!_aiInputs.empty())
					_aiInputs.erase(_aiInputs.begin());
				_continueAi = !_aiInputs.empty();
				if (_continueAi)
				{
					SetStatus(tr("%1 of %2 point clouds processed...").arg(_aiResults.size()).arg(_aiBatchTotal));
					return;
				}
				// Last cloud: this callback runs inside a viewer command, so the results are one undo step.
				const QString summary = AiBatchSummary(QString());
				AddAiResults();
				_aiFailures.clear();
				SetStatus(summary);
			});
	}

	void PointcloudEditorDialog::CloseAiProgressDialog()
	{
		if (!_aiProgressDialog)
		{
			return;
		}
		_aiProgressDialog->done(QDialog::Accepted);
		_aiProgressDialog->deleteLater();
		_aiProgressDialog = nullptr;
	}
} // namespace smcp
