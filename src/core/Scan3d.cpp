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


#include "core/Scan3d.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

#include <opencv2/imgproc/imgproc.hpp>

#include "core/Parallel.h"
#include "core/StructuredLight.h"

namespace smcp
{

namespace
{
    constexpr long long Min_Rows_Per_Thread = 32;
    /// Correspondences triangulated per thread at least.
    constexpr long long Min_Points_Per_Thread = 4096;

    /// 3x3 (CV_64F) times a column vector, summed left to right like cv::gemm does for this size.
    inline cv::Point3d Multiply(const double * m, const cv::Point3d & v)
    {
        return cv::Point3d(m[0]*v.x + m[1]*v.y + m[2]*v.z,
                           m[3]*v.x + m[4]*v.y + m[5]*v.z,
                           m[6]*v.x + m[7]*v.y + m[8]*v.z);
    }

    inline double Dot(const cv::Point3d & a, const cv::Point3d & b)
    {
        return a.x*b.x + a.y*b.y + a.z*b.z;
    }

    /// Projector-to-camera geometry as plain doubles, read once per reconstruction.
    struct StereoGeometry
    {
        double Rt[9];
        cv::Point3d T;

        StereoGeometry(const cv::Mat & RtMat, const cv::Mat & TMat)
        {
            cv::Mat rt64, t64;
            RtMat.convertTo(rt64, CV_64F);
            TMat.convertTo(t64, CV_64F);
            for (int i = 0; i < 9; i++)
            {
                Rt[i] = rt64.at<double>(i/3, i%3);
            }
            T = cv::Point3d(t64.at<double>(0), t64.at<double>(1), t64.at<double>(2));
        }

        /// Intersects the camera ray through `u1` with the projector ray through `u2` (normalized coordinates).
        cv::Point3d Intersect(const cv::Vec2d & u1, const cv::Vec2d & u2, double * distance) const
        {
            const cv::Point3d p1(u1[0], u1[1], 1.0);
            const cv::Point3d p2(u2[0], u2[1], 1.0);

            //to world coordinates
            const cv::Point3d w1 = p1;
            const cv::Point3d w2 = Multiply(Rt, cv::Point3d(p2.x - T.x, p2.y - T.y, p2.z - T.z));

            //world rays
            const cv::Point3d v1 = w1;
            const cv::Point3d v2 = Multiply(Rt, p2);

            return Scan3d::ApproximateRayIntersection(v1, w1, v2, w2, distance);
        }

        /// Position of the projector in the camera frame.
        cv::Point3d ProjectorCenter() const
        {
            return Multiply(Rt, cv::Point3d(-T.x, -T.y, -T.z));
        }

        /// True when the projector sits beside the camera rather than above or below it: then the
        /// projector column is the coordinate that changes with depth.
        bool ColumnsCarryDepth() const
        {
            const cv::Point3d center = ProjectorCenter();
            return std::abs(center.x) >= std::abs(center.y);
        }

        /// Cuts the camera ray through `u1` with the plane of light that holds the projector rays
        /// through `u2` and `u2Along` (normalized coordinates; the second one is the same projector
        /// coordinate one pixel further along its line). False when the ray is parallel to the plane
        /// or the point falls behind the camera.
        bool IntersectPlane(const cv::Vec2d & u1, const cv::Vec2d & u2, const cv::Vec2d & u2Along, cv::Point3d & point) const
        {
            const cv::Point3d ray(u1[0], u1[1], 1.0);
            const cv::Point3d a = Multiply(Rt, cv::Point3d(u2[0], u2[1], 1.0));
            const cv::Point3d b = Multiply(Rt, cv::Point3d(u2Along[0], u2Along[1], 1.0));
            const cv::Point3d normal(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
            const double denominator = Dot(normal, ray);
            if (std::abs(denominator) < 1e-15)
            {
                return false;
            }
            const double lambda = Dot(normal, ProjectorCenter())/denominator;
            if (!(lambda > 0.0))
            {
                return false;
            }
            point = cv::Point3d(lambda*ray.x, lambda*ray.y, lambda*ray.z);
            return true;
        }
    };

