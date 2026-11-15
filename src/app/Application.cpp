/*
Copyright (c) 2014, Daniel Moreno and Gabriel Taubin
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

#include "app/Application.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QProgressDialog>
#include <QStyleFactory>

#include <cstdio>
#include <cstring>
#include <iostream>

#include "app/TaskRunner.h"
#include "core/StructuredLight.h"
#include "ui/widgets/ComboBoxPopupStyle.h"

namespace smcp
{

namespace
{
	// User scope, native format (on Windows the registry: HKCU\Software\CENAM\SMCP).
	// IniFormat is not the default: QSettings writes the INI through QSaveFile and its rename fails
	// on profiles with an encrypted AppData (EFS), leaving the configuration unsaved.
	// SMCP_SETTINGS_DIR selects an INI file under that directory instead, so tests and side-by-side
	// copies of the program do not share (or overwrite) the user's settings.
	QSettings::Format SettingsFormat()
	{
		const QString directory = QString::fromLocal8Bit(qgetenv("SMCP_SETTINGS_DIR"));
		if (directory.isEmpty())
		{
			return QSettings::NativeFormat;
		}
		QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory);
		return QSettings::IniFormat;
	}
}

Application::Application(int& argc, char** argv) :
	QApplication(argc, argv),
	config(SettingsFormat(), QSettings::UserScope, Settings::App::Organization, Settings::App::App_Name, this),
	model(this),
	calib(),
	cornersWorld(),
	cornersCamera(),
	cornersProjector(),
	patternList(),
	minMaxList(),
	projectorViewList(),
	pointcloud(),
	// LoadConfig() and ApplyTheme() must run before the main window is constructed.
	mainWin((QWidget*)(LoadConfig(), ApplyTheme(), NULL)),
	processingDialog(&mainWin, Qt::Window | Qt::CustomizeWindowHint | Qt::WindowTitleHint)
{
	connect(this, SIGNAL(aboutToQuit()), this, SLOT(deinit()));

	//setup the main window state
	mainWin.show();
	mainWin.restoreGeometry(config.value(Settings::MainWindow::Geometry).toByteArray());
	QVariant windowState = config.value(Settings::MainWindow::State);
	if (windowState.isValid())
	{
		mainWin.setWindowState(static_cast<Qt::WindowStates>(windowState.toUInt()));
	}

	//set model
	SetRootDir(config.value(Settings::App::Root_Directory, QDir::currentPath()).toString());
	QModelIndex index = model.index(0, 0);
	mainWin._on_image_tree_currentChanged(index, index);
}

Application::~Application()
{}

QSettings& Application::GetSettings()
{
	return config;
}

void Application::deinit(void)
{
	config.setValue(Settings::MainWindow::Geometry, mainWin.saveGeometry());
	config.setValue(Settings::MainWindow::State, static_cast<unsigned>(mainWin.windowState()));
}

void Application::Clear(void)
{
	cornersWorld.clear();
	cornersCamera.clear();
	cornersProjector.clear();
	patternList.clear();
	minMaxList.clear();
	projectorViewList.clear();
	_decodeKeys.clear();
	_cornersSearched.clear();
	_usedInCalibration.clear();
	pointcloud.Clear();
}

// Single place where the look of the application is defined: Fusion style, base font
// and the resources/theme/smcp.qss sheet (see docs/STYLE.md). The .ui files carry no
// styleSheet or font properties.
void Application::ApplyTheme(void)
{
	// Default icon of every window; the Point Cloud Editor sets its own.
	setWindowIcon(QIcon(":/app-icon.png"));
	new ComboBoxPopupStyle(this);
	setStyle(QStyleFactory::create("Fusion"));
	QFont font("Segoe UI");
	font.setPointSizeF(9);
	setFont(font);

	QFile qss(":/theme/smcp.qss");
	if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
	{
		setStyleSheet(QString::fromUtf8(qss.readAll()));
	}
	else
	{
		std::cerr << "[theme] could not load :/theme/smcp.qss" << std::endl;
	}
}

void Application::LoadConfig(void)
{
	// Writes the default of every processing parameter that has never been stored.
	const auto ensure = [this](const char* key, const QVariant& value)
	{
		if (!config.value(key).isValid())
		{
			config.setValue(key, value);
		}
	};

	//decode
	ensure(Settings::Decode::Threshold, Settings::Decode::Threshold_Default_Value);
	ensure(Settings::Decode::B, Settings::Decode::B_Default_Value);
	ensure(Settings::Decode::M, Settings::Decode::M_Default_Value);

	//checkerboard size
	ensure(Settings::Chessboard::Columns, Settings::Chessboard::Columns_Default_Value);
	ensure(Settings::Chessboard::Rows, Settings::Chessboard::Rows_Default_Value);
	ensure(Settings::Chessboard::Width, Settings::Chessboard::Width_Default_Value);
	ensure(Settings::Chessboard::Height, Settings::Chessboard::Height_Default_Value);

	//reconstruction
	ensure(Settings::Reconstruction::Max_Dist, Settings::Reconstruction::Max_Dist_Default_Value);
	ensure(Settings::Reconstruction::Save_Normals, Settings::Reconstruction::Save_Normals_Default_Value);
	ensure(Settings::Reconstruction::Save_Colors, Settings::Reconstruction::Save_Colors_Default_Value);
	ensure(Settings::Reconstruction::Subpixel, Settings::Reconstruction::Subpixel_Default_Value);
	ensure(Settings::Calibration::Joint_Refinement, Settings::Calibration::Joint_Refinement_Default_Value);
	ensure(Settings::Calibration::Subpixel_Corners, Settings::Calibration::Subpixel_Corners_Default_Value);
	ensure(Settings::Reconstruction::Plane_Triangulation, Settings::Reconstruction::Plane_Triangulation_Default_Value);
}

void Application::SetRootDir(const QString& dirname)
{
	QDir rootDir(dirname);

	//reset internal data
	model.Clear();
	Clear();

	QStringList dirlist = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	foreach(const QString & item, dirlist)
	{
		QDir dir(rootDir.filePath(item));

		QStringList filters;
		filters << "*.jpg" << "*.bmp" << "*.png";

		QStringList filelist = dir.entryList(filters, QDir::Files, QDir::Name);
		QString path = dir.path();

		//setup the model
		int filecount = filelist.count();

		if (filecount < 1)
		{   //no images, skip
			continue;
		}

		unsigned row = model.rowCount();
		if (!model.InsertRow(row))
		{
			std::cout << "Failed model insert " << item.toStdString() << "(" << row << ")" << std::endl;
			continue;
		}

		//add the childrens
		QModelIndex parent = model.index(row, 0);
		model.setData(parent, item, Qt::DisplayRole);
		model.setData(parent, item, Qt::ToolTipRole);
		model.setData(parent, Qt::Checked, Qt::CheckStateRole);

		//read projector info
		int projectorWidth = 1024, projectorHeight = 768; //defaults compatible with old software
		QFile projectorFile(dirname + "/" + item + "/projector_info.txt");
		if (projectorFile.open(QIODevice::ReadOnly | QIODevice::Text))
		{   //projector info file exists
			int width = 0, height = 0;
			if (sscanf(projectorFile.readAll().constData(), "%d %d", &width, &height) == 2 && width > 0 && height > 0)
			{   //ok
				projectorWidth = width;
				projectorHeight = height;
			}
			else
			{
				std::cerr << "Projector info file has invalid values: " << item.toStdString() << std::endl;
			}
		}
		model.setData(parent, projectorWidth, ProjectorWidthRole);
		model.setData(parent, projectorHeight, ProjectorHeightRole);

		for (int i = 0; i < filecount; i++)
		{
			const QString& filename = filelist.at(i);
			if (!model.InsertRow(i, parent))
			{
				std::cout << "Failed model insert " << filename.toStdString() << "(" << row << ")" << std::endl;
				break;
			}

			QModelIndex index = model.index(i, 0, parent);
			QString label = QString("#%1 %2").arg(i, 2, 10, QLatin1Char('0')).arg(filename);
			model.setData(index, label, Qt::DisplayRole);
			model.setData(index, label, Qt::ToolTipRole);

			//additional data
			model.setData(index, path + "/" + filename, ImageFilenameRole);
		}
	}

	config.setValue(Settings::App::Root_Directory, dirname);
	emit root_dir_changed(dirname);
}

QString Application::GetRootDir(void) const
{
	return config.value(Settings::App::Root_Directory).toString();
}

bool Application::ChangeRootDir(QWidget* parentWidget)
{
	QString dirname = QFileDialog::getExistingDirectory(parentWidget, "Select Image Directory", config.value(Settings::App::Root_Directory, QString()).toString());

	if (dirname.isEmpty())
	{   //nothing selected
		return false;
	}

	SetRootDir(dirname);
	return true;
}

const cv::Mat Application::GetImage(unsigned level, unsigned n, Role role) const
{
	if (role != GrayImageRole && role != ColorImageRole)
	{   //invalid args
		return cv::Mat();
	}
	if (model.rowCount() <= static_cast<int>(level))
	{   //out of bounds
		return cv::Mat();
	}
	QModelIndex parent = model.index(level, 0);
	if (model.rowCount(parent) <= static_cast<int>(n))
	{   //out of bounds
		return cv::Mat();
	}

	const std::string filename = model.data(model.index(n, 0, parent), ImageFilenameRole).toString().toStdString();
	return role == ColorImageRole ? StructuredLight::GetColorImage(filename) : StructuredLight::GetGrayImage(filename);
}

int Application::GetProjectorWidth(unsigned level) const
{
	if (static_cast<int>(level) < model.rowCount())
	{   //ok
		QModelIndex parent = model.index(level, 0);
		return model.data(parent, ProjectorWidthRole).toInt();
	}
	return 0;
}

int Application::GetProjectorHeight(unsigned level) const
{
	if (static_cast<int>(level) < model.rowCount())
	{   //ok
		QModelIndex parent = model.index(level, 0);
		return model.data(parent, ProjectorHeightRole).toInt();
	}
	return 0;
}

/* Pipeline inputs =========================================================================== */

