#include "cpu_affinity.h"

#include <string>
#include <thread>

#if defined(__linux__)
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace mds {

#if defined(__linux__)

namespace {

// 探测可用 CPU 数。优先 sysconf（更精确，能反映 cgroup / cpuset 限制），
// 失败退到 std::thread::hardware_concurrency()。两个都失败返回 0。
unsigned detect_cpu_count() {
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) {
        return static_cast<unsigned>(online);
    }
    return std::thread::hardware_concurrency();
}

}  // namespace

bool pin_current_thread_to_cpu(int cpu, std::string& error) {
    if (cpu < 0) {
        error = "cpu index must be non-negative";
        return false;
    }
    const unsigned upper = detect_cpu_count();
    if (upper == 0) {
        // 探测失败时直接拒绝。不退到 CPU_SETSIZE(1024) 是因为：
        // 那样会让明显的"核号超界"错误最终在 pthread 层面以 EINVAL 抛出，
        // 错误信息变成"pthread failed"而不是更直观的"cpu 超过本机核数"。
        error = "failed to detect online cpu count; refuse to pin to avoid "
                "swallowing out-of-range errors";
        return false;
    }
    if (static_cast<unsigned>(cpu) >= upper) {
        error = "cpu index " + std::to_string(cpu) +
                " >= available cores " + std::to_string(upper);
        return false;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    // 注意：pthread_setaffinity_np 直接返回 errno-style 整数，不会设置全局 errno。
    // 所以用 strerror(rc) 而不是 strerror(errno) 才能拿到正确文案。
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) {
        error = "pthread_setaffinity_np failed, rc=" + std::to_string(rc) +
                " (" + std::strerror(rc) + ")";
        return false;
    }
    return true;
}

#else  // !__linux__

bool pin_current_thread_to_cpu(int cpu, std::string& error) {
    (void)cpu;
    error = "thread pinning not supported on this platform";
    return false;
}

#endif

}  // namespace mds
