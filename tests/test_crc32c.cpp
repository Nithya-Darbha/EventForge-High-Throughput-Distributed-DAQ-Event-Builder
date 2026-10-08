#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "daq/crc32c.hpp"

using namespace daq;

//standard check value for crc32c("123456789")
TEST(Crc32c, KnownVector) {
    const char* s="123456789";
    EXPECT_EQ(crc32c(s,9),0xE3069283u);
    EXPECT_EQ(crc32cSw(s,9),0xE3069283u);
    EXPECT_EQ(crc32c(nullptr,0),0u);
}

//odd lengths on purpose so the 8 byte loop + tail both get used
TEST(Crc32c, HardwareMatchesSoftware) {
    std::mt19937 rng(1);
    for(size_t len:{1u,7u,8u,9u,63u,64u,4095u,4096u,65537u}){
        std::vector<unsigned char> v(len);
        for(auto& c:v) c=static_cast<unsigned char>(rng());
        EXPECT_EQ(crc32c(v.data(),len),crc32cSw(v.data(),len)) << "len=" << len;
    }
}

TEST(Crc32c, Incremental) {
    const char* s="123456789";
    EXPECT_EQ(crc32c(s+4,5,crc32c(s,4)),0xE3069283u);
}

TEST(Crc32c, DetectsBitFlip) {
    std::vector<unsigned char> v(4096,0x5A);
    auto c=crc32c(v.data(),v.size());
    v[2000]^=0x10;
    EXPECT_NE(crc32c(v.data(),v.size()),c);
}
