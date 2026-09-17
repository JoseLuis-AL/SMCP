#include "ui/pointcloud_editor/AiInferenceProgressDialog.h"

#include <QKeyEvent>
#include <QTimer>

namespace smcp
{

AiInferenceProgressDialog::AiInferenceProgressDialog(const QString& modelName, QWidget* parent)
	: QDialog(parent), _elapsedTimer(new QTimer(this))
{
	setupUi(this);
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	message_label->setText(tr("%1 is processing the first point cloud. Depending on its size and "
		"parameters, this can take several minutes.").arg(modelName));

	_elapsed.start();
	connect(_elapsedTimer, &QTimer::timeout, this, &AiInferenceProgressDialog::UpdateElapsedTime);
	_elapsedTimer->start(1000);
}

void AiInferenceProgressDialog::SetOutput(const QString& text)
{
	output_label->setText(text);
}

void AiInferenceProgressDialog::SetLoadingResult()
{
	title_label->setText(tr("Almost done..."));
	message_label->setText(tr("The AI model finished. Loading its result into the editor."));
	cancel_button->setEnabled(false);
}

void AiInferenceProgressDialog::SetCancelling()
{
	title_label->setText(tr("Cancelling..."));
	message_label->setText(tr("Stopping the AI model. Its partial result will be discarded."));
	cancel_button->setEnabled(false);
}

/// Closing the window is a cancellation request, never a way to hide a running job.
void AiInferenceProgressDialog::reject()
{
	if (cancel_button->isEnabled())
	{
		emit CancelRequested();
	}
}

void AiInferenceProgressDialog::on_cancel_button_clicked(bool)
{
	reject();
}

/// Escape must not cancel a long inference by accident; only the button or the window close do.
void AiInferenceProgressDialog::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Escape)
	{
		event->accept();
		return;
	}
	QDialog::keyPressEvent(event);
}

void AiInferenceProgressDialog::UpdateElapsedTime()
{
	const qint64 seconds = _elapsed.elapsed() / 1000;
	elapsed_label->setText(tr("Elapsed time: %1:%2:%3")
		.arg(seconds / 3600, 2, 10, QChar('0'))
		.arg((seconds / 60) % 60, 2, 10, QChar('0'))
		.arg(seconds % 60, 2, 10, QChar('0')));
}

} // namespace smcp
