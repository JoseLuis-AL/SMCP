#pragma once

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QPushButton>
#include <QLayout>
#include <QStyle>

namespace smcp
{
	class EditorDialogStyle : public QObject
	{
	public:
		explicit EditorDialogStyle(QWidget* editor) : QObject(editor), _editor(editor)
		{
			qApp->installEventFilter(this);
		}

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override
		{
			if (event->type() != QEvent::Show)
				return false;
			auto* dialog = qobject_cast<QDialog*>(watched);
			if (!dialog)
				return false;
			auto* parent = dialog->parentWidget();
			while (parent && parent != _editor)
				parent = parent->parentWidget();
			if (!parent)
				return false;
			QList<QPushButton*> buttons;
			QPushButton* primary = nullptr;
			for (auto* box : dialog->findChildren<QDialogButtonBox*>())
				if (box->window() == dialog)
					for (auto* button : box->buttons())
					{
						const auto role = box->buttonRole(button);
						if (role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::RejectRole ||
							role == QDialogButtonBox::YesRole || role == QDialogButtonBox::NoRole ||
							role == QDialogButtonBox::DestructiveRole)
							if (auto* push = qobject_cast<QPushButton*>(button))
							{
								buttons << push;
								if (!primary && (role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole))
									primary = push;
							}
					}
			for (const auto* name : { "done_button", "cancel_button", "export_button" })
				if (auto* button = dialog->findChild<QPushButton*>(name))
					if (button->window() == dialog && !buttons.contains(button))
					{
						buttons << button;
						if (!primary && QByteArray(name) != "cancel_button")
							primary = button;
					}
			// One button per dialog is the action it exists for; the theme paints it with the accent.
			if (primary && !primary->property("primary").toBool())
			{
				primary->setProperty("primary", true);
				primary->style()->unpolish(primary);
				primary->style()->polish(primary);
			}
			int width = 0;
			for (auto* button : buttons)
				width = qMax(width, button->sizeHint().width());
			for (auto* button : buttons)
				button->setFixedWidth(width);
			for (auto* button : buttons)
				if (auto* layout = button->parentWidget()->layout())
				{
					layout->invalidate();
					layout->activate();
				}
			return false;
		}

	private:
		QWidget* _editor;
	};
}
