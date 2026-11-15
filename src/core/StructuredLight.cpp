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


#include "core/StructuredLight.h"

#include <filesystem>
#include <fstream>
#include <limits>

#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "core/Parallel.h"

namespace smcp
{

namespace StructuredLight
{
    const float Pixel_Uncertain = std::numeric_limits<float>::quiet_NaN();
    const unsigned short Bit_Uncertain = 0xffff;
};

namespace
{
    /// Rows handed to one thread at least; below this a thread costs more than it saves.
    constexpr long long Min_Rows_Per_Thread = 32;

    /// Images of the direct/global light estimation: the 4 highest-frequency-but-four patterns of each axis.
    constexpr int Direct_Light_Count = 4;
    constexpr int Direct_Light_Offset = 4;

    /* From Wikipedia: http://en.wikipedia.org/wiki/Gray_code */
    inline unsigned UtilBinaryToGray(unsigned num)
    {
        return (num>>1) ^ num;
    }

    inline unsigned UtilGrayToBinary(unsigned num, unsigned numBits)
    {
        for (unsigned shift = 1; shift < numBits; shift <<= 1)
        {
            num ^= num >> shift;
        }
        return num;
    }

    inline int GrayToBinary(int value, unsigned offset) {return (UtilGrayToBinary(value, 32) - offset);}

    /// Whole file in memory, opened through a UTF-8 path (cv::imread only takes ANSI paths on Windows).
    bool ReadFile(const std::string & filename, std::vector<unsigned char> & data)
    {
        std::ifstream in(std::filesystem::u8path(filename), std::ios::binary | std::ios::ate);
        if (!in.is_open())
        {
            return false;
        }
        const std::streamoff size = in.tellg();
        if (size <= 0)
        {
            return false;
        }
        data.resize(static_cast<size_t>(size));
        in.seekg(0, std::ios::beg);
        return static_cast<bool>(in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size())));
    }

    /// One image pair of the sequence: which projector axis and bit it encodes.
    struct BitPair
    {
        const cv::Mat * image1;
        const cv::Mat * image2;
        unsigned channel;
        unsigned bit;
    };
}

cv::Mat StructuredLight::GetColorImage(const std::string & filename)
{
    try
    {
        std::vector<unsigned char> data;
        if (!ReadFile(filename, data))
        {
            return cv::Mat();
        }
        cv::Mat rgbImage = cv::imdecode(cv::Mat(data), CV_LOAD_IMAGE_COLOR);
        if (rgbImage.rows>0 && rgbImage.cols>0)
        {
            return rgbImage;
        }
    }
    catch (const std::exception &)
    {   //unreadable path or corrupt file: reported as "failed to load" by the caller
    }
    return cv::Mat();
}

cv::Mat StructuredLight::GetGrayImage(const std::string & filename)
{
    cv::Mat rgbImage = GetColorImage(filename);
    if (rgbImage.rows>0 && rgbImage.cols>0)
    {
        cv::Mat grayImage;
        cv::cvtColor(rgbImage, grayImage, CV_BGR2GRAY);
        return grayImage;
    }
    return cv::Mat();
}

