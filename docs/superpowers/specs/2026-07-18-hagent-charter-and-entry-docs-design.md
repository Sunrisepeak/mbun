# 设计:hagent 协作系统 —— 总纲 + 双入口文档(子项目 1)

- 日期:2026-07-18
- 状态:待 review
- 作者:人类维护者 + agent(brainstorming 协作产出)
- 范围:hagent 六子系统中的**子项目 1**(总纲 charter + 双入口成文规范)。仅产出**文档 + 一个 PR 模板**,不实现 ③④⑤⑥ 的任何自动化/权限/标签机制。

---

## 1. 背景与目标

mbun 的 README 目标 #3 明确把「验证与探索 AI-agent 驱动的开发与协作」当作一项端到端实验。目前这套协作方式只散落在 `.agents/skills/`(`mcpp-style-ref` / `tdd-workflow` / `mbun-runtime-debugging`)、`changelog.md` 工作日志、conventional commits 约定与 CI 里,**没有对外成文,也没有被当作一个可复用的产物来组织**。

本设计把这套机制提炼成一个独立、可复用、可将来抽离成单独 repo 的**「AI 时代开源协作系统/规范」**,命名为 **hagent**(human-agent,人-agent 协作),诞生于 mbun 并首先在 mbun 内 dogfood。

**hagent 是什么(一句话)**:一套面向 AI 时代开源的协作系统/规范——**agent 是执行引擎,人是构建者/创作者**;把「一个贡献从提出到 merge」的端到端流程、角色分工、受保护面与证据/署名约定成文,使人与 agent 能在大型 codebase 上一致协作。

## 2. 核心命题(hagent 立身之本)

**分工按工作性质,不按数量。**

- **Agent = 执行引擎**:实现、测试、PR 互审、常规改动的 merge —— 占吞吐的 90%+。
- **人 = 构建者 / 创作者**:**架构、决策、品位(taste)、规范(norms)**,以及少数**特殊的细节设计 / 优化**。人是「参与构建与创作」,而非单纯守门人。
- **受保护面 / 宪法** = 划定人的创作域的机制(规范、定架构的规则、agent SOP、CI)。它存在不是为了卡流程,而是因为那些正是人的判断与品位所在。
- **90%+ 是结果,不是定义**——执行占了体量的绝大部分,于是自然落到 agent 身上。

这条命题同时约束 charter 的「角色」与「分工」两节:分工从"数量分成"改写为"**执行 vs 构建/创作/品位/规范**"的性质分工。

## 3. 决策记录(brainstorming 已确认)

| # | 决策 | 结论 |
|---|------|------|
| D1 | 面向对象 | 两者都要,**分层**:对外贡献门槛/审查 + 对内人机协作操作 |
| D2 | 公私边界 | 单仓、全公开;`docs/README` 的"私有副本"声明为预留,不影响本设计 |
| D3 | 入口结构 | `CONTRIBUTING.md` + `AGENTS.md` 双入口 + 极薄 `CLAUDE.md` 指针 |
| D4 | 新立协作机制 | 署名/来源可追溯、人工审查边界、共享记忆/多 agent 协调、验收证据强制(四者全选) |
| D5 | 打包形态 | 当成**独立协作系统产品**,放专属目录 `hagent/`,有 README 门面 + 链接索引;将来可抽离成 repo |
| D6 | 系统名 | `hagent/`(human-agent) |
| D7 | 语言 | 英文主 + `*.zh-CN.md` 镜像(与 README 一致) |
| D8 | 起点 | 六子系统分解认可;先做子项目 1(总纲 + 双入口文档),③④⑤⑥ 仅在 roadmap 点名 |

## 4. 六子系统分解与本子项目边界

| # | 子系统 | 性质 | 本子项目 |
|---|--------|------|:---:|
| ① | 协作范式总纲(charter) | 文档/政策 | ✅ 实现 |
| ② | 双入口成文规范(contributing + agents) | 文档 | ✅ 实现 |
| ③ | 受保护面 & 双向人审(CODEOWNERS/分支保护) | 机制 | 🗺️ roadmap 点名 |
| ④ | 标签系统 | 机制 | 🗺️ roadmap 点名 |
| ⑤ | 身份与权限模型(bot 账号/授权分层) | 机制 | 🗺️ roadmap 点名 |
| ⑥ | 自动化工作流(CI 闸门/自动 merge) | 自动化 | 🗺️ roadmap 点名 |

