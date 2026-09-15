#pragma once

#include <functional>
#include <vector>

#include <QDialog>
#include <QFutureWatcher>
#include <QString>
#include <QtConcurrent/QtConcurrentRun>

#include "core/pointcloud_ops.h"
#include "ui_PointcloudEditorDialog.h"

namespace smcp
{
/// <summary>
/// Editor de nubes de puntos: carga los .xyz del directorio de trabajo, permite eliminar
/// outliers y ajustar modelos geometricos (plano, esfera) por RANSAC, compara el resultado
/// con la nube original en el visor 3D y guarda el resultado.
/// </summary>
/// <remarks>
/// Los modelos ajustables se registran en el desplegable `fit_model_combo` (enum FitModel);
/// para anadir uno nuevo basta con extender el enum, `add_fit_models()` y `on_fit_button_clicked()`.
/// Los comandos se ejecutan en un hilo de trabajo (QtConcurrent) con la barra de comandos
/// deshabilitada, de modo que la interfaz sigue respondiendo con nubes grandes.
/// </remarks>
class PointcloudEditorDialog : public QDialog, public Ui::PointcloudEditorDialog
{
	Q_OBJECT

public:
	explicit PointcloudEditorDialog(const QString& root_dir, QWidget* parent = nullptr,
		Qt::WindowFlags flags = Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
	~PointcloudEditorDialog() override;

	void reject() override;

public slots:
	// Point cloud.
	void on_refresh_button_clicked(bool checked = false);
	void on_pointcloud_combo_currentIndexChanged(int index);

	// Commands.
	void on_remove_outliers_button_clicked(bool checked = false);
	void on_fit_model_combo_currentIndexChanged(int index);
	void on_fit_button_clicked(bool checked = false);
	void on_save_button_clicked(bool checked = false);
	void on_close_button_clicked(bool checked = false);

private:
	/// Modelos geometricos disponibles en `fit_model_combo` (guardados como userData).
	enum class FitModel
	{
		Plane,
		Sphere
	};

	enum class LastCommand
	{
		None,
		FitPlanes,
		FitSpheres,
		Other
	};

	void add_fit_models();
	void update_pointcloud_combo();
	void fit_planes();
	void fit_spheres();
	void set_status(const QString& text);
	void set_busy(bool busy);
	void reset_result();

	/// Ejecuta `work` en un hilo de trabajo y, al terminar, `done(result)` en el hilo de la
	/// interfaz. Mientras tanto la barra de comandos queda deshabilitada.
	template <typename Result>
	void run_async(const QString& status, std::function<Result()> work, std::function<void(const Result&)> done)
	{
		set_busy(true);
		set_status(status);

		auto* watcher = new QFutureWatcher<Result>(this);
		connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, done]() {
			done(watcher->result());
			watcher->deleteLater();
			set_busy(false);
		});
		watcher->setFuture(QtConcurrent::run(work));
	}

	/// Directorio en el que se buscan los archivos .xyz.
	QString _root_dir;

	/// Nube actual (resultado del ultimo comando) y copia sin modificar para comparar.
	pointcloud::ColorCloudPtr _current_cloud;
	pointcloud::ColorCloudPtr _original_cloud;

	/// Planos individuales de la ultima deteccion (para guardarlos por separado).
	std::vector<pointcloud::ColorCloudPtr> _planes;

	LastCommand _last_command = LastCommand::None;
	bool _busy = false;
};
} // namespace smcp