bool StructuredLight::DecodeGraySet(const std::vector<std::string> & images, cv::Size const& projectorSize, const DecodeParams & params,
                                    DecodedSet & decoded, std::string & error, ProgressSink * progress)
{
    decoded = DecodedSet();
    error.clear();

    const int totalImages = static_cast<int>(images.size());
    const int totalPatterns = totalImages/2 - 1;
    if (totalPatterns < Direct_Light_Count + Direct_Light_Offset)
    {
        error = "ERROR: too few pattern images";
        return false;
    }
    const int totalBits = totalPatterns/2;
    if (2+4*totalBits!=totalImages)
    {
        error = "ERROR: cannot detect pattern and bit count from image set";
        return false;
    }

    //images used to estimate the direct component (0-based file indices)
    std::vector<int> directIndices;
    for (int i = 0; i < Direct_Light_Count; i++)
    {
        const int index = totalImages - totalPatterns - Direct_Light_Count - Direct_Light_Offset + i;
        directIndices.push_back(index);
        directIndices.push_back(index + totalPatterns);
    }

    for (int index : directIndices)
    {
        if (index < 2 || index >= totalImages)
        {
            error = "ERROR: too few pattern images";
            return false;
        }
    }

    if (progress)
    {
        progress->Status("Decoding: loading images...");
    }
    std::vector<cv::Mat> gray;
    if (!LoadGraySet(images, gray, error, progress))
    {
        return false;
    }

    //estimate direct component
    if (progress)
    {
        progress->Status("Decoding: estimating direct and global light components...");
    }
    std::vector<cv::Mat> directImages;
    for (int index : directIndices)
    {
        if (gray[index].size() != gray[directIndices.front()].size())
        {
            error = "ERROR: images of different size in the set";
            return false;
        }
        directImages.push_back(gray[index]);
    }
    cv::Mat directLight = EstimateDirectLight(directImages, params.b);
    if (progress)
    {
        progress->Message("Estimate direct and global light components... done.");
        progress->Status("Decoding: projector column and row values...");
        progress->Message("Decoding, please wait...");
    }

    return DecodePattern(gray, decoded.pattern, decoded.minMax, projectorSize, RobustDecode | GrayPatternDecode, directLight, params.m, error);
}

bool StructuredLight::LoadGraySet(const std::vector<std::string> & images, std::vector<cv::Mat> & grayImages, std::string & error,
                                  ProgressSink * progress)
{
    //each image exactly once; entries 0 and 1 (all white/all black) are not needed to decode
    const int totalImages = static_cast<int>(images.size());
    grayImages.assign(totalImages, cv::Mat());
    Parallel::ParallelFor(std::max(0, totalImages - 2), 1, [&](long long first, long long last) {
        for (long long i = first; i < last; i++)
        {
            if (progress && progress->Canceled())
            {
                return;
            }
            grayImages[i + 2] = GetGrayImage(images[i + 2]);
        }
    });
    if (progress && progress->Canceled())
    {
        error = "Decode canceled";
        return false;
    }
    for (int index = 2; index < totalImages; index++)
    {
        if (grayImages[index].rows < 1)
        {
            error = "ERROR: failed to load " + images[index];
            return false;
        }
    }
    return true;
}

namespace
{
    /// One line of the camera image (a row for the projector columns, a column for the projector
    /// rows) seen by RefineLine: `step` is the distance in elements between consecutive pixels.
    struct RefineAxis
    {
        int channel;        ///< 0: projector column along camera rows, 1: projector row along camera columns.
        int length;         ///< Pixels in one line.
        int offset;         ///< Offset of the Gray code of this axis (see DecodePattern).
        int totalBits;
        bool intensities;   ///< false: no images, borders go halfway between pixels.
    };

    /// Index of the image pair whose bit differs between the projector coordinates `a` and `a+1`.
    inline int BorderPair(int a, const RefineAxis & axis)
    {
        const unsigned n = static_cast<unsigned>(a + axis.offset);
        unsigned changed = (n ^ (n >> 1)) ^ ((n + 1) ^ ((n + 1) >> 1));  //exactly one bit
        int bit = 0;
        while (changed > 1)
        {
            changed >>= 1;
            bit++;
        }
        return (axis.totalBits - bit - 1) + 1 + axis.channel*axis.totalBits;
    }