std::vector<Calibration::CaptureSet> Application::CaptureSets(void) const
{
	std::vector<Calibration::CaptureSet> sets(model.rowCount());
	for (int i = 0; i < model.rowCount(); i++)
	{
		const QModelIndex parent = model.index(i, 0);
		Calibration::CaptureSet& set = sets[i];
		set.name = model.data(parent, Qt::DisplayRole).toString().toStdString();
		set.selected = (model.data(parent, Qt::CheckStateRole).toInt() == Qt::Checked);
		const int images = model.rowCount(parent);
		set.images.reserve(images);
		for (int n = 0; n < images; n++)
		{
			set.images.push_back(model.data(model.index(n, 0, parent), ImageFilenameRole).toString().toStdString());
		}
	}
	return sets;
}

Calibration::Params Application::PipelineParams(void) const
{
	Calibration::Params params;
	params.chessboard.cornerCount = cv::Size(config.value(Settings::Chessboard::Columns).toUInt(), config.value(Settings::Chessboard::Rows).toUInt());
	params.chessboard.squareSize = cv::Size2f(config.value(Settings::Chessboard::Width).toDouble(), config.value(Settings::Chessboard::Height).toDouble());
	params.decode.b = config.value(Settings::Decode::B, Settings::Decode::B_Default_Value).toFloat();
	params.decode.m = config.value(Settings::Decode::M, Settings::Decode::M_Default_Value).toUInt();
	// The projector resolution of the first set applies to the whole workspace, as it always has.
	params.projectorSize = cv::Size(GetProjectorWidth(), GetProjectorHeight());
	params.homographyWindow = config.value(Settings::Calibration::H_Win, Settings::Calibration::H_Win_Default_Value).toUInt();
	params.shadowThreshold = config.value(Settings::Calibration::Shadow_Threshold, Settings::Calibration::Shadow_Threshold_Default_Value).toUInt();
	params.solve.jointRefinement = config.value(Settings::Calibration::Joint_Refinement, Settings::Calibration::Joint_Refinement_Default_Value).toBool();
	params.subpixelCorners = config.value(Settings::Calibration::Subpixel_Corners, Settings::Calibration::Subpixel_Corners_Default_Value).toBool();
	return params;
}

