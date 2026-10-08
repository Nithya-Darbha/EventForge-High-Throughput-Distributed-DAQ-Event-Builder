#pragma once
/*
 * CRC32C (Castagnoli). Uses the SSE4.2 crc32 instruction when the cpu has it,
 * otherwise a normal table lookup.
 */
#include <cstddef>
#include <cstdint>

namespace daq {

uint32_t crc32c(const void* data, size_t len, uint32_t crc=0);

//sw only version, tests use it to check the hw path gives the same answer
uint32_t crc32cSw(const void* data, size_t len, uint32_t crc=0);

} // namespace daq
