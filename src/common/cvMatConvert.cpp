#include "common/cvMatConvert.h"

namespace smcp
{

QPixmap cvMatConvert::ToQPixmap(const cv::Mat& mat)
{
	return QPixmap::fromImage(ToQImage(mat));
}

QImage cvMatConvert::ToQImage(const cv::Mat& mat)
{
	switch (mat.type())
	{
	case CV_8UC1: return ToQImageFromGray(mat);
	case CV_8UC3: return ToQImageFromRGB(mat);
	default:return {};
	}
}

QImage cvMatConvert::ToQImageFromRGB(const cv::Mat& mat)
{
	cv::Mat rgb;
	cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);

	const QImage image(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_RGB888);
	return image.copy();
}

QImage cvMatConvert::ToQImageFromGray(const cv::Mat& mat)
{
	const QImage image(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_Grayscale8);
	return image.copy();
}
} // namespace smcp
