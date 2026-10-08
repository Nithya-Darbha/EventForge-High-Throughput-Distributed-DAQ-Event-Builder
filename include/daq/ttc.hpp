#pragma once
/*
 * TTC = "timing, trigger and control", the name the LHC experiments use for the system
 * that sends the trigger (L1A) to every readout board at the same time.
 *
 * Here it is a small shared memory segment (/dev/shm/eventforge_<port>):
 * - the trigger generator (thread in the builder process) is the only writer of triggers.
 *   trigger n = (eventId n, trigger timestamp), kept in a ring so frontends can read it
 * - every frontend reads the same triggers -> all sources produce fragments for the same events
 * - busyMask: 1 bit per frontend. a frontend that cant keep up (no credits / buffer full)
 *   sets its bit and the generator VETOS triggers while any bit is set.
 *   vetoed triggers never happen = deadtime. same idea as the TTS busy in CMS.
 *
 * Frontends sleep on a futex (wakeSeq) instead of polling, the generator wakes them
 * after each batch of triggers.
 */
#include <atomic>
#include <cstdint>
#include <string>

namespace daq {

inline constexpr uint32_t TTC_MAGIC=0x43545444;  //"DTTC"
inline constexpr uint64_t TTC_RING=1u<<16;        //how far a frontend can lag before it loses triggers
inline constexpr uint64_t TTC_INVALID=UINT64_MAX;

enum class RunState : uint32_t { WAITING=0, RUNNING=1, STOPPED=2 };

//these live in shared mem between processes -> must be lock free (not a hidden mutex)
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<uint32_t>)==4);

struct TtcSlot {
    std::atomic<uint64_t> id{TTC_INVALID};  //TTC_INVALID while being rewritten (seqlock style)
    std::atomic<uint64_t> ts{0};
};

struct TtcShared {
    std::atomic<uint32_t> magic{0};  //set last, so a client never sees a half initialised segment
    uint32_t ringSize=TTC_RING;
    alignas(64) std::atomic<uint64_t> published{0};  //= triggers issued so far = next eventId
    std::atomic<uint32_t> wakeSeq{0};                //futex word, ++ after every batch
    std::atomic<uint32_t> waiters{0};                //frontends sleeping on wakeSeq
    alignas(64) std::atomic<uint64_t> busyMask{0};
    alignas(64) std::atomic<uint32_t> runState{static_cast<uint32_t>(RunState::WAITING)};
    std::atomic<uint64_t> t0{0};
    alignas(64) TtcSlot ring[TTC_RING];
};

std::string ttcName(uint16_t port);

//builder side: creates + owns the segment, publishes triggers
class TtcHost {
public:
    explicit TtcHost(const std::string& name);
    ~TtcHost();
    TtcHost(const TtcHost&)=delete;
    TtcHost& operator=(const TtcHost&)=delete;

    void publish(uint64_t triggerTs);  //only ONE thread may call this
    void wakeAll();
    void setRunState(RunState s);
    void clearBusy(uint16_t sourceId);  //frontend died with its bit set -> would veto forever
    TtcShared* shm() { return mem; }

private:
    std::string name;
    TtcShared* mem=nullptr;
};

//frontend side
class TtcClient {
public:
    enum class Result { GOT, END };

    TtcClient(const std::string& name, int timeoutMs=5000);
    ~TtcClient();
    TtcClient(const TtcClient&)=delete;
    TtcClient& operator=(const TtcClient&)=delete;

    //start at the current trigger (used when joining a run thats already going)
    void joinNow();
    //blocks till the next trigger is there, END when the run stopped and we read everything
    Result waitNext(uint64_t& eventId, uint64_t& triggerTs);

    uint64_t backlog() const;  //triggers issued that we havent read yet
    uint64_t overruns() const { return overrunCnt; }  //triggers lost bc we lagged > TTC_RING
    void setBusy(uint16_t sourceId, bool busy);
    RunState runState() const;

private:
    bool readSlot(uint64_t id, uint64_t& ts) const;

    TtcShared* mem=nullptr;
    uint64_t next=0;
    uint64_t overrunCnt=0;
};

} // namespace daq
