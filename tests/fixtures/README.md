# Benchmark fixtures

离线性能基线不依赖外部录制文件。

确定性数据集由 `src/benchmark/pipeline_benchmark.cpp` 内的
`make_trade_message` / `make_orderbook_message` 生成：同一输入参数下
JSON 内容固定，因此 `--bench-pipeline` 可复现对比。

**统一口径（默认）：** `messages=50000`，`gap_us=0`，**不写盘**（`--no-write`）。
写盘会显著改变 msgs/s，必须显式加 `--bench-write`，并在表里单独标 `write=yes`。

跑法：

```bash
bash tests/benchmark.sh --runs 3
# 等价于：
# bash tests/benchmark.sh --messages 50000 --gap-us 0 --no-write --runs 3
```
