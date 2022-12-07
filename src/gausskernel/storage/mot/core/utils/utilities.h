/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * -------------------------------------------------------------------------
 *
 * utilities.h
 *    MOT utilities.
 *
 * IDENTIFICATION
 *    src/gausskernel/storage/mot/core/utils/utilities.h
 *
 * -------------------------------------------------------------------------
 */

#ifndef UTILITIES_H
#define UTILITIES_H

#include <string>

// include mostly used files by external modules
#include "log_level.h"
#include "logger.h"
#include "mot_log.h"
#include "debug_utils.h"

namespace MOT {
#define MAX_EXPONENT 63  // 2^63 is the maximum power of 2 in uint64_t
#define POWER_OF_TWO(x) (((x) & ((x) - 1)) == 0)

/**
 * @brief Executes system command (opens a pipe to a sub-process).
 * @param cmd The command to execute.
 * @return The command output.
 */
std::string ExecOsCommand(const std::string& cmd);

/**
 * @brief Executes system command (opens a pipe to a sub-process).
 * @param cmd The command to execute.
 * @return The command output.
 */
std::string ExecOsCommand(const char* cmd);

/**
 * @brief Converts binary data to hexa-decimal string format.
 * @param data The binary data pointer.
 * @param len The binary data length in bytes.
 * @return The formatted hexa-decimal string.
 */
std::string HexStr(const uint8_t* data, uint16_t len);

/**
 * @brief Computes the nearest power of 2 lower than the given value.
 * @param value Value for which to calculate the nearest power of 2.
 * @return Nearest power of 2 value.
 */
inline uint64_t ComputeNearestLowPow2(uint64_t value)
{
    if (value == 0) {
        return 1;
    }

    if (POWER_OF_TWO(value)) {
        return value;
    }

    // Count the number zero bits in the left, compute the power and return the result.
    uint64_t zeroCount = __builtin_clzll(value);
    uint64_t powValue = (MAX_EXPONENT - zeroCount);
    uint64_t result = ((uint64_t)1) << powValue;
    return result;
}

/**
 * @brief Computes the nearest power of 2 higher than the given value.
 * @param value Value for which to calculate the nearest power of 2.
 * @return Nearest power of 2 value.
 */
inline uint64_t ComputeNearestHighPow2(uint64_t value)
{
    if (value == 0) {
        return 1;
    }

    if (POWER_OF_TWO(value)) {
        return value;
    }

    // Count the number zero bits in the left, compute the power and return the result.
    uint64_t zeroCount = __builtin_clzll(value);
    uint64_t powValue = ((MAX_EXPONENT - zeroCount) + 1);
    if (powValue > MAX_EXPONENT) {
        powValue = MAX_EXPONENT;
    }
    uint64_t result = ((uint64_t)1) << powValue;
    return result;
}

/** @define Likely execution path to assist compiler optimizations. */
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

/** @define Unlikely execution path to assist compiler optimizations. */
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

/** @define Align size to 8 bytes. */
#define ALIGN8(len) (((len) + 8 - 1) & ~((size_t)(8 - 1)))

/** @define Align size to N bytes. */
#define ALIGN_N(len, n) (((len) + n - 1) & ~((size_t)(n - 1)))

/** @define High nibble of a byte. */
#define HIGH_NIBBLE(byte) (((byte) >> 4) & 0x0F)

/** @define Low nibble of a byte. */
#define LOW_NIBBLE(byte) ((byte) & 0x0F)

/** @define Compile-time conversion of identifier to string literal. */
#define stringify(name) #name

template <typename T>
void SetNullIfPtr(T obj)
{}

template <typename T>
void SetNullIfPtr(T*& ptr)
{
    ptr = nullptr;
}
}  // namespace MOT

#endif  // UTILITIES_H
