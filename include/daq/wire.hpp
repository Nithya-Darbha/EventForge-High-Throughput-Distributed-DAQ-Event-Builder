#pragma once
/*
 * Wire format between frontends and the event builder.
 * Every msg = 16 byte MsgHeader + body. All ints are little endian (native),
 * encode/decode always goes thru memcpy so alignment never matters.
 */
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace daq {

//we just memcpy structs onto the socket so both ends must agree on byte order
static_assert(std::endian::native==std::endian::little, "wire format assumes little endian");

inline constexpr uint32_t MAGIC=0x46514144;          //bytes on the wire: 44 41 51 46 = "DAQF"
inline constexpr uint8_t WIRE_VERSION=1;
inline constexpr uint32_t MAX_BODY_LEN=1u<<20;       //1 MiB, bigger than this = stream is desynced

//starts at 1 so a zeroed buffer is never a valid msg
enum class MsgType : uint8_t {
    HELLO=1,     //frontend -> builder, first msg on a conn
    FRAGMENT=2,  //frontend -> builder, one detector fragment
    CREDIT=3,    //builder -> frontend, more credit
    BYE=4,       //frontend -> builder, clean end of run
    START=5,     //builder -> frontend, run started
};

struct MsgHeader {
    uint32_t magic;
    uint8_t version;
    MsgType type;
    uint16_t reserved;    //always 0
    uint32_t bodyLen;     //bytes after this header
    uint32_t reserved2;   //always 0, also pads to 16
};

struct FragmentHeader {
    uint64_t eventId;
    uint64_t triggerTs;   //when the trigger fired (CLOCK_MONOTONIC ns), used for e2e latency
    uint64_t sendTs;      //when the frontend actually sent it
    uint16_t sourceId;
    uint16_t flags;       //unused for now
    uint32_t seq;         //per conn, only frags that go on the wire (or get "lost" on the way) get one
    uint32_t payloadLen;
    uint32_t payloadCrc;  //crc32c of the payload
};

struct HelloMsg {
    uint16_t sourceId;
    uint16_t pad;
    uint32_t pid;
};

struct StartMsg {
    uint64_t t0;           //trigger epoch
    uint64_t creditLimit;  //initial credit
};

//credits are cumulative (like a tcp window): frontend may send while fragsSent<creditLimit
//so a duplicated/reordered credit msg cant break anything
struct CreditMsg {
    uint64_t creditLimit;
};

struct ByeMsg {
    uint64_t fragsSent;  //frags put on the wire this conn, builder compares with what it got
};

static_assert(sizeof(MsgHeader)==16);
static_assert(sizeof(FragmentHeader)==40);
static_assert(sizeof(HelloMsg)==8);
static_assert(sizeof(StartMsg)==16);
static_assert(sizeof(CreditMsg)==8);
static_assert(sizeof(ByeMsg)==8);
static_assert(std::is_trivially_copyable_v<MsgHeader>);
static_assert(std::is_trivially_copyable_v<FragmentHeader>);

//memcpy not reinterpret_cast -> no strict aliasing / misaligned access UB
template <class T>
inline void put(std::byte* dst, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(dst,&v,sizeof(T));
}

template <class T>
inline T get(const std::byte* src) {
    static_assert(std::is_trivially_copyable_v<T>);
    T v;
    std::memcpy(&v,src,sizeof(T));
    return v;
}

inline MsgHeader makeHeader(MsgType t, uint32_t bodyLen) {
    return MsgHeader{MAGIC,WIRE_VERSION,t,0,bodyLen,0};
}

enum class HeaderCheck { OK, BAD_MAGIC, BAD_VERSION, BAD_TYPE, TOO_LONG };

inline HeaderCheck validate(const MsgHeader& h) {
    if(h.magic!=MAGIC) return HeaderCheck::BAD_MAGIC;
    if(h.version!=WIRE_VERSION) return HeaderCheck::BAD_VERSION;
    auto t=static_cast<uint8_t>(h.type);
    if(t<1 || t>5) return HeaderCheck::BAD_TYPE;
    if(h.bodyLen>MAX_BODY_LEN) return HeaderCheck::TOO_LONG;
    return HeaderCheck::OK;
}

//header + small fixed body in one go, dst needs 16+sizeof(T)
template <class T>
inline size_t encodeSimple(std::byte* dst, MsgType type, const T& body) {
    put(dst,makeHeader(type,sizeof(T)));
    put(dst+sizeof(MsgHeader),body);
    return sizeof(MsgHeader)+sizeof(T);
}

inline constexpr size_t fragWireSize(uint32_t payloadLen) {
    return sizeof(MsgHeader)+sizeof(FragmentHeader)+payloadLen;
}

} // namespace daq
