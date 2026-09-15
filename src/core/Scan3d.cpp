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

#include <iostream>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include <QApplication>
#include <QProgressDialog>
#include <QMap>

#include "core/StructuredLight.h"

namespace smcp
{

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
                                cv::Size const& projectorSize, int threshold, double maxDist, QWidget * parentWidget)
{
    ReconstructModelPatchCenter(pointcloud, calib, patternImage, minMaxImage, colorImage, projectorSize, 
                                    threshold, maxDist, parentWidget);
}

void Scan3d::ReconstructModelSimple(Pointcloud & pointcloud, CalibrationData const& calib, 
                                cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
                                cv::Size const& projectorSize, int threshold, double maxDist, QWidget * parentWidget)
{
    if (!patternImage.data || patternImage.type()!=CV_32FC2)
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid pattern_image\n";
        return;
    }
    if (!minMaxImage.data || minMaxImage.type()!=CV_8UC2)
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid min_max_image\n";
        return;
    }
    if (colorImage.data && colorImage.type()!=CV_8UC3)
    {   //not standard RGB image
        std::cerr << "[ReconstructModel] ERROR invalid color_image\n";
        return;
    }
    if (!calib.IsValid())
    {   //invalid calibration
        return;
    }

    //parameters
    //const unsigned threshold = config.value("main/shadow_threshold", 70).toUInt();
    //const double   maxDist  = config.value("main/max_dist_threshold", 40).toDouble();
    //const bool     removeBackground = config.value("main/remove_background", true).toBool();
    //const double   planeDist = config.value("main/plane_dist", 100.0).toDouble();
    double planeDist = 100.0;

    /* background removal
    cv::Point2i planeCoord[3];
    planeCoord[0] = cv::Point2i(config.value("background_plane/x1").toUInt(), config.value("background_plane/y1").toUInt());
    planeCoord[1] = cv::Point2i(config.value("background_plane/x2").toUInt(), config.value("background_plane/y2").toUInt());
    planeCoord[2] = cv::Point2i(config.value("background_plane/x3").toUInt(), config.value("background_plane/y3").toUInt());

    if (planeCoord[0].x<=0 || planeCoord[0].x>=patternLocal.cols
        || planeCoord[0].y<=0 || planeCoord[0].y>=patternLocal.rows)
    {
        planeCoord[0] = cv::Point2i(50, 50);
        config.setValue("background_plane/x1", planeCoord[0].x);
        config.setValue("background_plane/y1", planeCoord[0].y);
    }
    if (planeCoord[1].x<=0 || planeCoord[1].x>=patternLocal.cols
        || planeCoord[1].y<=0 || planeCoord[1].y>=patternLocal.rows)
    {
        planeCoord[1] = cv::Point2i(50, patternLocal.rows-50);
        config.setValue("background_plane/x2", planeCoord[1].x);
        config.setValue("background_plane/y2", planeCoord[1].y);
    }
    if (planeCoord[2].x<=0 || planeCoord[2].x>=patternLocal.cols
        || planeCoord[2].y<=0 || planeCoord[2].y>=patternLocal.rows)
    {
        planeCoord[2] = cv::Point2i(patternLocal.cols-50, 50);
        config.setValue("background_plane/x3", planeCoord[2].x);
        config.setValue("background_plane/y3", planeCoord[2].y);
    }
    */

    //init point cloud
    int scaleFactor = 1;
    int outCols = patternImage.cols/scaleFactor;
    int outRows = patternImage.rows/scaleFactor;
    pointcloud.Clear();
    pointcloud.InitPoints(outRows, outCols);
    pointcloud.InitColor(outRows, outCols);

    //progress
    QProgressDialog * progress = NULL;
    if (parentWidget)
    {
        progress = new QProgressDialog("Reconstruction in progress.", "Abort", 0, patternImage.rows, parentWidget, 
                                        Qt::Dialog|Qt::CustomizeWindowHint|Qt::WindowCloseButtonHint);
        progress->setWindowModality(Qt::WindowModal);
        progress->setWindowTitle("Processing");
        progress->setMinimumWidth(400);
    }

    //take 3 points in back plane
    /*cv::Mat plane;
    if (removeBackground)
    {
        cv::Point3d p[3];
        for (unsigned i=0; i<3;i++)
        {
            for (unsigned j=0; 
                j<10 && (
                    Invalid(patternLocal.at<cv::Vec2f>(planeCoord[i].y, planeCoord[i].x)[0])
                    || Invalid(patternLocal.at<cv::Vec2f>(planeCoord[i].y, planeCoord[i].x)[1])); j++)
            {
                planeCoord[i].x += 1.f;
            }
            const cv::Vec2f & pattern = patternLocal.at<cv::Vec2f>(planeCoord[i].y, planeCoord[i].x);

            const float col = pattern[0];
            const float row = pattern[1];

            if (projectorSize.width<=static_cast<int>(col) || projectorSize.height<=static_cast<int>(row))
            {   //abort
                continue;
            }

            //shoot a ray through the image: u=\lambda*v + q
            cv::Point3d u1 = camera.ToWorldCoord(planeCoord[i].x, planeCoord[i].y);
            cv::Point3d v1 = camera.worldRay(planeCoord[i].x, planeCoord[i].y);

            //shoot a ray through the projector: u=\lambda*v + q
            cv::Point3d u2 = projector.ToWorldCoord(col, row);
            cv::Point3d v2 = projector.worldRay(col, row);

            //compute ray-ray approximate intersection
            double distance = 0.0;
            p[i] = Geometry::ApproximateRayIntersection(v1, u1, v2, u2, &distance);
            std::cout << "Plane point " << i << " distance " << distance << std::endl;
        }
        plane = Geometry::GetPlane(p[0], p[1], p[2]);
        if (cv::Mat(plane.rowRange(0,3).t()*cv::Mat(cv::Point3d(p[0].x, p[0].y, p[0].z-1.0)) + plane.at<double>(3,0)).at<double>(0,0)
                <0.0)
        {
            plane = -1.0*plane;
        }
        std::cout << "Background plane: " << plane << std::endl;
    }
    */

    cv::Mat Rt = calib.R.t();

    unsigned good = 0;
    unsigned bad  = 0;
    unsigned invalid = 0;
    unsigned repeated = 0;
    for (int h=0; h<patternImage.rows; h+=scaleFactor)
    {
        if (progress && h%4==0)
        {
            progress->setValue(h);
            progress->setLabelText(QString("Reconstruction in progress: %1 good points/%2 bad points").arg(good).arg(bad));
            QApplication::instance()->processEvents();
        }
        if (progress && progress->wasCanceled())
        {   //abort
            pointcloud.Clear();
            return;
        }

        const cv::Vec2f * currPatternRow = patternImage.ptr<cv::Vec2f>(h);
        const cv::Vec2b * minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);
        for (int w=0; w<patternImage.cols; w+=scaleFactor)
        {
            double distance = maxDist;  //quality meassure
            cv::Point3d p;               //reconstructed point
            //cv::Point3d normal(0.0, 0.0, 0.0);

            const cv::Vec2f & pattern = currPatternRow[w];
            const cv::Vec2b & minMax = minMaxRow[w];

            if (StructuredLight::Invalid(pattern) || pattern[0]<0.f || pattern[1]<0.f
                || (minMax[1]-minMax[0])<static_cast<int>(threshold))
            {   //skip
                invalid++;
                continue;
            }

            const float col = pattern[0];
            const float row = pattern[1];

            if (projectorSize.width<=static_cast<int>(col) || projectorSize.height<=static_cast<int>(row))
            {   //abort
                continue;
            }

            cv::Vec3f & cloudPoint = pointcloud.points.at<cv::Vec3f>(h/scaleFactor, w/scaleFactor);
            if (!StructuredLight::Invalid(cloudPoint[0]))
            {   //point already reconstructed!
                repeated++;
                continue;
            }

            //standard
            cv::Point2d p1(w, h);
            cv::Point2d p2(col, row);
            TriangulateStereo(calib.camK, calib.camKc, calib.projK, calib.projKc, Rt, calib.T, p1, p2, p, &distance);

            //save texture coordinates
            /*
            normal.x = static_cast<float>(w)/static_cast<float>(colorImage.cols);
            normal.y = static_cast<float>(h)/static_cast<float>(colorImage.rows);
            normal.z = 0;
            */

            if (distance < maxDist)
            {   //good point

                //evaluate the plane
                double d = planeDist+1;
                /*if (removeBackground)
                {
                    d = cv::Mat(plane.rowRange(0,3).t()*cv::Mat(p) + plane.at<double>(3,0)).at<double>(0,0);
                }*/
                if (d>planeDist)
                {   //object point, keep
                    good++;

                    cloudPoint[0] = p.x;
                    cloudPoint[1] = p.y;
                    cloudPoint[2] = p.z;

                    //normal
                    /*cpoint.normalX = normal.x;
                    cpoint.normalY = normal.y;
                    cpoint.normalZ = normal.z;*/

                    if (colorImage.data)
                    {
                        const cv::Vec3b & vec = colorImage.at<cv::Vec3b>(h, w);
                        cv::Vec3b & cloudColor = pointcloud.colors.at<cv::Vec3b>(h/scaleFactor, w/scaleFactor);
                        cloudColor[0] = vec[0];
                        cloudColor[1] = vec[1];
                        cloudColor[2] = vec[2];
                    }
                }
            }
            else
            {   //skip
                bad++;
                //std::cout << " d = " << distance << std::endl;
            }
        }   //for each column
    }   //for each row

    if (progress)
    {
        progress->setValue(patternImage.rows);
        progress->close();
        delete progress;
        progress = NULL;
    }

    std::cout << "Reconstructed points[simple]: " << good << " (" << bad << " skipped, " << invalid << " invalid) " << std::endl
                << " - repeated points: " << repeated << " (ignored) " << std::endl;
}

