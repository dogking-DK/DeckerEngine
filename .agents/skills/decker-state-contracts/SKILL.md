---
name: decker-state-contracts
description: "Define and review state changes, commit points, and failure behavior in DeckerEngine scene edits, transactions, persistence, resource/cache changes, or task lifecycles. Use when an operation changes owned state or has partial-failure risks; skip ordinary formatting, IDE grouping, and unrelated pure calculations."
---

# 状态变更与失败处理

contract 指可观察的行为约定。此 skill 用于找出本次操作必须保持的性质，
具体操作的约定仍归属 [模块设计](../../../spec/design/README.md) 和相应行为测试。
先读取实际模块设计与实现，只检查本次改动涉及的状态，不自动发起全工程审查。

## 开发或审查时

1. 列出操作会修改的状态及所有者：例如实体内容、revision/dirty、历史、文件、缓存或任务状态。
   区分持久身份与会话/运行时身份，明确调用者是否可以持有会失效的引用。
2. 明确成功后的变化、无变化输入的含义，以及拒绝参数/业务失败后的状态。
   列出需要保证的失败类别；不能把文件、内存、OOM 和进程中断混为同一种可恢复失败。
3. 找到真正的提交点：校验/计算/必要分配完成后才发布状态，避免失败留下部分修改。
   跨资源操作若只能提供部分保证，要在设计和调用文档中明确，不自动承诺事务或补偿。
4. 选择能够证伪约定的最小案例：正常变化、无变化（适用时）、关键中途失败、相关数值/容量边界。
   断言结果和状态，不固定实现细节；只新增缺失且有意义的测试。
5. 设计变化写回原模块设计，具体缺陷保留回归用例。审查结果给出位置、触发条件、
   预期与实际状态；不要为每条命令新增一份契约文档。

只有涉及内存编辑/事务或持久化时，按需读取 [已有模式](references/state-patterns.md) 对应部分。
新增操作复用已有模式；出现新的可复用状态处理模式再扩展参考，不把历史 bug 清单塞进入口。
异步机制尚未实现时，只为当前开发明确终态、取消与发布的竞争、资源回收和退出行为，
不要将建议的状态机描述为已有功能。

交付给实现/验证的结果可以只是模块设计中的几行：
“修改哪些状态；成功如何变化；无变化如何处理；失败保留什么；在哪里提交；用哪些案例验证”。
验证范围由本次影响决定，可结合 [定向验证 skill](../decker-build-verify/SKILL.md)；
留档粒度仍遵循 [spec 规范](../../../spec/README.md)。
