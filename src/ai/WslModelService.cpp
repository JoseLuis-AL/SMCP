#include "ai/WslModelService.h"

#include <cmath>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

namespace smcp
{
namespace
{
	constexpr int Introspection_Timeout_Ms = 20000;
	constexpr int Inference_Timeout_Ms = 2 * 60 * 60 * 1000;
	/// Time the WSL job has to stop after --cancel-job before wsl.exe is terminated.
	constexpr int Cancellation_Grace_Ms = 15000;

	bool MatchesType(const QJsonValue& value, const QString& type)
	{
		if (type == "string") return value.isString();
		if (type == "boolean") return value.isBool();
		if (type == "number") return value.isDouble() && std::isfinite(value.toDouble());
		if (type == "integer")
		{
			return value.isDouble() && std::isfinite(value.toDouble()) && std::floor(value.toDouble()) == value.toDouble();
		}
		return false;
	}

	bool ParseModelObject(const QJsonObject& object, AiModelInfo& model, QString& error)
	{
		model.id = object.value("id").toString();
		model.displayName = object.value("display_name").toString();
		model.outputSuffix = object.value("output_suffix").toString();
		if (model.id.isEmpty() || model.displayName.isEmpty() || model.outputSuffix.isEmpty()
			|| !object.value("configuration").isObject() || !object.value("configuration_schema").isObject())
		{
			error = QObject::tr("The model metadata returned by WSL is incomplete.");
			return false;
		}
		model.configuration = object.value("configuration").toObject();
		model.configurationSchema = object.value("configuration_schema").toObject();
		return WslModelService::ValidateConfiguration(model.configuration, model.configurationSchema, error);
	}
}

WslModelService::WslModelService(QObject* parent, QString distribution)
	: QObject(parent), _distribution(std::move(distribution)), _process(new QProcess(this)), _timeout(new QTimer(this))
{
	_timeout->setSingleShot(true);
	connect(_process, &QProcess::readyReadStandardOutput, this, &WslModelService::OnReadyRead);
	connect(_process, &QProcess::readyReadStandardError, this, &WslModelService::OnReadyRead);
	connect(_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		this, &WslModelService::OnFinished);
	connect(_process, &QProcess::errorOccurred, this, &WslModelService::OnProcessError);
	connect(_timeout, &QTimer::timeout, this, &WslModelService::OnTimeout);
}

WslModelService::~WslModelService()
{
	// The owner may already be in its destructor; never dispatch a late callback from kill().
	if (_operation == Operation::Inference)
	{
		RequestJobCancellation();
	}
	_operation = Operation::None;
	_modelsCallback = {};
	_modelCallback = {};
	_inferenceCallback = {};
	_timeout->stop();
	_process->blockSignals(true);
	if (_process->state() != QProcess::NotRunning)
	{
		_process->kill();
		_process->waitForFinished(1000);
	}
}

bool WslModelService::IsBusy() const
{
	return _operation != Operation::None;
}

void WslModelService::ListModels(ModelsCallback callback)
{
	if (IsBusy())
	{
		callback({}, tr("Another WSL model operation is already running."));
		return;
	}
	_cancelled = false;
	_modelsCallback = std::move(callback);
	StartSmcp({ "--list-models", "--json" }, Operation::List, Introspection_Timeout_Ms);
}

void WslModelService::DescribeModel(const QString& modelId, ModelCallback callback)
{
	if (IsBusy())
	{
		callback({}, tr("Another WSL model operation is already running."));
		return;
	}
	_cancelled = false;
	_modelCallback = std::move(callback);
	StartSmcp({ "--describe-model", modelId, "--json" }, Operation::Describe, Introspection_Timeout_Ms);
}

void WslModelService::RunInference(const QString& modelId, const QString& inputPath, const QString& outputPath,
	const QString& configurationPath, InferenceCallback callback)
{
	if (IsBusy())
	{
		callback(false, tr("Another WSL model operation is already running."));
		return;
	}
	_inferenceCallback = std::move(callback);
	_inferenceModel = modelId;
	_inferenceJobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	_windowsPaths = QStringList{ inputPath, outputPath, configurationPath };
	_wslPaths.clear();
	_pathIndex = 0;
	_cancelled = false;
	StartNextPathConversion();
}

void WslModelService::Cancel()
{
	if (!IsBusy())
	{
		return;
	}
	_cancelled = true;
	_timeout->stop();
	if (_operation != Operation::Inference)
	{
		_process->kill();
		return;
	}
	RequestJobCancellation();
	_timeout->start(Cancellation_Grace_Ms);
}

/// Asks WSL to stop the inference process tree. Terminating wsl.exe alone is not enough to rely on:
/// the model runs in its own Linux session, which the smcp supervisor ends when its launcher dies.
/// Requesting cancellation explicitly stops the GPU work without waiting for that detection.
void WslModelService::RequestJobCancellation()
{
	if (_inferenceJobId.isEmpty())
	{
		return;
	}
	QProcess::startDetached("wsl.exe", QStringList{
		"-d", _distribution, "--exec", "bash", "-lc",
		"exec \"$HOME/.local/bin/smcp\" \"$@\"", "smcp", "--cancel-job", _inferenceJobId
	});
}

void WslModelService::StartSmcp(const QStringList& arguments, Operation operation, int timeoutMs)
{
	// The command is constant and values are passed through "$@" as separate argv entries.
	// Model identifiers, paths and configuration never undergo shell interpolation.
	QStringList wslArguments{
		"-d", _distribution, "--exec", "bash", "-lc",
		"exec \"$HOME/.local/bin/smcp\" \"$@\"", "smcp"
	};
	wslArguments.append(arguments);
	StartProcess("wsl.exe", wslArguments, operation, timeoutMs);
}

void WslModelService::StartProcess(const QString& program, const QStringList& arguments,
	Operation operation, int timeoutMs)
{
	ClearProcessOutput();
	_operation = operation;
	_process->setProgram(program);
	_process->setArguments(arguments);
	_process->start();
	_timeout->start(timeoutMs);
}

void WslModelService::StartNextPathConversion()
{
	if (_pathIndex >= _windowsPaths.size())
	{
		StartInferenceProcess();
		return;
	}
	StartProcess("wsl.exe", { "-d", _distribution, "--exec", "wslpath", "-a", "-u",
		_windowsPaths[_pathIndex] }, Operation::ConvertPath, Introspection_Timeout_Ms);
}

void WslModelService::StartInferenceProcess()
{
	if (_wslPaths.size() != 3)
	{
		FailCurrent(tr("Could not convert the temporary files to WSL paths."));
		return;
	}
	StartSmcp({ "--model", _inferenceModel, "--input", _wslPaths[0], "--output", _wslPaths[1],
		"--config-json", _wslPaths[2], "--job-id", _inferenceJobId }, Operation::Inference, Inference_Timeout_Ms);
}

void WslModelService::OnReadyRead()
{
	const QByteArray output = _process->readAllStandardOutput();
	const QByteArray error = _process->readAllStandardError();
	_standardOutput += output;
	_standardError += error;
	if (_operation == Operation::Inference)
	{
		const QString text = QString::fromUtf8(output + error).trimmed();
		if (!text.isEmpty()) emit OutputReceived(text);
	}
}

void WslModelService::OnFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
	if (_operation == Operation::None)
	{
		return;
	}
	OnReadyRead();
	_timeout->stop();
	const Operation completedOperation = _operation;
	if (_cancelled)
	{
		FailCurrent(tr("AI inference was cancelled."));
		return;
	}
	if (exitStatus != QProcess::NormalExit || exitCode != 0)
	{
		FailCurrent(ProcessErrorMessage(exitCode, QString::fromUtf8(_standardError)));
		return;
	}

