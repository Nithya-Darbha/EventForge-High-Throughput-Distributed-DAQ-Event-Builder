#pragma once
/*
 * Tiny "--key value" arg parser, didnt want a CLI lib just for a few flags.
 * "--flag" with no value counts as "1".
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace daq {

class Args {
public:
    Args(int argc, char** argv) {
        for(int i=1;i<argc;i++){
            std::string k=argv[i];
            if(k.rfind("--",0)!=0) die("expected --flag, got "+k);
            k=k.substr(2);
            bool hasValue=i+1<argc && std::string(argv[i+1]).rfind("--",0)!=0;
            if(hasValue){
                kv[k]=argv[++i];
            }
            else{
                kv[k]="1";
            }
        }
    }

    std::string str(const std::string& k, const std::string& def) const {
        auto it=kv.find(k);
        return it==kv.end() ? def : it->second;
    }
    uint64_t u64(const std::string& k, uint64_t def) const {
        auto it=kv.find(k);
        return it==kv.end() ? def : std::strtoull(it->second.c_str(),nullptr,10);
    }
    double f64(const std::string& k, double def) const {
        auto it=kv.find(k);
        return it==kv.end() ? def : std::strtod(it->second.c_str(),nullptr);
    }
    bool flag(const std::string& k) const {
        auto it=kv.find(k);
        return it!=kv.end() && it->second!="0";
    }

private:
    [[noreturn]] static void die(const std::string& m) {
        std::fprintf(stderr,"args: %s\n",m.c_str());
        std::exit(2);
    }
    std::map<std::string, std::string> kv;
};

} // namespace daq