QByteArray Application::DecodeKey(const Calibration::CaptureSet& set, const Calibration::Params& params) const
{
	QByteArray key;
	key.append(reinterpret_cast<const char*>(&params.decode.b), sizeof(params.decode.b));
	key.append(QByteArray::number(params.decode.m)).append('|');
	key.append(QByteArray::number(params.projectorSize.width)).append('x').append(QByteArray::number(params.projectorSize.height));
	for (const std::string& image : set.images)
	{
		const QFileInfo info(QString::fromStdString(image));
		key.append('|').append(image.c_str()).append('|').append(QByteArray::number(info.size()));
		key.append('|').append(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
	}
	return key;
}

std::vector<StructuredLight::DecodedSet> Application::ValidDecodes(const std::vector<Calibration::CaptureSet>& sets, const Calibration::Params& params) const
{
	std::vector<StructuredLight::DecodedSet> valid(sets.size());
	for (std::size_t i = 0; i < sets.size(); i++)
	{
		if (sets[i].selected && i < patternList.size() && i < minMaxList.size() && i < _decodeKeys.size()
			&& patternList[i].data && minMaxList[i].data && !_decodeKeys[i].isEmpty() && _decodeKeys[i] == DecodeKey(sets[i], params))
		{
			valid[i].pattern = patternList[i];
			valid[i].minMax = minMaxList[i];
		}
	}
	return valid;
}

void Application::ResizeResults(std::size_t count)
{
	patternList.resize(count);
	minMaxList.resize(count);
	_decodeKeys.resize(count);
	if (projectorViewList.size() < count)
	{
		projectorViewList.resize(count);
	}
}

void Application::StoreDecode(std::size_t level, const StructuredLight::DecodedSet& decoded, const QByteArray& key)
{
	if (level >= patternList.size() || !decoded.pattern.data)
	{
		return;
	}
	if (patternList[level].data != decoded.pattern.data && level < projectorViewList.size())
	{   //new data: the cached projector view no longer matches
		projectorViewList[level] = cv::Mat();
	}
	patternList[level] = decoded.pattern;
	minMaxList[level] = decoded.minMax;
	_decodeKeys[level] = key;
}

/* Background execution ====================================================================== */

void Application::RunWithProcessingDialog(const std::function<void(ProgressSink&)>& work)
{
	QueuedProgress sink;
	connect(&sink, &QueuedProgress::MessageReported, &processingDialog, [this](const QString& text) { processingDialog.Message(text); });
	connect(&sink, &QueuedProgress::StatusReported, &processingDialog, [this](const QString& text) { processingDialog.SetCurrentMessage(text); });
	connect(&sink, &QueuedProgress::StepReported, &processingDialog, [this](int done, int total)
		{
			processingDialog.SetProgressTotal(static_cast<unsigned>(std::max(0, total)));
			processingDialog.SetProgressValue(static_cast<unsigned>(std::max(0, done)));
		});
	connect(&processingDialog, &ProcessingDialog::CancelRequested, &sink, &QueuedProgress::Cancel);
	if (processingDialog.Canceled())
	{
		sink.Cancel();
	}

	const std::exception_ptr error = RunBlocking([&]() { work(sink); });
	// Reports still queued when the worker finished.
	sendPostedEvents(&processingDialog);
	if (error)
	{
		const QString text = QString("ERROR: the operation failed (%1)").arg(DescribeError(error));
		processingDialog.SetCurrentMessage(text);
		processingDialog.Message(text);
	}
}

bool Application::RunWithProgressDialog(QWidget* parentWidget, const QString& label, const std::function<void(ProgressSink&)>& work)
{
	QueuedProgress sink;
	BusyProgressDialog progress(label, parentWidget ? parentWidget : &mainWin);
	connect(&sink, &QueuedProgress::StatusReported, &progress, [&progress, &sink](const QString& text)
		{
			if (!sink.Canceled())
			{
				progress.setLabelText(text);
			}
		});
	connect(&progress, &BusyProgressDialog::canceled, &sink, &QueuedProgress::Cancel);
	progress.show();

	const std::exception_ptr error = RunBlocking([&]() { work(sink); });
	const bool canceled = sink.Canceled();
	progress.Finish();
	if (error)
	{
		QMessageBox::critical(parentWidget, "Error", QString("The operation failed: %1").arg(DescribeError(error)));
		return false;
	}
	return !canceled;
}

/* Corners =================================================================================== */

bool Application::ExtractChessboardCorners(void)
{
	const std::vector<Calibration::CaptureSet> sets = CaptureSets();
	const Calibration::Params params = PipelineParams();

	std::vector<Calibration::SetCorners> corners;
	bool completed = false;
	RunWithProcessingDialog([&](ProgressSink& progress)
		{
			cv::Size imageSize;
			completed = Calibration::ExtractCorners(sets, params.chessboard, corners, imageSize, progress);
		});

	cornersWorld.assign(sets.size(), std::vector<cv::Point3f>());
	cornersCamera.assign(sets.size(), std::vector<cv::Point2f>());
	_cornersSearched.assign(sets.size(), 0);
	bool allFound = completed;
	for (std::size_t i = 0; i < sets.size() && i < corners.size(); i++)
	{
		cornersWorld[i] = corners[i].world;
		cornersCamera[i] = corners[i].camera;
		_cornersSearched[i] = completed && sets[i].selected;
		allFound = allFound && (!sets[i].selected || !corners[i].camera.empty());
	}
	UpdateSetStatus();
	return allFound;
}

/* Decoding ================================================================================== */

void Application::DecodeAll(void)
{
	const std::vector<Calibration::CaptureSet> sets = CaptureSets();
	const Calibration::Params params = PipelineParams();
	const std::vector<StructuredLight::DecodedSet> valid = ValidDecodes(sets, params);
	const int count = static_cast<int>(sets.size());

	std::vector<cv::Size> projectorSizes(sets.size());
	for (int i = 0; i < count; i++)
	{
		projectorSizes[i] = cv::Size(GetProjectorWidth(i), GetProjectorHeight(i));
	}

	std::vector<StructuredLight::DecodedSet> decoded(sets.size());
	RunWithProcessingDialog([&](ProgressSink& progress)
		{
			cv::Size imageSize(0, 0);
			progress.Step(0, count);
			progress.Status("Decoding...");

			for (int i = 0; i < count; i++)
			{
				const std::string& name = sets[i].name;
				if (!sets[i].selected)
				{   //skip
					progress.Message(" * " + name + ": skipped [not selected]");
					progress.Step(i + 1, count);
					continue;
				}
				if (progress.Canceled())
				{
					progress.Status("Decode canceled");
					progress.Message("Decode canceled");
					return;
				}

				progress.Status("Decoding... " + name);
				const bool reused = valid[i].pattern.data != NULL;
				if (reused)
				{
					decoded[i] = valid[i];
				}
				else
				{
					std::string error;
					if (!StructuredLight::DecodeGraySet(sets[i].images, params.projectorSize, params.decode, decoded[i], error, &progress))
					{
						progress.Message(error);
						progress.Status(progress.Canceled() ? "Decode canceled" : "ERROR: decode of set " + name + " failed");
						return;
					}
				}

				const cv::Size size = decoded[i].pattern.size();
				if (imageSize.width == 0)
				{
					imageSize = size;
				}
				else if (imageSize != size)
				{
					decoded[i] = StructuredLight::DecodedSet();
					progress.Message("ERROR: pattern image of different size: set " + name);
					progress.Status("ERROR: pattern image of different size: set " + name);
					return;
				}
				else if (projectorSizes[0] != projectorSizes[i])
				{
					progress.Message("WARNING: projector resolution does not match: set " + name + " [expected "
						+ std::to_string(projectorSizes[0].width) + "x" + std::to_string(projectorSizes[0].height) + ", got "
						+ std::to_string(projectorSizes[i].width) + "x" + std::to_string(projectorSizes[i].height) + "]");
				}

				progress.Message(" * " + name + (reused ? ": decoded (reused)" : ": decoded"));
				progress.Step(i + 1, count);
			}

			progress.Status("Decode finished");
			progress.Step(count, count);
		});

	ResizeResults(sets.size());
	for (std::size_t i = 0; i < sets.size(); i++)
	{
		StoreDecode(i, decoded[i], DecodeKey(sets[i], params));
	}
	UpdateSetStatus();
}

bool Application::Decode(int level, QWidget* parentWidget)
{
	if (level < 0 || level >= model.rowCount())
	{   //invalid row
		return false;
	}

	const std::vector<Calibration::CaptureSet> sets = CaptureSets();
	const Calibration::Params params = PipelineParams();
	ResizeResults(sets.size());

	const QByteArray key = DecodeKey(sets[level], params);
	if (patternList[level].data && minMaxList[level].data && _decodeKeys[level] == key)
	{   //already decoded from the same images with the same parameters
		return true;
	}

	StructuredLight::DecodedSet decoded;
	std::string error;
	bool ok = false;
	const bool finished = RunWithProgressDialog(parentWidget, "Decoding...", [&](ProgressSink& progress)
		{
			ok = StructuredLight::DecodeGraySet(sets[level].images, params.projectorSize, params.decode, decoded, error, &progress);
		});
	if (!finished)
	{
		return false;
	}
	if (!ok)
	{
		QMessageBox::critical(parentWidget, "Error", QString("Decode failed: %1").arg(QString::fromStdString(error)));
		return false;
	}

	StoreDecode(level, decoded, key);
	UpdateSetStatus();
	return true;
}

/* Calibration =============================================================================== */

void Application::Calibrate(void)
{   //try to calibrate the camera, projector, and stereo system
	const std::vector<Calibration::CaptureSet> sets = CaptureSets();
	const Calibration::Params params = PipelineParams();
	const std::vector<StructuredLight::DecodedSet> valid = ValidDecodes(sets, params);
	const QString path = config.value(Settings::App::Root_Directory).toString();

	calib.Clear();

	Calibration::Result result;
	RunWithProcessingDialog([&](ProgressSink& progress)
		{
			result = Calibration::Run(sets, params, valid, progress);
			if (!result.success)
			{
				return;
			}

			//save to file
			QString filename = path + "/calibration.yml";
			progress.Message(result.calibration.SaveCalibration(filename)
				? QString("Calibration saved: %1").arg(filename).toStdString()
				: QString("[ERROR] Saving %1 failed").arg(filename).toStdString());

			//save to MATLAB format
			filename = path + "/calibration.m";
			progress.Message(result.calibration.SaveCalibration(filename)
				? QString("Calibration saved [MATLAB]: %1").arg(filename).toStdString()
				: QString("[ERROR] Saving %1 failed").arg(filename).toStdString());

			//save corners
			std::string error;
			if (!Calibration::SaveCorners(path.toStdString(), result, error))
			{
				progress.Message(error);
			}

			progress.Status("Calibration finished");
			progress.Message("Calibration finished");
		});

	//adopt whatever the pipeline produced, also when it stopped early
	ResizeResults(sets.size());
	cornersWorld.assign(sets.size(), std::vector<cv::Point3f>());
	cornersCamera.assign(sets.size(), std::vector<cv::Point2f>());
	cornersProjector.assign(sets.size(), std::vector<cv::Point2f>());
	_cornersSearched.assign(sets.size(), 0);
	_usedInCalibration.assign(sets.size(), 0);
	for (std::size_t i = 0; i < sets.size(); i++)
	{
		if (i < result.corners.size())
		{
			cornersWorld[i] = result.corners[i].world;
			cornersCamera[i] = result.corners[i].camera;
			_cornersSearched[i] = sets[i].selected && result.cornersExtracted;
		}
		if (result.success && i < result.projectorCorners.size())
		{
			_usedInCalibration[i] = !cornersWorld[i].empty() && !cornersCamera[i].empty() && !result.projectorCorners[i].empty();
		}
		if (i < result.projectorCorners.size())
		{
			cornersProjector[i] = result.projectorCorners[i];
		}
		if (i < result.decoded.size())
		{
			StoreDecode(i, result.decoded[i], DecodeKey(sets[i], params));
		}
	}
	if (result.success)
	{
		calib = result.calibration;
		calib.filename = path + "/calibration.yml";
		calib.Display();
	}
	UpdateSetStatus();
}

bool Application::LoadDump(const char* filename, int type, cv::Mat2f& patternImage, cv::Mat2b& minMaxImage, cv::Mat3b& colorImage) const
{
	patternImage = cv::Mat2f();
	minMaxImage = cv::Mat2b();
	colorImage = cv::Mat3b();
	if (!filename)
	{
		return false;
	}

	FILE* fp = fopen(filename, "rb");
	if (!fp)
	{
		return false;
	}

	// Layout: type, cols, rows (int); pattern columns, pattern rows (float planes);
	// minimum, maximum (byte planes); BGR image.
	int rows = 0, cols = 0;
	bool ok = fread(&type, sizeof(int), 1, fp) == 1 && fread(&cols, sizeof(int), 1, fp) == 1 && fread(&rows, sizeof(int), 1, fp) == 1
		&& rows > 0 && cols > 0 && rows <= 65536 && cols <= 65536;
	if (ok)
	{
		const std::size_t pixels = static_cast<std::size_t>(rows) * cols;
		patternImage.create(rows, cols);
		minMaxImage.create(rows, cols);
		colorImage.create(rows, cols);

		std::vector<float> floats(pixels);
		std::vector<unsigned char> bytes(pixels);
		for (int channel = 0; ok && channel < 2; channel++)
		{
			ok = fread(floats.data(), sizeof(float), pixels, fp) == pixels;
			for (std::size_t i = 0; ok && i < pixels; i++)
			{
				patternImage(static_cast<int>(i / cols), static_cast<int>(i % cols))[channel] = floats[i];
			}
		}
		for (int channel = 0; ok && channel < 2; channel++)
		{
			ok = fread(bytes.data(), 1, pixels, fp) == pixels;
			for (std::size_t i = 0; ok && i < pixels; i++)
			{
				minMaxImage(static_cast<int>(i / cols), static_cast<int>(i % cols))[channel] = bytes[i];
			}
		}
		for (int h = 0; ok && h < rows; ++h)
		{
			ok = fread(colorImage.ptr<cv::Vec3b>(h), 3, cols, fp) == static_cast<std::size_t>(cols);
		}
	}
	fclose(fp);

	if (!ok)
	{   //truncated or foreign file
		patternImage = cv::Mat2f();
		minMaxImage = cv::Mat2b();
		colorImage = cv::Mat3b();
	}
	return ok;
}

bool Application::LoadCalibration(QWidget* parentWidget)
{
	QString name = config.value(Settings::Calibration::File, config.value(Settings::App::Root_Directory)).toString();
	QString filename = QFileDialog::getOpenFileName(parentWidget, "Open calibration", name, "Calibration (*.yml)");
	if (!filename.isEmpty() && calib.LoadCalibration(filename))
	{   //ok
		config.setValue(Settings::Calibration::File, filename);
		mainWin.show_message(QString("Calibration loaded from %1").arg(filename));
		calib.Display();
		return true;
	}
	if (!filename.isEmpty())
	{   //error
		QMessageBox::critical(parentWidget, "Error", QString("Calibration not loaded from %1").arg(filename));
	}
	return false;
}

bool Application::SaveCalibration(QWidget* parentWidget)
{
	if (!calib.IsValid())
	{   //invalid calibration
		QMessageBox::critical(parentWidget, "Error", "No valid calibration found.");
		return false;
	}
	QString name = config.value(Settings::Calibration::File, config.value(Settings::App::Root_Directory)).toString();
	QString filename = QFileDialog::getSaveFileName(parentWidget, "Save calibration", name, "Calibration (*.yml *.m)");
	if (!filename.isEmpty() && calib.SaveCalibration(filename))
	{   //ok
		config.setValue(Settings::Calibration::File, filename);
		mainWin.show_message(QString("Calibration saved to %1").arg(filename));
		calib.Display();
		return true;
	}
	if (!filename.isEmpty())
	{   //error
		QMessageBox::critical(parentWidget, "Error", QString("Calibration not saved to %1").arg(filename));
	}
	return false;
}

/* Reconstruction ============================================================================ */

void Application::ReconstructModel(int level, Scan3d::Pointcloud& pointcloud, QWidget* parentWidget)
{
	pointcloud.Clear();
	if (level < 0 || level >= model.rowCount())
	{   //invalid row
		return;
	}
	if (!calib.IsValid())
	{   //invalid calibration
		QMessageBox::critical(parentWidget, "Error", "No valid calibration found.");
		return;
	}

	//decode first (reused when the set is already decoded with the current parameters)
	if (!Decode(level, parentWidget))
	{
		return;
	}

	const cv::Mat patternImage = patternList.at(level);
	const cv::Mat minMaxImage = minMaxList.at(level);
	const cv::Mat colorImage = GetImage(level, 0, ColorImageRole);
	ReconstructModelDump(patternImage, minMaxImage, colorImage, pointcloud, parentWidget, CaptureSets()[level].images);

	//save the projector view: the classic cloud is organized like the projector image, so its colors
	//are that view. A sub-pixel cloud is organized like the camera image; the view is then built on demand.
	if (pointcloud.points.data && pointcloud.colors.size() == cv::Size(GetProjectorWidth(), GetProjectorHeight()))
	{
		pointcloud.colors.copyTo(projectorViewList[level]);
	}
}

void Application::ReconstructModelDump(cv::Mat2f const& patternImage, cv::Mat2b const& minMaxImage, cv::Mat3b const& colorImage, Scan3d::Pointcloud& pointcloud, QWidget* parentWidget,
	const std::vector<std::string>& images)
{
	pointcloud.Clear();
	if (!patternImage.data || !minMaxImage.data || !colorImage.data)
	{   //invalid dump
		return;
	}
	if (!calib.IsValid())
	{   //invalid calibration
		QMessageBox::critical(parentWidget, "Error", "No valid calibration found.");
		return;
	}

	const cv::Size projectorSize(GetProjectorWidth(), GetProjectorHeight());
	const int threshold = config.value(Settings::Decode::Threshold, Settings::Decode::Threshold_Default_Value).toInt();
	const double maxDist = config.value(Settings::Reconstruction::Max_Dist, Settings::Reconstruction::Max_Dist_Default_Value).toDouble();
	const CalibrationData calibration = calib;

	// A decoded dump has no images, and sub-pixel without them is worse than the classic method.
	const bool subpixel = !images.empty()
		&& config.value(Settings::Reconstruction::Subpixel, Settings::Reconstruction::Subpixel_Default_Value).toBool();

	const Scan3d::Triangulation triangulation =
		config.value(Settings::Reconstruction::Plane_Triangulation, Settings::Reconstruction::Plane_Triangulation_Default_Value).toBool()
			? Scan3d::Triangulation::ProjectorPlane
			: Scan3d::Triangulation::RayMidpoint;

	Scan3d::Pointcloud result;
	std::string error;
	const bool finished = RunWithProgressDialog(parentWidget, "Reconstruction in progress.", [&](ProgressSink& progress)
		{
			if (!subpixel)
			{
				Scan3d::ReconstructModel(result, calibration, patternImage, minMaxImage, colorImage, projectorSize, threshold, maxDist, &progress,
					triangulation);
				return;
			}
			std::vector<cv::Mat> gray;
			progress.Status("Reconstruction in progress: loading images");
			if (!StructuredLight::LoadGraySet(images, gray, error, &progress))
			{
				return;
			}
			const cv::Mat refined = StructuredLight::RefinePattern(gray, patternImage, minMaxImage, projectorSize, threshold, &progress);
			if (!refined.data)
			{
				if (!progress.Canceled())
				{
					error = "the images of the set do not match its decoded pattern";
				}
				return;
			}
			gray.clear();  //the images are not needed to triangulate
			Scan3d::ReconstructModelSubpixel(result, calibration, refined, minMaxImage, colorImage, projectorSize, threshold, maxDist, &progress,
				triangulation);
		});
	if (finished && !error.empty())
	{
		QMessageBox::critical(parentWidget, "Error", QString("Sub-pixel reconstruction failed: %1").arg(QString::fromStdString(error)));
		return;
	}
	if (finished)
	{
		pointcloud = result;
	}
}

void Application::ComputeNormals(Scan3d::Pointcloud& pointcloud)
{
	Scan3d::ComputeNormals(pointcloud);
}

/* Views ===================================================================================== */

void Application::MakePatternImages(int level, cv::Mat& colImage, cv::Mat& rowImage)
{
	colImage = cv::Mat();
	rowImage = cv::Mat();

	if (level < 0 || level >= model.rowCount())
	{   //invalid level
		return;
	}
	if (patternList.size() <= static_cast<size_t>(level) || minMaxList.size() <= static_cast<size_t>(level))
	{   //no decoded
		return;
	}

	cv::Mat const& patternImage = patternList.at(level);
	cv::Mat const& minMaxImage = minMaxList.at(level);

	if (!patternImage.data || !minMaxImage.data)
	{   //no decoded
		return;
	}

	//apply threshold
	int threshold = config.value(Settings::Decode::Threshold, Settings::Decode::Threshold_Default_Value).toInt();
	cv::Mat patternImageNew = cv::Mat(patternImage.size(), patternImage.type());
	for (int h = 0; h < patternImage.rows; h++)
	{
		const cv::Vec2f* patternRow = patternImage.ptr<cv::Vec2f>(h);
		const cv::Vec2b* minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);
		cv::Vec2f* patternNewRow = patternImageNew.ptr<cv::Vec2f>(h);
		for (int w = 0; w < patternImage.cols; w++)
		{
			cv::Vec2f const& pattern = patternRow[w];
			cv::Vec2b const& minMax = minMaxRow[w];
			cv::Vec2f& patternNew = patternNewRow[w];

			if (StructuredLight::Invalid(pattern) || (minMax[1] - minMax[0]) < static_cast<int>(threshold))
			{   //invalid
				patternNew = cv::Vec2f(StructuredLight::Pixel_Uncertain, StructuredLight::Pixel_Uncertain);
			}
			else
			{   //ok
				patternNew = pattern;
			}
		}   //for each column
	}   //for each row

	colImage = StructuredLight::ColorizePattern(patternImageNew, 0, GetProjectorWidth(level));
	rowImage = StructuredLight::ColorizePattern(patternImageNew, 1, GetProjectorHeight(level));
}

