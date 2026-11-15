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

#include <QDialog>
#include <QString>

#include "core/Compensation.h"
#include "core/SphereMeasurement.h"
#include "ui_SphereMeasurementDialog.h"

namespace smcp
{
/// Configures sphere measurement: the source (the first layer or every .xyz scan in a folder), the
/// reference artifact measured on a CMM, and the detection and fitting parameters.
class SphereMeasurementDialog : public QDialog, public Ui::SphereMeasurementDialog
{
	Q_OBJECT

public:
	struct Settings
	{
		Pointcloud::SphereMeasurementParams params;
		bool useFolder = false;
		QString folder;
		QString artifactPath;  ///< Empty: measure without a reference (no labels or errors).
		bool compensate = false;  ///< Correct every cloud with the compensation model before measuring.
		QString compensationPath;
		/// Add every fitted sphere to the editor as a layer (sphere_A, sphere_B, ...). Off: the
		/// measurement only opens the results table and leaves the layers as they are.
		bool addSphereLayers = true;
	};

	/// `layerName` is the first layer, or empty when the editor has none.
	SphereMeasurementDialog(const Settings& settings, const QString& layerName, QWidget* parent = nullptr);

	Settings Values() const;
	/// Reference read from the artifact file when the dialog was accepted (Count() == 0 without one).
	const Pointcloud::ArtifactReference& Reference() const { return _reference; }
	/// Compensation model loaded when the dialog was accepted with compensation enabled.
	const Pointcloud::CompensationModel& Compensation() const { return _compensation; }

	/// Reads an artifact file: {"name": "...", "spheres": [{"name": "A", "diameter": 100.01}, ...],
	/// "distances": [{"from": "A", "to": "B", "value": 250.02}, ...]}, in millimeters.
	static bool LoadArtifact(const QString& path, Pointcloud::ArtifactReference& reference, QString& error);

public slots:
	void on_done_button_clicked(bool checked = false);
	void on_cancel_button_clicked(bool checked = false);
	void on_folder_browse_button_clicked(bool checked = false);
	void on_artifact_browse_button_clicked(bool checked = false);
	void on_artifact_edit_editingFinished();
	void on_compensation_browse_button_clicked(bool checked = false);

private:
	/// Loads the artifact named in the field and shows its summary; true when it is empty or valid.
	bool LoadArtifactField();

	Pointcloud::ArtifactReference _reference;
	Pointcloud::CompensationModel _compensation;
};
} // namespace smcp
