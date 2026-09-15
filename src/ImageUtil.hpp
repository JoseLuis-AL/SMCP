#ifndef __IMAGE_UTILS_HPP__
#define __IMAGE_UTILS_HPP__

#include <QPixmap>
#include <QImage>
#include "opencv2/opencv.hpp"

namespace ImageUtil
{
    QPixmap cvMatToQPixmap(const cv::Mat& mat);
    QImage cvMatToQImage(const cv::Mat& mat);
    QImage cvMatToQImageFromRGB(const cv::Mat& mat);
    QImage cvMatToQImageFromGray(const cv::Mat& mat);
}

#endif