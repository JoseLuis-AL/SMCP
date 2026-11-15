#include "ui/pointcloud_editor/SphereMeasurementDialog.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>

namespace smcp
{

	SphereMeasurementDialog::SphereMeasurementDialog(
		const Settings& settings, const QString& layerName, QWidget* parent) :
		QDialog(parent)
	{
		setupUi(this);
		layer_radio->setText(
			layerName.isEmpty() ? tr("First layer (none loaded)") : tr("First layer: %1").arg(layerName));
		layer_radio->setEnabled(!layerName.isEmpty());
		(settings.useFolder || layerName.isEmpty() ? folder_radio : layer_radio)->setChecked(true);
		folder_edit->setText(QDir::toNativeSeparators(settings.folder));
		artifact_edit->setText(QDir::toNativeSeparators(settings.artifactPath));
		compensation_check->setChecked(settings.compensate);
		compensation_edit->setText(QDir::toNativeSeparators(settings.compensationPath));

		const Pointcloud::SphereMeasurementParams& params = settings.params;
		nominal_diameter_spin->setValue(params.nominalDiameter);
		radius_tolerance_spin->setValue(params.radiusTolerance * 100.0);
		ransac_threshold_spin->setValue(params.ransacThreshold);
		ransac_iterations_spin->setValue(params.ransacIterations);
		expected_spheres_spin->setValue(static_cast<int>(params.expectedSpheres));
		remove_planes_check->setChecked(params.removePlanes);
		add_layers_check->setChecked(settings.addSphereLayers);
		rejection_sigma_spin->setValue(params.rejectionSigma);
		LoadArtifactField();
	}

	SphereMeasurementDialog::Settings SphereMeasurementDialog::Values() const
	{
		Settings settings;
		settings.useFolder = folder_radio->isChecked();
		settings.folder = QDir::fromNativeSeparators(folder_edit->text().trimmed());
		settings.artifactPath = QDir::fromNativeSeparators(artifact_edit->text().trimmed());
		settings.compensate = compensation_check->isChecked();
		settings.compensationPath = QDir::fromNativeSeparators(compensation_edit->text().trimmed());
		settings.params.nominalDiameter = nominal_diameter_spin->value();
		settings.params.radiusTolerance = radius_tolerance_spin->value() / 100.0;
		settings.params.ransacThreshold = ransac_threshold_spin->value();
		settings.params.ransacIterations = ransac_iterations_spin->value();
		settings.params.expectedSpheres = static_cast<std::size_t>(expected_spheres_spin->value());
		settings.params.removePlanes = remove_planes_check->isChecked();
		settings.addSphereLayers = add_layers_check->isChecked();
		settings.params.rejectionSigma = rejection_sigma_spin->value();
		return settings;
	}

