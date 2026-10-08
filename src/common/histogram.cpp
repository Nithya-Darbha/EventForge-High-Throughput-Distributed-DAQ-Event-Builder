#include "daq/histogram.hpp"

#include <algorithm>
#include <cmath>

namespace daq {

/*
v<64 -> bucket v
else -> shift so the top 6 bits are left (32..63), bucket = shift*32+those bits
*/
int Histogram::bucketOf(uint64_t v) {
    if(v<static_cast<uint64_t>(LINEAR)) return static_cast<int>(v);
    int msb=63-__builtin_clzll(v);       //>= SUB_BITS+1
    int shift=msb-SUB_BITS;              //>= 1
    int sub=static_cast<int>(v>>shift);  //32..63
    return shift*SUB+sub;
}

uint64_t Histogram::bucketUpper(int idx) {
    if(idx<LINEAR) return static_cast<uint64_t>(idx);
    int shift=idx/SUB-1;
    uint64_t sub=static_cast<uint64_t>(idx-shift*SUB);
    return ((sub+1)<<shift)-1;
}

void Histogram::record(uint64_t v) {
    b[static_cast<size_t>(bucketOf(v))]++;
    cnt++;
    sum+=v;
    minV=std::min(minV,v);
    maxV=std::max(maxV,v);
}

void Histogram::merge(const Histogram& o) {
    for(size_t i=0;i<b.size();i++){
        b[i]+=o.b[i];
    }
    cnt+=o.cnt;
    sum+=o.sum;
    minV=std::min(minV,o.minV);
    maxV=std::max(maxV,o.maxV);
}

void Histogram::reset() {
    *this=Histogram{};
}

uint64_t Histogram::percentile(double p) const {
    if(cnt==0) return 0;
    auto target=static_cast<uint64_t>(std::ceil(p/100.0*static_cast<double>(cnt)));
    target=std::clamp<uint64_t>(target,1,cnt);
    uint64_t seen=0;
    for(size_t i=0;i<b.size();i++){
        seen+=b[i];
        if(seen>=target){
            return std::min(bucketUpper(static_cast<int>(i)),maxV);
        }
    }
    return maxV;
}

} // namespace daq
