# PokerServer

一个面向 C++ 后端 / 游戏服务器校招的实时无限注德州扑克项目。项目由 Muduo 聊天教程演进而来，但当前代码库只保留 PokerServer 实现：牌局由纯 C++ 状态机权威执行，房间采用单写者模型，网络使用长度前缀 Protobuf，钱包和结算进入 MySQL 事务，多节点由 Redis TTL 路由。

> 仅使用虚拟筹码，不包含充值、提现或任何真钱功能。

## 已实现

- 2～6 人现金桌；正确处理单挑与多人盲注、行动顺序、最小加注和短全押不重新开放加注。
- 弃牌、过牌、跟注、下注、加注、全押、自动发完公共牌、多级边池、平分底池和奇数筹码。
- 服务端权威洗牌与判定；客户端只能发送操作意图；实时底牌按观看者过滤。
- 4 字节大端长度前缀、Protobuf v1、拆包/粘包、包大小限制和每连接令牌桶限流。
- `request_id` 重放缓存、`client_sequence` 顺序检查、心跳、超时自动过牌/弃牌和断线快照。
- 按 `table_id` 固定到逻辑分片的单写者房间执行器，牌桌对象无需共享锁。
- Argon2id 密码、随机会话令牌、原子令牌刷新与登出、预处理 SQL、MySQL 连接池、钱包账本与事务结算。
- Redis 全局桌号、节点心跳、负载路由、单节点房间归属和一次性短期入桌凭证。
- 大厅 + 两个游戏节点演示；节点失联超过保护窗口后按持久化快照退款，不迁移进行中的牌局。
- `/health/live`、`/health/ready`、`/metrics`，Prometheus 配置与异步连接压测脚本。
- Qt 6 Quick/QML 客户端：登录/登出、大厅、建桌/入桌、牌桌、操作、重连认证和结算快照。
- 19 项自包含核心测试，其中穷举全部 2,598,960 种五张牌组合；ASan/UBSan、TSan 与 libFuzzer CI 入口。

## 快速开始

核心规则测试只依赖 C++17、CMake 和线程库：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build
./scripts/build.sh dev
```

开发单实例：

```bash
cp .env.example .env
# 修改 .env 中的示例密码
docker compose --profile single up --build
```

大厅 + 两个游戏节点：

```bash
docker compose --profile cluster up --build
```

同时启动 Prometheus：

```bash
docker compose --profile cluster --profile monitoring up --build
```

入口与指标：

- 大厅 TCP：`127.0.0.1:6000`
- 游戏节点：`127.0.0.1:7001`、`127.0.0.1:7002`
- 节点指标：`http://127.0.0.1:9110/metrics`、`9111`、`9112`
- Prometheus：`http://127.0.0.1:9090`

Compose 是本地开发环境，TCP 默认明文。公网部署必须在 Nginx 或其他入口配置 TLS，并替换所有示例口令。

## Qt 客户端

```bash
sudo apt-get install -y qt6-base-dev qt6-declarative-dev libprotobuf-dev protobuf-compiler
cmake --preset client
cmake --build --preset client --parallel
./build/client/apps/client/poker_client
```

客户端先连接大厅；建桌或选择桌后，会使用服务端返回的一次性凭证直连所属游戏节点，并用现有会话重新认证。

## 压测与验证

短时连通性测试：

```bash
python3 tools/load/connection_load.py --connections 100 --duration 30
```

目标验收运行：

```bash
python3 tools/load/connection_load.py \
  --connections 5000 --duration 1800 \
  --output benchmarks/results/connection-5000.json
```

合法牌局机器人：

```bash
sudo apt-get install -y protobuf-compiler python3-protobuf
./scripts/run_active_load.sh --tables 10 --duration 60
```

把 `--tables` 提升到 500 前应先提高文件描述符上限，并为批量 Argon2 注册预留启动时间。连接脚本与牌局脚本分别报告，不能用心跳吞吐冒充牌局吞吐。

## 设计文档

- [项目长期记忆与当前状态](PROJECT_MEMORY.md)
- [原始 20 周整体计划](docs/original-20-week-plan.md)
- [Ubuntu 逐步学习与验证计划](docs/ubuntu-migration-learning-plan.md)
- [架构与并发](docs/architecture.md)
- [协议与重连](docs/protocol.md)
- [状态机和边池](docs/state-machine.md)
- [数据库设计](docs/database.md)
- [一致性与故障处理](docs/consistency.md)
- [测试与验收](docs/testing.md)
- [压测方法](docs/benchmark.md)
- [剩余路线](docs/roadmap.md)

## 目录

```text
apps/server/          Muduo 服务与协议调度
apps/client/          Qt Quick 桌面客户端
include/poker/        领域、应用、存储、集群等公共接口
src/poker/            核心与基础设施实现
protocol/             Protobuf v1
deploy/               MySQL、Nginx、Prometheus 配置
tests/                确定性与穷举测试
fuzz/                 libFuzzer 协议入口
tools/load/           可复现连接压测
```
