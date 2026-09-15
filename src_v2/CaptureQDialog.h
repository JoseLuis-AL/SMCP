#pragma once

// UI.
#include "ui_CaptureQDialog.h"

// OpenCV, Spinnaker
#include <SystemPtr.h>
#include <CameraList.h>

// Qt
#include <QDialog>

// Camera & Projector handler
#include "CameraWorker.h"
#include "SpinnakerCaptureWorker.h"
#include "ProjectorWidget.hpp"

namespace SMCP
{
	class CaptureQDialog : public QDialog, public Ui::CaptureQDialogClass
	{
		Q_OBJECT

	public:
		explicit CaptureQDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowMaximizeButtonHint);
		~CaptureQDialog() override;

	public slots:
		void on_close_cancel_button_clicked(bool checked = false);
		void _on_root_dir_changed(const QString& dirname) const;
		void on_output_dir_line_textEdited(const QString& text);
		void on_output_dir_button_clicked(bool checked = false);

		// Projector.
		void on_screen_combo_currentIndexChanged(int index);
		void on_test_check_stateChanged(int state);
		void on_test_prev_button_clicked(bool checked = false);
		void on_test_next_button_clicked(bool checked = false);
		void auto_next();

		// Preview.
		void _on_new_projector_image(QPixmap image) const;
		void _on_new_camera_frame(const QPixmap& new_frame, const QString& gray_stats) const;

		// Alignment mode.
		void on_alignment_mode_check_stateChanged(int new_state);

		// Camera settings.
		void on_camera_combo_currentIndexChanged(int index);
		void on_camera_black_level_spin_valueChanged(double new_value);
		void on_camera_exposure_spin_valueChanged(double new_value);
		void on_camera_gain_spin_valueChanged(double new_value);
		void on_camera_gamma_spin_valueChanged(double new_value);
		void _on_new_camera_settings(const CameraSettings& settings) const;

		void _on_image_stored();
		void _on_image_saved(int total_images_to_save, int current_image_saved) const;
		void _on_all_images_saved();

		// Capture.
		void on_capture_button_clicked(bool checked = false);

		// Camera signals.
	signals:
		void onNewCameraBlackLevelSignal(double new_value);
		void onNewCameraExposureTimeSignal(double new_value);
		void onNewCameraGainSignal(double new_value);
		void onNewCameraGammaSignal(double new_value);

		// Alignment signals.
		void _on_alignment_signal(bool is_align_mode_active);

		// Image saving signals.
		void startCaptureSignal(int n_pattern);
		void endCaptureSignal();
		void needStoreImageSignal(const QString& filename);

	private:

		/* METHODS ================================================================================= */
		// Init.
		void init_spinnaker();
		void init_signals();
		void init_controls() const;

		// Camera.
		void start_camera();
		void stop_camera();
		void setup_camera_thread();
		void update_camera_combo();

		// Projector.
		int update_screen_combo() const;

		// Utilities.
		static void wait_time(int milliseconds);
		void enable_controls() const;
		void disable_controls() const;

		/* ATTRIBUTES ============================================================================== */
		// Projector.
		ProjectorWidget projector_widget;

		// Camera [Spinnaker].
		Spinnaker::SystemPtr spinnaker_system_ptr;
		Spinnaker::CameraPtr camera_ptr;
		Spinnaker::CameraList camera_list;

		QThread camera_thread;
		CameraWorker* camera_worker;
		QString camera_serial_number;

		int camera_idx{ -1 };
		int n_cameras{ 0 };

		// Image processing.
		bool is_storing_image{ false };
		bool is_saving_image{ false };

		// Working directory.
		QString session;
	};
}
