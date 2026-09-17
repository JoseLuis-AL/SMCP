#include <iostream>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include "ai/WslModelService.h"

namespace
{
	bool Check(bool condition, const char* message)
	{
		if (!condition) std::cerr << "FAIL: " << message << '\n';
		return condition;
	}
}

int main(int argc, char* argv[])
{
	QCoreApplication application(argc, argv);
	bool success = true;

	const QByteArray response = R"json({
		"schema_version": 1,
		"models": [{
			"id": "example", "display_name": "Example", "output_suffix": "example",
			"configuration": {"iterations": 2, "device": "auto"},
			"configuration_schema": {
				"iterations": {"type": "integer", "minimum": 1},
				"device": {"type": "string", "enum": ["auto", "cpu"]}
			}
		}]
	})json";
	QVector<smcp::AiModelInfo> models;
	QString error;
	success &= Check(smcp::WslModelService::ParseModels(response, models, error), "model list parses");
	success &= Check(models.size() == 1 && models.front().id == "example", "model metadata is retained");

	QJsonObject valid{ { "iterations", 5 }, { "device", "cpu" } };
	success &= Check(smcp::WslModelService::ValidateConfiguration(valid,
		models.front().configurationSchema, error), "valid configuration is accepted");
	QJsonObject invalid{ { "iterations", 0 } };
	success &= Check(!smcp::WslModelService::ValidateConfiguration(invalid,
		models.front().configurationSchema, error), "range violation is rejected");
	QJsonObject unknown{ { "clusters", 3 } };
	success &= Check(!smcp::WslModelService::ValidateConfiguration(unknown,
		models.front().configurationSchema, error), "unknown configuration key is rejected");

	success &= Check(smcp::WslModelService::ResultFilename("example_piece.xyz", "score_denoise")
		== "example_piece_score_denoise.xyz", "result suffix is deterministic");
	success &= Check(smcp::WslModelService::ProcessErrorMessage(7, QString()).contains("7"),
		"exit code is mapped to a diagnostic");
	success &= Check(!smcp::WslModelService::ParseModels("not-json", models, error),
		"malformed process output is rejected");

	// Opt-in end-to-end test used on configured development machines. It exercises
	// QProcess, the safe WSL wrapper, wslpath, JSON introspection, Conda and a real model.
	if (qEnvironmentVariableIsSet("SMCP_TEST_WSL"))
	{
		smcp::WslModelService service;
		QVector<smcp::AiModelInfo> discovered;
		QString integrationError;
		QEventLoop discoveryLoop;
		service.ListModels([&](const QVector<smcp::AiModelInfo>& value, const QString& valueError) {
			discovered = value;
			integrationError = valueError;
			discoveryLoop.quit();
		});
		QTimer::singleShot(30000, &discoveryLoop, &QEventLoop::quit);
		discoveryLoop.exec();
		success &= Check(integrationError.isEmpty() && discovered.size() >= 3,
			"WSL model discovery returns the installed models");

		const QString inputPath = qEnvironmentVariable("SMCP_TEST_INPUT");
		const QString configurationPath = qEnvironmentVariable("SMCP_TEST_CONFIG");
		if (!inputPath.isEmpty() && !configurationPath.isEmpty())
		{
			QTemporaryDir temporary;
			const QString outputPath = temporary.filePath("small-cloud_score_denoise.xyz");
			bool inferenceDone = false;
			bool inferenceSucceeded = false;
			QEventLoop inferenceLoop;
			service.RunInference("score-denoise", inputPath, outputPath, configurationPath,
				[&](bool value, const QString& valueError) {
					inferenceDone = true;
					inferenceSucceeded = value;
					integrationError = valueError;
					inferenceLoop.quit();
				});
			QTimer::singleShot(120000, &inferenceLoop, &QEventLoop::quit);
			inferenceLoop.exec();
			success &= Check(inferenceDone && inferenceSucceeded && QFileInfo::exists(outputPath),
				"WSL service completes inference and creates the expected output");
		}
	}

	return success ? 0 : 1;
}