**依赖顺序**:①② 是脊椎(定义词汇与规则),③④⑤⑥ 都在执行 ①② 定下的决定。故 ①② 先行。

## 5. 文件布局

```
hagent/                          # 协作系统"产品"根;将来可整体抽离成独立 repo
  README.md                     # 门面:系统介绍 + 组成索引/链接 + 状态
  README.zh-CN.md
  charter.md                    # ① 总纲(宪法)
  charter.zh-CN.md
  contributing.md               # ② 对外层
  contributing.zh-CN.md
  agents.md                     # ② 对内层(工具中立 agent 入口)
  agents.zh-CN.md
  roadmap.md                    # ③④⑤⑥ 点名 + 状态
  roadmap.zh-CN.md

# 根目录极薄 stub(仅供工具/GitHub 自动发现,一行转发,不重复内容):
CONTRIBUTING.md                 # → hagent/contributing.md
AGENTS.md                       # → hagent/agents.md
CLAUDE.md                       # → hagent/agents.md
GOVERNANCE.md                   # → hagent/charter.md
.github/PULL_REQUEST_TEMPLATE.md
```

**要点**
- 内容(产品)全在 `hagent/`;根 stub 只做工具发现,一行转发。
- 后续 ③④⑤⑥ 每个机制子项目往 `hagent/` 加一个 `.md`(如 `protected-surfaces.md` / `labels.md` / `permissions.md` / `automation.md`),系统自然生长。
- 中英镜像命名与 README 一致(`*.zh-CN.md`)。英文为权威本,zh-CN 镜像顶部标注"以英文本为准"。

## 6. 各文件内容规格

