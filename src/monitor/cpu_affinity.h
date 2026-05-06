#pragma once

#include <string>

namespace mds {

// 把"调用方"线程钉到指定 CPU 核（成功返回 true）。
//
// 使用约束（很重要）：
//   - 只动当前线程，不动子线程；调用者负责在目标线程入口处调用，避免误把
//     reporter / writer 一并锁到同一个核上发生抢占（这反而会拉高尾延迟）
//   - Linux 上 pthread_create 默认继承父线程的 affinity 掩码，所以 pin 必须
//     在所有需要"自由调度"的子线程（如 BinaryWriter 的后台 flush 线程）
//     启动之后再调用，否则子线程会被传染到同一核
//
// 错误处理：
//   - 失败不致命：返回 false，error 写入原因；上层选择"打印警告 + 继续不绑"
//   - 仅 Linux 真正实现；其他平台静默 noop 返回 false（error 给出说明）
//
// CPU 上界：
//   - 优先用 sysconf(_SC_NPROCESSORS_ONLN)，failback 到 hardware_concurrency()，
//     都失败则直接拒绝（不放行到 CPU_SETSIZE，因为那只会推迟到 pthread 报
//     EINVAL，错误信息更不友好）
bool pin_current_thread_to_cpu(int cpu, std::string& error);

}  // namespace mds
