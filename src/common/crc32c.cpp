#include "daq/crc32c.hpp"

#include <array>
#include <cstring>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace daq {
namespace {

constexpr uint32_t POLY=0x82F63B78u;  //castagnoli poly, reflected

//build the 256 entry table at compile time
constexpr std::array<uint32_t, 256> makeTable() {
    std::array<uint32_t, 256> t{};
    for(uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int k=0;k<8;k++){
            c=(c&1) ? (c>>1)^POLY : (c>>1);
        }
        t[i]=c;
    }
    return t;
}
constexpr auto TABLE=makeTable();

#if defined(__x86_64__)
/*
8 bytes at a time w/ the crc32 instruction
leftover bytes one by one
*/
__attribute__((target("sse4.2"))) uint32_t crc32cHw(const void* data, size_t len, uint32_t crc) {
    auto p=static_cast<const unsigned char*>(data);
    uint64_t c=~crc;
    while(len>=8){
        uint64_t w;
        std::memcpy(&w,p,8);
        c=_mm_crc32_u64(c,w);
        p+=8;
        len-=8;
    }
    auto c32=static_cast<uint32_t>(c);
    while(len--){
        c32=_mm_crc32_u8(c32,*p++);
    }
    return ~c32;
}

bool haveSse42() {
    static const bool v=__builtin_cpu_supports("sse4.2");
    return v;
}
#endif

} // namespace

uint32_t crc32cSw(const void* data, size_t len, uint32_t crc) {
    auto p=static_cast<const unsigned char*>(data);
    uint32_t c=~crc;
    while(len--){
        c=TABLE[(c^*p++)&0xFF]^(c>>8);
    }
    return ~c;
}

uint32_t crc32c(const void* data, size_t len, uint32_t crc) {
#if defined(__x86_64__)
    if(haveSse42()) return crc32cHw(data,len,crc);
#endif
    return crc32cSw(data,len,crc);
}

} // namespace daq
