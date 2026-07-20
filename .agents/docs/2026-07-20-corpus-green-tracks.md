# 语料转绿三轨(#6 dns / #7 js-sql / #8 crypto)开发方案

关联 issue:#6、#7、#8(各自 issue 下已贴基线数据与再定标提案)。
基线来源:`rewrite_bun_in_mcpp` 分支全量语料轮(1902 文件,`--timeout 30`,
GCC lane 二进制 `mbun 2026.07.18.0`),结果在 `target/integration/baseline-bun/`。

## 目标

三个 issue 的**共同真实意图**是「把对应子树的 file-level green 抬上去」。按基线实测,
三条轨道里有两条的原定数字在语料里不成立(见下),因此本方案按**实测可达面**定标,
并要求证据同时给出「归因子系统的增量」与「子树总增量」两个数字,避免把非归因的
增量算到某个子系统头上。

| 轨 | 子树 | 基线 green | 原定目标 | 实测可达面 | 本方案目标 |
| --- | --- | ---: | --- | --- | --- |
| #6 | `js/node`(291) | 142 | +6(dns 归因) | dns 相关文件仅 3 个,2 个已绿 → dns 最多 +1 | dns 修 1 个;子树总 +≥6(近绿池) |
| #7 | `js/sql`(44) | 8 | +8(bun:sqlite 归因) | 11 个 test-failure 全是 pg/mysql 线协议;23 个 timeout 需真实数据库 | 吃掉 11 个 test-failure,目标 +≥8 |
| #8 | `js/web`+`js/node` | 74 / 142 | +8(crypto 归因) | 全仓 crypto 非绿文件仅 8 个,其中 4 个失败断言 ≥35 | crypto +≥3 绿;大文件报断言级增量 |

## 模块边界

- #6:`modules/dns`、`modules/runtime_dns`(dns 归因部分);近绿池按文件落到各自子系统
  (`modules/jsc/src/runtime/*.inc` 的绑定层,或对应 `modules/<子系统>`)。
- #7:`modules/postgres`(pg 线协议)+ mysql 编解码;`modules/sqlite` 仅在
  `js/bun/sqlite/sqlite.test.js` 单独报数时涉及。
- #8:`modules/crypto`、`modules/runtime_crypto`,绑定在 `modules/jsc/src/runtime/`
  (`node_crypto.inc` / `webcrypto.inc` / `crypto_asym.inc`)。

新增行为一律优先落在既有成员内;若需要新子系统,再按 mcpp-style-ref 起独立成员 + `.cppm`。

## 接口设计

不新增对外 API 形态——三轨都是**补齐既有 bun/node API 的语义缺口**。每处改动必须对照
上游参考实现(`compat/bun/src/…`)确定期望行为,不黑盒臆测;禁止为了通过而放宽
`compat/` 下的断言(该目录只读)。

## 测试计划

1. 红:用上游语料文件本身当验收信号(`mbun test compat/bun/test/<file>`,经
   `tools/integration/safe-test.sh` 沙箱),记录改动前 pass/fail。
2. 绿:最小实现让该文件的失败断言通过。
3. 回归:该子树整棵重跑(`bun_corpus_runner.py --discover compat/bun/test/js/<tree>`),
   与基线逐文件对照,green 不得下降。
4. 纯逻辑增量(如编解码)同时落成员单测 `modules/<member>/tests/`。

## 兼容性与风险

- **测量污染**:基线轮与最终轮必须在**机器安静**时跑。本次基线是在并行开发中跑的,
  timeout 桶被 CPU 争用抬高(107 vs 历史记录的 57),因此 timeout 数字只做定性使用,
  最终数字以安静轮为准。
- **分类口径**:`no-tests` / `all-skipped` 都不算通过;带 out-of-test error 的文件不算
  green(见 `tools/integration/bun_corpus_runner.py` classify)。
- **归因风险**:近绿池的增量与 dns/crypto 无关,证据里必须分开列,不得合并成
  「dns 带来 +6」这类不实说法。
- **外部依赖**:`js/sql` 的 23 个 timeout 需真实 MySQL/Postgres,不在本方案范围;
  `cli/install` 类网络用例受 #13(IPv6 无回退)影响,同样不计入。