    /// `pixels` moved one pixel along the projector line that does not carry depth.
    cv::Mat AlongLine(const cv::Mat & pixels, bool columnsCarryDepth)
    {
        cv::Mat moved = pixels.clone();
        for (int i = 0; i < moved.rows; i++)
        {
            moved.at<cv::Vec2d>(i, 0)[columnsCarryDepth ? 1 : 0] += 1.0;
        }
        return moved;
    }
}

void Scan3d::Pointcloud::Clear(void)
{
    points = cv::Mat();
    colors = cv::Mat();
    normals = cv::Mat();
}

void Scan3d::Pointcloud::InitPoints(int rows, int cols)
{
    points = cv::Mat(rows, cols, CV_32FC3);
    size_t total = points.total()*points.channels();
    float * data = points.ptr<float>(0);
    for (size_t i=0; i<total; i++)
    {
        data[i] = std::numeric_limits<float>::quiet_NaN();
    }
}

void Scan3d::Pointcloud::InitColor(int rows, int cols)
{
    colors = cv::Mat::zeros(rows, cols, CV_8UC3);
    memset(colors.data, 255, colors.total()*colors.channels()); //white
}

void Scan3d::Pointcloud::InitNormals(int rows, int cols)
{
    normals = cv::Mat(rows, cols, CV_32FC3);
    size_t total = normals.total()*normals.channels();;
    float * data = normals.ptr<float>(0);
    for (size_t i=0; i<total; i++)
    {
        data[i] = std::numeric_limits<float>::quiet_NaN();
    }
}

void Scan3d::ReconstructModel(Pointcloud & pointcloud, CalibrationData const& calib,
                                cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
                                cv::Size const& projectorSize, int threshold, double maxDist, ProgressSink * progress,
                                Triangulation triangulation)
{
    pointcloud.Clear();

    if (!patternImage.data || patternImage.type()!=CV_32FC2)
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid pattern_image\n";
        return;
    }
    if (!minMaxImage.data || minMaxImage.type()!=CV_8UC2 || minMaxImage.size()!=patternImage.size())
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid min_max_image\n";
        return;
    }
    if (colorImage.data && (colorImage.type()!=CV_8UC3 || colorImage.size()!=patternImage.size()))
    {   //not standard RGB image
        std::cerr << "[ReconstructModel] ERROR invalid color_image\n";
        return;
    }
    if (!calib.IsValid() || projectorSize.width<1 || projectorSize.height<1)
    {   //invalid calibration
        return;
    }

    //one output point per projector pixel
    const int scaleFactorX = 1;
    const int scaleFactorY = (projectorSize.width>projectorSize.height ? 1 : 2); //XXX HACK: preserve regular aspect ratio XXX HACK
    const int outCols = projectorSize.width/scaleFactorX;
    const int outRows = projectorSize.height/scaleFactorY;
    if (outCols<1 || outRows<1)
    {
        return;
    }
    pointcloud.InitPoints(outRows, outCols);
    pointcloud.InitColor(outRows, outCols);

    if (progress)
    {
        progress->Status("Reconstruction in progress: collecting points");
    }

    //camera pixels seen by each projector pixel, accumulated in image scan order
    const size_t cells = static_cast<size_t>(outRows)*outCols;
    std::vector<unsigned> count(cells, 0);
    std::vector<double> sumX(cells, 0.0), sumY(cells, 0.0);
    std::vector<cv::Point2f> projPoints(cells);

