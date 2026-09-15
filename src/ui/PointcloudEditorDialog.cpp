#include "ui/PointcloudEditorDialog.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QIcon>

#include "export/IOExport.h"

namespace smcp
{

namespace
{
	/// Colores para distinguir los planos detectados.
	const QColor kPlaneColors[] = {
		QColor(4, 92, 195), QColor(237, 167, 59), QColor(62, 137, 62), QColor(60, 151, 139),
		QColor(228, 59, 68), QColor(26, 188, 156), QColor(230, 126, 34), QColor(149, 165, 166)
	};

	/// Colores para distinguir las esferas detectadas.
	const QColor kSphereColors[] = {
		QColor(231, 76, 60), QColor(52, 152, 219), QColor(46, 204, 113), QColor(241, 196, 15),
		QColor(155, 89, 182), QColor(26, 188, 156), QColor(230, 126, 34), QColor(149, 165, 166)
	};

	template <std::size_t N>
	const QColor& color_at(const QColor (&colors)[N], std::size_t i) { return colors[i % N]; }
} // namespace

/* Construccion ============================================================================ */

PointcloudEditorDialog::PointcloudEditorDialog(const QString& root_dir, QWidget* parent, Qt::WindowFlags flags)
	: QDialog(parent, flags), _root_dir(root_dir)
{
	setupUi(this);
	add_fit_models();
	update_pointcloud_combo();
	set_status(QString());
	showMaximized();
}

PointcloudEditorDialog::~PointcloudEditorDialog() = default;

/// Registra los modelos ajustables en el desplegable. Para anadir uno: nuevo valor en
/// FitModel, una entrada aqui y su rama en on_fit_button_clicked().
void PointcloudEditorDialog::add_fit_models()
{
	fit_model_combo->clear();
	fit_model_combo->addItem(QIcon(":/icon-plane.svg"), tr("Plane"), static_cast<int>(FitModel::Plane));
	fit_model_combo->addItem(QIcon(":/icon-sphere.svg"), tr("Sphere"), static_cast<int>(FitModel::Sphere));
	on_fit_model_combo_currentIndexChanged(fit_model_combo->currentIndex());
}

void PointcloudEditorDialog::set_status(const QString& text)
{
	status_label->setText(text);
}

/// Deshabilita la barra de comandos y muestra el cursor de espera mientras un comando
/// se ejecuta en segundo plano.
void PointcloudEditorDialog::set_busy(bool busy)
{
	if (_busy == busy)
	{
		return;
	}
	_busy = busy;

	for (QWidget* w : { static_cast<QWidget*>(pointcloud_combo), static_cast<QWidget*>(refresh_button),
			 static_cast<QWidget*>(remove_outliers_button), static_cast<QWidget*>(fit_model_combo),
			 static_cast<QWidget*>(fit_button), static_cast<QWidget*>(save_button), static_cast<QWidget*>(close_button) })
	{
		w->setEnabled(!busy);
	}

	if (busy)
	{
		QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
	}
	else
	{
		QApplication::restoreOverrideCursor();
	}
}

void PointcloudEditorDialog::reset_result()
{
	_current_cloud.reset();
	_original_cloud.reset();
	_planes.clear();
	_last_command = LastCommand::None;
	preview_widget->clearPointclouds();
}

/// El boton de cierre de la ventana no interrumpe un comando en curso.
void PointcloudEditorDialog::reject()
{
	if (_busy)
	{
		return;
	}
	QDialog::reject();
}

/* Nube de puntos ========================================================================== */

void PointcloudEditorDialog::update_pointcloud_combo()
{
	pointcloud_combo->clear();

	const QDir dir(_root_dir);
	for (const QFileInfo& file : dir.entryInfoList({ "*.xyz" }, QDir::Files, QDir::Name))
	{
		pointcloud_combo->addItem(file.fileName());
	}
}

void PointcloudEditorDialog::on_refresh_button_clicked(bool)
{
	update_pointcloud_combo();
}

void PointcloudEditorDialog::on_pointcloud_combo_currentIndexChanged(int index)
{
	reset_result();
	if (index < 0 || pointcloud_combo->currentText().isEmpty())
	{
		return;
	}

	const QString name = pointcloud_combo->currentText();
	const std::string filepath = (_root_dir + "/" + name).toStdString();

	run_async<pointcloud::ColorCloudPtr>(
		tr("Cargando nube de puntos..."),
		[filepath]() {
			auto cloud = std::make_shared<pointcloud::ColorCloud>();
			return IOExport::read_xyz(filepath, *cloud) ? cloud : pointcloud::ColorCloudPtr();
		},
		[this, name](const pointcloud::ColorCloudPtr& cloud) {
			if (!cloud)
			{
				set_status(tr("Error: no se pudo abrir %1.").arg(name));
				return;
			}
			_current_cloud = cloud;
			_original_cloud = std::make_shared<pointcloud::ColorCloud>(*cloud);  // copia para comparar
			preview_widget->addPointcloud(_current_cloud, QColor(), name);
			set_status(tr("Nube de puntos cargada (%1 puntos).").arg(_current_cloud->size()));
		});
}

/* Comandos ================================================================================ */

void PointcloudEditorDialog::on_remove_outliers_button_clicked(bool)
{
	if (!_current_cloud || _current_cloud->empty())
	{
		set_status(tr("No hay nube de puntos cargada."));
		return;
	}

	const pointcloud::ColorCloudPtr input = _current_cloud;
	run_async<pointcloud::ColorCloudPtr>(
		tr("Removiendo outliers..."),
		[input]() {
			pointcloud::OutlierRemovalParams params;  // k = 50, 1.0 desviaciones
			return pointcloud::remove_statistical_outliers(*input, params);
		},
		[this, input](const pointcloud::ColorCloudPtr& filtered) {
			const auto removed = input->size() - filtered->size();
			_current_cloud = filtered;
			_planes.clear();
			_last_command = LastCommand::Other;

			preview_widget->clearPointclouds();
			preview_widget->addPointcloud(_current_cloud, QColor(), tr("Filtrada"));
			preview_widget->addPointcloud(_original_cloud, QColor(Qt::red), tr("Original"));

			set_status(tr("Outliers removidos (%1 puntos eliminados, %2 restantes).").arg(removed).arg(_current_cloud->size()));
		});
}

void PointcloudEditorDialog::on_fit_model_combo_currentIndexChanged(int index)
{
	// El boton "Fit" muestra el icono del modelo elegido.
	fit_button->setIcon(fit_model_combo->itemIcon(index));
}

void PointcloudEditorDialog::on_fit_button_clicked(bool)
{
	if (!_current_cloud || _current_cloud->empty())
	{
		set_status(tr("No hay nube de puntos cargada para ajustar un modelo."));
		return;
	}

	switch (static_cast<FitModel>(fit_model_combo->currentData().toInt()))
	{
	case FitModel::Plane:
		fit_planes();
		break;
	case FitModel::Sphere:
		fit_spheres();
		break;
	}
}

/// Extrae los planos dominantes por RANSAC y los muestra coloreados.
void PointcloudEditorDialog::fit_planes()
{
	const pointcloud::ColorCloudPtr input = _current_cloud;
	run_async<std::vector<pointcloud::PlaneFit>>(
		tr("Detectando planos..."),
		[input]() {
			pointcloud::PlaneFitParams params;  // hasta 5 planos, 1000 iteraciones, umbral 0.5
			return pointcloud::fit_planes(*input, params);
		},
		[this](const std::vector<pointcloud::PlaneFit>& planes) {
			preview_widget->clearPointclouds();
			_planes.clear();

			if (planes.empty())
			{
				_last_command = LastCommand::Other;
				set_status(tr("No se pudieron estimar planos para la nube de puntos."));
				return;
			}

			auto merged = std::make_shared<pointcloud::ColorCloud>();
			for (std::size_t i = 0; i < planes.size(); ++i)
			{
				_planes.push_back(planes[i].points);
				merged->insert(merged->end(), planes[i].points->begin(), planes[i].points->end());
				preview_widget->addPointcloud(planes[i].points, color_at(kPlaneColors, i), tr("Plano %1").arg(i + 1));
			}

			_current_cloud = merged;
			_last_command = LastCommand::FitPlanes;
			set_status(tr("Se detectaron %1 plano(s) con %2 puntos en total.").arg(planes.size()).arg(_current_cloud->size()));
		});
}

/// Elimina primero los planos dominantes (RANSAC con 3 puntos converge facil) y ajusta
/// esferas sobre el resto: asi la fraccion de inliers de esfera es suficiente para que
/// RANSAC con 4 puntos converja en pocos miles de iteraciones.
void PointcloudEditorDialog::fit_spheres()
{
	const pointcloud::ColorCloudPtr input = _current_cloud;
	run_async<std::vector<pointcloud::SphereFit>>(
		tr("Detectando esferas..."),
		[input]() {
			pointcloud::ColorCloudPtr remaining;
			pointcloud::fit_planes(*input, pointcloud::PlaneFitParams(), &remaining);
			if (!remaining)
			{
				remaining = std::make_shared<pointcloud::ColorCloud>();
			}

			pointcloud::SphereFitParams params;  // hasta 5 esferas, umbral 0.25, radio en [1, 10000]
			params.min_inliers = std::max<std::size_t>(50u, input->size() / 100u);
			return pointcloud::fit_spheres(*remaining, params);
		},
		[this](const std::vector<pointcloud::SphereFit>& spheres) {
			preview_widget->clearPointclouds();
			_planes.clear();

			if (spheres.empty())
			{
				_last_command = LastCommand::Other;
				set_status(tr("No se pudieron estimar esferas para la nube de puntos."));
				return;
			}

			auto merged = std::make_shared<pointcloud::ColorCloud>();
			QStringList details;
			for (std::size_t i = 0; i < spheres.size(); ++i)
			{
				const auto& sphere = spheres[i];
				merged->insert(merged->end(), sphere.points->begin(), sphere.points->end());
				preview_widget->addPointcloud(sphere.points, color_at(kSphereColors, i), tr("Esfera %1").arg(i + 1));
				details << tr("esfera %1: centro=(%2, %3, %4) radio=%5 (%6 puntos)")
					.arg(i + 1)
					.arg(sphere.model.center.x, 0, 'f', 3)
					.arg(sphere.model.center.y, 0, 'f', 3)
					.arg(sphere.model.center.z, 0, 'f', 3)
					.arg(sphere.model.radius, 0, 'f', 3)
					.arg(sphere.points->size());
			}

			_current_cloud = merged;
			_last_command = LastCommand::FitSpheres;
			set_status(tr("Se detectaron %1 esfera(s) con %2 puntos en total: %3")
				.arg(spheres.size())
				.arg(_current_cloud->size())
				.arg(details.join("; ")));
		});
}

void PointcloudEditorDialog::on_save_button_clicked(bool)
{
	if (!_current_cloud || _current_cloud->empty())
	{
		set_status(tr("No hay nube de puntos para guardar."));
		return;
	}

	const QString filepath = QFileDialog::getSaveFileName(this, tr("Guardar nube de puntos"), _root_dir,
		tr("Point Cloud (*.xyz);;All files (*)"));
	if (filepath.isEmpty())
	{
		return;
	}

	set_status(tr("Guardando..."));

	// Tras "Fit Plane" se guarda un archivo por plano: <nombre>_1.xyz, <nombre>_2.xyz, ...
	if (_last_command == LastCommand::FitPlanes && !_planes.empty())
	{
		const QFileInfo info(filepath);
		const QString suffix = info.suffix();
		for (std::size_t i = 0; i < _planes.size(); ++i)
		{
			const QString name = suffix.isEmpty()
				? QString("%1_%2").arg(info.completeBaseName()).arg(i + 1)
				: QString("%1_%2.%3").arg(info.completeBaseName()).arg(i + 1).arg(suffix);
			if (!IOExport::write_xyz(QDir(info.absolutePath()).filePath(name).toStdString(), *_planes[i]))
			{
				set_status(tr("Error: no se pudieron guardar todos los planos."));
				return;
			}
		}
		set_status(tr("Guardados %1 plano(s) usando el prefijo %2.").arg(_planes.size()).arg(info.fileName()));
		return;
	}

	if (!IOExport::write_xyz(filepath.toStdString(), *_current_cloud))
	{
		set_status(tr("Error: no se pudo abrir el archivo para escritura."));
		return;
	}
	set_status(tr("Guardado: %1 (%2 puntos).").arg(QFileInfo(filepath).fileName()).arg(_current_cloud->size()));
}

void PointcloudEditorDialog::on_close_button_clicked(bool)
{
	if (_busy)
	{
		return;
	}
	reset_result();
	accept();
}

} // namespace smcp
