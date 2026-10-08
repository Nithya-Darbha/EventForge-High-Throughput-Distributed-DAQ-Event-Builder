#include "frontend/trigger.hpp"

#include "daq/clock.hpp"

namespace daq {

TriggerInput::TriggerInput(const std::string& shmName, uint16_t src, bool thr, uint64_t depth)
    : ttc(shmName), sourceId(src), throttle(thr), derandDepth(depth<2 ? 2 : depth) {
    //start at whatever trigger is current. normal start = 0, restarted frontend = mid run
    ttc.joinNow();
}

TriggerInput::~TriggerInput() {
    dropBusy();
}

bool TriggerInput::next(uint64_t& eventId, uint64_t& triggerTs) {
    if(ttc.waitNext(eventId,triggerTs)==TtcClient::Result::END) return false;
    uint64_t lag=ttc.backlog();
    if(lag>maxLag) maxLag=lag;
    return true;
}

/*
not throttling (DROP policy) -> never busy
want busy  -> no credit or derand buffer full
can clear  -> credit back and backlog drained to half
*/
void TriggerInput::updateBusy(bool hasCredit) {
    if(!throttle) return;
    uint64_t lag=ttc.backlog();
    bool derandFull=lag>=derandDepth;
    bool wantBusy=!hasCredit || derandFull;
    bool canClear=hasCredit && lag<derandDepth/2;

    if(!busy && wantBusy){
        setBusy(true);
    }
    else if(busy && canClear){
        setBusy(false);
    }
    //first try was just setBusy(wantBusy) every time -> busy flapped on/off every frag
    //setBusy(wantBusy);
}

void TriggerInput::dropBusy() {
    if(busy) setBusy(false);
}

void TriggerInput::setBusy(bool b) {
    uint64_t now=nowNs();
    if(b){
        busySince=now;
        busyAsserts++;
    }
    else{
        busyTotal+=now-busySince;
    }
    busy=b;
    ttc.setBusy(sourceId,b);
}

uint64_t TriggerInput::busyNs() const {
    return busy ? busyTotal+(nowNs()-busySince) : busyTotal;
}

} // namespace daq
