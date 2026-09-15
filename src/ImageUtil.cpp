#include "ImageUtil.hpp"

QPixmap ImageUtil::cvMatToQPixmap(const cv::Mat& mat)
{
    return QPixmap::fromImage(cvMatToQImage(mat));
}

QImage ImageUtil::cvMatToQImage(const cv::Mat& mat)
{
    switch (mat.type())
    {
    case CV_8UC1: return cvMatToQImageFromGray(mat);
    case CV_8UC3: return cvMatToQImageFromRGB(mat);
    default:return {};
    }
}

QImage ImageUtil::cvMatToQImageFromRGB(const cv::Mat& mat)
{
    cv::Mat rgb;
    cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);

    const QImage image(mat.data, mat.cols, mat.rows, mat.step, QImage::Format_RGB888);
    return image.copy();
}

QImage ImageUtil::cvMatToQImageFromGray(const cv::Mat& mat)
{
    const QImage image(mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Grayscale8);
    return image.copy();
}