### 6.1 `hagent/README.md` — 门面
- **它是什么**:第 1–2 节的核心命题浓缩。
- **谁在用**:诞生于 mbun(goal #3 实验),dogfood 中;声明可被其他项目复用/抽离。
- **组成索引**:charter / contributing / agents / roadmap 的一句话简介 + 链接;回链 mbun 的 `.agents/skills/`。
- **状态**:experimental;标注 ①② 已落地、③④⑤⑥ 规划中(链 roadmap)。

### 6.2 `hagent/charter.md` — ① 总纲(宪法)
1. **角色与信任层级**:人类维护者 / agent 贡献者 / agent 审查者 /(未来)授权 bot 账号——各自能做/不能做什么(执行 vs 构建/创作/品位/规范)。
2. **分工原则(执行 vs 创作)**:常规改动 agent 全权(含互审 + merge);人拥有架构/决策/品位/规范 + 少数特殊细节。90%+ 为结果。
3. **受保护面(强制双向人审)**:见 §8 清单。改动 = "宪法修正",双向都要人类签字。
4. **贡献生命周期(端到端)**:见 §7 流程图。
5. **验收底线**:引 `tdd-workflow` —— `compat/` 上游只读、不得弱化断言语义、无证据不得声称完成。
6. **路线图指针** → `roadmap.md`(③④⑤⑥)。

### 6.3 `hagent/contributing.md` — ② 对外层
面向外部人类 + 其 agent:
- setup(链 README quick start);
- 何谓好贡献:scope、conventional commits、TDD、一 PR 一开发项;
- 验收底线:别碰 `compat/` 语义;测试一律经 `tools/integration/` 沙箱(`safe-test.sh` / `bounded_run`);
- 证据要求(链 PR 模板);
- 署名怎么打(§6.6 trailer);
- 审查 & merge:agent 互审为常态,何时触发人审(受保护面 / 高风险);
- 链向 `agents.md` 与 `charter.md`。

### 6.4 `hagent/agents.md` — ② 对内层(工具中立 agent 入口)
- **skill 导航(何时读)**:`mcpp-style-ref`(写/审 C++ 模块)、`tdd-workflow`(开发任何功能/修复)、`mbun-runtime-debugging`(测试跑不过/崩溃)。
- **硬规则**:bounded 沙箱(`safe-test.sh`/`bounded_run.py`)、`compat/` 只读、无证据不得声称完成、conventional commits、changelog 协议。
- **受保护面提醒**:不得自 merge `hagent/` / `.agents/` / `.github/` / `CODEOWNERS` / `LICENSE`。
- **署名 trailer 格式**(§6.6)。
- **协调/共享记忆**:`changelog.md` 为共享工作日志与交接协议——如何拿任务、如何记录进展供下一个接手、如何避免并发冲突。
- 链向 `charter.md`(权威)与 `contributing.md`。

### 6.5 根 stub(4 个,各一行转发)
- `CONTRIBUTING.md` / `AGENTS.md` / `CLAUDE.md` / `GOVERNANCE.md`:每个仅一句 "This project's <X> lives in [`hagent/<file>`](hagent/<file>)." 不重复内容,避免双真相源。

### 6.6 署名 / 来源可追溯约定
**commit trailer**(附于 conventional-commit body 末):
```
Co-authored-by: Claude (Opus 4.8) <noreply@anthropic.com>
Contribution: agent-authored     # agent-assisted | human-authored
```
- `Co-authored-by`:GitHub 可渲染的 agent 身份(模型 + 工具)。
- `Contribution`:三态分类,供事后度量"多少由 agent 产出"。

### 6.7 `.github/PULL_REQUEST_TEMPLATE.md` — 验收证据 + 署名
纯模板(无自动化,本子项目不写 CI 校验),四块:
```
## What & why

## Acceptance evidence   (声称测试/行为变化时必填)
- Suite: <file/group> — before x/y → after x'/y'
- Reproduce: <runner 命令 或 compat/data 链接>

## Provenance
- [ ] agent-authored  - [ ] agent-assisted  - [ ] human-authored
- Co-authored-by / model & tool:

## Protected surface?
- [ ] 触及 hagent/ / .agents/ / .github/ / CODEOWNERS / LICENSE → 需人类 review
```

### 6.8 `hagent/roadmap.md` — ③④⑤⑥ 点名
每个子系统一段:一句话定义 + 依赖 + 状态=planned。明确它们将各自走一轮 spec → plan → 实现,并往 `hagent/` 加对应 `.md`。

## 7. 贡献生命周期(charter 用图)

```
提出/认领 ──▶ 建分支 ──▶ TDD(red→green) ──▶ 收集证据 ──▶ 开 PR(模板)
                                                              │
                                                              ▼
                                        agent 互审 ──▶ 闸门检查(证据/风格/回归)
                                                              │
                                    ┌─────────────────────────┴───────────────┐
                                    ▼                                          ▼
                         非受保护面 且 绿                             受保护面 或 高风险
                                    │                                          │
                                    ▼                                          ▼
                         agent 可自 merge                             强制人类 review/签字
                                    │                                          │
                                    └─────────────┬────────────────────────────┘
                                                  ▼
                                    merge ──▶ 记 changelog + 署名 trailer
```

> 本子项目只把该流程**写进 charter**;闸门/自动 merge/受保护面拦截的**自动化实现属于 ③⑥**,不在此。

## 8. 受保护面清单(强制双向人审)

改动以下路径 = 宪法级变更,agent 不得自 merge,须人类签字(双向:agent 提出要人批;人类提出也留痕):

- `hagent/**` —— 规范本身
- `.agents/skills/**` —— agent SOP
- `CONTRIBUTING.md` / `AGENTS.md` / `CLAUDE.md` / `GOVERNANCE.md` 根 stub
- `.github/workflows/**` —— CI
- `CODEOWNERS`(未来)、`LICENSE`

> 本子项目仅以**文字**在 charter 声明该清单;`CODEOWNERS`/分支保护的**强制**属于子系统 ③。

## 9. 语言与镜像约定

- 每个 `hagent/*.md` 配 `*.zh-CN.md` 镜像;英文为权威本。
- zh-CN 镜像顶部一行:"以英文本 [`<file>.md`] 为准。"
- 根 stub 只英文(工具/GitHub 读),不做镜像。

## 10. 本子项目的验收标准(何谓"done")

- [ ] `hagent/` 下 10 个文件(README/charter/contributing/agents/roadmap × 中英,charter 图与受保护面清单齐全)成稿。
- [ ] 4 个根 stub 各一行转发,链接正确、无内容重复。
- [ ] `.github/PULL_REQUEST_TEMPLATE.md` 四块齐全。
- [ ] README(项目根)在合适处加一句指向 hagent(可选,视 review)。
- [ ] 所有内部链接可达;中英镜像结构对齐。
- [ ] 不含任何 CI/CODEOWNERS/标签/权限的**实现**(仅 roadmap 点名)。

## 11. 非目标 / YAGNI(本子项目明确不做)

- ❌ 不实现 CODEOWNERS / 分支保护 / 自动 merge / 标签 / bot 账号 / CI 闸门(= ③④⑤⑥)。
- ❌ 不改动 `.agents/skills/` 现有内容(仅在 agents.md 里导航链接)。
- ❌ 不引入署名/证据的**自动校验**(PR 模板是提示,非强制门)。
- ❌ 不做把 hagent 抽离成独立 repo 的实际拆分(仅在布局上为将来抽离留好边界)。

## 12. 后续

本 spec 通过后 → 逐文件落地(至顶向下:README → charter → contributing → agents → 根 stub → PR 模板),每步可 review。③④⑤⑥ 各自后续再开 spec。

---

## 13. 修订(2026-07-18,/goal 补充)

以下覆盖前文相应部分。

**R1 改名 `aioss` → `hagent`(human-agent)**。含义从 "AI-era OSS" 改为「人-agent 协作」;`README.md` 为顶层门面。

**R2 结构精简(覆盖 §5)**:`hagent/` 下 **4 个概念文件、无多层目录**,各配 `*.zh-CN.md` 镜像(共 8 文件,英文为权威本)。
```
hagent/
  README.md       # 顶层门面 + 核心命题 + 归属原则 + 索引 + 状态
  charter.md      # 宪法:角色 / 执行-创作分工 / 归属原则 / 权限层级 / 受保护面 / 生命周期 / roadmap
  contributing.md # 对外:参与方式 + commit·署名规范(详例)+ 证据 + 沙箱底线
  agents.md       # 对内:agent 操作入口(skills 导航 / 硬规则 / 协调 / 署名)
```
根 stub(`CONTRIBUTING.md`/`AGENTS.md`/`CLAUDE.md`/`GOVERNANCE.md`)+ `.github/PULL_REQUEST_TEMPLATE.md` 不变。**roadmap 与 permissions 折入 charter**,不单独成文(保持 4 文件)。

**R3 归属一律归人(覆盖 §6.6)**:所有操作——即使由 agent 执行——**主作者/主签名恒为人**(commit `Author` = 人的邮箱;`Signed-off-by` = 人)。agent 以 **co-author** 记录对应模型。commit 规范:
```
<type>(<scope>): <标题>

<详细描述>

- #<issue>            # 相关 issue;可多行,GitHub 自动展开(关闭用 Closes #n)

Signed-off-by: <人名> <human@email>
Co-authored-by: <Agent 名> (<model>) <官方 no-reply 邮箱,或留空>
```
主签名=人;co-author=agent+模型(**无 `Agent:` trailer**)。各工具(Claude/Codex/Cursor/OpenCode/GitHub Copilot…)的 `Co-authored-by:` 写法列在 contributing 表;无官方邮箱统一留空 `<>`;相关 issue 用 `- #<n>` 列表(可多行)。

**R4 权限层级(新增,折入 charter)**:
- **触发权限(trigger)**:agent 可**自助申请**;资格门槛 = 该账号已有 **10 个已合入 PR**。
- **写权限(write / push·merge)**:**必须仓库维护者审核批准**。
- 二者为子系统 ⑤ 的精简先行版;完整自动化仍属 ⑤。

**R5 语言**:英文为权威本,4 概念文件各配 `*.zh-CN.md` 镜像(镜像顶部标注"以英文本为准")。

**R6 根 README(落实 §10 开放点)**:`README.md` 与 `README.zh-CN.md` 在「兼容性数据」与「相关项目」之间新增「Contributing & collaboration — hagent / 贡献与协作」章节:核心命题一句 + 指向 hagent README/charter/contributing/agents 的核心链接。

**R7 commit(落实 §10)**:`hagent/**` 为受保护面(charter §5),agent 不自 commit/merge——本批文件由维护者 review 后再提交。

**R8 名称**:系统对外名定为 **「AI Agent 开源协作规范」**(英文 "AI Agent open-source collaboration spec");`hagent` 保留为短名/目录名(= human + agent)。README/charter 标题与根 stub 措辞统一到此名。

**R9 中文目录**:zh 镜像从 `hagent/*.zh-CN.md` 迁到独立目录 **`hagent/zh/*.md`**;英文留在 `hagent/`。所有跨文件相对链接相应修正(zh 内互链同目录直连;回链项目根/`.agents`/`.github` 用 `../../`;英文/中文互链用 `../`)。目录树:
```
hagent/
  README.md  charter.md  contributing.md  agents.md      # 英文(权威),README 顶层
  zh/
    README.md  charter.md  contributing.md  agents.md    # 中文镜像
```

**R10 根 README 章节(覆盖 R6)**:章节移到**「兼容性数据」之前**,标题更名为「Contributing & collaboration — AI Agent open-source spec (hagent) / 贡献与协作 —— AI Agent 开源协作规范」,并新增**「用 agent 参与贡献」三步**:①启动 agent(自动加载 `AGENTS.md`+skill,已知规范,无需粘贴提示词)②挑任务(自选 or 问 agent 推荐,agent 知偏好则荐相关领域)③开发/验证/review agent 产出(开 PR 前确保清楚它做了什么)。

**R11 标签系统(模块 ④,已提前落地)**:一版**简化双语标签**(命名 `中文 | English`),构建者与其 agent 均可识别/自助打。共 **10 个**,4 组:
- 类型 4:`特性|feature` `缺陷|bug` `测试|test` `文档|docs`
- 来源 2:`AI 主导|agent-led` `构建者主导|builder-led`(hagent 度量轴)
- 流转 3:`待认领|available` `待维护者审|needs-maintainer`(维护者签字)`需修改|changes-requested`
- 守卫 1:`受保护面|protected`(charter §5)

已用 `gh` 配置到 `Sunrisepeak/mbun`(10 个);默认标签**保留未删**(删除需另行确认)。文档落地为 `hagent/labels.md` + `hagent/zh/labels.md`,charter §8 表 ④ 改 ✅。

**R12 术语:人/人类 → 构建者;签字权 → 维护者(覆盖 R3 及相关)**:
- 人机分工中的「人」定名为 **构建者 / builder**(创作/判断/署名/问责的主体);治理签字主体为 **维护者 / maintainer**(拥有治理权的构建者)。全部 hagent 文档与根 README 的 hagent 章节据此统一(goal #3 顶层叙述保留 "humans/人" 不改,避免术语前置)。
- **删除 `须读懂 | must-understand` 标签**:「构建者必须读懂自己提交的 diff」改为**默认要求**,写入 charter §3(不再是单独标签)。
- 标签改名:`人主导|human-led`→`构建者主导|builder-led`;`待人审|needs-human`→`待维护者审|needs-maintainer`。charter §5 标题改「受保护面(须维护者签字)」,labels.md 锚点同步。

**R13 参与贡献分层 + 维护者表 + 权限角色 + 新增 skill(覆盖 R4/R10)**:
- **README「贡献与协作」拆两大段**:①**参与贡献**——三类:`报告与讨论` / `验证与审查` / `开发`(不同层级开发者各取所需,均经 agent);②**项目维护者**——角色表(Triager / Committer / 模块维护者 / 项目维护者 + 权限 + 申请条件 + 持有者),目前仅 `@sunrisepeak`。中英同步。
- **权限模型改版(覆盖 R4 的 trigger/10PR)**:`trigger(≥10 PR)` → **Triager**(分诊/打标签/验证/跑 CI,**≥3 已合入 PR 自助**);写权限 = **Committer**(相关模块背景 + 重要贡献 + 维护者批准);**模块维护者 / 项目维护者**为更高层级,**待完善**。charter §4 与 contributing「获取权限」同步。
- **新增两个 skill**(`.agents/skills/` 为受保护面,经维护者指示添加):
  - `issue-reporting`:提问 SOP——环境/版本、报错、初步分析、相关资料 + **去隐私**(用户名/token 替换),含可复制模板。
  - `dev-process`:issue 先行,按 `bugfix / 优化 / 新功能` 分流;新功能须 issue 充分讨论 + 在 `.agents/docs/` 落地设计方案;衔接 tdd-workflow 与 本体验证/测试/CI/PR。
  - 新建 `.agents/docs/`(设计方案目录)+ README。
- 受保护面口径统一:PR 模板与 dev-process 收窄为 `.agents/skills/**`(与 charter §5 一致,`.agents/docs` 不受门控)。

**R14 目标分支**:PR 一律提交到 **`rewrite_bun_in_mcpp`** 分支,**不得合入 `main`**。已标注于 contributing(中英)、agents(中英)、`dev-process` skill §5、PR 模板。
