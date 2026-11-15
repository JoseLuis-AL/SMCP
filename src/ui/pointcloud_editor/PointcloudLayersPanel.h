/*
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
*/

#pragma once

#include <QWidget>
#include <utility>
#include <vector>

class QCheckBox;
class QLabel;
class QPushButton;
class QToolButton;
class QTreeWidget;

namespace smcp
{
	class PointcloudPreviewWidget;

	/// Point Clouds panel: a tree with one row per layer of the viewer, like the Workspace tree of
	/// the main window. Each row has a check box, a color square and the name of the cloud.
	///
	/// A layer has two independent states:
	///  - checked: the cloud is shown. Unchecking a row hides it.
	///  - highlighted: the row is selected in the tree (click; Ctrl or Shift for several).
	///
	/// The clouds a command works on are its targets, SelectedLayerIds(): the highlighted rows
	/// that are checked or, with "Apply to all" on, every checked row. Highlighting several rows
	/// at once also checks them, so a group picked with Shift or Ctrl is ready to work on.
	/// All, None and "Apply to all" sit in one row under the tree, like the Workspace buttons. Rows and their
	/// stable IDs mirror the viewer; dragging rows changes the drawing priority.
	class PointcloudLayersPanel : public QWidget
	{
		Q_OBJECT

	public:
		explicit PointcloudLayersPanel(QWidget* parent = nullptr);

		/// Lists the layers of `preview` and keeps the tree in sync with it.
		void SetPreview(PointcloudPreviewWidget* preview);
		/// Topmost target cloud ID, or -1.
		int SelectedLayerId() const { return _selectedId; }
		/// Targets of the commands, in the order of the rows.
		std::vector<int> SelectedLayerIds() const { return _selectedIds; }
		/// Highlighted rows, checked or not, in the order of the rows.
		std::vector<int> HighlightedLayerIds() const;
		/// Rows the Delete key removes: every checked one with "Apply to all checked" on, the
		/// highlighted ones (also when they are hidden) otherwise.
		std::vector<int> LayersToDelete() const;
		/// Highlights the row of `id` only.
		void SelectLayer(int id);
		void SelectLayers(const std::vector<int>& ids);
		bool AppliesToAllChecked() const;
		void SetApplyToAllChecked(bool enabled);
		/// Checks (shows) or unchecks (hides) every layer as one undoable command.
		void SetAllChecked(bool checked);
		void AddHeaderButtons(QToolButton* remove, QToolButton* merge, QToolButton* load, QToolButton* save);

	signals:
		/// The targets changed (by the user, or because a target was removed or hidden).
		void SelectedLayerChanged();
		/// The highlighted rows changed (the targets may be the same, for example on hidden rows).
		void HighlightChanged();
		/// The user pressed Delete on the tree.
		void DeleteRequested();

	private:
		void Rebuild();
		/// Recomputes the targets from the highlight, the checks and the scope; true when they changed.
		bool UpdateTargets();
		void UpdateDetails();
		/// Applies a drag-and-drop move of the rows to the viewer's drawing order.
		void ApplyRowOrder();
		/// Shows or hides layers (pairs of row and new state) and makes the resulting targets the
		/// active layers of the viewer, all as one command, so one undo step brings both back.
		void ApplyChecks(const std::vector<std::pair<int, bool>>& changes, const QString& label);
		void ChooseLayerColor(int index);
		void Rename(int index);

		PointcloudPreviewWidget* _preview = nullptr;
		QLabel* _titleLabel = nullptr;
		QTreeWidget* _tree = nullptr;
		QCheckBox* _applyChecked = nullptr;
		QPushButton* _checkAllButton = nullptr;
		QPushButton* _uncheckAllButton = nullptr;
		QWidget* _detailsWidget = nullptr;
		QLabel* _detailsNameLabel = nullptr;
		QLabel* _detailsInfoLabel = nullptr;
		/// Highlighted rows by ID, retained across rebuilds of the tree.
		std::vector<int> _highlightedIds;
		std::vector<int> _selectedIds;
		int _selectedId = -1;
		bool _rebuilding = false;
	};
} // namespace smcp
