#pragma once
#include <filesystem>
#include <fstream>
#include <cmath>
#include <vector>
#include <cstdint>

inline void WriteStreamingFixture(const std::filesystem::path& path, int color, unsigned size=512)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path,std::ios::binary);
    unsigned char header[18]{}; header[2]=2;
    header[12]=size&255;header[13]=size>>8;header[14]=size&255;header[15]=size>>8;
    header[16]=32;header[17]=0x28;
    file.write(reinterpret_cast<char*>(header),18);
    for(unsigned y=0;y<size;++y) for(unsigned x=0;x<size;++x)
    {
        const float u=(x+.5f)/size*2-1,v=(y+.5f)/size*2-1;
        const bool inside=u*u/.7f+v*v/.9f<1.f;
        const unsigned char bright=((x/16+y/16)&1)?220:160;
        unsigned char pixel[4]={32,32,32,static_cast<unsigned char>(inside?255:0)};
        pixel[2-color]=bright;
        file.write(reinterpret_cast<char*>(pixel),4);
    }
}

inline void WriteStreamingCapture(const std::filesystem::path& stem,const std::vector<uint8_t>& pixels,unsigned size)
{
    std::ofstream image(stem.string()+".ppm",std::ios::binary),mask(stem.string()+".pgm",std::ios::binary);
    image<<"P6\n"<<size<<' '<<size<<"\n255\n";mask<<"P5\n"<<size<<' '<<size<<"\n255\n";
    for(size_t pixel=0;pixel<pixels.size();pixel+=4)
    {
        image.write(reinterpret_cast<const char*>(pixels.data()+pixel),3);
        mask.put(pixels[pixel]>5 || pixels[pixel+1]>5 || pixels[pixel+2]>5 ? char(255):char(0));
    }
}
