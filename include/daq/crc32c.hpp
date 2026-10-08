#pragma once
/*
 * CRC32C (Castagnoli). Uses the crc32c instruction when the cpu has one
 * (SSE4.2 on x86, the CRC extension on ARMv8 / Apple Silicon), otherwise a table lookup.
 */
#include <cstddef>
#include <cstdint>

namespace daq {

uint32_t crc32c(const void* data, size_t len, uint32_t crc=0);

//sw only version, tests use it to check the hw path gives the same answer
uint32_t crc32cSw(const void* data, size_t len, uint32_t crc=0);

//which one crc32c() uses on this cpu: "sse4.2", "armv8-crc" or "software"
const char* crc32cImpl();

} // namespace daq
