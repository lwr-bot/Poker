# 测试与验收

## 当前自动测试

默认 `poker_tests` 不依赖 Muduo、MySQL、Redis、Protobuf 或 Qt，因此可快速验证核心：

- 全部 2,598,960 种五张牌组合，分类数量与标准计数一致。
- 七张牌最佳组合、单挑与多人行动顺序、弃牌结算。
- 多级边池、短全押、平分底池、奇数筹码。
- 1,000 个固定种子的随机牌局：无重复牌、行动者有效、筹码守恒。
- TCP 拆包、粘包、空包、超大包和失败解码器。
- 请求重放、处理中重复、陈旧/跳跃序号、容量饱和和“只淘汰已完成响应”。
- 同桌串行执行、任务异常后的工作线程存活、断线快照和超时行动。
- 认证不保存明文、会话撤销与到期边界、节点负载选择和一次性凭证。
- 钱包托管、买入/兑出幂等、动作唯一键、损坏结算拒绝、事务结算和宕机退款。
- Prometheus 指标、令牌桶限流和失联节点保护窗口。

```bash
./scripts/build.sh dev
./scripts/build.sh asan
./scripts/build.sh tsan
```

## 协议模糊测试

CI 使用 Clang/libFuzzer 同时攻击长度解码器和 Protobuf 解析：

```bash
cmake -S . -B build/fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DPOKER_BUILD_TESTS=OFF -DPOKER_BUILD_FUZZERS=ON
cmake --build build/fuzz --parallel
./build/fuzz/fuzz/protocol_fuzzer -max_total_time=300
```

## Ubuntu 集成测试清单

这些测试需要实际 Compose 环境，结果应保存到 `benchmarks/results/`：

1. 注册、登录、会话过期和同用户旧连接关闭。
2. 买入、兑出、重复请求、结算事务和账本余额核对。
3. 在每一条街断线并重连，快照与底牌可见性正确。
4. 停止 Redis：进行中的房间继续；新建/路由失败；不会误退款。
5. 短暂停止 MySQL：新的金融操作失败关闭，不发布未提交结算。
6. 停止一个游戏节点：6 秒后停止分配新桌；15 秒保护窗口后原桌退款。
7. 两个游戏节点均获得不同桌，单桌始终只有一个所属节点。

## 最终验收门槛

- 领域核心行覆盖率不低于 85%。
- 固定 2 核 4 GB Ubuntu 上维持 5,000 连接、500 活跃桌 30 分钟。
- 无错误结算；服务端操作响应 p99 不高于 100 ms。
- 报告必须包含提交版本、机器规格、命令、CPU、RSS、吞吐、延迟和错误数。
- 简历只能引用已经复现并提交原始结果的数据。

当前仓库已提供 5,000 真实连接/Protobuf 心跳压测，但“500 活跃桌”机器人场景与真实基准结果仍待 Ubuntu 实机完成。
