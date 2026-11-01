#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace Engine::Serialization
{
inline std::string EncodeHex(const std::vector<uint8_t>& bytes)
{
    static constexpr char digits[]="0123456789abcdef";
    std::string result(bytes.size()*2,'0');
    for(size_t i=0;i<bytes.size();++i) { result[i*2]=digits[bytes[i]>>4]; result[i*2+1]=digits[bytes[i]&15]; }
    return result;
}
inline bool DecodeHex(const std::string& text,std::vector<uint8_t>& bytes)
{
    if(text.size()%2) return false;
    const auto digit=[](char c)->int { if(c>='0' && c<='9') return c-'0'; if(c>='a' && c<='f') return c-'a'+10; if(c>='A' && c<='F') return c-'A'+10; return -1; };
    bytes.resize(text.size()/2);
    for(size_t i=0;i<bytes.size();++i)
    { const auto a=digit(text[i*2]),b=digit(text[i*2+1]); if(a<0 || b<0) { bytes.clear(); return false; } bytes[i]=uint8_t(a*16+b); }
    return true;
}
}