	if (completedOperation == Operation::ConvertPath)
	{
		const QString converted = QString::fromUtf8(_standardOutput).trimmed();
		if (converted.isEmpty())
		{
			FailCurrent(tr("WSL returned an empty converted path."));
			return;
		}
		_wslPaths.append(converted);
		++_pathIndex;
		_operation = Operation::None;
		StartNextPathConversion();
		return;
	}

	_operation = Operation::None;
	if (completedOperation == Operation::List)
	{
		QVector<AiModelInfo> models;
		QString error;
		ParseModels(_standardOutput, models, error);
		auto callback = std::move(_modelsCallback);
		if (callback) callback(models, error);
	}
	else if (completedOperation == Operation::Describe)
	{
		AiModelInfo model;
		QString error;
		ParseModel(_standardOutput, model, error);
		auto callback = std::move(_modelCallback);
		if (callback) callback(model, error);
	}
	else if (completedOperation == Operation::Inference)
	{
		auto callback = std::move(_inferenceCallback);
		const QString diagnostics = QString::fromUtf8(_standardOutput + _standardError).trimmed();
		if (callback) callback(true, diagnostics);
	}
}

void WslModelService::OnProcessError(QProcess::ProcessError error)
{
	if (error == QProcess::FailedToStart && IsBusy())
	{
		FailCurrent(tr("Could not start wsl.exe. Verify that WSL and %1 are installed.").arg(_distribution));
	}
}

void WslModelService::OnTimeout()
{
	if (!IsBusy()) return;
	const bool cancelled = _cancelled;
	if (_operation == Operation::Inference && !cancelled)
	{
		// Killing wsl.exe must not leave the model using the GPU after the UI reports the timeout.
		RequestJobCancellation();
	}
	FailCurrent(cancelled ? tr("AI inference was cancelled.") : tr("The WSL model operation timed out."));
	_process->kill();
}