    for (int h=0; h<patternImage.rows; h++)
    {
        const cv::Vec2f * currPatternRow = patternImage.ptr<cv::Vec2f>(h);
        const cv::Vec2b * minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);
        for (int w=0; w<patternImage.cols; w++)
        {
            const cv::Vec2f & pattern = currPatternRow[w];
            const cv::Vec2b & minMax = minMaxRow[w];

            if (StructuredLight::Invalid(pattern)
                || pattern[0]<0.f || pattern[0]>=projectorSize.width || pattern[1]<0.f || pattern[1]>=projectorSize.height
                || (minMax[1]-minMax[0])<static_cast<int>(threshold))
            {   //skip
                continue;
            }

            //ok
            const cv::Point2f projPoint(pattern[0]/scaleFactorX, pattern[1]/scaleFactorY);
            const size_t index = static_cast<size_t>(static_cast<unsigned>(projPoint.y))*outCols + static_cast<unsigned>(projPoint.x);
            if (index >= cells)
            {   //odd-sized portrait projector: last half row has no output pixel
                continue;
            }
            projPoints[index] = projPoint;   //the last camera pixel defines the projector coordinate
            const cv::Point2f camPoint(static_cast<float>(w), static_cast<float>(h));
            sumX[index] += camPoint.x;
            sumY[index] += camPoint.y;
            count[index]++;
        }
    }
    if (progress && progress->Canceled())
    {
        pointcloud.Clear();
        return;
    }

    //correspondences, in increasing projector pixel order
    std::vector<size_t> used;
    for (size_t index = 0; index < cells; index++)
    {
        if (count[index])
        {
            used.push_back(index);
        }
    }
    const int total = static_cast<int>(used.size());
    if (total > 0)
    {
        cv::Mat camPixels(total, 1, CV_64FC2), projPixels(total, 1, CV_64FC2);
        for (int i = 0; i < total; i++)
        {
            const size_t index = used[i];
            const cv::Point2f & projPoint = projPoints[index];
            camPixels.at<cv::Vec2d>(i, 0) = cv::Vec2d(sumX[index]/count[index], sumY[index]/count[index]);
            projPixels.at<cv::Vec2d>(i, 0) = cv::Vec2d(projPoint.x*scaleFactorX, projPoint.y*scaleFactorY);
        }

        const StereoGeometry geometry(calib.R.t(), calib.T);
        const bool usePlane = triangulation == Triangulation::ProjectorPlane;
        const bool columnsCarryDepth = geometry.ColumnsCarryDepth();
        if (progress)
        {
            progress->Status("Reconstruction in progress: triangulating");
        }

        Parallel::ParallelFor(total, Min_Points_Per_Thread, [&](long long first, long long last) {
            if (progress && progress->Canceled())
            {
                return;
            }
            const cv::Range range(static_cast<int>(first), static_cast<int>(last));

            //to normalized camera coordinates
            cv::Mat camRays, projRays;
            cv::undistortPoints(camPixels.rowRange(range), camRays, calib.camK, calib.camKc);
            cv::undistortPoints(projPixels.rowRange(range), projRays, calib.projK, calib.projKc);
            cv::Mat projRaysAlong;
            if (usePlane)
            {
                cv::undistortPoints(AlongLine(projPixels.rowRange(range), columnsCarryDepth), projRaysAlong, calib.projK, calib.projKc);
            }

            for (int i = range.start; i < range.end; i++)
            {
                double distance = maxDist;  //quality meassure
                cv::Point3d p = geometry.Intersect(camRays.at<cv::Vec2d>(i - range.start, 0),
                                                   projRays.at<cv::Vec2d>(i - range.start, 0), &distance);
                if (!(distance < maxDist))
                {   //rays too far apart: skip
                    continue;
                }
                if (usePlane && !geometry.IntersectPlane(camRays.at<cv::Vec2d>(i - range.start, 0), projRays.at<cv::Vec2d>(i - range.start, 0),
                                                         projRaysAlong.at<cv::Vec2d>(i - range.start, 0), p))
                {
                    continue;
                }

                const size_t index = used[i];
                const int row = static_cast<int>(index/outCols);
                const int col = static_cast<int>(index%outCols);
                cv::Vec3f & cloudPoint = pointcloud.points.at<cv::Vec3f>(row, col);
                cloudPoint[0] = static_cast<float>(p.x);
                cloudPoint[1] = static_cast<float>(p.y);
                cloudPoint[2] = static_cast<float>(p.z);

                if (colorImage.data)
                {
                    const cv::Vec2d & cam = camPixels.at<cv::Vec2d>(i, 0);
                    pointcloud.colors.at<cv::Vec3b>(row, col) =
                        colorImage.at<cv::Vec3b>(static_cast<unsigned>(cam[1]), static_cast<unsigned>(cam[0]));
                }
            }
        });
    }

    if (progress && progress->Canceled())
    {
        pointcloud.Clear();
        return;
    }
}

