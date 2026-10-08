#pragma once
/*
 * Receiver: the network side of the builder, one thread, epoll over all frontend conns.
 *  - run control: waits for every source to say HELLO, then starts the trigger + sends START
 *  - framing: tcp is a byte stream, cut it back into msgs (header -> body_len)
 *  - per frag: pool slot, seq tracking (loss/dup/reorder), crc check, push to shard eventId % shards
 *  - credits: watches how many frags per source got released and sends CREDIT msgs back
 *  - failures: conn closed without BYE = producer failure -> source marked dead, its busy bit cleared
 */
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "builder/builder.hpp"
#include "builder/shard.hpp"
#include "builder/source_tracker.hpp"
#include "daq/metrics.hpp"
#include "daq/ttc.hpp"

namespace daq {

class Receiver {
public:
    using StartFn=std::function<uint64_t()>;  //starts the run, returns t0
    using DoneFn=std::function<bool()>;       //true once the trigger has stopped

    Receiver(const BuilderConfig& cfg, Shared& shared, std::vector<std::unique_ptr<Shard>>& shards,
             TtcHost& ttc, StartFn onStart, DoneFn triggerDone);
    ~Receiver();

    //returns when the run is over (or stop gets set)
    void run(const std::atomic<bool>& stop);

    bool runStarted() const { return t0!=0; }
    const std::vector<SourceTracker>& trackers() const { return track; }  //after run()

    Counter fragmentsRx;
    Counter bytesRx;
    Counter crcErrors;
    Counter protocolErrors;
    Counter producerFailures;
    Counter reconnects;
    Counter duplicatesRx;
    Counter tooOldRx;
    Counter inboxFullSpins;
    Counter poolEmpty;
    Counter creditMsgs;
    Counter byeMismatch;
    Counter seqGaps;
    Counter reordered;
    Counter sourceSkips;
    Counter connected;

private:
    struct Conn {
        int fd=-1;
        int source=-1;
        uint32_t gen=0;
        bool bye=false;
        uint64_t byeCount=0;
        uint64_t rxCount=0;
        uint64_t lastGrant=0;
        bool stalled=false;  //pool was empty, msgs waiting in rx
        bool reading=true;   //EPOLLIN on/off
        std::vector<std::byte> rx;
        size_t have=0;
        std::vector<std::byte> tx;
    };
    enum class Parse { OK, STALL, ERROR };

    void accept();
    void onReadable(Conn& c);
    Parse parse(Conn& c);
    Parse handleMsg(Conn& c, const MsgHeader& h, const std::byte* body);
    Parse handleHello(Conn& c, const std::byte* body, uint32_t len);
    Parse handleFragment(Conn& c, const std::byte* body, uint32_t len);
    void startRun();
    void sendStart(Conn& c);
    void grantCredits();
    void queueSend(Conn& c, const std::byte* p, size_t n);
    void flushTx(Conn& c);
    void setReading(Conn& c, bool on);
    void closeConn(int fd);

    const BuilderConfig& cfg;
    Shared& shared;
    std::vector<std::unique_ptr<Shard>>& shards;
    TtcHost& ttc;
    StartFn onStart;
    DoneFn triggerDone;

    int listenFd=-1;
    int ep=-1;
    std::unordered_map<int, Conn> conns;
    std::vector<int> sourceFd;
    std::vector<SourceTracker> track;
    uint64_t everConnected=0;  //bit per source
    int hellos=0;
    uint64_t t0=0;
    uint64_t grantStep=1;
};

} // namespace daq
