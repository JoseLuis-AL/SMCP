#pragma once

#include <QAbstractItemView>
#include <QComboBox>
#include <QEvent>
#include <QMenu>
#include <QObject>
#include <QPainterPath>
#include <QRegion>
#include <QStyledItemDelegate>
#include <QTimer>

namespace smcp
{
	// QSS cannot disable the native popup shadow or clip the popup window's corners. This filter does
	// both for the list of every combobox (4 px corners) and for every menu (8 px corners).
	class ComboBoxPopupStyle : public QObject
	{
	public:
		explicit ComboBoxPopupStyle(QObject* application) : QObject(application)
		{
			application->installEventFilter(this);
		}

	protected:
		bool eventFilter(QObject* watched, QEvent* event) override
		{
			if (event->type() == QEvent::Polish)
			{
				if (auto* combo = qobject_cast<QComboBox*>(watched))
				{
					// The styled delegate honors the popup row height and padding from QSS.
					combo->setItemDelegate(new QStyledItemDelegate(combo));
					auto* popup = combo->view()->window();
					popup->setObjectName("combo_popup");
					popup->setWindowFlag(Qt::NoDropShadowWindowHint);
				}
				else if (auto* menu = qobject_cast<QMenu*>(watched))
				{
					menu->setWindowFlag(Qt::NoDropShadowWindowHint);
				}
			}
			else if (event->type() == QEvent::Show || event->type() == QEvent::Resize)
			{
				if (auto* popup = qobject_cast<QWidget*>(watched))
					if (popup->objectName() == "combo_popup" || qobject_cast<QMenu*>(popup))
					{
						const auto roundCorners = [popup]()
						{
							QPainterPath corners;
							const int radius = popup->objectName() == "combo_popup" ? 4 : 8;
							corners.addRoundedRect(QRectF(popup->rect()), radius, radius);
							popup->setMask(QRegion(corners.toFillPolygon().toPolygon()));
						};
						roundCorners();
						// Qt's native showPopup() can reset the mask after Show has been delivered.
						QTimer::singleShot(0, popup, roundCorners);
					}
			}
			return QObject::eventFilter(watched, event);
		}
	};
}
