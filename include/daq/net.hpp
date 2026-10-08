#pragma once
/*
 * Socket helpers (tcp, ipv4 only, its all loopback anyway)
 */
#include <cstddef>
#include <cstdint>
#include <string>

namespace daq::net {

int listenTcp(uint16_t port, int backlog=64);                          //nonblocking listener, throws
int connectTcp(const std::string& host, uint16_t port, int retryMs=5000);  //blocking, retries till builder is up
void setNonBlocking(int fd);
void setNoDelay(int fd);
void setSndBuf(int fd, int bytes);
void setRcvBuf(int fd, int bytes);

//blocking, false on EOF/error
bool writeAll(int fd, const void* buf, size_t len);
bool readAll(int fd, void* buf, size_t len);

} // namespace daq::net