void Scan3d::ReconstructModelSubpixel(Pointcloud & pointcloud, CalibrationData const& calib,
                                cv::Mat const& subpixelImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
                                cv::Size const& projectorSize, int threshold, double maxDist, ProgressSink * progress,
                                Triangulation triangulation)
{
    pointcloud.Clear();

    if (!subpixelImage.data || subpixelImage.type()!=CV_32FC2
        || !minMaxImage.data || minMaxImage.type()!=CV_8UC2 || minMaxImage.size()!=subpixelImage.size())
    {
        std::cerr << "[ReconstructModelSubpixel] ERROR invalid input images\n";
        return;
    }
    if (colorImage.data && (colorImage.type()!=CV_8UC3 || colorImage.size()!=subpixelImage.size()))
    {
        std::cerr << "[ReconstructModelSubpixel] ERROR invalid color_image\n";
        return;
    }
    if (!calib.IsValid() || projectorSize.width<1 || projectorSize.height<1)
    {
        return;
    }

    const int rows = subpixelImage.rows, cols = subpixelImage.cols;
    pointcloud.InitPoints(rows, cols);
    pointcloud.InitColor(rows, cols);

    const StereoGeometry geometry(calib.R.t(), calib.T);
    const bool usePlane = triangulation == Triangulation::ProjectorPlane;
    const bool columnsCarryDepth = geometry.ColumnsCarryDepth();
    if (progress)
    {
        progress->Status("Reconstruction in progress: triangulating");
    }

    Parallel::ParallelFor(rows, Min_Rows_Per_Thread, [&](long long firstRow, long long lastRow) {
        std::vector<int> columns;
        for (int h = static_cast<int>(firstRow); h < static_cast<int>(lastRow); h++)
        {
            if (progress && progress->Canceled())
            {
                return;
            }
            const cv::Vec2f * subpixelRow = subpixelImage.ptr<cv::Vec2f>(h);
            const cv::Vec2b * minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);

            //pixels of this row with a projector coordinate
            columns.clear();
            for (int w = 0; w < cols; w++)
            {
                const cv::Vec2f & value = subpixelRow[w];
                if (!StructuredLight::Invalid(value) && (minMaxRow[w][1] - minMaxRow[w][0]) >= threshold)
                {
                    columns.push_back(w);
                }
            }
            const int total = static_cast<int>(columns.size());
            if (total < 1)
            {
                continue;
            }
            cv::Mat camPixels(total, 1, CV_64FC2), projPixels(total, 1, CV_64FC2);
            for (int i = 0; i < total; i++)
            {
                const cv::Vec2f & value = subpixelRow[columns[i]];
                camPixels.at<cv::Vec2d>(i, 0) = cv::Vec2d(columns[i], h);
                projPixels.at<cv::Vec2d>(i, 0) = cv::Vec2d(value[0], value[1]);
            }

            //to normalized camera coordinates
            cv::Mat camRays, projRays;
            cv::undistortPoints(camPixels, camRays, calib.camK, calib.camKc);
            cv::undistortPoints(projPixels, projRays, calib.projK, calib.projKc);
            cv::Mat projRaysAlong;
            if (usePlane)
            {
                cv::undistortPoints(AlongLine(projPixels, columnsCarryDepth), projRaysAlong, calib.projK, calib.projKc);
            }

            cv::Vec3f * pointsRow = pointcloud.points.ptr<cv::Vec3f>(h);
            for (int i = 0; i < total; i++)
            {
                double distance = maxDist;
                cv::Point3d p = geometry.Intersect(camRays.at<cv::Vec2d>(i, 0), projRays.at<cv::Vec2d>(i, 0), &distance);
                if (!(distance < maxDist))
                {   //rays too far apart: skip
                    continue;
                }
                if (usePlane && !geometry.IntersectPlane(camRays.at<cv::Vec2d>(i, 0), projRays.at<cv::Vec2d>(i, 0),
                                                         projRaysAlong.at<cv::Vec2d>(i, 0), p))
                {
                    continue;
                }
                const int w = columns[i];
                pointsRow[w] = cv::Vec3f(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
                if (colorImage.data)
                {
                    pointcloud.colors.at<cv::Vec3b>(h, w) = colorImage.at<cv::Vec3b>(h, w);
                }
            }
        }
    });

    if (progress && progress->Canceled())
    {
        pointcloud.Clear();
    }
}