    /// Fills `refined[i]` for one line. `code[i]` is the decoded coordinate, or NaN when the pixel
    /// cannot be used; `difference(pair, i)` is the stripe image minus its inverse at pixel i.
    template <typename Difference>
    void RefineLine(const RefineAxis & axis, const float * code, float * refined, std::vector<float> & border,
                    std::vector<float> & slope, const Difference & difference)
    {
        const int length = axis.length;
        const float nan = std::numeric_limits<float>::quiet_NaN();

        //border[i]: position of the border between pixels i and i+1, when they are consecutive codes
        for (int i = 0; i + 1 < length; i++)
        {
            border[i] = nan;
            const float a = code[i], b = code[i + 1];
            if (std::isnan(a) || std::isnan(b) || std::abs(b - a) != 1.f)
            {
                continue;
            }
            float t = 0.5f;
            if (axis.intensities)
            {
                const int pair = BorderPair(static_cast<int>(std::min(a, b)), axis);
                const float d0 = static_cast<float>(difference(pair, i));
                const float d1 = static_cast<float>(difference(pair, i + 1));
                if ((d0 < 0.f && d1 > 0.f) || (d0 > 0.f && d1 < 0.f))
                {
                    t = d0/(d0 - d1);
                }
            }
            border[i] = static_cast<float>(i) + t;
        }
        if (length > 0)
        {
            border[length - 1] = nan;
        }

        //runs of one code: interpolate between their two borders
        for (int start = 0; start < length; )
        {
            refined[start] = nan;
            slope[start] = nan;
            if (std::isnan(code[start]))
            {
                start++;
                continue;
            }
            int end = start;
            while (end + 1 < length && code[end + 1] == code[start])
            {
                end++;
            }
            const bool left = start > 0 && !std::isnan(border[start - 1]);
            const bool right = !std::isnan(border[end]);
            float runSlope = nan;
            if (left && right)
            {
                const float x0 = border[start - 1], x1 = border[end];
                const float u0 = 0.5f*(code[start - 1] + code[start]), u1 = 0.5f*(code[end] + code[end + 1]);
                if (u0 != u1 && x1 > x0)
                {   //the coordinate goes on through this pixel (not a fold of the surface)
                    runSlope = (u1 - u0)/(x1 - x0);
                    for (int i = start; i <= end; i++)
                    {
                        refined[i] = u0 + (static_cast<float>(i) - x0)*runSlope;
                    }
                }
            }
            for (int i = start; i <= end; i++)
            {
                slope[i] = runSlope;
                if (std::isnan(runSlope))
                {
                    refined[i] = nan;
                }
            }
            start = end + 1;
        }

        //runs with one border: continue the slope of the complete run on the other side of it
        for (int start = 0; start < length; )
        {
            if (std::isnan(code[start]) || !std::isnan(slope[start]))
            {
                start++;
                continue;
            }
            int end = start;
            while (end + 1 < length && code[end + 1] == code[start])
            {
                end++;
            }
            const bool left = start > 0 && !std::isnan(border[start - 1]) && !std::isnan(slope[start - 1]);
            const bool right = !std::isnan(border[end]) && end + 1 < length && !std::isnan(slope[end + 1]);
            if (left != right)
            {
                const float x = left ? border[start - 1] : border[end];
                const float u = left ? 0.5f*(code[start - 1] + code[start]) : 0.5f*(code[end] + code[end + 1]);
                const float neighbourSlope = left ? slope[start - 1] : slope[end + 1];
                for (int i = start; i <= end; i++)
                {
                    const float value = u + (static_cast<float>(i) - x)*neighbourSlope;
                    //only while the value stays inside the projector pixel that was decoded
                    refined[i] = std::abs(value - code[i]) <= 0.5f ? value : nan;
                }
            }
            start = end + 1;
        }
    }
}