void Scan3d::ReconstructModelPatchCenter(Pointcloud & pointcloud, CalibrationData const& calib, 
                                cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
                                cv::Size const& projectorSize, int threshold, double maxDist, QWidget * parentWidget)
{
    if (!patternImage.data || patternImage.type()!=CV_32FC2)
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid pattern_image\n";
        return;
    }
    if (!minMaxImage.data || minMaxImage.type()!=CV_8UC2)
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid min_max_image\n";
        return;
    }
    if (colorImage.data && colorImage.type()!=CV_8UC3)
    {   //not standard RGB image
        std::cerr << "[ReconstructModel] ERROR invalid color_image\n";
        return;
    }
    if (!calib.IsValid())
    {   //invalid calibration
        return;
    }

    //parameters
    //const unsigned threshold = config.value("main/shadow_threshold", 70).toUInt();
    //const double   maxDist  = config.value("main/max_dist_threshold", 40).toDouble();
    //const bool     removeBackground = config.value("main/remove_background", true).toBool();
    //const double   planeDist = config.value("main/plane_dist", 100.0).toDouble();
    double planeDist = 100.0;

    /* background removal
    cv::Point2i planeCoord[3];
    planeCoord[0] = cv::Point2i(config.value("background_plane/x1").toUInt(), config.value("background_plane/y1").toUInt());
    planeCoord[1] = cv::Point2i(config.value("background_plane/x2").toUInt(), config.value("background_plane/y2").toUInt());
    planeCoord[2] = cv::Point2i(config.value("background_plane/x3").toUInt(), config.value("background_plane/y3").toUInt());

    if (planeCoord[0].x<=0 || planeCoord[0].x>=patternLocal.cols
        || planeCoord[0].y<=0 || planeCoord[0].y>=patternLocal.rows)
    {
        planeCoord[0] = cv::Point2i(50, 50);
        config.setValue("background_plane/x1", planeCoord[0].x);
        config.setValue("background_plane/y1", planeCoord[0].y);
    }
    if (planeCoord[1].x<=0 || planeCoord[1].x>=patternLocal.cols
        || planeCoord[1].y<=0 || planeCoord[1].y>=patternLocal.rows)
    {
        planeCoord[1] = cv::Point2i(50, patternLocal.rows-50);
        config.setValue("background_plane/x2", planeCoord[1].x);
        config.setValue("background_plane/y2", planeCoord[1].y);
    }
    if (planeCoord[2].x<=0 || planeCoord[2].x>=patternLocal.cols
        || planeCoord[2].y<=0 || planeCoord[2].y>=patternLocal.rows)
    {
        planeCoord[2] = cv::Point2i(patternLocal.cols-50, 50);
        config.setValue("background_plane/x3", planeCoord[2].x);
        config.setValue("background_plane/y3", planeCoord[2].y);
    }
    */

    //init point cloud
    int scaleFactorX = 1;
    int scaleFactorY = (projectorSize.width>projectorSize.height ? 1 : 2); //XXX HACK: preserve regular aspect ratio XXX HACK
    int outCols = projectorSize.width/scaleFactorX;
    int outRows = projectorSize.height/scaleFactorY;
    pointcloud.Clear();
    pointcloud.InitPoints(outRows, outCols);
    pointcloud.InitColor(outRows, outCols);

    //progress
    QProgressDialog * progress = NULL;
    if (parentWidget)
    {
        progress = new QProgressDialog("Reconstruction in progress.", "Abort", 0, patternImage.rows, parentWidget, 
                                        Qt::Dialog|Qt::CustomizeWindowHint|Qt::WindowCloseButtonHint);
        progress->setWindowModality(Qt::WindowModal);
        progress->setWindowTitle("Processing");
        progress->setMinimumWidth(400);
        progress->show();
    }

    //take 3 points in back plane
    /*cv::Mat plane;
    if (removeBackground)
    {
        cv::Point3d p[3];
        for (unsigned i=0; i<3;i++)
        {
            for (unsigned j=0; 
                j<10 && (
                    Invalid(patternLocal.at<cv::Vec2f>(planeCoord[i].y, planeCoord[i].x)[0])
                    || Invalid(patternLocal.at<cv::Vec2f>(planeCoord[i].y, planeCoord[i].x)[1])); j++)
            {
                planeCoord[i].x += 1.f;
            }
            const cv::Vec2f & pattern = patternLocal.at<cv::Vec2f>(planeCoord[i].y, planeCoord[i].x);

            const float col = pattern[0];
            const float row = pattern[1];

            if (projectorSize.width<=static_cast<int>(col) || projectorSize.height<=static_cast<int>(row))
            {   //abort
                continue;
            }

            //shoot a ray through the image: u=\lambda*v + q
            cv::Point3d u1 = camera.ToWorldCoord(planeCoord[i].x, planeCoord[i].y);
            cv::Point3d v1 = camera.worldRay(planeCoord[i].x, planeCoord[i].y);

            //shoot a ray through the projector: u=\lambda*v + q
            cv::Point3d u2 = projector.ToWorldCoord(col, row);
            cv::Point3d v2 = projector.worldRay(col, row);

            //compute ray-ray approximate intersection
            double distance = 0.0;
            p[i] = Geometry::ApproximateRayIntersection(v1, u1, v2, u2, &distance);
            std::cout << "Plane point " << i << " distance " << distance << std::endl;
        }
        plane = Geometry::GetPlane(p[0], p[1], p[2]);
        if (cv::Mat(plane.rowRange(0,3).t()*cv::Mat(cv::Point3d(p[0].x, p[0].y, p[0].z-1.0)) + plane.at<double>(3,0)).at<double>(0,0)
                <0.0)
        {
            plane = -1.0*plane;
        }
        std::cout << "Background plane: " << plane << std::endl;
    }
    */

    //candidate points
    QMap<unsigned, cv::Point2f> projPoints;
    QMap<unsigned, std::vector<cv::Point2f> > camPoints;

    //cv::Mat projImage = cv::Mat::zeros(outRows, outCols, CV_8UC3);

    unsigned good = 0;
    unsigned bad  = 0;
    unsigned invalid = 0;
    unsigned repeated = 0;
    for (int h=0; h<patternImage.rows; h++)
    {
        if (progress && h%4==0)
        {
            progress->setValue(h);
            progress->setLabelText(QString("Reconstruction in progress: collecting points"));
            QApplication::instance()->processEvents();
        }
        if (progress && progress->wasCanceled())
        {   //abort
            pointcloud.Clear();
            return;
        }

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
            unsigned index = static_cast<unsigned>(projPoint.y)*outCols + static_cast<unsigned>(projPoint.x);
            projPoints.insert(index, projPoint);
            camPoints[index].push_back(cv::Point2f(w, h));

            //projImage.at<cv::Vec3b>(static_cast<unsigned>(projPoint.y), static_cast<unsigned>(projPoint.x)) = colorImage.at<cv::Vec3b>(h, w);
        }
    }

    //cv::imwrite("proj_image.png", projImage);
    
    if (progress)
    {
        progress->setValue(patternImage.rows);
    }

    cv::Mat Rt = calib.R.t();

    if (progress)
    {
        progress->setMaximum(projPoints.size());
    }

    QMapIterator<unsigned, cv::Point2f> iter1(projPoints);
    unsigned n = 0;
    while (iter1.hasNext()) 
    {
        n++;
        if (progress && n%1000==0)
        {
            progress->setValue(n);
            progress->setLabelText(QString("Reconstruction in progress: %1 good points/%2 bad points").arg(good).arg(bad));
            QApplication::instance()->processEvents();
        }
        if (progress && progress->wasCanceled())
        {   //abort
            pointcloud.Clear();
            return;
        }

        iter1.next();
        unsigned index = iter1.key();
        const cv::Point2f & projPoint = iter1.value();
        const std::vector<cv::Point2f> & camPointList = camPoints.value(index);
        const unsigned count = static_cast<int>(camPointList.size());

        if (!count)
        {   //empty list
            continue;
        }

        //center average
        cv::Point2d sum(0.0, 0.0), sum2(0.0, 0.0);
        for (std::vector<cv::Point2f>::const_iterator iter2=camPointList.begin(); iter2!=camPointList.end(); iter2++)
        {
            sum.x += iter2->x;
            sum.y += iter2->y;
            sum2.x += (iter2->x)*(iter2->x);
            sum2.y += (iter2->y)*(iter2->y);
        }
        cv::Point2d cam(sum.x/count, sum.y/count);
        cv::Point2d proj(projPoint.x*scaleFactorX, projPoint.y*scaleFactorY);

        //triangulate
        double distance = maxDist;  //quality meassure
        cv::Point3d p;          //reconstructed point
        TriangulateStereo(calib.camK, calib.camKc, calib.projK, calib.projKc, Rt, calib.T, cam, proj, p, &distance);

        if (distance < maxDist)
        {   //good point

            //evaluate the plane
            double d = planeDist+1;
            /*if (removeBackground)
            {
                d = cv::Mat(plane.rowRange(0,3).t()*cv::Mat(p) + plane.at<double>(3,0)).at<double>(0,0);
            }*/
            if (d>planeDist)
            {   //object point, keep
                good++;

                cv::Vec3f & cloudPoint = pointcloud.points.at<cv::Vec3f>(projPoint.y, projPoint.x);
                cloudPoint[0] = p.x;
                cloudPoint[1] = p.y;
                cloudPoint[2] = p.z;

                if (colorImage.data)
                {
                    const cv::Vec3b & vec = colorImage.at<cv::Vec3b>(static_cast<unsigned>(cam.y), static_cast<unsigned>(cam.x));
                    cv::Vec3b & cloudColor = pointcloud.colors.at<cv::Vec3b>(projPoint.y, projPoint.x);
                    cloudColor[0] = vec[0];
                    cloudColor[1] = vec[1];
                    cloudColor[2] = vec[2];
                }
            }
        }
        else
        {   //skip
            bad++;
            //std::cout << " d = " << distance << std::endl;
        }
    }   //while

    if (progress)
    {
        progress->setValue(projPoints.size());
        progress->close();
        delete progress;
        progress = NULL;
    }

    std::cout << "Reconstructed points [patch center]: " << good << " (" << bad << " skipped, " << invalid << " invalid) " << std::endl
                << " - repeated points: " << repeated << " (ignored) " << std::endl;
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
    assert(outp1.type()==CV_64FC2 && outp1.rows==1 && outp1.cols==1);
    assert(outp2.type()==CV_64FC2 && outp2.rows==1 && outp2.cols==1);
    const cv::Vec2d & outvec1 = outp1.at<cv::Vec2d>(0,0);
    const cv::Vec2d & outvec2 = outp2.at<cv::Vec2d>(0,0);
    cv::Point3d u1(outvec1[0], outvec1[1], 1.0);
    cv::Point3d u2(outvec2[0], outvec2[1], 1.0);

    //to world coordinates
    cv::Point3d w1 = u1;
    cv::Point3d w2 = cv::Point3d(cv::Mat(Rt*(cv::Mat(u2) - T)));

    //world rays
    cv::Point3d v1 = w1;
    cv::Point3d v2 = cv::Point3d(cv::Mat(Rt*cv::Mat(u2)));

    //compute ray-ray approximate intersection
    p3d = ApproximateRayIntersection(v1, w1, v2, w2, distance);
}

