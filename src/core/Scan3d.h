/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania
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
#include <QWidget>
#include <QString>
#include <opencv2/core/core.hpp>

#ifndef _MSC_VER
#  ifndef _isnan
#    include <math.h>
#    define _isnan std::isnan
#  endif
#endif

#include "core/CalibrationData.h"

namespace smcp
{

namespace Scan3d
{
    class Pointcloud
    {
    public:
        void Clear(void);
        void InitPoints(int rows, int cols);
        void InitColor(int rows, int cols);
        void InitNormals(int rows, int cols);

        //data
        cv::Mat points;
        cv::Mat colors;
        cv::Mat normals;
    };

    void ReconstructModel(Pointcloud & pointcloud, CalibrationData const& calib, 
            cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
            cv::Size const& projectorSize, int threshold, double maxDist, QWidget * parentWidget = NULL);

    void ReconstructModelSimple(Pointcloud & pointcloud, CalibrationData const& calib, 
            cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
            cv::Size const& projectorSize, int threshold, double maxDist, QWidget * parentWidget = NULL);

    void ReconstructModelPatchCenter(Pointcloud & pointcloud, CalibrationData const& calib, 
            cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
            cv::Size const& projectorSize, int threshold, double maxDist, QWidget * parentWidget = NULL);

    void TriangulateStereo(const cv::Mat & K1, const cv::Mat & kc1, const cv::Mat & K2, const cv::Mat & kc2, 
                            const cv::Mat & Rt, const cv::Mat & T, const cv::Point2d & p1, const cv::Point2d & p2, 
                            cv::Point3d & p3d, double * distance = NULL);

    cv::Point3d ApproximateRayIntersection(const cv::Point3d & v1, const cv::Point3d & q1,
                                        const cv::Point3d & v2, const cv::Point3d & q2,
                                        double * distance = NULL, double * outLambda1 = NULL, double * outLambda2 = NULL);

    void ComputeNormals(Scan3d::Pointcloud & pointcloud);

    cv::Mat MakeProjectorView(cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage, 
                                        cv::Size const& projectorSize, int threshold);
};

} // namespace smcp
