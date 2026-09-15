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

#include <iostream>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

namespace smcp
{

namespace StructuredLight
{
    const float Pixel_Uncertain = std::numeric_limits<float>::quiet_NaN();
    const unsigned short Bit_Uncertain = 0xffff;
};

bool StructuredLight::DecodePattern(const std::vector<std::string> & images, cv::Mat & patternImage, cv::Mat & minMaxImage, cv::Size const& projectorSize, unsigned flags, const cv::Mat & directLight, unsigned m)
{
    bool binary   = (flags & GrayPatternDecode)!=GrayPatternDecode;
    bool robust   = (flags & RobustDecode)==RobustDecode;

    std::cout << " --- DecodePattern START ---\n";

    //delete previous data
    patternImage = cv::Mat();
    minMaxImage = cv::Mat();
    bool init = true;

    std::cout << "Decode: " << (binary?"Binary ":"Gray ")
                            << (robust?"Robust ":"") 
                            << std::endl;

    int totalImages = static_cast<int>(images.size());
    int totalPatterns = totalImages/2 - 1;
    int totalBits = totalPatterns/2;
    if (2+4*totalBits!=totalImages)
    {   //error
        std::cout << "[StructuredLight::DecodePattern] ERROR: cannot detect pattern and bit count from image set.\n";
        return false;
    }

    const unsigned bitCount[] = {0, static_cast<unsigned>(totalBits), static_cast<unsigned>(totalBits)};  //pattern bits
    const unsigned setSize[]  = {1, static_cast<unsigned>(totalBits), static_cast<unsigned>(totalBits)};  //number of image pairs
    const unsigned Count = 2*(setSize[0]+setSize[1]+setSize[2]); //total image count
    const int patternOffset[2] = {((1<<totalBits)-projectorSize.width)/2, ((1<<totalBits)-projectorSize.height)/2};

    if (images.size()<Count)
    {   //error
        std::cout << "Image list size does not match set size, please supply exactly " << Count << " image names.\n";
        return false;
    }

    //load every image pair and compute the maximum, minimum, and bit code
    unsigned set = 0;
    unsigned current = 0;
    for (unsigned t=0; t<Count; t+=2, current++)
    {
        if (current==setSize[set])
        {
            set++;
            current = 0;
        }

        if (set==0)
        {   //skip
            continue;
        }

        unsigned bit = bitCount[set] - current - 1; //current bit: from 0 to (bitCount[set]-1)
        unsigned channel = set - 1;

        //load images
        const cv::Mat & grayImage1 = GetGrayImage(images.at(t+0));
        if (grayImage1.rows<1)
        {
            std::cout << "Failed to load " << images.at(t+0) << std::endl;
            return false;
        }
        const cv::Mat & grayImage2 = GetGrayImage(images.at(t+1));
        if (grayImage2.rows<1)
        {
            std::cout << "Failed to load " << images.at(t+1) << std::endl;
            return false;
        }

        //initialize data structures
        if (init)
        {
            //sanity check
            if (grayImage1.size()!=grayImage2.size())
            {   //different size
                std::cout << " --> Initial images have different size: \n";
                return false;
            }
            if (robust && grayImage1.size()!=directLight.size())
            {   //different size
                std::cout << " --> Direct Component image has different size: \n";
                return false;
            }
            patternImage = cv::Mat(grayImage1.size(), CV_32FC2);
            minMaxImage = cv::Mat(grayImage1.size(), CV_8UC2);
        }

        //sanity check
        if (grayImage1.size()!=patternImage.size())
        {   //different size
            std::cout << " --> Image 1 has different size, image pair " << t << " (skipped!)\n";
            continue;
        }
        if (grayImage2.size()!=patternImage.size())
        {   //different size
            std::cout << " --> Image 2 has different size, image pair " << t << " (skipped!)\n";
            continue;
        }

        //compare
        for (int h=0; h<patternImage.rows; h++)
        {
            const unsigned char * row1 = grayImage1.ptr<unsigned char>(h);
            const unsigned char * row2 = grayImage2.ptr<unsigned char>(h);
            const cv::Vec2b * rowLight = (robust ? directLight.ptr<cv::Vec2b>(h) : NULL);
            cv::Vec2f * patternRow = patternImage.ptr<cv::Vec2f>(h);
            cv::Vec2b * minMaxRow = minMaxImage.ptr<cv::Vec2b>(h);

            for (int w=0; w<patternImage.cols; w++)
            {
                cv::Vec2f & pattern = patternRow[w];
                cv::Vec2b & minMax = minMaxRow[w];
                unsigned char value1 = row1[w];
                unsigned char value2 = row2[w];

                if (init)
                {
                    pattern[0] = 0.f; //vertical
                    pattern[1] = 0.f; //horizontal
                }

                //min/max
                if (init || value1<minMax[0] || value2<minMax[0])
                {
                    minMax[0] = (value1<value2?value1:value2);
                }
                if (init || value1>minMax[1] || value2>minMax[1])
                {
                    minMax[1] = (value1>value2?value1:value2);
                }
                
                if (!robust)
                {   // [simple] pattern bit assignment
                    if (value1>value2)
                    {   //set bit n to 1
                        pattern[channel] += (1<<bit);
                    }
                }
                else
                {   // [robust] pattern bit assignment
                    if (rowLight && (init || pattern[channel]!=Pixel_Uncertain))
                    {
                        const cv::Vec2b & L = rowLight[w];
                        unsigned short p = GetRobustBit(value1, value2, L[0], L[1], m);
                        if (p==Bit_Uncertain)
                        {
                            pattern[channel] = Pixel_Uncertain;
                        }
                        else
                        {
                            pattern[channel] += (p<<bit);
                        }
                    }
                }

            }   //for each column
        }   //for each row

        init = false;
    }   //for all image pairs

    if (!binary)
    {   //not binary... it must be gray code
        ConvertPattern(patternImage, projectorSize, patternOffset, binary);
    }

    std::cout << " --- DecodePattern END ---\n";

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

void StructuredLight::ConvertPattern(cv::Mat & patternImage, cv::Size const& projectorSize, const int offset[2], bool binary)
{
    if (patternImage.rows==0)
    {   //no pattern image
        return;
    }
    if (patternImage.type()!=CV_32FC2)
    {
        return;
    }

    if (binary)
    {
       std::cout << "Converting binary code to gray\n";
    }
    else
    {
        std::cout << "Converting gray code to binary\n";
    }

    for (int h=0; h<patternImage.rows; h++)
    {
        cv::Vec2f * patternRow = patternImage.ptr<cv::Vec2f>(h);
        for (int w=0; w<patternImage.cols; w++)
        {
            cv::Vec2f & pattern = patternRow[w];
            if (binary)
            {
                if (!Invalid(pattern[0]))
                {
                    int p = static_cast<int>(pattern[0]);
                    pattern[0] = BinaryToGray(p, offset[0]) + (pattern[0] - p);
                }
                if (!Invalid(pattern[1]))
                {
                    int p = static_cast<int>(pattern[1]);
                    pattern[1] = BinaryToGray(p, offset[1]) + (pattern[1] - p);
                }
            }
            else
            {
                if (!Invalid(pattern[0]))
                {
                    int p = static_cast<int>(pattern[0]);
                    int code = GrayToBinary(p, offset[0]);

                    if (code<0) {code = 0;}
                    else if (code>=projectorSize.width) {code = projectorSize.width - 1;}

                    pattern[0] = code + (pattern[0] - p);
                }
                if (!Invalid(pattern[1]))
                {
                    int p = static_cast<int>(pattern[1]);
                    int code = GrayToBinary(p, offset[1]);

                    if (code<0) {code = 0;}
                    else if (code>=projectorSize.height) {code = projectorSize.height - 1;}

                    pattern[1] = code + (pattern[1] - p);
                }
            }
        }
    }
}

cv::Mat StructuredLight::EstimateDirectLight(const std::vector<cv::Mat> & images, float b)
{
    static const unsigned Count = 10; // max number of images

    unsigned count = static_cast<int>(images.size());
    if (count<1)
    {   //no images
        return cv::Mat();
    }
    
    std::cout << " --- EstimateDirectLight START ---\n";

    if (count>Count)
    {
        count = Count;
        std::cout << "WARNING: Using only " << Count << " of " << count << std::endl;
    }

    for (unsigned i=0; i<count; i++)
    {
        if (images.at(i).type()!=CV_8UC1)
        {   //error
            std::cout << "Gray images required\n";
            return cv::Mat();
        }
    }

    cv::Size size = images.at(0).size();

    //initialize direct light image
    cv::Mat directLight(size, CV_8UC2);

    double b1 = 1.0/(1.0 - b);
    double b2 = 2.0/(1.0 - b*1.0*b);

    for (unsigned h=0; static_cast<int>(h)<size.height; h++)
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

            //std::cout << "Ld=" << (int)rowLight[w][0] << " iTotal=" <<(int) rowLight[w][1] << std::endl;
        }
    }

