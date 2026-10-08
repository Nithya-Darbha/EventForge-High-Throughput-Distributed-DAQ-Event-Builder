/*
 * End to end tests: start the real daq_builder + daq_frontend binaries, small runs,
 * then check the builder's summary json. The point is exact accounting: every fault
 * a frontend injects must show up in the builder's counters, no buffer may leak.
 */
#include <gtest/gtest.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace {

const std::string BIN=DAQ_BIN_DIR;

//each test gets its own port so they can run in parallel
uint16_t nextPort() {
    static uint16_t p=static_cast<uint16_t>(20000+(getpid()%2000)*10);
    return p++;
}

std::string tmpFile(const std::string& tag) {
    return "/tmp/eventforge_e2e_"+std::to_string(getpid())+"_"+tag;
}

pid_t spawn(const std::vector<std::string>& args, const std::string& outFile, const std::string& errFile) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa,1,outFile.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0644);
    posix_spawn_file_actions_addopen(&fa,2,errFile.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0644);
    std::vector<char*> argv;
    for(auto& a:args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid=0;
    int rc=posix_spawn(&pid,args[0].c_str(),&fa,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&fa);
    if(rc!=0) return -1;
    return pid;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

//poor mans json lookup: first "key": number after `from`. good enough for flat summary keys
double num(const std::string& json, const std::string& key, size_t from=0) {
    size_t k=json.find("\""+key+"\":",from);
    if(k==std::string::npos) return -1;
    return std::strtod(json.c_str()+k+key.size()+3,nullptr);
}

//the per_source object for one source id
size_t sourcePos(const std::string& json, int id) {
    size_t ps=json.find("\"per_source\"");
    return json.find("{\"id\":"+std::to_string(id)+",",ps);
}

struct Run {
    std::string builder;                //summary json
    std::vector<std::string> frontends; //last stderr line of each frontend
};

/*
start builder, give it a moment, start frontends
wait for all, read outputs
*/
Run runSystem(const std::vector<std::string>& builderArgs, const std::vector<std::vector<std::string>>& fronts) {
    uint16_t port=nextPort();
    std::string tag=std::to_string(port);
    std::vector<std::string> b={BIN+"/daq_builder","--port",std::to_string(port),"--quiet",
                                "--sources",std::to_string(fronts.size())};
    b.insert(b.end(),builderArgs.begin(),builderArgs.end());
    pid_t bp=spawn(b,tmpFile(tag+"_b.out"),tmpFile(tag+"_b.err"));

    std::vector<pid_t> fps;
    for(size_t i=0;i<fronts.size();i++){
        std::vector<std::string> f={BIN+"/daq_frontend","--port",std::to_string(port),"--source-id",std::to_string(i)};
        f.insert(f.end(),fronts[i].begin(),fronts[i].end());
        fps.push_back(spawn(f,"/dev/null",tmpFile(tag+"_f"+std::to_string(i))));
    }
    for(pid_t p:fps) waitpid(p,nullptr,0);
    waitpid(bp,nullptr,0);

    Run r;
    r.builder=slurp(tmpFile(tag+"_b.out"));
    for(size_t i=0;i<fronts.size();i++){
        std::string err=slurp(tmpFile(tag+"_f"+std::to_string(i)));
        size_t lastLine=err.rfind("{\"source_id\"");
        r.frontends.push_back(lastLine==std::string::npos ? "" : err.substr(lastLine));
        std::remove(tmpFile(tag+"_f"+std::to_string(i)).c_str());
    }
    std::remove(tmpFile(tag+"_b.out").c_str());
    std::remove(tmpFile(tag+"_b.err").c_str());
    return r;
}

} // namespace

TEST(E2E, CleanRunEveryEventComplete) {
    auto r=runSystem({"--rate","5000","--duration-s","1","--verify"},{{},{},{}});
    ASSERT_FALSE(r.builder.empty());
    double issued=num(r.builder,"triggers_issued");
    EXPECT_NEAR(issued,5000,5);
    EXPECT_EQ(num(r.builder,"events_complete"),issued);
    EXPECT_EQ(num(r.builder,"events_incomplete"),0);
    EXPECT_EQ(num(r.builder,"verify_fail"),0);
    EXPECT_EQ(num(r.builder,"pool_leaked"),0);
    EXPECT_EQ(num(r.builder,"bye_mismatch"),0);
    EXPECT_EQ(num(r.builder,"producer_failures"),0);
}

