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

// Gray-code structured light: image loading, direct/global light estimation and decoding.
//
// DecodeGraySet() is the whole decoding stage for one capture set. It loads every image once (in
// parallel), then decodes row ranges in parallel. Pixels are independent, so the result is
// bit-identical to the serial algorithm of Moreno & Taubin that it replaces.

#include <cmath>
#include <string>
#include <vector>

#include <opencv2/core/core.hpp>

#include "core/Progress.h"

namespace smcp
{
namespace StructuredLight
{
    enum DecodeFlags {SimpleDecode = 0x00, GrayPatternDecode = 0x01, RobustDecode = 0x02};

    extern const float Pixel_Uncertain;
    extern const unsigned short Bit_Uncertain;

    /// Robust decoding parameters (Moreno & Taubin): b is a ratio in [0, 1), m a number of levels.
    struct DecodeParams
    {
        float b = 0.5f;
        unsigned m = 5;

        bool operator==(const DecodeParams & other) const {return b==other.b && m==other.m;}
    };

    /// Result of decoding one capture set.
    struct DecodedSet
    {
        cv::Mat pattern;  ///< CV_32FC2: projector column and row seen by each camera pixel (NaN = uncertain).
        cv::Mat minMax;   ///< CV_8UC2: minimum and maximum intensity of each camera pixel over the sequence.
    };

    /// Loads an image as 8-bit gray (BGR decode + BGR2GRAY, like the capture pipeline has always done).
    /// The file is read through a UTF-8 path, so folders with accents work. Empty Mat on failure.
    cv::Mat GetGrayImage(const std::string & filename);
    /// Same decoding as GetGrayImage but keeps the BGR image.
    cv::Mat GetColorImage(const std::string & filename);

    /// Decodes a complete Gray-code capture set (2 + 4*bits images, in capture order) with the robust
    /// pixel classification. Returns false and fills `error` when the set cannot be decoded; `progress`
    /// may be null.
    bool DecodeGraySet(const std::vector<std::string> & images, cv::Size const& projectorSize, const DecodeParams & params,
                        DecodedSet & decoded, std::string & error, ProgressSink * progress = nullptr);

    /// Decodes already loaded gray images (indexed like the capture set; entries 0 and 1 are not used).
    bool DecodePattern(const std::vector<cv::Mat> & grayImages, cv::Mat & patternImage, cv::Mat & minMaxImage, cv::Size const& projectorSize,
                        unsigned flags, const cv::Mat & directLight, unsigned m, std::string & error);
    unsigned short GetRobustBit(unsigned value1, unsigned value2, unsigned Ld, unsigned Lg, unsigned m);

    /// Loads the gray images of a capture set that the decoding uses (entries 0 and 1 stay empty),
    /// each one once and in parallel. Returns false and fills `error` when one cannot be read.
    bool LoadGraySet(const std::vector<std::string> & images, std::vector<cv::Mat> & grayImages, std::string & error,
                        ProgressSink * progress = nullptr);

    /// Sub-pixel projector coordinates for the sub-pixel reconstruction (the classic one does not
    /// use them). `patternImage` holds whole projector columns and rows; here every camera pixel
    /// gets a fractional column and row, in the same coordinates (a whole number is the centre of
    /// that projector pixel), as a CV_32FC2 image with NaN where no value can be given.
    ///
    /// Along a camera row the decoded column is constant over a few pixels and then steps by one.
    /// A step is the border between two projector columns, that is, a half-integer coordinate, and
    /// it is located between the two camera pixels where the stripe image of the Gray bit that
    /// changes there crosses its inverse (linear interpolation of their difference). Between two
    /// consecutive borders the coordinate is interpolated linearly, which assumes a smooth surface
    /// across one projector pixel. A run with a border on one side only continues the slope of the
    /// neighbouring run; a run with no border, or bounded by a jump or by pixels that are uncertain
    /// or below `threshold`, gets no value. Rows of the projector are treated the same way along
    /// camera columns.
    ///
    /// `grayImages` are the images of the set as LoadGraySet returns them. When the vector is empty
    /// (a decoded dump without its images) every border is placed halfway between its two pixels.
    cv::Mat RefinePattern(const std::vector<cv::Mat> & grayImages, const cv::Mat & patternImage, const cv::Mat & minMaxImage,
                        cv::Size const& projectorSize, int threshold, ProgressSink * progress = nullptr);
    cv::Mat EstimateDirectLight(const std::vector<cv::Mat> & images, float b);

    static inline bool Invalid(float value) {return std::isnan(value);}
    static inline bool Invalid(const cv::Vec2f & pt) {return std::isnan(pt[0]) || std::isnan(pt[1]);}
    static inline bool Invalid(const cv::Vec3f & pt) {return std::isnan(pt[0]) || std::isnan(pt[1]) || std::isnan(pt[2]);}

    int BinaryToGray(int value);

    cv::Mat ColorizePattern(const cv::Mat & patternImage, unsigned set, float maxValue);
};

} // namespace smcp