	bool SphereMeasurementDialog::LoadArtifact(
		const QString& path, Pointcloud::ArtifactReference& reference, QString& error)
	{
		reference = Pointcloud::ArtifactReference();
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly))
		{
			error = tr("Cannot open %1.").arg(QDir::toNativeSeparators(path));
			return false;
		}
		QJsonParseError parseError;
		const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
		if (!document.isObject())
		{
			error = tr("Invalid JSON: %1.").arg(parseError.errorString());
			return false;
		}

		reference.name = document.object().value("name").toString().trimmed().toStdString();
		const QJsonArray spheres = document.object().value("spheres").toArray();
		if (spheres.size() < 2)
		{
			error = tr("The artifact needs at least two entries in \"spheres\".");
			return false;
		}
		QStringList names;
		for (const QJsonValue& value : spheres)
		{
			const QJsonObject sphere = value.toObject();
			const QString name = sphere.value("name").toString().trimmed();
			if (name.isEmpty() || names.contains(name))
			{
				error = tr("Every sphere needs a unique \"name\".");
				return false;
			}
			names << name;
			reference.names.push_back(name.toStdString());
			reference.diameters.push_back(sphere.value("diameter").toDouble(0.0));
		}

		const std::size_t count = reference.names.size();
		reference.distances.assign(count, std::vector<double>(count, 0.0));
		for (const QJsonValue& value : document.object().value("distances").toArray())
		{
			const QJsonObject distance = value.toObject();
			const int from = names.indexOf(distance.value("from").toString().trimmed());
			const int to = names.indexOf(distance.value("to").toString().trimmed());
			if (from < 0 || to < 0 || from == to)
			{
				error = tr("Each distance needs \"from\" and \"to\" naming two different spheres.");
				return false;
			}
			reference.distances[from][to] = reference.distances[to][from] = distance.value("value").toDouble(0.0);
		}
		return true;
	}

	bool SphereMeasurementDialog::LoadArtifactField()
	{
		const QString path = artifact_edit->text().trimmed();
		_reference = Pointcloud::ArtifactReference();
		if (path.isEmpty())
		{
			artifact_summary_label->setText(tr("No reference: spheres are measured but not labeled or compared."));
			return true;
		}
		QString error;
		if (!LoadArtifact(QDir::fromNativeSeparators(path), _reference, error))
		{
			artifact_summary_label->setText(tr("Error: %1").arg(error));
			return false;
		}

		QStringList spheres;
		for (std::size_t i = 0; i < _reference.Count(); ++i)
		{
			spheres << QString("%1 ⌀%2")
						   .arg(QString::fromStdString(_reference.names[i]))
						   .arg(_reference.diameters[i], 0, 'f', 4);
		}
		int distances = 0;
		for (std::size_t i = 0; i < _reference.Count(); ++i)
		{
			for (std::size_t j = i + 1; j < _reference.Count(); ++j)
			{
				distances += _reference.distances[i][j] > 0.0 ? 1 : 0;
			}
		}
		artifact_summary_label->setText(tr("%1 spheres (%2), %3 distances between centers.")
				.arg(_reference.Count())
				.arg(spheres.join(", "))
				.arg(distances));
		expected_spheres_spin->setValue(static_cast<int>(_reference.Count()));
		return true;
	}

	void SphereMeasurementDialog::on_folder_browse_button_clicked(bool)
	{
		const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder with the scans"), folder_edit->text(),
			QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
		if (!folder.isEmpty())
		{
			folder_edit->setText(QDir::toNativeSeparators(folder));
			folder_radio->setChecked(true);
		}
	}

	void SphereMeasurementDialog::on_artifact_browse_button_clicked(bool)
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Reference artifact"), artifact_edit->text(),
			tr("Artifact (*.json)"), nullptr, QFileDialog::DontUseNativeDialog);
		if (!path.isEmpty())
		{
			artifact_edit->setText(QDir::toNativeSeparators(path));
			LoadArtifactField();
		}
	}

	void SphereMeasurementDialog::on_artifact_edit_editingFinished()
	{
		LoadArtifactField();
	}

	void SphereMeasurementDialog::on_done_button_clicked(bool)
	{
		if (folder_radio->isChecked())
		{
			const QDir folder(QDir::fromNativeSeparators(folder_edit->text().trimmed()));
			if (folder_edit->text().trimmed().isEmpty() || !folder.exists() ||
				folder.entryList({ "*.xyz" }, QDir::Files).isEmpty())
			{
				QMessageBox::warning(this, windowTitle(), tr("Choose a folder that contains .xyz scans."));
				return;
			}
		}
		if (!LoadArtifactField())
		{
			QMessageBox::warning(this, windowTitle(), artifact_summary_label->text());
			return;
		}
		if (_reference.Count() > 8)
		{
			QMessageBox::warning(this, windowTitle(), tr("Labeling supports at most 8 reference spheres."));
			return;
		}
		if (compensation_check->isChecked())
		{
			QString error;
			if (!Pointcloud::LoadCompensationModel(
					QDir::fromNativeSeparators(compensation_edit->text().trimmed()), _compensation, error))
			{
				QMessageBox::warning(this, windowTitle(), tr("The compensation model cannot be used: %1").arg(error));
				return;
			}
		}
		accept();
	}

	void SphereMeasurementDialog::on_compensation_browse_button_clicked(bool)
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Compensation model"), compensation_edit->text(),
			tr("Compensation model (*.json)"), nullptr, QFileDialog::DontUseNativeDialog);
		if (!path.isEmpty())
		{
			compensation_edit->setText(QDir::toNativeSeparators(path));
			compensation_check->setChecked(true);
		}
	}

	void SphereMeasurementDialog::on_cancel_button_clicked(bool)
	{
		reject();
	}

} // namespace smcp
