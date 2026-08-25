# 压测方法

## 环境记录模板

每次结果至少记录：

```text
commit:
Ubuntu / kernel:
CPU quota / model:
memory limit:
compiler and build type:
Docker versions:
command:
start/end time:
```

## 第一阶段：5,000 连接

```bash
ulimit -n 20000
python3 tools/load/connection_load.py \
  --host 127.0.0.1 --port 6000 \
  --connections 5000 --duration 1800 \
  --ramp-per-second 500 \
  --output benchmarks/results/connection-5000.json
```

脚本建立真实 TCP 连接并持续发送长度前缀 Protobuf 心跳，输出成功连接、响应数、错误数、吞吐及 p50/p95/p99/max。

同时采集：

```bash
curl http://127.0.0.1:9110/metrics
docker stats --no-stream
```

## 第二阶段：500 活跃桌

机器人会完成注册、登录、建桌、路由、两人入座、准备以及合法 check/call 循环：

```bash
./scripts/run_active_load.sh \
  --tables 500 --duration 1800 --setup-concurrency 20 \
  --output benchmarks/results/active-500.json
```

批量 Argon2 注册本身成本较高，应记录建桌准备时间与稳定运行阶段，并与第一阶段连接结果分开。

观察重点：

- `poker_request_latency_seconds` p99；
- `poker_rejected_requests_total` 和 `poker_storage_failures_total`；
- CPU、RSS、MySQL 连接池和磁盘写入；
- 结算次数、重复结算数和筹码守恒检查；
- 停止一个节点后的路由与退款时间。

目标是 2 核 4 GB、5,000 连接、500 活跃桌、30 分钟无错误结算且操作 p99 ≤ 100 ms。没有原始结果前，README 和简历不得写这个数字已经达到。
