#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <thread>

#include "builder/trigger.hpp"
#include "daq/clock.hpp"
#include "daq/ttc.hpp"

using namespace daq;

namespace {
//unique shm name per test so parallel ctest runs dont collide
std::string shmName(const char* tag) {
    return std::string("/eventforge_test_")+tag+"_"+std::to_string(getpid());
}
} // namespace

TEST(Ttc, PublishAndRead) {
    TtcHost host(shmName("pub"));
    TtcClient cli(shmName("pub"));
    host.setRunState(RunState::RUNNING);
    for(uint64_t i=0;i<10;i++) host.publish(1000+i);
    EXPECT_EQ(cli.backlog(),10u);
    uint64_t id=0;
    uint64_t ts=0;
    for(uint64_t i=0;i<10;i++){
        ASSERT_EQ(cli.waitNext(id,ts),TtcClient::Result::GOT);
        EXPECT_EQ(id,i);
        EXPECT_EQ(ts,1000+i);
    }
    host.setRunState(RunState::STOPPED);
    EXPECT_EQ(cli.waitNext(id,ts),TtcClient::Result::END);
}

//client that falls more than a ring behind loses triggers and jumps to the newest
TEST(Ttc, OverrunIsDetected) {
    TtcHost host(shmName("ovr"));
    TtcClient cli(shmName("ovr"));
    host.setRunState(RunState::RUNNING);
    for(uint64_t i=0;i<TTC_RING+100;i++) host.publish(i);
    uint64_t id=0;
    uint64_t ts=0;
    host.publish(TTC_RING+100);
    ASSERT_EQ(cli.waitNext(id,ts),TtcClient::Result::GOT);
    EXPECT_GT(cli.overruns(),0u);
    EXPECT_GE(id,100u);  //slots 0..99 got overwritten
    EXPECT_EQ(ts,id);
}

TEST(Ttc, JoinNowSkipsOldTriggers) {
    TtcHost host(shmName("join"));
    host.setRunState(RunState::RUNNING);
    for(uint64_t i=0;i<50;i++) host.publish(i);
    TtcClient cli(shmName("join"));
    cli.joinNow();
    host.publish(50);
    uint64_t id=0;
    uint64_t ts=0;
    ASSERT_EQ(cli.waitNext(id,ts),TtcClient::Result::GOT);
    EXPECT_EQ(id,50u);
}

//futex wakeup: a sleeping client has to see a trigger published later from another thread
TEST(Ttc, SleepingClientGetsWoken) {
    TtcHost host(shmName("wake"));
    TtcClient cli(shmName("wake"));
    host.setRunState(RunState::RUNNING);
    std::thread pub([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        host.publish(123);
        host.wakeAll();
    });
    uint64_t id=0;
    uint64_t ts=0;
    EXPECT_EQ(cli.waitNext(id,ts),TtcClient::Result::GOT);
    EXPECT_EQ(ts,123u);
    pub.join();
}

TEST(RateProfile, ParseAndLookup) {
    auto p=RateProfile::parse("1:2000,0.5:500",100);
    EXPECT_DOUBLE_EQ(p.rateAt(0.0),100);    //before first point -> default
    EXPECT_DOUBLE_EQ(p.rateAt(0.6),500);
    EXPECT_DOUBLE_EQ(p.rateAt(1.0),2000);
    EXPECT_DOUBLE_EQ(p.rateAt(99),2000);
    auto flat=RateProfile::parse("",42);
    EXPECT_DOUBLE_EQ(flat.rateAt(5),42);
}

//generator + busy bit: while a client holds busy every trigger is vetoed, nothing published
TEST(TriggerGen, BusyVetoesTriggers) {
    TtcHost host(shmName("veto"));
    TtcClient cli(shmName("veto"));
    cli.setBusy(3,true);
    TriggerGen gen(host,RateProfile::parse("",20000),0.1,20'000);
    gen.start(nowNs()+1'000'000);
    gen.join();
    EXPECT_EQ(gen.issued.get(),0u);
    EXPECT_NEAR(static_cast<double>(gen.vetoed.get()),2000,50);
    EXPECT_EQ(cli.runState(),RunState::STOPPED);
}

//no busy: everything issued, rate correct, client reads them all in order
TEST(TriggerGen, IssuesAtRate) {
    TtcHost host(shmName("rate"));
    TtcClient cli(shmName("rate"));
    TriggerGen gen(host,RateProfile::parse("",10000),0.2,20'000);
    uint64_t t0=nowNs()+1'000'000;
    gen.start(t0);
    uint64_t id=0;
    uint64_t ts=0;
    uint64_t n=0;
    uint64_t lastTs=0;
    while(cli.waitNext(id,ts)==TtcClient::Result::GOT){
        EXPECT_EQ(id,n);
        EXPECT_GE(ts,t0);
        EXPECT_GE(ts,lastTs);
        lastTs=ts;
        n++;
    }
    gen.join();
    EXPECT_EQ(n,gen.issued.get());
    EXPECT_NEAR(static_cast<double>(n),2000,2);
    EXPECT_EQ(gen.vetoed.get(),0u);
}