cv::Mat StructuredLight::RefinePattern(const std::vector<cv::Mat> & grayImages, const cv::Mat & patternImage, const cv::Mat & minMaxImage,
                                        cv::Size const& projectorSize, int threshold, ProgressSink * progress)
{
    if (!patternImage.data || patternImage.type()!=CV_32FC2 || !minMaxImage.data || minMaxImage.type()!=CV_8UC2
        || minMaxImage.size()!=patternImage.size())
    {
        return cv::Mat();
    }
    const int rows = patternImage.rows, cols = patternImage.cols;

    //bits per axis, as DecodePattern derives them from the image count; without images, from the projector
    int totalBits = 0;
    bool intensities = !grayImages.empty();
    if (intensities)
    {
        const int totalImages = static_cast<int>(grayImages.size());
        totalBits = (totalImages/2 - 1)/2;
        if (totalBits < 1 || 2 + 4*totalBits != totalImages)
        {
            return cv::Mat();
        }
        for (int i = 2; i < totalImages; i++)
        {
            if (grayImages[i].type()!=CV_8UC1 || grayImages[i].size()!=patternImage.size())
            {
                return cv::Mat();
            }
        }
    }
    else
    {
        while ((1<<totalBits) < std::max(projectorSize.width, projectorSize.height))
        {
            totalBits++;
        }
    }
    const int offset[2] = {((1<<totalBits)-projectorSize.width)/2, ((1<<totalBits)-projectorSize.height)/2};
    const float limit[2] = {static_cast<float>(projectorSize.width), static_cast<float>(projectorSize.height)};

    cv::Mat refined(rows, cols, CV_32FC2);

    //usable decoded coordinate of one pixel, or NaN
    const auto usable = [&](int h, int w, int channel) -> float
    {
        const cv::Vec2b & minMax = minMaxImage.at<cv::Vec2b>(h, w);
        const float value = patternImage.at<cv::Vec2f>(h, w)[channel];
        if (std::isnan(value) || (minMax[1] - minMax[0]) < threshold || value < 0.f || value >= limit[channel])
        {
            return std::numeric_limits<float>::quiet_NaN();
        }
        return value;
    };

    if (progress)
    {
        progress->Status("Reconstruction in progress: locating stripe borders");
    }

    //projector columns, along camera rows
    Parallel::ParallelFor(rows, Min_Rows_Per_Thread, [&](long long firstRow, long long lastRow) {
        const RefineAxis axis = {0, cols, offset[0], totalBits, intensities};
        std::vector<float> code(cols), value(cols), border(cols), slope(cols);
        for (int h = static_cast<int>(firstRow); h < static_cast<int>(lastRow); h++)
        {
            if (progress && progress->Canceled())
            {
                return;
            }
            for (int w = 0; w < cols; w++)
            {
                code[w] = usable(h, w, 0);
            }
            RefineLine(axis, code.data(), value.data(), border, slope, [&](int pair, int w) {
                return static_cast<int>(grayImages[2*pair].at<unsigned char>(h, w))
                     - static_cast<int>(grayImages[2*pair + 1].at<unsigned char>(h, w));
            });
            cv::Vec2f * row = refined.ptr<cv::Vec2f>(h);
            for (int w = 0; w < cols; w++)
            {
                row[w][0] = value[w];
            }
        }
    });

    //projector rows, along camera columns
    Parallel::ParallelFor(cols, Min_Rows_Per_Thread, [&](long long firstCol, long long lastCol) {
        const RefineAxis axis = {1, rows, offset[1], totalBits, intensities};
        std::vector<float> code(rows), value(rows), border(rows), slope(rows);
        for (int w = static_cast<int>(firstCol); w < static_cast<int>(lastCol); w++)
        {
            if (progress && progress->Canceled())
            {
                return;
            }
            for (int h = 0; h < rows; h++)
            {
                code[h] = usable(h, w, 1);
            }
            RefineLine(axis, code.data(), value.data(), border, slope, [&](int pair, int h) {
                return static_cast<int>(grayImages[2*pair].at<unsigned char>(h, w))
                     - static_cast<int>(grayImages[2*pair + 1].at<unsigned char>(h, w));
            });
            for (int h = 0; h < rows; h++)
            {
                refined.at<cv::Vec2f>(h, w)[1] = value[h];
            }
        }
    });

    if (progress && progress->Canceled())
    {
        return cv::Mat();
    }
    return refined;
}