cv::Point3d Scan3d::ApproximateRayIntersection(const cv::Point3d & v1, const cv::Point3d & q1,
                                                    const cv::Point3d & v2, const cv::Point3d & q2,
                                                    double * distance, double * outLambda1, double * outLambda2)
{
    cv::Mat v1mat = cv::Mat(v1);
    cv::Mat v2mat = cv::Mat(v2);
    
    double v1tv1 = cv::Mat(v1mat.t()*v1mat).at<double>(0,0);
    double v2tv2 = cv::Mat(v2mat.t()*v2mat).at<double>(0,0);
    double v1tv2 = cv::Mat(v1mat.t()*v2mat).at<double>(0,0);
    double v2tv1 = cv::Mat(v2mat.t()*v1mat).at<double>(0,0);

    //cv::Mat V(2, 2, CV_64FC1);
    //V.at<double>(0,0) = v1tv1;  V.at<double>(0,1) = -v1tv2;
    //V.at<double>(1,0) = -v2tv1; V.at<double>(1,1) = v2tv2;
    //std::cout << " V: "<< V << std::endl;

    cv::Mat Vinv(2, 2, CV_64FC1);
    double detV = v1tv1*v2tv2 - v1tv2*v2tv1;
    Vinv.at<double>(0,0) = v2tv2/detV;  Vinv.at<double>(0,1) = v1tv2/detV;
    Vinv.at<double>(1,0) = v2tv1/detV; Vinv.at<double>(1,1) = v1tv1/detV;
    //std::cout << " V.inv(): "<< V.inv() << std::endl << " Vinv: " << Vinv << std::endl;

    //cv::Mat Q(2, 1, CV_64FC1);
    //Q.at<double>(0,0) = cv::Mat(v1mat.t()*(cv::Mat(q2-q1))).at<double>(0,0);
    //Q.at<double>(1,0) = cv::Mat(v2mat.t()*(cv::Mat(q1-q2))).at<double>(0,0);
    //std::cout << " Q: "<< Q << std::endl;

    cv::Point3d q2Q1 = q2 - q1;
    double Q1 = v1.x*q2Q1.x + v1.y*q2Q1.y + v1.z*q2Q1.z;
    double Q2 = -(v2.x*q2Q1.x + v2.y*q2Q1.y + v2.z*q2Q1.z);

    //cv::Mat L = V.inv()*Q;
    //cv::Mat L = Vinv*Q;
    //std::cout << " L: "<< L << std::endl;
    
    double lambda1 = (v2tv2 * Q1 + v1tv2 * Q2) /detV;
    double lambda2 = (v2tv1 * Q1 + v1tv1 * Q2) /detV;
    //std::cout << "lambda1: " << lambda1 << " lambda2: " << lambda2 << std::endl;

    //cv::Mat p1 = L.at<double>(0,0)*v1mat + cv::Mat(q1); //ray1
    //cv::Mat p2 = L.at<double>(1,0)*v2mat + cv::Mat(q2); //ray2
    //cv::Point3d p1 = L.at<double>(0,0)*v1 + q1; //ray1
    //cv::Point3d p2 = L.at<double>(1,0)*v2 + q2; //ray2
    cv::Point3d p1 = lambda1*v1 + q1; //ray1
    cv::Point3d p2 = lambda2*v2 + q2; //ray2

    //cv::Point3d p = cv::Point3d(cv::Mat((p1+p2)/2.0));
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

    for (int h=1; h+1<pointcloud.points.rows; h++)
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
}

