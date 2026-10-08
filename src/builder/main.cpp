/*
 * Event builder process.
 *
 *  frontends --tcp--> [receiver] --SPSC--> [shard x N] --done queue--> [sink x M]
 *                        |  ^                                              |
 *                        |  +------------- credits (slots released) <------+
 *                        +--> trigger gen thread -> TTC shared mem -> frontends
 *  + sampler thread -> metrics.jsonl every 100ms
 *  + summary json on stdout at the end
 */
#include <csignal>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "builder/builder.hpp"
#include "builder/receiver.hpp"
#include "builder/shard.hpp"
#include "builder/sink.hpp"
#include "builder/trigger.hpp"
#include "daq/args.hpp"
#include "daq/clock.hpp"
#include "daq/crc32c.hpp"
#include "daq/metrics.hpp"
#include "daq/ttc.hpp"

using namespace daq;

namespace {

std::atomic<bool> stopFlag{false};
void onSignal(int) { stopFlag.store(true); }  //lock free atomic store is ok in a signal handler

} // namespace

BuilderConfig BuilderConfig::fromArgs(const Args& a) {
    BuilderConfig c;
    c.port=static_cast<uint16_t>(a.u64("port",9000));
    c.sources=static_cast<uint16_t>(a.u64("sources",4));
    c.credits=static_cast<uint32_t>(a.u64("credits",256));
    c.shards=static_cast<uint32_t>(a.u64("shards",2));
    c.sinks=static_cast<uint32_t>(a.u64("sinks",2));
    c.spscQueues=a.str("queue","spsc")!="mutex";
    c.inboxCap=static_cast<uint32_t>(a.u64("inbox-cap",0));
    c.doneCap=static_cast<uint32_t>(a.u64("done-cap",1024));
    c.timeoutNs=a.u64("timeout-ms",100)*NS_PER_MS;
    c.degraded=a.flag("degraded");
    c.maxPayload=static_cast<uint32_t>(a.u64("max-payload",8192));
    c.rate=a.f64("rate",10000);
    c.rateProfile=a.str("rate-profile","");
    c.durationS=a.f64("duration-s",5);
    c.ttcBatchNs=a.u64("ttc-batch-us",20)*NS_PER_US;
    c.startDelayNs=a.u64("start-delay-ms",200)*NS_PER_MS;
    c.sinkDelayNs=static_cast<uint64_t>(a.f64("sink-delay-us",0)*1e3);
    c.slowAtS=a.f64("slow-at-s",-1);
    c.slowForS=a.f64("slow-for-s",0);
    c.slowDelayNs=static_cast<uint64_t>(a.f64("slow-delay-us",0)*1e3);
    c.verify=a.flag("verify");
    c.metricsOut=a.str("metrics-out","");
    c.sampleNs=a.u64("sample-ms",100)*NS_PER_MS;
    c.maxRunS=a.f64("max-run-s",c.durationS+60);
    c.quiet=a.flag("quiet");
    c.debug=a.flag("debug");
    return c;
}

namespace {

std::string latencyJson(const Histogram& h) {
    JsonLine j;
    j.add("p50",nsToUs(h.percentile(50)))
     .add("p95",nsToUs(h.percentile(95)))
     .add("p99",nsToUs(h.percentile(99)))
     .add("p999",nsToUs(h.percentile(99.9)))
     .add("max",nsToUs(h.max()))
     .add("mean",h.mean()/1e3);
    return j.str();
}

/*
sampler: every sampleNs write one json line with rates (since last line), queue depths,
latency of events finished in that interval, deadtime, cpu ...
python only ever reads these files
*/
class Sampler {
public:
    Sampler(const BuilderConfig& c, Shared& s, std::vector<std::unique_ptr<Shard>>& sh,
            BoundedQueue<Event>& d, SinkPool& sk, TriggerGen& tg, Receiver& r, TtcHost& t)
        : cfg(c), shared(s), shards(sh), done(d), sinks(sk), trig(tg), recv(r), ttc(t) {
        if(!cfg.metricsOut.empty()){
            out=std::fopen(cfg.metricsOut.c_str(),"w");
            if(!out) std::fprintf(stderr,"builder: cant open %s\n",cfg.metricsOut.c_str());
        }
    }
    ~Sampler() {
        stop();
        if(out) std::fclose(out);
    }

