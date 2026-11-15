#include "ui/pointcloud_editor/PointcloudLayersPanel.h"

#include <algorithm>
#include <functional>

#include <QAbstractItemModel>
#include <QBrush>
#include <QItemSelectionModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/preview/PointcloudPreviewWidget.h"

namespace smcp
{

	namespace
	{
		/// Item roles of a row: its position in the viewer and the stable ID of its layer.
		constexpr int Index_Role = Qt::UserRole;
		constexpr int Id_Role = Qt::UserRole + 1;

		/// Side of the color square of a row, in logical pixels.
		constexpr int Swatch_Size = 14;

		/// Label that elides its text in the middle to fit, keeping both the prefix (icp_...) and the
		/// suffix ((aligned), (reference)) of long layer names visible.
		class ElidedLabel : public QLabel
		{
		public:
			using QLabel::QLabel;

			QSize minimumSizeHint() const override { return QSize(16, QLabel::minimumSizeHint().height()); }
			QSize sizeHint() const override { return QSize(16, QLabel::sizeHint().height()); }

		protected:
			void paintEvent(QPaintEvent*) override
			{
				QPainter painter(this);
				const QRect rect = contentsRect();
				const QString elided = fontMetrics().elidedText(text(), Qt::ElideMiddle, rect.width());
				style()->drawItemText(&painter, rect, alignment(), palette(), isEnabled(), elided, foregroundRole());
			}
		};

		QIcon ColorPreviewIcon(const QColor& color)
		{
			QPixmap preview(18, 18);
			preview.fill(Qt::transparent);
			QPainter painter(&preview);
			painter.setRenderHint(QPainter::Antialiasing, false);
			painter.setPen(QColor("#777777"));
			painter.setBrush(color.isValid() ? color : QColor(Qt::transparent));
			painter.drawRect(1, 1, 15, 15);
			if (!color.isValid())
			{
				painter.drawLine(2, 15, 15, 2);
			}
			return QIcon(preview);
		}

		/// Color square of a row: 4 px corners and a thin border, drawn at the screen's pixel ratio.
		/// The color belongs to each layer, so it is painted here and not in the theme.
		QIcon ColorSwatchIcon(const QColor& color, qreal pixelRatio)
		{
			const int side = qRound(Swatch_Size * pixelRatio);
			QPixmap swatch(side, side);
			swatch.setDevicePixelRatio(pixelRatio);
			swatch.fill(Qt::transparent);
			QPainter painter(&swatch);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(QPen(QColor("#8a8a8a"), 1));
			painter.setBrush(color);
			painter.drawRoundedRect(QRectF(0.5, 0.5, Swatch_Size - 1.0, Swatch_Size - 1.0), 4.0, 4.0);
			return QIcon(swatch);
		}

		/// The tree of the panel: a flat list of rows that can be dragged to reorder them.
		class LayerTree : public QTreeWidget
		{
		public:
			using QTreeWidget::QTreeWidget;
			std::function<void()> dropped;  ///< Called after a drag-and-drop move of rows.

		protected:
			void dropEvent(QDropEvent* event) override
			{
				QTreeWidget::dropEvent(event);
				if (dropped)
				{
					dropped();
				}
			}
		};

		/// Turns clicks on the parts of a row into actions: the color square opens the color chooser
		/// and a double click on the name renames. The check box keeps its standard behavior.
		class LayerRowDelegate : public QStyledItemDelegate
		{
		public:
			using QStyledItemDelegate::QStyledItemDelegate;
			std::function<void(const QModelIndex&)> colorClicked;
			std::function<void(const QModelIndex&)> nameDoubleClicked;

			bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option,
				const QModelIndex& index) override
			{
				const bool release = event->type() == QEvent::MouseButtonRelease;
				const bool doubleClick = event->type() == QEvent::MouseButtonDblClick;
				if (release || doubleClick)
				{
					const auto* mouse = static_cast<QMouseEvent*>(event);
					QStyleOptionViewItem row = option;
					initStyleOption(&row, index);
					const QStyle* style = row.widget ? row.widget->style() : nullptr;
					if (style && mouse->button() == Qt::LeftButton)
					{
						const QRect color = style->subElementRect(QStyle::SE_ItemViewItemDecoration, &row, row.widget);
						const QRect check = style->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &row, row.widget);
						if (color.contains(mouse->pos()))
						{
							if (release && colorClicked)
							{
								colorClicked(index);
							}
							return true;
						}
						if (doubleClick && !check.contains(mouse->pos()))
						{
							if (nameDoubleClicked)
							{
								nameDoubleClicked(index);
							}
							return true;
						}
					}
				}
				return QStyledItemDelegate::editorEvent(event, model, option, index);
			}
		};

	} // namespace

	/* Construction ============================================================================ */

	PointcloudLayersPanel::PointcloudLayersPanel(QWidget* parent) : QWidget(parent)
	{
		setMinimumWidth(200);

		auto* layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(6);

		_titleLabel = new QLabel(tr("Point Clouds"), this);
		_titleLabel->setObjectName("layer_title_label");
		auto* header = new QHBoxLayout();
		header->setObjectName("cloud_header");
		header->addWidget(_titleLabel);
		header->addStretch();
		layout->addLayout(header);

		// Styled in resources/theme/smcp.qss (#layer_tree).
		auto* tree = new LayerTree(this);
		_tree = tree;
		_tree->setObjectName("layer_tree");
		_tree->setColumnCount(1);
		_tree->setHeaderHidden(true);
		_tree->setRootIsDecorated(false);
		_tree->setIndentation(0);
		_tree->setUniformRowHeights(true);
		_tree->setIconSize(QSize(Swatch_Size, Swatch_Size));
		_tree->setTextElideMode(Qt::ElideMiddle);
		_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
		_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
		_tree->setDragDropMode(QAbstractItemView::InternalMove);
		_tree->setDefaultDropAction(Qt::MoveAction);
		_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		layout->addWidget(_tree, 1);

		// Under the tree, as in the Workspace of the main window: check all, uncheck all and the scope.
		auto* controls = new QHBoxLayout();
		controls->setObjectName("layer_controls");
		controls->setSpacing(6);
		_checkAllButton = new QPushButton(QIcon(":/square-check.svg"), tr("All"), this);
		_checkAllButton->setObjectName("check_all_button");
		_checkAllButton->setToolTip(tr("Check (show) every point cloud"));
		_uncheckAllButton = new QPushButton(QIcon(":/square-minus.svg"), tr("None"), this);
		_uncheckAllButton->setObjectName("uncheck_all_button");
		_uncheckAllButton->setToolTip(tr("Uncheck (hide) every point cloud"));
		for (auto* button : { _checkAllButton, _uncheckAllButton })
		{
			button->setIconSize(QSize(20, 20));
			button->setAccessibleName(button->toolTip());
			controls->addWidget(button);
		}
		_applyChecked = new QCheckBox(tr("Apply to all"), this);
		_applyChecked->setObjectName("apply_checked_check");
		_applyChecked->setToolTip(tr("Checked: commands, tools, AI models and export work on every checked point cloud. "
									 "Unchecked: only on the highlighted ones."));
		controls->addWidget(_applyChecked);
		controls->addStretch();
		layout->addLayout(controls);

		auto* delegate = new LayerRowDelegate(_tree);
		delegate->colorClicked = [this](const QModelIndex& index) { ChooseLayerColor(index.data(Index_Role).toInt()); };
		delegate->nameDoubleClicked = [this](const QModelIndex& index) { Rename(index.data(Index_Role).toInt()); };
		_tree->setItemDelegate(delegate);

		_detailsWidget = new QWidget(this);
		_detailsWidget->setObjectName("layer_details");
		auto* detailsLayout = new QVBoxLayout(_detailsWidget);
		detailsLayout->setContentsMargins(0, 0, 0, 0);
		detailsLayout->setSpacing(4);

		_detailsNameLabel = new ElidedLabel(_detailsWidget);
		_detailsNameLabel->setObjectName("layer_details_name");
		detailsLayout->addWidget(_detailsNameLabel);

		_detailsInfoLabel = new QLabel(_detailsWidget);
		_detailsInfoLabel->setObjectName("layer_details_info");
		_detailsInfoLabel->setWordWrap(true);
		detailsLayout->addWidget(_detailsInfoLabel);

		layout->addWidget(_detailsWidget);

		connect(_tree, &QTreeWidget::itemSelectionChanged, this,
			[this]()
			{
				if (_rebuilding)
					return;
				_highlightedIds = HighlightedLayerIds();
				if (UpdateTargets())
					emit SelectedLayerChanged();
				UpdateDetails();
				emit HighlightChanged();

				// Several rows highlighted together are a group to work on: the hidden ones among
				// them are checked too. One highlighted row keeps its check, so a hidden cloud can
				// still be renamed or deleted without showing it. Deferred like any check change.
				std::vector<std::pair<int, bool>> toCheck;
				for (int row = 0; _highlightedIds.size() >= 2 && row < _tree->topLevelItemCount(); ++row)
				{
					const auto* item = _tree->topLevelItem(row);
					if (item->isSelected() && item->checkState(0) != Qt::Checked)
						toCheck.push_back({ item->data(0, Index_Role).toInt(), true });
				}
				if (!toCheck.empty())
					QTimer::singleShot(0, this, [this, toCheck]() { ApplyChecks(toCheck, tr("Show point clouds")); });
			});
		// A check box shows or hides its cloud. The viewer answers by rebuilding the tree, which must
		// not happen while the tree is still delivering this change, so the request is deferred.
		connect(_tree, &QTreeWidget::itemChanged, this,
			[this](QTreeWidgetItem* item, int)
			{
				if (_rebuilding || !_preview)
					return;
				const int index = item->data(0, Index_Role).toInt();
				const bool checked = item->checkState(0) == Qt::Checked;
				QTimer::singleShot(0, this,
					[this, index, checked]()
					{
						ApplyChecks({ { index, checked } }, checked ? tr("Show point cloud") : tr("Hide point cloud"));
					});
			});
		// Applying a drag-and-drop move is deferred for the same reason: the tree is still moving its rows.
		tree->dropped = [this]() { QTimer::singleShot(0, this, [this]() { ApplyRowOrder(); }); };
		connect(_applyChecked, &QCheckBox::toggled, this,
			[this]()
			{
				if (UpdateTargets())
					emit SelectedLayerChanged();
				UpdateDetails();
			});
		connect(_checkAllButton, &QPushButton::clicked, this, [this]() { SetAllChecked(true); });
		connect(_uncheckAllButton, &QPushButton::clicked, this, [this]() { SetAllChecked(false); });
		auto* deleteShortcut = new QShortcut(QKeySequence::Delete, _tree);
		deleteShortcut->setContext(Qt::WidgetWithChildrenShortcut);
		connect(deleteShortcut, &QShortcut::activated, this, [this]() { emit DeleteRequested(); });

		Rebuild();
	}

	void PointcloudLayersPanel::SetPreview(PointcloudPreviewWidget* preview)
	{
		if (_preview)
		{
			disconnect(_preview, nullptr, this, nullptr);
		}
		_preview = preview;
		if (_preview)
		{
			connect(_preview, &PointcloudPreviewWidget::PointcloudsChanged, this, &PointcloudLayersPanel::Rebuild);
			// Undo, redo and opening a project bring back the targets they had. With the scope on
			// every checked cloud the targets do not depend on the highlight, which is left alone.
			connect(_preview, &PointcloudPreviewWidget::ActiveLayersRestored, this,
				[this]()
				{
					if (AppliesToAllChecked())
						return;
					// The viewer reports this after every change, most of which are the targets this
					// panel has just set. Only different targets (an undo, a redo, an opened project)
					// replace the highlight; otherwise highlighted hidden rows would lose it.
					std::vector<int> restored = _preview->CaptureScene().activeLayers;
					std::vector<int> current = _selectedIds;
					std::sort(restored.begin(), restored.end());
					std::sort(current.begin(), current.end());
					if (restored != current)
						SelectLayers(_preview->CaptureScene().activeLayers);
				});
		}
		Rebuild();
	}

	/* Rows ==================================================================================== */

	void PointcloudLayersPanel::Rebuild()
	{
		const auto layers = _preview ? _preview->Layers() : std::vector<PointcloudPreviewWidget::LayerInfo>();
		_titleLabel->setText(tr("Point Clouds"));

		// The viewer reports every change, also the ones that keep the same layers in the same
		// order (a highlight, a check, a color, a name). Then the rows are updated where they are:
		// recreating them would send the scroll back to the top and forget the row that Shift+click
		// extends the highlight from.
		bool sameRows = _tree->topLevelItemCount() == static_cast<int>(layers.size());
		for (int i = 0; sameRows && i < static_cast<int>(layers.size()); ++i)
			sameRows = _tree->topLevelItem(i)->data(0, Id_Role).toInt() == layers[i].id;

		_rebuilding = true;
		{
			const QSignalBlocker blocker(_tree);
			// When the rows do change (a cloud added, removed or moved) the row with the keyboard
			// focus is found again by its layer.
			const QTreeWidgetItem* current = _tree->currentItem();
			const int currentId = current ? current->data(0, Id_Role).toInt() : -1;
			if (!sameRows)
			{
				_tree->clear();
				for (int i = 0; i < static_cast<int>(layers.size()); ++i)
				{
					auto* item = new QTreeWidgetItem();
					// Not a drop target: dropping between rows reorders them, and no row gets children.
					item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
					_tree->addTopLevelItem(item);
				}
			}
			for (int i = 0; i < static_cast<int>(layers.size()); ++i)
			{
				const auto& layer = layers[i];
				auto* item = _tree->topLevelItem(i);
				item->setData(0, Index_Role, i);
				item->setData(0, Id_Role, layer.id);
				item->setCheckState(0, layer.visible ? Qt::Checked : Qt::Unchecked);
				item->setText(0, layer.name);
				item->setIcon(0,
					ColorSwatchIcon(layer.color.isValid() ? layer.color : QColor(200, 200, 200), devicePixelRatioF()));
				item->setToolTip(0, layer.details.isEmpty() ? layer.name : layer.name + "\n" + layer.details);
				item->setForeground(0, layer.visible ? QBrush() : QBrush(QColor("#666666")));
				const bool highlighted = std::find(_highlightedIds.begin(), _highlightedIds.end(), layer.id) != _highlightedIds.end();
				if (item->isSelected() != highlighted)
					item->setSelected(highlighted);
				if (!sameRows && layer.id == currentId)
					_tree->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
			}
		}
		_rebuilding = false;

		// Rows that no longer exist leave the highlight.
		_highlightedIds = HighlightedLayerIds();
		const bool any = !layers.empty();
		_checkAllButton->setEnabled(any);
		_uncheckAllButton->setEnabled(any);
		const bool changed = UpdateTargets();
		UpdateDetails();
		if (changed)
			emit SelectedLayerChanged();
	}

	bool PointcloudLayersPanel::UpdateTargets()
	{
		std::vector<int> targets;
		const bool allChecked = AppliesToAllChecked();
		for (int row = 0; row < _tree->topLevelItemCount(); ++row)
		{
			const auto* item = _tree->topLevelItem(row);
			if (item->checkState(0) == Qt::Checked && (allChecked || item->isSelected()))
				targets.push_back(item->data(0, Id_Role).toInt());
		}
		const bool changed = targets != _selectedIds;
		_selectedIds = std::move(targets);
		_selectedId = _selectedIds.empty() ? -1 : _selectedIds.front();
		return changed;
	}

	void PointcloudLayersPanel::UpdateDetails()
	{
		// The details describe the first target or, when there is none, the first highlighted row.
		const int id = _selectedId >= 0 ? _selectedId : (_highlightedIds.empty() ? -1 : _highlightedIds.front());
		const int row = _preview ? _preview->IndexOfLayer(id) : -1;
		const auto layers = _preview ? _preview->Layers() : std::vector<PointcloudPreviewWidget::LayerInfo>();
		if (row < 0 || row >= static_cast<int>(layers.size()))
		{
			_detailsWidget->hide();
			return;
		}

		const auto& layer = layers[row];
		_detailsNameLabel->setText(layer.name);
		_detailsNameLabel->setToolTip(layer.name);

		const QStringList info = {
			tr("%1 points").arg(QLocale().toString(layer.pointCount)),
			layer.colorOverride.isValid() ? tr("Color: %1").arg(layer.colorOverride.name(QColor::HexRgb))
										  : tr("Color: point colors"),
		};
		_detailsInfoLabel->setText(info.join("\n"));
		_detailsWidget->show();
	}

	void PointcloudLayersPanel::ApplyRowOrder()
	{
		if (!_preview)
		{
			return;
		}
		std::vector<int> order;
		order.reserve(_tree->topLevelItemCount());
		for (int i = 0; i < _tree->topLevelItemCount(); ++i)
		{
			order.push_back(_tree->topLevelItem(i)->data(0, Index_Role).toInt());
		}
		_preview->ReorderPointclouds(order);
	}

	void PointcloudLayersPanel::SetAllChecked(bool checked)
	{
		const auto layers = _preview ? _preview->Layers() : std::vector<PointcloudPreviewWidget::LayerInfo>();
		std::vector<std::pair<int, bool>> changes;
		for (int i = 0; i < static_cast<int>(layers.size()); ++i)
		{
			if (layers[i].visible != checked)
				changes.push_back({ i, checked });
		}
		ApplyChecks(changes, checked ? tr("Check all point clouds") : tr("Uncheck all point clouds"));
	}

	void PointcloudLayersPanel::ApplyChecks(const std::vector<std::pair<int, bool>>& changes, const QString& label)
	{
		if (!_preview || changes.empty())
		{
			return;
		}
		auto layers = _preview->Layers();
		for (const auto& change : changes)
		{
			if (change.first >= 0 && change.first < static_cast<int>(layers.size()))
				layers[change.first].visible = change.second;
		}
		// The targets the panel will have once the checks are applied.
		std::vector<int> targets;
		const bool allChecked = AppliesToAllChecked();
		for (const auto& layer : layers)
		{
			const bool highlighted = std::find(_highlightedIds.begin(), _highlightedIds.end(), layer.id) != _highlightedIds.end();
			if (layer.visible && (allChecked || highlighted))
				targets.push_back(layer.id);
		}
		try
		{
			_preview->BeginCommand(label);
			for (const auto& change : changes)
				_preview->SetPointcloudVisible(change.first, change.second);
			_preview->SetActiveLayers(targets);
			_preview->EndCommand();
		}
		catch (const std::logic_error&)
		{
			// Another command is being published (the panel is disabled then, so this is not
			// expected): the tree goes back to the state of the viewer.
			_preview->AbortCommand();
			Rebuild();
		}
	}

	/* Actions ================================================================================= */

	void PointcloudLayersPanel::ChooseLayerColor(int index)
	{
		const auto layers = _preview ? _preview->Layers() : std::vector<PointcloudPreviewWidget::LayerInfo>();
		if (index < 0 || index >= static_cast<int>(layers.size()))
		{
			return;
		}

		const QStringList colors = { "None", "00529d", "f2a023", "e43b44", "8351bd", "f5f5f5", "03648b", "e5b83f",
			"d93f4b", "25855b" };
		const QColor currentOverride = layers[index].colorOverride;
		QString selected = currentOverride.isValid() ? currentOverride.name(QColor::HexRgb).mid(1) : "None";

		for (;;)
		{
			QInputDialog dialog(this);
			dialog.setWindowTitle(tr("Layer color"));
			dialog.setLabelText(tr("Color (RRGGBB or None):"));
			dialog.setComboBoxItems(colors);
			dialog.setComboBoxEditable(true);
			if (auto* combo = dialog.findChild<QComboBox*>())
			{
				combo->setIconSize(QSize(18, 18));
				for (int colorIndex = 1; colorIndex < colors.size(); ++colorIndex)
				{
					combo->setItemIcon(colorIndex, ColorPreviewIcon(QColor("#" + colors[colorIndex])));
				}
				combo->setItemIcon(0, ColorPreviewIcon(QColor()));
				combo->setEditText(selected);
			}

			if (dialog.exec() != QDialog::Accepted)
			{
				return;
			}
			if (auto* combo = dialog.findChild<QComboBox*>())
			{
				selected = combo->currentText().trimmed();
			}
			else
			{
				selected = dialog.textValue().trimmed();
			}
			if (selected.compare("None", Qt::CaseInsensitive) == 0)
			{
				_preview->SetPointcloudColor(index, QColor());
				return;
			}
			if (selected.startsWith('#'))
			{
				selected.remove(0, 1);
			}
			if (QRegularExpression("^[0-9A-Fa-f]{6}$").match(selected).hasMatch())
			{
				_preview->SetPointcloudColor(index, QColor("#" + selected));
				return;
			}

			QMessageBox::warning(
				this, tr("Invalid color"), tr("Enter None or a six-digit hexadecimal color such as 03648b."));
		}
	}

	void PointcloudLayersPanel::Rename(int index)
	{
		const auto layers = _preview ? _preview->Layers() : std::vector<PointcloudPreviewWidget::LayerInfo>();
		if (index < 0 || index >= static_cast<int>(layers.size()))
		{
			return;
		}
		bool accepted = false;
		const QString name = QInputDialog::getText(
			this, tr("Rename point cloud"), tr("Name:"), QLineEdit::Normal, layers[index].name, &accepted);
		if (accepted)
		{
			_preview->RenamePointcloud(index, name);
		}
	}

	/* Highlight, targets and scope =============================================================== */

	std::vector<int> PointcloudLayersPanel::HighlightedLayerIds() const
	{
		std::vector<int> ids;
		for (int row = 0; row < _tree->topLevelItemCount(); ++row)
		{
			const auto* item = _tree->topLevelItem(row);
			if (item->isSelected())
				ids.push_back(item->data(0, Id_Role).toInt());
		}
		return ids;
	}

	std::vector<int> PointcloudLayersPanel::LayersToDelete() const
	{
		return AppliesToAllChecked() ? _selectedIds : HighlightedLayerIds();
	}

	void PointcloudLayersPanel::SelectLayer(int id)
	{
		SelectLayers({ id });
	}

	void PointcloudLayersPanel::SelectLayers(const std::vector<int>& ids)
	{
		_highlightedIds = ids;
		Rebuild();
		emit SelectedLayerChanged();
	}

	bool PointcloudLayersPanel::AppliesToAllChecked() const
	{
		return _applyChecked->isChecked();
	}

	void PointcloudLayersPanel::SetApplyToAllChecked(bool enabled)
	{
		_applyChecked->setChecked(enabled);
	}

	void PointcloudLayersPanel::AddHeaderButtons(
		QToolButton* remove, QToolButton* merge, QToolButton* load, QToolButton* save)
	{
		auto* header = findChild<QHBoxLayout*>("cloud_header");
		header->addWidget(remove);
		header->addWidget(merge);
		header->addWidget(load);
		header->addWidget(save);
	}
} // namespace smcp
