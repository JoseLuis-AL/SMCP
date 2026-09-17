#pragma once

#include <QDialog>
#include <QJsonObject>

class QDialogButtonBox;
class QLabel;
class QPlainTextEdit;

namespace smcp
{
class AiModelConfigDialog : public QDialog
{
	Q_OBJECT

public:
	AiModelConfigDialog(const QString& modelName, const QJsonObject& configuration,
		const QJsonObject& schema, QWidget* parent = nullptr);

	QJsonObject Configuration() const;

private slots:
	void ValidateText();

private:
	QJsonObject _schema;
	QJsonObject _configuration;
	QPlainTextEdit* _editor = nullptr;
	QLabel* _validationLabel = nullptr;
	QDialogButtonBox* _buttons = nullptr;
};
} // namespace smcp