    void start() { th=std::thread([this]{ run(); }); }
    void stop() {
        running.store(false);
        if(th.joinable()) th.join();
    }
    std::map<std::string, double> threadCpu() const {
        std::lock_guard<std::mutex> lk(cpuMtx);
        return cpu;
    }

private:
    void run() {
        nameThread("eb-sampler");
        uint64_t lastT=nowNs();
        double lastCpu=processCpuSeconds();
        uint64_t lastEvents=0;
        uint64_t lastBytes=0;
        uint64_t lastIssued=0;
        uint64_t lastVetoed=0;
        uint64_t lastFrags=0;
        while(running.load()){
            sleepNs(cfg.sampleNs);
            uint64_t now=nowNs();
            snapshotThreadCpu();
            if(!trig.started() || now<trig.startTs()){
                lastT=now;
                lastCpu=processCpuSeconds();
                continue;
            }
            double dt=nsToS(now-lastT);
            double cpuNow=processCpuSeconds();

            uint64_t events=sinks.events.get();
            uint64_t bytes=sinks.bytes.get();
            uint64_t issued=trig.issued.get();
            uint64_t vetoed=trig.vetoed.get();
            uint64_t frags=recv.fragmentsRx.get();
            uint64_t dIssued=issued-lastIssued;
            uint64_t dVetoed=vetoed-lastVetoed;
            Histogram lat=sinks.takeInterval();

            uint64_t inboxSum=0;
            uint64_t inboxMax=0;
            uint64_t partials=0;
            uint64_t complete=0;
            uint64_t incomplete=0;
            uint64_t degraded=0;
            uint64_t blockedNs=0;
            for(auto& s:shards){
                uint64_t d=s->inbox.size();
                inboxSum+=d;
                if(d>inboxMax) inboxMax=d;
                partials+=s->partials.get();
                complete+=s->complete.get();
                incomplete+=s->incomplete.get();
                degraded+=s->degraded.get();
                blockedNs+=s->doneBlockedNs.get();
            }
            uint64_t busyMask=ttc.shm()->busyMask.load(std::memory_order_relaxed);
            uint64_t live=shared.liveMask.load(std::memory_order_relaxed);

            JsonLine j;
            j.add("t",nsToS(now-trig.startTs()))
             .add("trig_rate",static_cast<double>(dIssued)/dt)
             .add("veto_rate",static_cast<double>(dVetoed)/dt)
             .add("deadtime",dIssued+dVetoed ? static_cast<double>(dVetoed)/static_cast<double>(dIssued+dVetoed) : 0.0)
             .add("ev_rate",static_cast<double>(events-lastEvents)/dt)
             .add("mb_rate",static_cast<double>(bytes-lastBytes)/dt/1e6)
             .add("frag_rate",static_cast<double>(frags-lastFrags)/dt)
             .add("lat_p50_us",nsToUs(lat.percentile(50)))
             .add("lat_p99_us",nsToUs(lat.percentile(99)))
             .add("lat_max_us",nsToUs(lat.max()))
             .add("inbox_sum",inboxSum)
             .add("inbox_max",inboxMax)
             .add("done_q",static_cast<uint64_t>(done.size()))
             .add("pool_used",static_cast<uint64_t>(shared.pool.inUse()))
             .add("pool_frac",static_cast<double>(shared.pool.inUse())/static_cast<double>(shared.pool.capacity()))
             .add("partials",partials)
             .add("complete",complete)
             .add("incomplete",incomplete)
             .add("degraded",degraded)
             .add("src_skips",recv.sourceSkips.get())
             .add("seq_gaps",recv.seqGaps.get())
             .add("crc_errors",recv.crcErrors.get())
             .add("dups",recv.duplicatesRx.get())
             .add("producer_failures",recv.producerFailures.get())
             .add("busy_sources",static_cast<uint64_t>(__builtin_popcountll(busyMask)))
             .add("live_sources",static_cast<uint64_t>(__builtin_popcountll(live)))
             .add("shard_blocked_ms",static_cast<double>(blockedNs)/1e6)
             .add("cpu_pct",100.0*(cpuNow-lastCpu)/dt);
            if(out){
                std::fprintf(out,"%s\n",j.str().c_str());
                std::fflush(out);
            }

            lastT=now;
            lastCpu=cpuNow;
            lastEvents=events;
            lastBytes=bytes;
            lastIssued=issued;
            lastVetoed=vetoed;
            lastFrags=frags;
        }
    } /* Sampler::run() */

