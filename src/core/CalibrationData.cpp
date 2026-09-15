/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
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

#include "core/CalibrationData.h"

#include <QFileInfo>

#include <iostream>
#include <opencv2/calib3d/calib3d.hpp>

namespace smcp
{

CalibrationData::CalibrationData() :
    camK(), camKc(),
    projK(), projKc(),
    R(), T(),
    camError(0.0), projError(0.0), stereoError(0.0),
    filename()
{
}

CalibrationData::~CalibrationData()
{
}

void CalibrationData::Clear(void)
{
    camK = cv::Mat();
    camKc = cv::Mat();
    projK = cv::Mat();
    projKc = cv::Mat();
    R = cv::Mat();
    T = cv::Mat();
    filename = QString();
}

bool CalibrationData::IsValid(void) const
{
    return (camK.data && camKc.data && projK.data && projKc.data && R.data && T.data);
}

bool CalibrationData::LoadCalibration(QString const& filename)
{
    QFileInfo info(filename);
    QString type = info.suffix();

    if (type=="yml") {return LoadCalibrationYml(filename);}

    return false;
}

bool CalibrationData::SaveCalibration(QString const& filename)
{
    QFileInfo info(filename);
    QString type = info.suffix();

    if (type=="yml") {return SaveCalibrationYml(filename);}
    if (type=="m"  ) {return SaveCalibrationMatlab(filename);}

    return false;
}

bool CalibrationData::LoadCalibrationYml(QString const& filename)
{
    cv::FileStorage fs(filename.toStdString(), cv::FileStorage::READ);
    if (!fs.isOpened())
    {
        return false;
    }

    fs["cam_K"] >> camK;
    fs["cam_kc"] >> camKc;
    fs["proj_K"] >> projK;
    fs["proj_kc"] >> projKc;
    fs["R"] >> R;
    fs["T"] >> T;

    fs["cam_error"] >> camError;
    fs["proj_error"] >> projError;
    fs["stereo_error"] >> stereoError;

    fs.release();

    this->filename = filename;

    return true;
}

bool CalibrationData::SaveCalibrationYml(QString const& filename)
{
    cv::FileStorage fs(filename.toStdString(), cv::FileStorage::WRITE);
    if (!fs.isOpened())
    {
        return false;
    }

    fs << "cam_K" << camK << "cam_kc" << camKc
       << "proj_K" << projK << "proj_kc" << projKc
       << "R" << R << "T" << T
       << "cam_error" << camError
       << "proj_error" << projError
       << "stereo_error" << stereoError
       ;
    fs.release();

    this->filename = filename;

    return true;
}

bool CalibrationData::SaveCalibrationMatlab(QString const& filename)
{
    FILE * fp = fopen(qPrintable(filename), "w");
    if (!fp)
    {
        return false;
    }

    cv::Mat rvec;
    cv::Rodrigues(R, rvec);
    fprintf(fp, 
        "%% Projector-Camera Stereo calibration parameters:\n"
        "\n"
        "%% Intrinsic parameters of camera:\n"
        "fc_left = [ %lf %lf ]; %% Focal Length\n"
        "cc_left = [ %lf %lf ]; %% Principal point\n"
        "alpha_c_left = [ %lf ]; %% Skew\n"
        "kc_left = [ %lf %lf %lf %lf %lf ]; %% Distortion\n"
        "\n"
        "%% Intrinsic parameters of projector:\n"
        "fc_right = [ %lf %lf ]; %% Focal Length\n"
        "cc_right = [ %lf %lf ]; %% Principal point\n"
        "alpha_c_right = [ %lf ]; %% Skew\n"
        "kc_right = [ %lf %lf %lf %lf %lf ]; %% Distortion\n"
        "\n"
        "%% Extrinsic parameters (position of projector wrt camera):\n"
        "om = [ %lf %lf %lf ]; %% Rotation vector\n"
        "T = [ %lf %lf %lf ]; %% Translation vector\n",
        camK.at<double>(0,0), camK.at<double>(1,1), camK.at<double>(0,2), camK.at<double>(1,2), camK.at<double>(0,1),
        camKc.at<double>(0,0), camKc.at<double>(0,1), camKc.at<double>(0,2), camKc.at<double>(0,3), camKc.at<double>(0,4), 
        projK.at<double>(0,0), projK.at<double>(1,1), projK.at<double>(0,2), projK.at<double>(1,2), projK.at<double>(0,1),
        projKc.at<double>(0,0), projKc.at<double>(0,1), projKc.at<double>(0,2), projKc.at<double>(0,3), projKc.at<double>(0,4),
        rvec.at<double>(0,0), rvec.at<double>(1,0), rvec.at<double>(2,0), 
        T.at<double>(0,0), T.at<double>(1,0), T.at<double>(2,0)
        );
    fclose(fp);

    return true;
}

void CalibrationData::Display(std::ostream & stream) const
{
    stream << "Camera Calib: " << std::endl
        << " - reprojection error: " << camError << std::endl
        << " - K:\n" << camK << std::endl
        << " - kc: " << camKc << std::endl
        ;
    stream << std::endl;
    stream << "Projector Calib: " << std::endl
        << " - reprojection error: " << projError << std::endl
        << " - K:\n" << projK << std::endl
        << " - kc: " << projKc << std::endl
        ;
    stream << std::endl;
    stream << "Stereo Calib: " << std::endl
        << " - reprojection error: " << stereoError << std::endl
        << " - R:\n" << R << std::endl
        << " - T:\n" << T << std::endl
        ;
}
} // namespace smcp