    std::cout << " --- EstimateDirectLight END ---\n";

    return directLight;
}

cv::Mat StructuredLight::GetGrayImage(const std::string & filename)
{
    //load image
    cv::Mat rgbImage = cv::imread(filename);
    if (rgbImage.rows>0 && rgbImage.cols>0)
    {
        //gray scale
        cv::Mat grayImage;
        cvtColor(rgbImage, grayImage, CV_BGR2GRAY);
        return grayImage;
    }
    return cv::Mat();
}

/*      From Wikipedia: http://en.wikipedia.org/wiki/Gray_code
        The purpose of this function is to convert an unsigned
        binary number to reflected binary Gray code.
*/
static unsigned UtilBinaryToGray(unsigned num)
{
        return (num>>1) ^ num;
}
 
/*      From Wikipedia: http://en.wikipedia.org/wiki/Gray_code
        The purpose of this function is to convert a reflected binary
        Gray code number to a binary number.
*/
static unsigned UtilGrayToBinary(unsigned num, unsigned numBits)
{
    for (unsigned shift = 1; shift < numBits; shift <<= 1)
    {
        num ^= num >> shift;
    }
    return num;
}

int StructuredLight::BinaryToGray(int value) {return UtilBinaryToGray(value);}

inline int StructuredLight::BinaryToGray(int value, unsigned offset) {return UtilBinaryToGray(value + offset);}
inline int StructuredLight::GrayToBinary(int value, unsigned offset) {return (UtilGrayToBinary(value, 32) - offset);}

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

    float maxT = maxValue;
    float n = 4.f;
    float dt = 255.f/n;
    for (int h=0; h<patternImage.rows; h++)
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
    return image;
}

} // namespace smcp
