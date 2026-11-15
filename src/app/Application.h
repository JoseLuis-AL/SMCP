/*
Copyright (c) 2014, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the Brown University nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DANIEL MORENO AND GABRIEL TAUBIN BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once
#ifndef WINVER
#define WINVER 0x0500
#define _WIN32_WINNT 0x0500
#endif

#include <functional>

#include <QApplication>
#include <QByteArray>
#include <QSettings>

#include <opencv2/core/core.hpp>

#include "ui/models/TreeModel.h"
#include "app/MainWindow.h"
#include "ui/ProcessingDialog.h"
#include "core/Calibration.h"
#include "core/CalibrationData.h"
#include "core/Progress.h"
#include "core/Scan3d.h"
#include "common/Settings.h"

namespace smcp
{

enum Role {
	ImageFilenameRole = Qt::UserRole, GrayImageRole, ColorImageRole,
	ProjectorWidthRole, ProjectorHeightRole
};

/// Application-wide state and workflow controller.
///
/// It owns the settings, the capture-set model and the results of each processing stage, and it
/// turns user actions into jobs for the numerical core (src/core). Jobs run on a worker thread
/// through RunWithProcessingDialog()/RunWithProgressDialog(); the results are adopted on the GUI
/// thread when the job returns, so widgets never see half-written data.
class Application : public QApplication
{
	Q_OBJECT
public:
	Application(int& argc, char** argv);
	~Application();

	QSettings& GetSettings();

	//data dir
	void SetRootDir(const QString& dirname);
	QString GetRootDir(void) const;
	bool ChangeRootDir(QWidget* parentWidget = NULL);

	void Clear(void);

	const cv::Mat GetImage(unsigned level, unsigned n, Role role = GrayImageRole) const;
	int GetProjectorWidth(unsigned level = 0) const;
	int GetProjectorHeight(unsigned level = 0) const;

	/// Detects the chessboard of every selected set. Returns true when all of them were found.
	bool ExtractChessboardCorners(void);
	/// Decodes every selected set (sets already decoded with the current parameters are reused).
	void DecodeAll(void);
	/// Makes sure set `level` is decoded with the current parameters. False on failure or cancel.
	bool Decode(int level, QWidget* parentWidget = NULL);
	/// Corners, decoding, projector corners and solver; saves calibration.yml/.m and the corner lists.
	void Calibrate(void);

	bool LoadDump(const char* filename, int type, cv::Mat2f& patternImage, cv::Mat2b& minMaxImage, cv::Mat3b& colorImage) const;

	void LoadConfig(void);
	void ApplyTheme(void);

	//Detection/Decoding/Calibration processing
	inline void ProcessingReset(void) { processingDialog.Reset(); }

	//calibration
	bool LoadCalibration(QWidget* parentWidget = NULL);
	bool SaveCalibration(QWidget* parentWidget = NULL);

	//reconstruction
	void ReconstructModel(int level, Scan3d::Pointcloud& pointcloud, QWidget* parentWidget = NULL);
	/// `images` are the files of the capture set, when there is one: the sub-pixel reconstruction
	/// reads them again to locate the stripe borders. Without them (a decoded dump) the classic
	/// method is used whatever the setting says: sub-pixel without the images is less accurate than it.
	void ReconstructModelDump(cv::Mat2f const& patternImage, cv::Mat2b const& minMaxImage, cv::Mat3b const& colorImage, Scan3d::Pointcloud& pointcloud, QWidget* parentWidget = NULL,
		const std::vector<std::string>& images = std::vector<std::string>());
	void ComputeNormals(Scan3d::Pointcloud& pointcloud);

	void MakePatternImages(int level, cv::Mat& colImage, cv::Mat& rowImage);
	cv::Mat GetProjectorView(int level, bool forceUpdate = false);

	//model
	void SelectNone(void);
	void SelectAll(void);
	int SelectedSetCount(void) const;
	/// Sets used by the last successful calibration of this workspace.
	int CalibratedSetCount(void) const;

public slots:
	void deinit(void);

Q_SIGNALS:
	void root_dir_changed(const QString& dirname);

public:
	QSettings  config;
	TreeModel  model;

	CalibrationData calib;

	std::vector<std::vector<cv::Point3f> > cornersWorld;
	std::vector<std::vector<cv::Point2f> > cornersCamera;
	std::vector<std::vector<cv::Point2f> > cornersProjector;
	std::vector<cv::Mat> patternList;
	std::vector<cv::Mat> minMaxList;
	std::vector<cv::Mat> projectorViewList;
	Scan3d::Pointcloud pointcloud;

	MainWindow mainWin;
	mutable ProcessingDialog processingDialog;

private:
	/// Capture sets of the model as plain data for the core.
	std::vector<Calibration::CaptureSet> CaptureSets(void) const;
	Calibration::Params PipelineParams(void) const;
	/// Identifies what a decode result was computed from: files (path, size, date), b, m and projector size.
	QByteArray DecodeKey(const Calibration::CaptureSet& set, const Calibration::Params& params) const;
	/// Decoded data that is still valid for `sets` with `params`; empty entries must be decoded.
	std::vector<StructuredLight::DecodedSet> ValidDecodes(const std::vector<Calibration::CaptureSet>& sets, const Calibration::Params& params) const;
	void StoreDecode(std::size_t level, const StructuredLight::DecodedSet& decoded, const QByteArray& key);
	void ResizeResults(std::size_t count);

	/// Runs `work` on a worker thread, reporting into the processing dialog (already shown by the caller).
	void RunWithProcessingDialog(const std::function<void(ProgressSink&)>& work);
	/// Runs `work` on a worker thread behind a modal progress dialog. Returns false if the user aborted.
	bool RunWithProgressDialog(QWidget* parentWidget, const QString& label, const std::function<void(ProgressSink&)>& work);

	/// Shows in the set list what has been done with each set (icon and tooltip).
	void UpdateSetStatus(void);

	/// DecodeKey of the data in patternList/minMaxList, index-aligned; empty = not decoded.
	std::vector<QByteArray> _decodeKeys;
	/// Per set: the chessboard was searched for / the set took part in the last calibration.
	std::vector<char> _cornersSearched;
	std::vector<char> _usedInCalibration;
};

#define APP dynamic_cast<Application *>(Application::instance())

} // namespace smcp