bool StructuredLight::DecodePattern(const std::vector<cv::Mat> & images, cv::Mat & patternImage, cv::Mat & minMaxImage, cv::Size const& projectorSize, unsigned flags, const cv::Mat & directLight, unsigned m, std::string & error)
{
    const bool binary   = (flags & GrayPatternDecode)!=GrayPatternDecode;
    const bool robust   = (flags & RobustDecode)==RobustDecode;

    patternImage = cv::Mat();
    minMaxImage = cv::Mat();

    const int totalImages = static_cast<int>(images.size());
    const int totalPatterns = totalImages/2 - 1;
    const int totalBits = totalPatterns/2;
    if (totalBits < 1 || 2+4*totalBits!=totalImages)
    {
        error = "ERROR: cannot detect pattern and bit count from image set";
        return false;
    }
    if (totalBits > 23)
    {   //codes are accumulated as float in the result image
        error = "ERROR: too many pattern bits";
        return false;
    }

    const int patternOffset[2] = {((1<<totalBits)-projectorSize.width)/2, ((1<<totalBits)-projectorSize.height)/2};
    const int projectorLimit[2] = {projectorSize.width, projectorSize.height};

    //pairs in capture order: pair 0 is skipped, then `totalBits` pairs per axis, most significant bit first
    std::vector<BitPair> pairs;
    cv::Size size(0, 0);
    for (int pair = 1; pair <= 2*totalBits; pair++)
    {
        const unsigned channel = (pair <= totalBits ? 0 : 1);
        const unsigned current = static_cast<unsigned>(channel==0 ? pair - 1 : pair - 1 - totalBits);
        const cv::Mat & grayImage1 = images.at(2*pair+0);
        const cv::Mat & grayImage2 = images.at(2*pair+1);
        if (grayImage1.rows<1 || grayImage2.rows<1 || grayImage1.type()!=CV_8UC1 || grayImage2.type()!=CV_8UC1)
        {
            error = "ERROR: failed to load image pair " + std::to_string(2*pair);
            return false;
        }

        if (pairs.empty())
        {   //the first pair defines the image size
            if (grayImage1.size()!=grayImage2.size())
            {
                error = "ERROR: initial images have different size";
                return false;
            }
            if (robust && grayImage1.size()!=directLight.size())
            {
                error = "ERROR: direct component image has different size";
                return false;
            }
            size = grayImage1.size();
        }
        if (grayImage1.size()!=size || grayImage2.size()!=size)
        {   //different size: this pair is ignored, as it always was
            continue;
        }

        BitPair item;
        item.image1 = &grayImage1;
        item.image2 = &grayImage2;
        item.channel = channel;
        item.bit = static_cast<unsigned>(totalBits) - current - 1;
        pairs.push_back(item);
    }

    patternImage = cv::Mat(size, CV_32FC2);
    minMaxImage = cv::Mat(size, CV_8UC2);
    const int cols = size.width;

    Parallel::ParallelFor(size.height, Min_Rows_Per_Thread, [&](long long firstRow, long long lastRow) {
        //per row: integer code and "uncertain" flag of both axes, and the intensity range
        std::vector<int> code(static_cast<size_t>(cols)*2);
        std::vector<unsigned char> uncertain(static_cast<size_t>(cols)*2);
        std::vector<unsigned char> minValue(cols), maxValue(cols);

        for (int h = static_cast<int>(firstRow); h < static_cast<int>(lastRow); h++)
        {
            std::fill(code.begin(), code.end(), 0);
            std::fill(uncertain.begin(), uncertain.end(), static_cast<unsigned char>(0));
            std::fill(minValue.begin(), minValue.end(), static_cast<unsigned char>(255));
            std::fill(maxValue.begin(), maxValue.end(), static_cast<unsigned char>(0));

            const cv::Vec2b * rowLight = (robust ? directLight.ptr<cv::Vec2b>(h) : NULL);

            for (const BitPair & pair : pairs)
            {
                const unsigned char * row1 = pair.image1->ptr<unsigned char>(h);
                const unsigned char * row2 = pair.image2->ptr<unsigned char>(h);
                int * codeRow = code.data() + static_cast<size_t>(pair.channel)*cols;
                unsigned char * uncertainRow = uncertain.data() + static_cast<size_t>(pair.channel)*cols;
                const unsigned bit = pair.bit;

                for (int w = 0; w < cols; w++)
                {
                    const unsigned char value1 = row1[w];
                    const unsigned char value2 = row2[w];

                    const unsigned char low = (value1<value2?value1:value2);
                    const unsigned char high = (value1>value2?value1:value2);
                    if (low<minValue[w]) {minValue[w] = low;}
                    if (high>maxValue[w]) {maxValue[w] = high;}

                    if (!robust)
                    {   // [simple] pattern bit assignment
                        if (value1>value2)
                        {
                            codeRow[w] += (1<<bit);
                        }
                    }
                    else
                    {   // [robust] pattern bit assignment
                        const cv::Vec2b & L = rowLight[w];
                        const unsigned short p = GetRobustBit(value1, value2, L[0], L[1], m);
                        if (p==Bit_Uncertain)
                        {
                            uncertainRow[w] = 1;
                        }
                        else
                        {
                            codeRow[w] += (p<<bit);
                        }
                    }
                }
            }

            cv::Vec2f * patternRow = patternImage.ptr<cv::Vec2f>(h);
            cv::Vec2b * minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);
            for (int w = 0; w < cols; w++)
            {
                minMaxRow[w][0] = minValue[w];
                minMaxRow[w][1] = maxValue[w];
                for (int channel = 0; channel < 2; channel++)
                {
                    if (uncertain[static_cast<size_t>(channel)*cols + w])
                    {
                        patternRow[w][channel] = Pixel_Uncertain;
                        continue;
                    }
                    int value = code[static_cast<size_t>(channel)*cols + w];
                    if (!binary)
                    {   //gray code to binary, clamped to the projector
                        value = GrayToBinary(value, patternOffset[channel]);
                        if (value<0) {value = 0;}
                        else if (value>=projectorLimit[channel]) {value = projectorLimit[channel] - 1;}
                    }
                    patternRow[w][channel] = static_cast<float>(value);
                }
            }
        }
    });

    return true;
}

