/*
Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef INC_ROCTRACER_HSA_RT_UTILS_HPP_
#define INC_ROCTRACER_HSA_RT_UTILS_HPP_

#include <hsa.h>

#include <cstdint>
#include <cstddef>

#define HSART_CALL(call)                                                                           \
  do {                                                                                             \
    hsa_status_t status = call;                                                                    \
    if (status != HSA_STATUS_SUCCESS) {                                                            \
      std::cerr << "HSA-rt call '" << #call << "' error(" << std::hex << status << ")"             \
        << std::dec << std::endl << std::flush;                                                    \
      abort();                                                                                     \
    }                                                                                              \
  } while (0)

namespace hsa_rt_utils {

// HSA runtime timer implementation
class Timer {
  public:
  typedef uint64_t timestamp_t;
  typedef long double freq_t;
  
  Timer() {
    timestamp_t timestamp_hz = 0;
    HSART_CALL(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP_FREQUENCY, &timestamp_hz));
    timestamp_rate_ = (freq_t)1000000000 / (freq_t)timestamp_hz;
  }

  // Returns HSA runtime timestamp rate
  freq_t timestamp_rate() const { return timestamp_rate_; }

  // Convert a given timestamp to ns
  timestamp_t timestamp_to_ns(const timestamp_t &timestamp) const {
    return timestamp_t((freq_t)timestamp * timestamp_rate_);
  }

  // Return timestamp in 'ns'
  timestamp_t timestamp_ns() const {
    timestamp_t timestamp;
    HSART_CALL(hsa_system_get_info(HSA_SYSTEM_INFO_TIMESTAMP, &timestamp));
    return timestamp_to_ns(timestamp);
  }

  private:
  // Timestamp rate
  freq_t timestamp_rate_;
};

}  // namespace hsa_rt_utils

#endif  // INC_ROCTRACER_HSA_RT_UTILS_HPP_