cv::Mat Application::GetProjectorView(int level, bool forceUpdate)
{
	if (level < 0 || level >= model.rowCount())
	{   //invalid row
		return cv::Mat();
	}
	if (patternList.size() <= static_cast<size_t>(level) || minMaxList.size() <= static_cast<size_t>(level))
	{   //not decoded
		return cv::Mat();
	}
	if (projectorViewList.size() < model.rowCount<size_t>())
	{
		projectorViewList.resize(model.rowCount());
	}

	cv::Mat& projector_image = projectorViewList[level];

	if (!projector_image.data || forceUpdate)
	{   //make projector view with the current configuration
		int threshold = config.value(Settings::Decode::Threshold, Settings::Decode::Threshold_Default_Value).toInt();

		cv::Mat patternImage = patternList.at(level);
		cv::Mat minMaxImage = minMaxList.at(level);;
		cv::Mat colorImage = GetImage(level, 0, ColorImageRole);
		cv::Size projectorSize(GetProjectorWidth(), GetProjectorHeight());

		projector_image = Scan3d::MakeProjectorView(patternImage, minMaxImage, colorImage, projectorSize, threshold);
	}

	return projector_image;
}

int Application::SelectedSetCount(void) const
{
	int selected = 0;
	for (int i = 0; i < model.rowCount(); i++)
	{
		selected += (model.data(model.index(i, 0), Qt::CheckStateRole).toInt() == Qt::Checked);
	}
	return selected;
}

