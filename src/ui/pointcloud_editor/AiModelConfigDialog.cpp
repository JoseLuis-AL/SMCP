#include "ui/pointcloud_editor/AiModelConfigDialog.h"

#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "ai/WslModelService.h"

namespace smcp
{
AiModelConfigDialog::AiModelConfigDialog(const QString& modelName, const QJsonObject& configuration,
	const QJsonObject& schema, QWidget* parent)
	: QDialog(parent), _schema(schema), _configuration(configuration)
{
	setWindowTitle(tr("%1 configuration").arg(modelName));
	resize(620, 520);

	auto* layout = new QVBoxLayout(this);
	auto* instructions = new QLabel(tr(
		"Edit the inference parameters as JSON. You can adjust iterations, clusters, patch sizes, "
		"device selection, and every other parameter registered for this model."), this);
	instructions->setWordWrap(true);
	layout->addWidget(instructions);

	_editor = new QPlainTextEdit(this);
	_editor->setObjectName("ai_model_configuration_editor");
	_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
	_editor->setPlainText(QString::fromUtf8(QJsonDocument(configuration).toJson(QJsonDocument::Indented)));
	_editor->setTabStopDistance(24.0);
	layout->addWidget(_editor, 1);

	_validationLabel = new QLabel(this);
	_validationLabel->setWordWrap(true);
	layout->addWidget(_validationLabel);

	_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(_buttons);
	connect(_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(_editor, &QPlainTextEdit::textChanged, this, &AiModelConfigDialog::ValidateText);
	ValidateText();
}

QJsonObject AiModelConfigDialog::Configuration() const
{
	return _configuration;
}

void AiModelConfigDialog::ValidateText()
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(_editor->toPlainText().toUtf8(), &parseError);
	QString error;
	if (parseError.error != QJsonParseError::NoError)
	{
		error = tr("Invalid JSON at offset %1: %2").arg(parseError.offset).arg(parseError.errorString());
	}
	else if (!document.isObject())
	{
		error = tr("The configuration must be a JSON object.");
	}
	else if (!WslModelService::ValidateConfiguration(document.object(), _schema, error))
	{
		// The validator supplied the message.
	}
	else
	{
		_configuration = document.object();
	}

	_validationLabel->setText(error.isEmpty() ? tr("Configuration is valid.") : error);
	_validationLabel->setStyleSheet(error.isEmpty() ? QString() : QStringLiteral("color: #e43b44;"));
	_buttons->button(QDialogButtonBox::Ok)->setEnabled(error.isEmpty());
}
} // namespace smcp