TEST(E2E, MutexQueuesSameResult) {
    auto r=runSystem({"--rate","5000","--duration-s","1","--queue","mutex"},{{},{}});
    double issued=num(r.builder,"triggers_issued");
    EXPECT_GT(issued,4900);
    EXPECT_EQ(num(r.builder,"events_complete"),issued);
    EXPECT_EQ(num(r.builder,"pool_leaked"),0);
}

//faults on source 1 only: everything injected has to be counted exactly once by the builder
TEST(E2E, FaultAccountingIsExact) {
    auto r=runSystem({"--rate","5000","--duration-s","1.5"},
                     {{},{"--loss","0.01","--dup","0.01","--reorder","0.01","--corrupt","0.005","--seed","11"},{}});
    const std::string& f=r.frontends[1];
    ASSERT_FALSE(f.empty());
    double lost=num(f,"lost_injected");
    double dups=num(f,"dup_injected");
    double reord=num(f,"reorder_injected");
    double corrupt=num(f,"corrupt_injected");
    ASSERT_GT(lost,0);
    ASSERT_GT(dups,0);
    ASSERT_GT(reord,0);
    ASSERT_GT(corrupt,0);

    size_t s1=sourcePos(r.builder,1);
    EXPECT_EQ(num(r.builder,"net_transit_loss",s1),lost);
    EXPECT_EQ(num(r.builder,"reordered",s1),reord);
    EXPECT_EQ(num(r.builder,"duplicates",s1),dups);
    EXPECT_EQ(num(r.builder,"crc_errors"),corrupt);
    //every lost or corrupted frag kills exactly its own event
    EXPECT_EQ(num(r.builder,"events_incomplete"),lost+corrupt);
    EXPECT_EQ(num(r.builder,"missing_in_incomplete",s1),lost+corrupt);
    EXPECT_EQ(num(r.builder,"missing_in_incomplete",sourcePos(r.builder,0)),0);
    EXPECT_EQ(num(r.builder,"pool_leaked"),0);
}

TEST(E2E, ProducerCrashDetectedDegradedMode) {
    auto r=runSystem({"--rate","4000","--duration-s","1.5","--degraded"},{{},{"--die-at-s","0.5"},{}});
    ASSERT_FALSE(r.builder.empty());
    EXPECT_EQ(num(r.builder,"producer_failures"),1);
    EXPECT_GT(num(r.builder,"events_degraded"),3000);  //~1s x 4kHz without source 1
    EXPECT_LT(num(r.builder,"events_incomplete"),50);
    EXPECT_EQ(num(r.builder,"pool_leaked"),0);
}

//BLOCK under overload: nothing lost, overload turns into deadtime instead
TEST(E2E, BlockPolicyIsLosslessUnderOverload) {
    auto r=runSystem({"--rate","20000","--duration-s","1","--sinks","1","--sink-delay-us","200","--credits","64"},
                     {{"--policy","block"},{"--policy","block"}});
    double issued=num(r.builder,"triggers_issued");
    EXPECT_EQ(num(r.builder,"events_complete"),issued);
    EXPECT_EQ(num(r.builder,"events_incomplete"),0);
    EXPECT_GT(num(r.builder,"deadtime_frac"),0.3);
    EXPECT_EQ(num(r.builder,"pool_leaked"),0);
}

//DROP under the same overload: no deadtime, frontends throw frags away instead
TEST(E2E, DropPolicyHasNoDeadtime) {
    auto r=runSystem({"--rate","20000","--duration-s","1","--sinks","1","--sink-delay-us","200","--credits","64"},
                     {{"--policy","drop"},{"--policy","drop"}});
    EXPECT_EQ(num(r.builder,"triggers_vetoed"),0);
    EXPECT_GT(num(r.builder,"source_skips",sourcePos(r.builder,0)),1000);
    EXPECT_LT(num(r.builder,"events_complete"),num(r.builder,"triggers_issued"));
    EXPECT_EQ(num(r.builder,"pool_leaked"),0);
}
