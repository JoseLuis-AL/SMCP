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


#pragma once

// Stereo triangulation of decoded projector-camera correspondences.
//
// The module has no widget dependency: progress and cancellation go through a ProgressSink.
// Triangulation runs in parallel over projector pixels; every pixel is computed independently, so
// the cloud is identical whatever the thread count.

#include <opencv2/core/core.hpp>

#include "core/CalibrationData.h"
#include "core/Progress.h"

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

    /// How a camera pixel and its projector coordinate become a 3D point.
    enum class Triangulation
    {
        /// Middle of the shortest segment between the camera ray and the projector ray. It uses both
        /// projector coordinates, so an error in either one moves the point. The classic method.
        RayMidpoint,
        /// Camera ray cut by the plane of light of the projector coordinate that runs across the
        /// baseline (the column for a projector beside the camera, the row for one above or below
        /// it). The other coordinate, which carries almost no depth information, only chooses where
        /// along that plane the lens distortion is evaluated, so its noise does not reach the point.
        ProjectorPlane
    };

    /// Reconstructs one point per projector pixel: the camera pixels that decode to the same projector
    /// pixel are averaged and the two rays are intersected. Leaves `pointcloud` empty when the input
    /// is invalid or `progress` (optional) reports a cancellation.
    void ReconstructModel(Pointcloud & pointcloud, CalibrationData const& calib,
            cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
            cv::Size const& projectorSize, int threshold, double maxDist, ProgressSink * progress = nullptr,
            Triangulation triangulation = Triangulation::RayMidpoint);

    /// Sub-pixel reconstruction: one point per camera pixel. `subpixelImage` is the fractional
    /// projector coordinate of every camera pixel (StructuredLight::RefinePattern); each pixel that
    /// has one is triangulated on its own, so the cloud is organized like the camera image and has
    /// its resolution. Pixels below `threshold` or whose rays pass farther apart than `maxDist` are
    /// left out, as in ReconstructModel.
    void ReconstructModelSubpixel(Pointcloud & pointcloud, CalibrationData const& calib,
            cv::Mat const& subpixelImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
            cv::Size const& projectorSize, int threshold, double maxDist, ProgressSink * progress = nullptr,
            Triangulation triangulation = Triangulation::RayMidpoint);

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
