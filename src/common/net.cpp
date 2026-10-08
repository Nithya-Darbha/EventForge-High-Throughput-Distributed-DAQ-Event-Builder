#include "daq/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace daq::net {

static std::runtime_error sysError(const char* what) {
    return std::runtime_error(std::string(what)+": "+std::strerror(errno));
}

void setNonBlocking(int fd) {
    int fl=fcntl(fd,F_GETFL,0);
    if(fl<0 || fcntl(fd,F_SETFL,fl|O_NONBLOCK)<0) throw sysError("fcntl");
}

void setNoDelay(int fd) {
    int one=1;
    setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
}

void setSndBuf(int fd, int bytes) { setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&bytes,sizeof(bytes)); }
void setRcvBuf(int fd, int bytes) { setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&bytes,sizeof(bytes)); }

int listenTcp(uint16_t port, int backlog) {
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0) throw sysError("socket");
    int one=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    sockaddr_in a{};
    a.sin_family=AF_INET;
    a.sin_port=htons(port);
    a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0) throw sysError("bind");
    if(listen(fd,backlog)<0) throw sysError("listen");
    setNonBlocking(fd);
    return fd;
}

/*
try connect
fail -> close, sleep 50ms, try again
give up after retryMs
(so frontends can be started before the builder is listening)
*/
int connectTcp(const std::string& host, uint16_t port, int retryMs) {
    sockaddr_in a{};
    a.sin_family=AF_INET;
    a.sin_port=htons(port);
    if(inet_pton(AF_INET,host.c_str(),&a.sin_addr)!=1) throw std::runtime_error("bad host "+host);
    for(int waited=0;;waited+=50){
        int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
        if(fd<0) throw sysError("socket");
        if(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0) return fd;
        int e=errno;
        close(fd);
        if(waited>=retryMs){
            errno=e;
            throw sysError("connect");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool writeAll(int fd, const void* buf, size_t len) {
    auto p=static_cast<const char*>(buf);
    while(len>0){
        ssize_t n=send(fd,p,len,MSG_NOSIGNAL);
        if(n<0){
            if(errno==EINTR) continue;
            return false;
        }
        p+=n;
        len-=static_cast<size_t>(n);
    }
    return true;
}

bool readAll(int fd, void* buf, size_t len) {
    auto p=static_cast<char*>(buf);
    while(len>0){
        ssize_t n=recv(fd,p,len,0);
        if(n==0) return false;
        if(n<0){
            if(errno==EINTR) continue;
            return false;
        }
        p+=n;
        len-=static_cast<size_t>(n);
    }
    return true;
}

} // namespace daq::net
