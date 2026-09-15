#pragma once

#include <QPixmap>
#include <QImage>
#include "opencv2/opencv.hpp"

namespace smcp::cvMatConvert
{
/**
 * Convierte una imagen de tipo cv::Mat en un QPixmap.
 * @param mat Imagen a convertir.
 * @return Imagen en formato QPixmap.
 */
QPixmap ToQPixmap(const cv::Mat& mat);

/**
 * Convierte una imagen de tipo cv::Mat en un QImage.
 * @param mat Imagen a convertir.
 * @return Imagen en formato QImage.
 */
QImage ToQImage(const cv::Mat& mat);

/**
 * Convierte una imagen RGB en un QImage.
 * @param mat Imagen a convertir.
 * @return Imagen en formato QImage.
 */
QImage ToQImageFromRGB(const cv::Mat& mat);

/**
 * Convierte una imagen en escala de grises a un QImage.
 * @param mat Imagen a convertir.
 * @return Imagen en formato QImage.
 */
QImage ToQImageFromGray(const cv::Mat& mat);
}