    //cpu per thread only goes up, keep the max so we still have it after threads exit
    void snapshotThreadCpu() {
        auto now=threadCpuSeconds();
        std::lock_guard<std::mutex> lk(cpuMtx);
        for(auto& [name,secs]:now){
            if(secs>cpu[name]) cpu[name]=secs;
        }
    }

    const BuilderConfig& cfg;
    Shared& shared;
    std::vector<std::unique_ptr<Shard>>& shards;
    BoundedQueue<Event>& done;
    SinkPool& sinks;
    TriggerGen& trig;
    Receiver& recv;
    TtcHost& ttc;
    FILE* out=nullptr;
    std::atomic<bool> running{true};
    std::thread th;
    mutable std::mutex cpuMtx;
    std::map<std::string, double> cpu;
};

} // namespace

int main(int argc, char** argv) {
    Args a(argc,argv);
    BuilderConfig cfg=BuilderConfig::fromArgs(a);
    if(cfg.sources==0 || cfg.sources>64 || cfg.shards==0 || cfg.sinks==0 || cfg.credits==0){
        std::fprintf(stderr,"builder: bad config\n");
        return 2;
    }
    std::signal(SIGINT,onSignal);
    std::signal(SIGTERM,onSignal);
    std::signal(SIGPIPE,SIG_IGN);
    tightTimerSlack();

    //how good are short sleeps on this box? decides how much the trigger + sinks spin
    double spinArg=a.f64("spin-us",-1);
    if(spinArg>=0){
        setSpinNs(static_cast<uint64_t>(spinArg*1e3));
    }
    else{
        calibrateSpin();
    }
    if(!cfg.quiet || sleepOvershootNs()>500*NS_PER_US){
        std::fprintf(stderr,"builder: short sleeps wake up %.0f us late (p90) -> spinning the last %.0f us, crc32c=%s\n",
                     nsToUs(sleepOvershootNs()),nsToUs(spinNs()),crc32cImpl());
    }

    Shared shared(cfg);
    TtcHost ttc(ttcName(cfg.port));
    BoundedQueue<Event> done(cfg.doneCap);
    TriggerGen trig(ttc,RateProfile::parse(cfg.rateProfile,cfg.rate),cfg.durationS,cfg.ttcBatchNs);
    SinkPool sinks(cfg,shared,done);

    std::vector<std::unique_ptr<Shard>> shards;
    for(uint32_t i=0;i<cfg.shards;i++){
        shards.push_back(std::make_unique<Shard>(i,cfg,shared,done));
    }

    //run start: everyone is connected -> t0 a bit in the future so START reaches all frontends first
    auto onStart=[&]() -> uint64_t {
        uint64_t t0=nowNs()+cfg.startDelayNs;
        sinks.setRunStart(t0);
        trig.start(t0);
        return t0;
    };
    Receiver recv(cfg,shared,shards,ttc,onStart,[&]{ return trig.finished(); });
    Sampler sampler(cfg,shared,shards,done,sinks,trig,recv,ttc);

    for(auto& s:shards) s->start();
    sinks.start();
    sampler.start();
    if(!cfg.quiet){
        std::fprintf(stderr,"builder: listening on %u, waiting for %u sources (%s queues, %u shards, %u sinks, %u credits)\n",
                     cfg.port,cfg.sources,cfg.spscQueues ? "spsc" : "mutex",cfg.shards,cfg.sinks,cfg.credits);
    }

    recv.run(stopFlag);

    /*
    shutdown order matters:
    trigger off -> shards drain + flush partials -> done queue closed -> sinks drain -> sampler off
    */
    trig.requestStop();
    trig.join();
    for(auto& s:shards) s->inbox.close();
    for(auto& s:shards) s->join();
    done.close();
    sinks.join();
    sampler.stop();

    //---------- summary ----------
    AssemblerStats tot;
    std::vector<uint64_t> missing(cfg.sources,0);
    for(auto& s:shards){
        const auto& st=s->assembler().stats();
        tot.fragmentsIn+=st.fragmentsIn;
        tot.eventsComplete+=st.eventsComplete;
        tot.eventsDegraded+=st.eventsDegraded;
        tot.eventsIncomplete+=st.eventsIncomplete;
        tot.duplicates+=st.duplicates;
        tot.late+=st.late;
        tot.maxPartials+=st.maxPartials;
        for(uint16_t i=0;i<cfg.sources;i++){
            missing[i]+=s->assembler().missingPerSource()[i];
        }
    }
    uint64_t t0=trig.startTs();
    uint64_t lastCommit=sinks.lastCommitTs.get();
    double span=lastCommit>t0 ? nsToS(lastCommit-t0) : 0.0;
    //rate over the whole trigger window, not just till the last commit (DROP can stop committing early)
    double window=span>cfg.durationS ? span : cfg.durationS;
    uint64_t issued=trig.issued.get();
    uint64_t vetoed=trig.vetoed.get();
    uint64_t seen=tot.eventsComplete+tot.eventsIncomplete;
    Histogram e2e=sinks.totalE2e();
    Histogram build=sinks.totalBuild();

    std::string perSource="[";
    for(uint16_t i=0;i<cfg.sources;i++){
        const auto& t=recv.trackers()[i];
        JsonLine j;
        j.add("id",static_cast<uint64_t>(i))
         .add("fragments",t.frags)
         .add("seq_gaps",t.seqGaps)
         .add("reordered",t.reordered)
         .add("net_transit_loss",t.netTransitLoss())
         .add("duplicates",t.duplicates)
         .add("source_skips",t.sourceSkips)
         .add("missing_in_incomplete",missing[i]);
        perSource+=(i ? "," : "")+j.str();
    }
    perSource+="]";

    JsonLine threads;
    for(auto& [name,secs]:sampler.threadCpu()){
        threads.add(name,secs);
    }

    JsonLine cfgJ;
    cfgJ.add("sources",static_cast<uint64_t>(cfg.sources))
        .add("credits",static_cast<uint64_t>(cfg.credits))
        .add("shards",static_cast<uint64_t>(cfg.shards))
        .add("sinks",static_cast<uint64_t>(cfg.sinks))
        .add("queue",std::string(cfg.spscQueues ? "spsc" : "mutex"))
        .add("rate",cfg.rate)
        .add("rate_profile",cfg.rateProfile)
        .add("duration_s",cfg.durationS)
        .add("timeout_ms",static_cast<double>(cfg.timeoutNs)/1e6)
        .add("degraded",static_cast<uint64_t>(cfg.degraded))
        .add("sink_delay_us",static_cast<double>(cfg.sinkDelayNs)/1e3);

    JsonLine j;
    j.raw("config",cfgJ.str())
     .add("span_s",span)
     .add("triggers_issued",issued)
     .add("triggers_vetoed",vetoed)
     .add("deadtime_frac",issued+vetoed ? static_cast<double>(vetoed)/static_cast<double>(issued+vetoed) : 0.0)
     .add("events_complete",tot.eventsComplete)
     .add("events_degraded",tot.eventsDegraded)
     .add("events_incomplete",tot.eventsIncomplete)
     .add("events_never_seen",issued>seen ? issued-seen : 0)
     .add("events_per_s",static_cast<double>(sinks.events.get())/window)
     .add("mb_per_s",static_cast<double>(sinks.bytes.get())/window/1e6)
     .add("offered_per_s",static_cast<double>(issued+vetoed)/cfg.durationS)
     .add("fragments_rx",recv.fragmentsRx.get())
     .add("bytes_rx",recv.bytesRx.get())
     .raw("latency_e2e_us",latencyJson(e2e))
     .raw("latency_build_us",latencyJson(build))
     .add("crc_errors",recv.crcErrors.get())
     .add("duplicates",recv.duplicatesRx.get()+tot.duplicates)
     .add("late_fragments",tot.late)
     .add("protocol_errors",recv.protocolErrors.get())
     .add("producer_failures",recv.producerFailures.get())
     .add("reconnects",recv.reconnects.get())
     .add("bye_mismatch",recv.byeMismatch.get())
     .add("verify_fail",sinks.verifyFail.get())
     .add("inbox_full_spins",recv.inboxFullSpins.get())
     .add("pool_empty",recv.poolEmpty.get())
     .add("pool_slots",static_cast<uint64_t>(shared.pool.capacity()))
     .add("pool_leaked",static_cast<uint64_t>(shared.pool.inUse()))
     .add("credit_msgs",recv.creditMsgs.get())
     .add("max_partials",tot.maxPartials)
     .add("cpu_s",processCpuSeconds())
     .add("crc32c_impl",std::string(crc32cImpl()))
     .add("sleep_overshoot_us",nsToUs(sleepOvershootNs()))
     .add("spin_us",nsToUs(spinNs()))
     .raw("thread_cpu_s",threads.str())
     .raw("per_source",perSource);
    std::printf("%s\n",j.str().c_str());
    return 0;
} /* main() */