cv::Mat Scan3d::MakeProjectorView(cv::Mat const& patternImage, cv::Mat const& minMaxImage, cv::Mat const& colorImage,
                                        cv::Size const& projectorSize, int threshold)
{
    if (!patternImage.data || patternImage.type()!=CV_32FC2)
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid pattern_image\n";
        return cv::Mat();
    }
    if (!minMaxImage.data || minMaxImage.type()!=CV_8UC2)
    {   //pattern not correctly decoded
        std::cerr << "[ReconstructModel] ERROR invalid min_max_image\n";
        return cv::Mat();
    }
    if (colorImage.data && colorImage.type()!=CV_8UC3)
    {   //not standard RGB image
        std::cerr << "[ReconstructModel] ERROR invalid color_image\n";
        return cv::Mat();
    }

    //init
    int scaleFactorX = 1;
    int scaleFactorY = (projectorSize.width>projectorSize.height ? 1 : 2); //XXX HACK: preserve regular aspect ratio XXX HACK
    int outCols = projectorSize.width/scaleFactorX;
    int outRows = projectorSize.height/scaleFactorY;
    cv::Mat projector_image = cv::Mat::zeros(outRows, outCols, CV_8UC3);
    memset(projector_image.data, 255, projector_image.total()*projector_image.channels()); //white

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
            projector_image.at<cv::Vec3b>(static_cast<unsigned>(projPoint.y), static_cast<unsigned>(projPoint.x)) = colorImage.at<cv::Vec3b>(h, w);
        }
    }

    return projector_image;
}
} // namespace smcp
