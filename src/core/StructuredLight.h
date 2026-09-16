/*
Copyright (c) 2012, Daniel Moreno and Gabriel Taubin
Copyright (c) 2024, José Luis Aguilera Luzania, Agustín Brau Ávila & Octavio Icasio Hernández
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
#include <opencv2/core/core.hpp>

#ifndef _MSC_VER
#  ifndef _isnan
#    include <math.h>
#    define _isnan std::isnan
#  endif
#endif


namespace smcp
{
namespace StructuredLight
{
    enum DecodeFlags {SimpleDecode = 0x00, GrayPatternDecode = 0x01, RobustDecode = 0x02};

    extern const float Pixel_Uncertain;
    extern const unsigned short Bit_Uncertain;

    bool DecodePattern(const std::vector<std::string> & images, cv::Mat & patternImage, cv::Mat & minMaxImage, cv::Size const& projectorSize,
                        unsigned flags = SimpleDecode, const cv::Mat & directLight = cv::Mat(), unsigned m = 5);
    unsigned short GetRobustBit(unsigned value1, unsigned value2, unsigned Ld, unsigned Lg, unsigned m);
    void ConvertPattern(cv::Mat & patternImage, cv::Size const& projectorSize, const int offset[2], bool binary);
    cv::Mat EstimateDirectLight(const std::vector<cv::Mat> & images, float b);

    cv::Mat GetGrayImage(const std::string & filename);
    static inline bool Invalid(float value) {return _isnan(value)>0;}
    static inline bool Invalid(const cv::Vec2f & pt) {return _isnan(pt[0]) || _isnan(pt[1]);}
    static inline bool Invalid(const cv::Vec3f & pt) {return _isnan(pt[0]) || _isnan(pt[1]) || _isnan(pt[2]);}

    int BinaryToGray(int value);
    inline int BinaryToGray(int value, unsigned offset);
    inline int GrayToBinary(int value, unsigned offset);

    cv::Mat ColorizePattern(const cv::Mat & patternImage, unsigned set, float maxValue);
};

} // namespace smcp
