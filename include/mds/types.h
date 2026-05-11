#pragma once

// MDS 公共数据类型对外入口。
// 现阶段：真定义仍在 src/common/types.h，本头只是转发壳，让外部使用者
//   #include <mds/types.h>
// 就能拿到 Trade / OrderBookSnapshot 等结构体。
//
// 待后续把 src/ 从公共 include 路径中收回（变 PRIVATE）时，再把真定义
// 物理迁移到本文件，src/common/types.h 反向转发。这一步暂不做。

#include "common/types.h"