void Scan3d::TriangulateStereo(const cv::Mat & K1, const cv::Mat & kc1, const cv::Mat & K2, const cv::Mat & kc2,
                                  const cv::Mat & Rt, const cv::Mat & T, const cv::Point2d & p1, const cv::Point2d & p2,
                                  cv::Point3d & p3d, double * distance)
{
    //to image camera coordinates
    cv::Mat inp1(1, 1, CV_64FC2), inp2(1, 1, CV_64FC2);
    inp1.at<cv::Vec2d>(0, 0) = cv::Vec2d(p1.x, p1.y);
    inp2.at<cv::Vec2d>(0, 0) = cv::Vec2d(p2.x, p2.y);
    cv::Mat outp1, outp2;
    cv::undistortPoints(inp1, outp1, K1, kc1);
    cv::undistortPoints(inp2, outp2, K2, kc2);

    p3d = StereoGeometry(Rt, T).Intersect(outp1.at<cv::Vec2d>(0,0), outp2.at<cv::Vec2d>(0,0), distance);
}

cv::Point3d Scan3d::ApproximateRayIntersection(const cv::Point3d & v1, const cv::Point3d & q1,
                                                    const cv::Point3d & v2, const cv::Point3d & q2,
                                                    double * distance, double * outLambda1, double * outLambda2)
{
    const double v1tv1 = Dot(v1, v1);
    const double v2tv2 = Dot(v2, v2);
    const double v1tv2 = Dot(v1, v2);
    const double v2tv1 = Dot(v2, v1);

    const double detV = v1tv1*v2tv2 - v1tv2*v2tv1;

    cv::Point3d q2Q1 = q2 - q1;
    double Q1 = v1.x*q2Q1.x + v1.y*q2Q1.y + v1.z*q2Q1.z;
    double Q2 = -(v2.x*q2Q1.x + v2.y*q2Q1.y + v2.z*q2Q1.z);

    double lambda1 = (v2tv2 * Q1 + v1tv2 * Q2) /detV;
    double lambda2 = (v2tv1 * Q1 + v1tv1 * Q2) /detV;

    cv::Point3d p1 = lambda1*v1 + q1; //ray1
    cv::Point3d p2 = lambda2*v2 + q2; //ray2

    cv::Point3d p = 0.5*(p1+p2);

    if (distance!=NULL)
    {
        *distance = cv::norm(p2-p1);
    }
    if (outLambda1)
    {
        *outLambda1 = lambda1;
    }
    if (outLambda2)
    {
        *outLambda2 = lambda2;
    }

    return p;
}