unsigned short StructuredLight::GetRobustBit(unsigned value1, unsigned value2, unsigned Ld, unsigned Lg, unsigned m)
{
    if (Ld < m)
    {
        return Bit_Uncertain;
    }
    if (Ld>Lg)
    {
        return (value1>value2 ? 1 : 0);
    }
    if (value1<=Ld && value2>=Lg)
    {
        return 0;
    }
    if (value1>=Lg && value2<=Ld)
    {
        return 1;
    }
    return Bit_Uncertain;
}

cv::Mat StructuredLight::EstimateDirectLight(const std::vector<cv::Mat> & images, float b)
{
    static const unsigned Count = 10; // max number of images

    unsigned count = static_cast<int>(images.size());
    if (count<1)
    {   //no images
        return cv::Mat();
    }
    if (count>Count)
    {
        count = Count;
    }

    for (unsigned i=0; i<count; i++)
    {
        if (images.at(i).rows<1 || images.at(i).type()!=CV_8UC1 || images.at(i).size()!=images.at(0).size())
        {   //error
            return cv::Mat();
        }
    }

    cv::Size size = images.at(0).size();

    //initialize direct light image
    cv::Mat directLight(size, CV_8UC2);

    const double b1 = 1.0/(1.0 - b);
    const double b2 = 2.0/(1.0 - b*1.0*b);

    Parallel::ParallelFor(size.height, Min_Rows_Per_Thread, [&](long long firstRow, long long lastRow) {
        for (unsigned h=static_cast<unsigned>(firstRow); h<static_cast<unsigned>(lastRow); h++)
        {
            unsigned char const* row[Count];
            for (unsigned i=0; i<count; i++)
            {
                row[i] = images.at(i).ptr<unsigned char>(h);
            }
            cv::Vec2b * rowLight = directLight.ptr<cv::Vec2b>(h);

            for (unsigned w=0; static_cast<int>(w)<size.width; w++)
            {
                unsigned Lmax = row[0][w];
                unsigned Lmin = row[0][w];
                for (unsigned i=0; i<count; i++)
                {
                    if (Lmax<row[i][w]) Lmax = row[i][w];
                    if (Lmin>row[i][w]) Lmin = row[i][w];
                }

                int Ld = static_cast<int>(b1*(Lmax - Lmin) + 0.5);
                int Lg = static_cast<int>(b2*(Lmin - b*Lmax) + 0.5);
                rowLight[w][0] = (Lg>0 ? static_cast<unsigned>(Ld) : Lmax);
                rowLight[w][1] = (Lg>0 ? static_cast<unsigned>(Lg) : 0);
            }
        }
    });

    return directLight;
}

