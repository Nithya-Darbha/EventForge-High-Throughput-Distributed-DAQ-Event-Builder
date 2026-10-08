#include "builder/trigger.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

#include "daq/clock.hpp"

namespace daq {

/*
"t:rate,t:rate" -> sorted points
empty -> constant defaultRate
*/
RateProfile RateProfile::parse(const std::string& spec, double defaultRate) {
    RateProfile p;
    if(spec.empty()){
        p.points.emplace_back(0.0,defaultRate);
        return p;
    }
    std::stringstream ss(spec);
    std::string item;
    while(std::getline(ss,item,',')){
        size_t colon=item.find(':');
        if(colon==std::string::npos) throw std::invalid_argument("bad rate profile item: "+item);
        double t=std::stod(item.substr(0,colon));
        double r=std::stod(item.substr(colon+1));
        if(r<=0) throw std::invalid_argument("rate must be > 0");
        p.points.emplace_back(t,r);
    }
    std::sort(p.points.begin(),p.points.end());
    if(p.points.front().first>0) p.points.insert(p.points.begin(),{0.0,defaultRate});
    return p;
}

double RateProfile::rateAt(double tS) const {
    double r=points.front().second;
    for(const auto& [t,rate]:points){
        if(t>tS) break;
        r=rate;
    }
    return r;
}

TriggerGen::TriggerGen(TtcHost& h, RateProfile p, double durationS, uint64_t batch)
    : ttc(h), profile(std::move(p)), durationNs(static_cast<uint64_t>(durationS*1e9)), batchNs(batch) {}

TriggerGen::~TriggerGen() {
    join();
}

void TriggerGen::start(uint64_t startTs) {
    t0.store(startTs,std::memory_order_release);
    ttc.shm()->t0.store(startTs,std::memory_order_release);
    th=std::thread([this]{ run(); });
}

void TriggerGen::join() {
    if(th.joinable()) th.join();
}

/*
wait till t0, state RUNNING
loop till duration is over
  issue every trigger whose time has passed:
     busy -> veto, else publish
     next time = + 1/rate(at that time)
  wake frontends if we published smtg
  sleep till the next trigger, but at least batchNs
STOPPED + wake so frontends see the end
*/
void TriggerGen::run() {
    nameThread("eb-trigger");
    tightTimerSlack();
    const uint64_t start=t0.load(std::memory_order_acquire);
    preciseSleepUntil(start);
    ttc.setRunState(RunState::RUNNING);

    TtcShared* shm=ttc.shm();
    const double end=static_cast<double>(start+durationNs);
    double next=static_cast<double>(start);  //double so 1e9/rate doesnt get truncated every step

    while(next<end && !stopReq.load(std::memory_order_acquire)){
        uint64_t now=nowNs();
        bool published=false;
        while(next<=static_cast<double>(now) && next<end){
            bool busy=shm->busyMask.load(std::memory_order_acquire)!=0;
            if(busy){
                vetoed.add();
            }
            else{
                ttc.publish(static_cast<uint64_t>(next));
                issued.add();
                published=true;
            }
            double tS=(next-static_cast<double>(start))/1e9;
            next+=1e9/profile.rateAt(tS);
        }
        if(published) ttc.wakeAll();

        uint64_t wakeAt=std::max(static_cast<uint64_t>(next),now+batchNs);
        preciseSleepUntil(wakeAt);  //plain nanosleep can be ms late in a VM, see clock.hpp
    }

    ttc.setRunState(RunState::STOPPED);
    done.store(true,std::memory_order_release);
} /* TriggerGen::run() */

} // namespace daq