void Scan3d::ComputeNormals(Scan3d::Pointcloud & pointcloud)
{
    if (!pointcloud.points.data)
    {
        return;
    }

    pointcloud.InitNormals(pointcloud.points.rows, pointcloud.points.cols);

    Parallel::ParallelFor(pointcloud.points.rows, Min_Rows_Per_Thread, [&](long long firstRow, long long lastRow) {
        for (int h=std::max(1, static_cast<int>(firstRow)); h<static_cast<int>(lastRow) && h+1<pointcloud.points.rows; h++)
        {
            const cv::Vec3f * pointsRow0 = pointcloud.points.ptr<cv::Vec3f>(h-1);
            const cv::Vec3f * pointsRow1 = pointcloud.points.ptr<cv::Vec3f>(h);
            const cv::Vec3f * pointsRow2 = pointcloud.points.ptr<cv::Vec3f>(h+1);

            cv::Vec3f * normalsRow = pointcloud.normals.ptr<cv::Vec3f>(h);

            for (int w=1; w+1<pointcloud.points.cols; w++)
            {
                cv::Vec3f const& w1 = pointsRow1[w-1];
                cv::Vec3f const& w2 = pointsRow1[w+1];

                cv::Vec3f const& h1 = pointsRow0[w];
                cv::Vec3f const& h2 = pointsRow2[w];

                if (StructuredLight::Invalid(w1[0]) || StructuredLight::Invalid(w2[0]) || StructuredLight::Invalid(h1[0]) || StructuredLight::Invalid(h2[0]))
                {
                    continue;
                }

                cv::Point3d n1(w2[0]-w1[0], w2[1]-w1[1], w2[2]-w1[2]);
                cv::Point3d n2(h2[0]-h1[0], h2[1]-h1[1], h2[2]-h1[2]);

                cv::Point3d normal = cv::Point3d(-n2.z*n1.y+n2.y*n1.z, n2.z*n1.x-n2.x*n1.z, -n2.y*n1.x+n2.x*n1.y);
                double norm = std::sqrt(normal.x*normal.x+normal.y*normal.y+normal.z*normal.z);
                if (norm>0.0)
                {
                    cv::Vec3f & cloudNormal = normalsRow[w];
                    cloudNormal[0] = normal.x/norm;
                    cloudNormal[1] = normal.y/norm;
                    cloudNormal[2] = normal.z/norm;
                }
            }
        }
    });
}

cv::Mat Scan3d::MakeProjectorView(cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
                                        cv::Size const& projectorSize, int threshold)
{
    if (!patternImage.data || patternImage.type()!=CV_32FC2)
    {   //pattern not correctly decoded
        std::cerr << "[MakeProjectorView] ERROR invalid pattern_image\n";
        return cv::Mat();
    }
    if (!minMaxImage.data || minMaxImage.type()!=CV_8UC2 || minMaxImage.size()!=patternImage.size())
    {   //pattern not correctly decoded
        std::cerr << "[MakeProjectorView] ERROR invalid min_max_image\n";
        return cv::Mat();
    }
    if (!colorImage.data || colorImage.type()!=CV_8UC3 || colorImage.size()!=patternImage.size())
    {   //not standard RGB image
        std::cerr << "[MakeProjectorView] ERROR invalid color_image\n";
        return cv::Mat();
    }
    if (projectorSize.width<1 || projectorSize.height<1)
    {
        return cv::Mat();
    }

    //init
    int scaleFactorX = 1;
    int scaleFactorY = (projectorSize.width>projectorSize.height ? 1 : 2); //XXX HACK: preserve regular aspect ratio XXX HACK
    int outCols = projectorSize.width/scaleFactorX;
    int outRows = projectorSize.height/scaleFactorY;
    if (outCols<1 || outRows<1)
    {
        return cv::Mat();
    }
    cv::Mat projector_image = cv::Mat::zeros(outRows, outCols, CV_8UC3);
    memset(projector_image.data, 255, projector_image.total()*projector_image.channels()); //white

    //sequential on purpose: when several camera pixels map to one projector pixel the last one wins
    for (int h=0; h<patternImage.rows; h++)
    {
        const cv::Vec2f * currPatternRow = patternImage.ptr<cv::Vec2f>(h);
        const cv::Vec2b * minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);
        for (int w=0; w<patternImage.cols; w++)
        {
            const cv::Vec2f & pattern = currPatternRow[w];
            const cv::Vec2b & minMax = minMaxRow[w];

            if (StructuredLight::Invalid(pattern)
                || pattern[0]<0.f || pattern[0]>=projectorSize.width || pattern[1]<0.f || pattern[1]>=projectorSize.height
                || (minMax[1]-minMax[0])<static_cast<int>(threshold))
            {   //skip
                continue;
            }

            //ok
            cv::Point2f projPoint(pattern[0]/scaleFactorX, pattern[1]/scaleFactorY);
            const unsigned row = static_cast<unsigned>(projPoint.y), col = static_cast<unsigned>(projPoint.x);
            if (row >= static_cast<unsigned>(outRows) || col >= static_cast<unsigned>(outCols))
            {
                continue;
            }
            projector_image.at<cv::Vec3b>(row, col) = colorImage.at<cv::Vec3b>(h, w);
        }
    }

    return projector_image;
}
} // namespace smcp
