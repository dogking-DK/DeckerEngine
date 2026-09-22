---
name: decker-command-development
description: "Add, change, remove, or document DeckerEngine commands through Commands, Operations and Services. Keep schema, guard/effect/undo behavior, command discovery, reference pages and focused examples consistent. Use for command-facing changes, not general C++, math or CMake work."
---

# DeckerEngine 命令开发

从 [命令参考目录](../../../spec/commands/README.md) 找到相关功能页；
具体命令定义始终放在那里，skill 不保存第二份命令列表。
根据本次改动读取 [命令注册/schema](../../../spec/design/commands.md)、
[服务与操作](../../../spec/design/application-services.md)；仅涉及分派、任务或传输时再读
[Runtime](../../../spec/design/runtime.md) 和 [协议](../../../spec/design/automation-protocol.md)。

## 实现路径

1. 明确命令面向的用户行为、参数/必填项/默认值、返回值和错误；
   修改已有命令时检查调用者和持久化/协议兼容影响，避免无理由创建相同用途的新命令。
2. 业务逻辑由拥有状态的 Service 实现；Operations 负责 JSON/强类型转换及注册，
   Commands 复用 schema 校验和能力发现。通常直接复用 Runtime 与 JSON-RPC，
   不为每个新命令增加传输分支，也不让协议层直接修改 ECS。
3. 明确是否要求活动文档和 guard、effect 分类、是否进入撤销历史、是否允许进入事务。
   文件/其他外部效果不能自动声称可撤销；状态复杂时使用
   [状态契约 skill](../decker-state-contracts/SKILL.md) 核对成功、失败和提交点。
4. 参数与结果使用实际注册表的 schema；数据语义由服务校验，避免手写第二套通用校验器。
   只在受影响时核对 UUID、整数范围、Unicode、未知字段等边界。
   若要支持 scene.transaction，核对现有编辑解码、白名单及 schema 复用是否需要同步。
5. 新增/修改/删除命令时同步 spec/commands 对应页及目录，包含用途、参数、结果、
   前置条件、guard、effect/撤销、副作用、常见错误和有效示例；保持创建时间，更新修改时间。
   同步实际受影响的 examples/README/调用者，不复制模块内部设计到用户使用说明。

## 核对和定向验证

- 用本次构建的 runner 查询 commands.list，确认命令存在/删除及 effect/undoable；
  用 commands.describe 查询本次涉及命令的 parameters/result schema，对照文档。
  未构建或功能未开启时说明未核验，不能拿旧二进制冒充新注册结果。
- 查询可以在空测试工程中执行；修改/保存类示例使用独立 out 测试目录和实际 guard/TaskId，
  不把文档占位符原样发送，不默认批量执行整个命令目录。
- 选择该命令的成功行为和关键失败案例：参数校验、服务前置条件、状态保护等按实际风险选取。
  协议无变化时不要求整个协议套件；会话/流行为变化时增加相关进程案例。
  可用 [定向验证 skill](../decker-build-verify/SKILL.md) 执行目标测试。
- 纯命令文档改动只核对描述与受影响示例，不触发 C++ 全量回归。

先同步相关模块设计再实现；开发记录按 [spec 规范](../../../spec/README.md) 新建、合并或免写。
交付说明指出命令可观察变化、文档入口和实际验证范围，不把未完成的异步/网络能力写成已支持。
