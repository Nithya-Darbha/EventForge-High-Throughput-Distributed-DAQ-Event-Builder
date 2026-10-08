#include <gtest/gtest.h>

#include <vector>

#include "daq/payload.hpp"
#include "daq/wire.hpp"

using namespace daq;

//encode into a misaligned buffer on purpose, memcpy based get/put shouldnt care
TEST(Wire, HeaderRoundTripUnaligned) {
    alignas(8) std::byte buf[64];
    std::byte* p=buf+3;
    FragmentHeader fh{};
    fh.eventId=0x0102030405060708ull;
    fh.triggerTs=42;
    fh.sourceId=7;
    fh.seq=99;
    fh.payloadLen=1234;
    fh.payloadCrc=0xDEADBEEF;
    put(p,makeHeader(MsgType::FRAGMENT,sizeof(FragmentHeader)+1234));
    put(p+sizeof(MsgHeader),fh);

    auto h=get<MsgHeader>(p);
    EXPECT_EQ(validate(h),HeaderCheck::OK);
    EXPECT_EQ(h.type,MsgType::FRAGMENT);
    EXPECT_EQ(h.bodyLen,sizeof(FragmentHeader)+1234);
    auto back=get<FragmentHeader>(p+sizeof(MsgHeader));
    EXPECT_EQ(back.eventId,fh.eventId);
    EXPECT_EQ(back.sourceId,7);
    EXPECT_EQ(back.seq,99u);
    EXPECT_EQ(back.payloadCrc,0xDEADBEEFu);
}

//the magic has to read "DAQF" in a hex dump
TEST(Wire, MagicBytesOnTheWire) {
    std::byte b[16];
    put(b,makeHeader(MsgType::FRAGMENT,0));
    EXPECT_EQ(static_cast<char>(b[0]),'D');
    EXPECT_EQ(static_cast<char>(b[1]),'A');
    EXPECT_EQ(static_cast<char>(b[2]),'Q');
    EXPECT_EQ(static_cast<char>(b[3]),'F');
    EXPECT_EQ(static_cast<uint8_t>(b[4]),WIRE_VERSION);
    EXPECT_EQ(static_cast<uint8_t>(b[5]),2);
}

TEST(Wire, ValidateRejectsGarbage) {
    auto h=makeHeader(MsgType::FRAGMENT,10);
    h.magic=0x12345678;
    EXPECT_EQ(validate(h),HeaderCheck::BAD_MAGIC);

    h=makeHeader(MsgType::FRAGMENT,MAX_BODY_LEN+1);
    EXPECT_EQ(validate(h),HeaderCheck::TOO_LONG);

    h=makeHeader(static_cast<MsgType>(99),0);
    EXPECT_EQ(validate(h),HeaderCheck::BAD_TYPE);

    h=makeHeader(MsgType::HELLO,8);
    h.version=2;
    EXPECT_EQ(validate(h),HeaderCheck::BAD_VERSION);

    //all zeros = most common garbage, must never be valid
    MsgHeader zero{};
    EXPECT_NE(validate(zero),HeaderCheck::OK);
}

TEST(Wire, EncodeSimple) {
    std::byte b[64];
    size_t n=encodeSimple(b,MsgType::CREDIT,CreditMsg{777});
    EXPECT_EQ(n,24u);
    auto h=get<MsgHeader>(b);
    EXPECT_EQ(h.type,MsgType::CREDIT);
    EXPECT_EQ(h.bodyLen,8u);
    EXPECT_EQ(get<CreditMsg>(b+16).creditLimit,777u);
}

TEST(Payload, DeterministicAndVerifiable) {
    std::vector<std::byte> a(1001);
    std::vector<std::byte> b(1001);
    fillPayload(3,12345,a);
    fillPayload(3,12345,b);
    EXPECT_EQ(a,b);
    EXPECT_TRUE(verifyPayload(3,12345,a));
    EXPECT_FALSE(verifyPayload(3,12346,a));
    EXPECT_FALSE(verifyPayload(4,12345,a));
    a[1000]^=std::byte{1};  //last byte, hits the not-multiple-of-8 tail path
    EXPECT_FALSE(verifyPayload(3,12345,a));
}