void WslModelService::FailCurrent(const QString& error)
{
	const Operation failedOperation = _operation;
	_operation = Operation::None;
	_cancelled = false;
	_timeout->stop();
	if (failedOperation == Operation::List)
	{
		auto callback = std::move(_modelsCallback);
		if (callback) callback({}, error);
	}
	else if (failedOperation == Operation::Describe)
	{
		auto callback = std::move(_modelCallback);
		if (callback) callback({}, error);
	}
	else
	{
		auto callback = std::move(_inferenceCallback);
		if (callback) callback(false, error);
	}
}

void WslModelService::ClearProcessOutput()
{
	_standardOutput.clear();
	_standardError.clear();
}

bool WslModelService::ParseModels(const QByteArray& json, QVector<AiModelInfo>& models, QString& error)
{
	models.clear();
	error.clear();
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
	{
		error = tr("Invalid model list returned by WSL: %1").arg(parseError.errorString());
		return false;
	}
	const QJsonObject root = document.object();
	if (root.value("schema_version").toInt() != 1 || !root.value("models").isArray())
	{
		error = tr("Unsupported or incomplete AI model registry response.");
		return false;
	}
	for (const QJsonValue& value : root.value("models").toArray())
	{
		if (!value.isObject())
		{
			error = tr("The AI model list contains an invalid entry.");
			return false;
		}
		AiModelInfo model;
		if (!ParseModelObject(value.toObject(), model, error)) return false;
		models.append(model);
	}
	if (root.value("warnings").isArray())
	{
		QStringList warnings;
		for (const QJsonValue& warning : root.value("warnings").toArray())
		{
			if (warning.isString()) warnings.append(warning.toString());
		}
		if (!warnings.isEmpty()) error = tr("Some AI models are unavailable: %1").arg(warnings.join("; "));
	}
	return true;
}

bool WslModelService::ParseModel(const QByteArray& json, AiModelInfo& model, QString& error)
{
	error.clear();
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
	{
		error = tr("Invalid model description returned by WSL: %1").arg(parseError.errorString());
		return false;
	}
	return ParseModelObject(document.object(), model, error);
}

bool WslModelService::ValidateConfiguration(const QJsonObject& configuration,
	const QJsonObject& schema, QString& error)
{
	error.clear();
	for (auto it = configuration.begin(); it != configuration.end(); ++it)
	{
		if (!schema.contains(it.key()) || !schema.value(it.key()).isObject())
		{
			error = tr("Unknown model parameter: %1").arg(it.key());
			return false;
		}
		const QJsonObject rule = schema.value(it.key()).toObject();
		const QString type = rule.value("type").toString();
		if (!MatchesType(it.value(), type))
		{
			error = tr("Parameter %1 must be %2.").arg(it.key(), type);
			return false;
		}
		if (rule.value("enum").isArray() && !rule.value("enum").toArray().contains(it.value()))
		{
			error = tr("Parameter %1 is not one of the allowed values.").arg(it.key());
			return false;
		}
		if (it.value().isDouble())
		{
			const double value = it.value().toDouble();
			if (rule.contains("minimum") && value < rule.value("minimum").toDouble())
			{
				error = tr("Parameter %1 must be at least %2.").arg(it.key()).arg(rule.value("minimum").toDouble());
				return false;
			}
			if (rule.contains("maximum") && value > rule.value("maximum").toDouble())
			{
				error = tr("Parameter %1 must be at most %2.").arg(it.key()).arg(rule.value("maximum").toDouble());
				return false;
			}
			if (rule.contains("exclusive_minimum") && value <= rule.value("exclusive_minimum").toDouble())
			{
				error = tr("Parameter %1 must be greater than %2.").arg(it.key()).arg(rule.value("exclusive_minimum").toDouble());
				return false;
			}
			if (rule.contains("exclusive_maximum") && value >= rule.value("exclusive_maximum").toDouble())
			{
				error = tr("Parameter %1 must be less than %2.").arg(it.key()).arg(rule.value("exclusive_maximum").toDouble());
				return false;
			}
		}
	}
	return true;
}

QString WslModelService::ResultFilename(const QString& inputName, const QString& outputSuffix)
{
	QString base = QFileInfo(inputName).completeBaseName();
	if (base.isEmpty()) base = QStringLiteral("pointcloud");
	base.replace(QRegularExpression("[<>:\"/\\\\|?*]"), "_");
	return QString("%1_%2.xyz").arg(base, outputSuffix);
}

QString WslModelService::ProcessErrorMessage(int exitCode, const QString& standardError)
{
	const QString details = standardError.trimmed();
	return details.isEmpty() ? tr("The WSL model command failed with exit code %1.").arg(exitCode) : details;
}
} // namespace smcp
