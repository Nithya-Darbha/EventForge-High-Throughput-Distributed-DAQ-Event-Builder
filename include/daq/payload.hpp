#pragma once
/*
 * Fake detector payload. The content is a function of (sourceId, eventId) only
 * so the sink can check every byte without anyone shipping reference data around.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace daq {

//splitmix64 of the pair, never 0 (xorshift gets stuck on 0)
inline uint64_t payloadSeed(uint16_t sourceId, uint64_t eventId) {
    uint64_t z=(static_cast<uint64_t>(sourceId)<<48)^eventId^0x9E3779B97F4A7C15ull;
    z=(z^(z>>30))*0xBF58476D1CE4E5B9ull;
    z=(z^(z>>27))*0x94D049BB133111EBull;
    z^=z>>31;
    return z ? z : 1;
}

inline uint64_t xorshift(uint64_t& s) {
    s^=s<<13;
    s^=s>>7;
    s^=s<<17;
    return s;
}

inline void fillPayload(uint16_t sourceId, uint64_t eventId, std::span<std::byte> out) {
    uint64_t s=payloadSeed(sourceId,eventId);
    size_t i=0;
    for(;i+8<=out.size();i+=8){
        uint64_t w=xorshift(s);
        std::memcpy(out.data()+i,&w,8);
    }
    //tail if len not a multiple of 8
    if(i<out.size()){
        uint64_t w=xorshift(s);
        std::memcpy(out.data()+i,&w,out.size()-i);
    }
}

inline bool verifyPayload(uint16_t sourceId, uint64_t eventId, std::span<const std::byte> in) {
    uint64_t s=payloadSeed(sourceId,eventId);
    size_t i=0;
    for(;i+8<=in.size();i+=8){
        uint64_t w=xorshift(s);
        if(std::memcmp(in.data()+i,&w,8)!=0) return false;
    }
    if(i<in.size()){
        uint64_t w=xorshift(s);
        if(std::memcmp(in.data()+i,&w,in.size()-i)!=0) return false;
    }
    return true;
}

} // namespace daq
