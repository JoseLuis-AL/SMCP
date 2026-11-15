#pragma once

#include <functional>

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>

class QTimer;

namespace smcp
{
struct AiModelInfo
{
	QString id;
	QString displayName;
	QString outputSuffix;
	QJsonObject configuration;
	QJsonObject configurationSchema;
};

/// A trainer registered in WSL (`smcp --list-trainers`), for example the PINN compensation.
struct AiTrainerInfo
{
	QString id;
	QString displayName;
	QJsonObject configuration;
	QJsonObject configurationSchema;
};

/// Asynchronous, shell-safe bridge to the versioned `smcp` CLI in WSL.
class WslModelService : public QObject
{
	Q_OBJECT

public:
	using ModelsCallback = std::function<void(const QVector<AiModelInfo>&, const QString&)>;
	using ModelCallback = std::function<void(const AiModelInfo&, const QString&)>;
	using InferenceCallback = std::function<void(bool, const QString&)>;
	using TrainersCallback = std::function<void(const QVector<AiTrainerInfo>&, const QString&)>;

	explicit WslModelService(QObject* parent = nullptr, QString distribution = QStringLiteral("Ubuntu-22.04"));
	~WslModelService() override;

	void ListModels(ModelsCallback callback);
	void DescribeModel(const QString& modelId, ModelCallback callback);
	void RunInference(const QString& modelId, const QString& inputPath, const QString& outputPath,
		const QString& configurationPath, InferenceCallback callback);
	void ListTrainers(TrainersCallback callback);
	/// Trains `trainerId` on a dataset folder; progress arrives through OutputReceived. Cancellable.
	void Train(const QString& trainerId, const QString& datasetDirectory, const QString& outputPath,
		const QString& configurationPath, InferenceCallback callback);
	void Cancel();
	bool IsBusy() const;

	static bool ParseModels(const QByteArray& json, QVector<AiModelInfo>& models, QString& error);
	static bool ParseModel(const QByteArray& json, AiModelInfo& model, QString& error);
	static bool ParseTrainers(const QByteArray& json, QVector<AiTrainerInfo>& trainers, QString& error);
	/// smcp arguments of a training job, from WSL paths in the order the call takes them.
	static QStringList TrainArguments(const QString& trainerId, const QStringList& wslPaths, const QString& jobId);
	static bool ValidateConfiguration(const QJsonObject& configuration, const QJsonObject& schema, QString& error);
	static QString ResultFilename(const QString& inputName, const QString& outputSuffix);
	static QString ProcessErrorMessage(int exitCode, const QString& standardError);

signals:
	void OutputReceived(const QString& text);

private slots:
	void OnReadyRead();
	void OnFinished(int exitCode, QProcess::ExitStatus exitStatus);
	void OnProcessError(QProcess::ProcessError error);
	void OnTimeout();

private:
	enum class Operation
	{
		None,
		List,
		Describe,
		ConvertPath,
		ListTrainers,
		Inference ///< Any supervised job: inference, training or prediction.
	};
	using JobArguments = std::function<QStringList(const QStringList& wslPaths, const QString& jobId)>;
	/// Converts `windowsPaths` to WSL paths, then runs the smcp job built by `arguments`.
	void RunJob(const QStringList& windowsPaths, JobArguments arguments, int timeoutMs, InferenceCallback callback);

	void StartSmcp(const QStringList& arguments, Operation operation, int timeoutMs);
	void StartProcess(const QString& program, const QStringList& arguments, Operation operation, int timeoutMs);
	void StartNextPathConversion();
	void StartInferenceProcess();
	void RequestJobCancellation();
	void FailCurrent(const QString& error);
	void ClearProcessOutput();

	QString _distribution;
	QProcess* _process = nullptr;
	QTimer* _timeout = nullptr;
	Operation _operation = Operation::None;
	QByteArray _standardOutput;
	QByteArray _standardError;
	bool _cancelled = false;

	ModelsCallback _modelsCallback;
	ModelCallback _modelCallback;
	InferenceCallback _inferenceCallback;
	TrainersCallback _trainersCallback;
	JobArguments _jobArguments;
	int _jobTimeoutMs = 0;

	QString _inferenceJobId;
	QStringList _windowsPaths;
	QStringList _wslPaths;
	int _pathIndex = 0;
};
} // namespace smcp
