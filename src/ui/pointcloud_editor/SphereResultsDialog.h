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

#include <vector>
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include <QDialog>
#include <QString>

#include "core/SphereScan.h"
#include "core/SphereMeasurement.h"
#include "ui_SphereResultsDialog.h"

namespace smcp
{
	/// Table of the measured spheres per scan (diameters, distances between centers, form errors, and the
	/// errors against the reference artifact), their mean and RMS, and the CSV and training-data exports.
	/// Original and scale-corrected values share one table, with separate error statistics and observations.
	/// Dimensional values default to micrometers; the unit checkbox also controls the measurement CSV.
	class SphereResultsDialog : public QDialog, public Ui::SphereResultsDialog
	{
		Q_OBJECT

	public:
		/// `note` appears in the observations, for example to say that the clouds were compensated.
		SphereResultsDialog(std::vector<SphereScanResult> results, Pointcloud::ArtifactReference reference,
			QString defaultDirectory, QString note = QString(), QWidget* parent = nullptr);
		~SphereResultsDialog() override;

		void reject() override;

	public slots:
		void on_export_csv_button_clicked(bool checked = false);
		void on_export_excel_button_clicked(bool checked = false);
		void on_export_training_button_clicked(bool checked = false);
		void on_close_button_clicked(bool checked = false);
		void on_show_mm_check_toggled(bool checked);
		void on_after_scale_check_toggled(bool checked);
		void on_centers_button_clicked(bool checked = false);

	signals:
		/// The user asked to see the sphere centers of every scan in the viewer, as small colored spheres.
		void CentersLayerRequested(const Pointcloud::ColorCloudPtr& cloud, const QString& name);

	private:
		/// Sphere names: the reference names, or S1..Sn without a reference.
		QStringList SphereNames() const;
		bool HasReference() const { return _reference.Count() > 0; }
		void FillTable();
		/// Fixed scale, validation and coverage, computed once from the results.
		void AnalyzeCampaign();
		void RunExport(std::function<QString()> work);
		bool _exporting = false;

		std::vector<SphereScanResult> _results;
		Pointcloud::ArtifactReference _reference;
		QString _defaultDirectory;
		QString _note;
		double _fixedScale = 1.0; ///< 1 without a reference.
		bool _hasScale = false;
		std::optional<double> _validationRms;
		Pointcloud::CoverageStats _coverage;
		int _coverageScans = 0;
		double _meanDiameter = 0.0;
	};
} // namespace smcp
