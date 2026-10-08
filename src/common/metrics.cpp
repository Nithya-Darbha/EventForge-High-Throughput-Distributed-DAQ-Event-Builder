#include "daq/metrics.hpp"

#include <dirent.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace daq {

void JsonLine::key(const std::string& k) {
    if(!body.empty()) body+=",";
    body+="\""+k+"\":";
}

JsonLine& JsonLine::add(const std::string& k, double v) {
    key(k);
    if(!std::isfinite(v)) v=0;  //json has no nan/inf
    char tmp[64];
    std::snprintf(tmp,sizeof(tmp),"%.6g",v);
    body+=tmp;
    return *this;
}

JsonLine& JsonLine::add(const std::string& k, uint64_t v) {
    key(k);
    body+=std::to_string(v);
    return *this;
}

JsonLine& JsonLine::add(const std::string& k, const std::string& v) {
    key(k);
    body+="\""+v+"\"";
    return *this;
}

JsonLine& JsonLine::raw(const std::string& k, const std::string& json) {
    key(k);
    body+=json;
    return *this;
}

double processCpuSeconds() {
    rusage ru{};
    getrusage(RUSAGE_SELF,&ru);
    return static_cast<double>(ru.ru_utime.tv_sec+ru.ru_stime.tv_sec)+
           static_cast<double>(ru.ru_utime.tv_usec+ru.ru_stime.tv_usec)/1e6;
}

/*
loop thru /proc/self/task/<tid>
comm = thread name, stat fields 14+15 = utime+stime in clock ticks
same name twice (unnamed threads) -> add them up
*/
std::map<std::string, double> threadCpuSeconds() {
    std::map<std::string, double> out;
    DIR* d=opendir("/proc/self/task");
    if(!d) return out;
    const double tick=static_cast<double>(sysconf(_SC_CLK_TCK));
    while(dirent* e=readdir(d)){
        if(e->d_name[0]=='.') continue;
        std::string base=std::string("/proc/self/task/")+e->d_name;
        std::ifstream commF(base+"/comm");
        std::string name;
        std::getline(commF,name);
        std::ifstream statF(base+"/stat");
        std::string stat;
        std::getline(statF,stat);
        //comm in stat is in (...) and can have spaces, so skip past the ')'
        size_t close=stat.rfind(')');
        if(close==std::string::npos) continue;
        std::istringstream rest(stat.substr(close+2));
        std::string field;
        double utime=0;
        double stime=0;
        for(int i=3;i<=15 && rest>>field;i++){
            if(i==14) utime=std::stod(field);
            if(i==15) stime=std::stod(field);
        }
        out[name]+=(utime+stime)/tick;
    }
    closedir(d);
    return out;
}

void nameThread(const std::string& name) {
    pthread_setname_np(pthread_self(),name.substr(0,15).c_str());  //linux max 15 chars
}

} // namespace daq
