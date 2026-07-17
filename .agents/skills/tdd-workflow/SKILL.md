---
name: tdd-workflow
description: 在 mbun 项目中开发任何功能或修复缺陷时使用。规定以 bun/node 原生测试集为验收标准的 TDD 开发循环、checkpoint 提交约定与记录流程。
---

# mbun TDD 开发流程

mbun 的核心开发约束：**bun / node 原生测试集是唯一验收标准，不得为通过测试而修改/弱化原测试语义**（`compat/` 下的上游语料是只读输入；可以做移植性适配，如路径、运行器桥接，但断言语义必须保持）。

## 开发循环（red → green → refactor）

1. **确定验收目标**：找到任务对应的上游测试文件（`compat/bun/test/` / `compat/node/test/parallel/`），或为纯逻辑模块圈定行为规范（如从 bun 参考源 `compat/bun/src/` 对照移植）。
2. **建立测试目标（red）**：
   - 纯逻辑模块：落地为 `modules/<member>/tests/test_<模块>.cpp`（成员目录内 `mcpp test` 自动发现），引擎级断言可直接翻译上游测试的期望值（参照 `modules/html_rewriter/tests/`）。
   - 运行时行为：直接以 `mbun test compat/bun/test/<file>` 的 pass/fail 为红绿信号（跑法与沙箱见 `mbun-runtime-debugging` skill）。
   - 确认新测试**失败**——失败原因必须是「功能未实现」，而非编译/环境错误。
3. **实现（green）**：按模块实现，遵循 `.agents/skills/mcpp-style-ref/SKILL.md` 编码规范（新功能优先独立 workspace 成员 + `.cppm` 模块），只写让测试通过所需的代码。
4. **重构（refactor）**：测试保持通过的前提下消除重复、理顺模块边界。
5. **回归验证**：改动波及的既有套件必须与基线对照（同文件改动前后 pass/fail 数一致或更好）；跑测一律经 `tools/integration/` 的资源受限沙箱，防 fork/泄漏冻机。
6. **checkpoint 提交**：
   ```
   <type>(<scope>): <subject>
   ```
   - type: `feat` / `fix` / `test` / `refactor` / `docs` / `chore` / `research`
   - 一个开发项至少一个 commit；红绿分离提交更佳（`test(scope): ...` 先行，`feat(scope): ...` 随后）。
7. **记录**：实质进展写入 `changelog.md`（带测试数据：套件 x/y → x'/y'，0 回归声明要有对照证据）；全量语料测量结果固化到 `compat/data/`（复现方法见 `compat/README.md`）；README「兼容性数据」表只放真实测得的数字。

## 禁止事项

- ❌ 修改 `compat/` 下上游测试的断言或删除测试用例来「让测试通过」
- ❌ 未真实跑过验收测试就声称任务完成（构建通过 ≠ 功能完成）
- ❌ 跳过 red 阶段直接写实现（无法证明测试有效）
- ❌ 一次 commit 混杂多个不相关开发项
- ❌ 裸跑可能 spawn/hang 的测试（必须经 safe-test.sh / bounded_run 沙箱）