int StructuredLight::BinaryToGray(int value) {return UtilBinaryToGray(value);}

cv::Mat StructuredLight::ColorizePattern(const cv::Mat & patternImage, unsigned set, float maxValue)
{
    if (patternImage.rows==0)
    {   //empty image
        return cv::Mat();
    }
    if (patternImage.type()!=CV_32FC2)
    {   //invalid image type
        return cv::Mat();
    }
    if (set!=0 && set!=1)
    {
        return cv::Mat();
    }

    cv::Mat image(patternImage.size(), CV_8UC3);

    const float maxT = maxValue;
    const float n = 4.f;
    const float dt = 255.f/n;
    Parallel::ParallelFor(patternImage.rows, Min_Rows_Per_Thread, [&](long long firstRow, long long lastRow) {
        for (int h=static_cast<int>(firstRow); h<static_cast<int>(lastRow); h++)
        {
            const cv::Vec2f * row1 = patternImage.ptr<cv::Vec2f>(h);
            cv::Vec3b * row2 = image.ptr<cv::Vec3b>(h);
            for (int w=0; w<patternImage.cols; w++)
            {
                if (row1[w][set]>maxValue || Invalid(row1[w][set]))
                {   //invalid value: use grey
                    row2[w] = cv::Vec3b(128, 128, 128);
                    continue;
                }
                //display
                float t = row1[w][set]*255.f/maxT;
                float c1 = 0.f, c2 = 0.f, c3 = 0.f;
                if (t<=1.f*dt)
                {   //black -> red
                    float c = n*(t-0.f*dt);
                    c1 = c;     //0-255
                    c2 = 0.f;   //0
                    c3 = 0.f;   //0
                }
                else if (t<=2.f*dt)
                {   //red -> red,green
                    float c = n*(t-1.f*dt);
                    c1 = 255.f; //255
                    c2 = c;     //0-255
                    c3 = 0.f;   //0
                }
                else if (t<=3.f*dt)
                {   //red,green -> green
                    float c = n*(t-2.f*dt);
                    c1 = 255.f-c;   //255-0
                    c2 = 255.f;     //255
                    c3 = 0.f;       //0
                }
                else if (t<=4.f*dt)
                {   //green -> blue
                    float c = n*(t-3.f*dt);
                    c1 = 0.f;       //0
                    c2 = 255.f-c;   //255-0
                    c3 = c;         //0-255
                }
                row2[w] = cv::Vec3b(static_cast<uchar>(c3), static_cast<uchar>(c2), static_cast<uchar>(c1));
            }
        }
    });
    return image;
}

} // namespace smcp