int Application::CalibratedSetCount(void) const
{
	int used = 0;
	for (char flag : _usedInCalibration)
	{
		used += flag ? 1 : 0;
	}
	return used;
}

void Application::UpdateSetStatus(void)
{
	// One small dot per state; the tooltip says the same in words.
	const auto dot = [](const QColor& color)
	{
		QPixmap pixmap(12, 12);
		pixmap.fill(Qt::transparent);
		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawEllipse(QRectF(1.5, 1.5, 9, 9));
		return pixmap;
	};
	static const QColor Calibrated(0x3e, 0x89, 0x48), Processed(0x00, 0x52, 0x9d), Problem(0xe4, 0x3b, 0x44);

	for (int i = 0; i < model.rowCount(); i++)
	{
		const std::size_t level = static_cast<std::size_t>(i);
		const QModelIndex index = model.index(i, 0);
		const bool searched = level < _cornersSearched.size() && _cornersSearched[level];
		const std::size_t corners = level < cornersCamera.size() ? cornersCamera[level].size() : 0;
		const bool decoded = level < patternList.size() && !patternList[level].empty();
		const bool calibrated = level < _usedInCalibration.size() && _usedInCalibration[level];

		QStringList lines;
		lines << model.data(index, Qt::DisplayRole).toString();
		lines << (corners ? QString("Chessboard: found (%1 corners)").arg(corners)
				  : searched ? QString("Chessboard: not found") : QString("Chessboard: not searched yet"));
		lines << (decoded ? QString("Decoded: yes") : QString("Decoded: no"));
		if (calibrated)
		{
			lines << QString("Used in the last calibration");
		}

		QVariant icon;
		if (calibrated)
		{
			icon = dot(Calibrated);
		}
		else if (searched && !corners)
		{
			icon = dot(Problem);
		}
		else if (corners || decoded)
		{
			icon = dot(Processed);
		}
		model.setData(index, icon, Qt::DecorationRole);
		model.setData(index, lines.join('\n'), Qt::ToolTipRole);
	}
}

void Application::SelectNone(void)
{
	int count = model.rowCount();
	for (int i = 0; i < count; i++)
	{
		model.setData(model.index(i, 0), Qt::Unchecked, Qt::CheckStateRole);
	}
}

void Application::SelectAll(void)
{
	int count = model.rowCount();
	for (int i = 0; i < count; i++)
	{
		model.setData(model.index(i, 0), Qt::Checked, Qt::CheckStateRole);
	}
}
} // namespace smcp
