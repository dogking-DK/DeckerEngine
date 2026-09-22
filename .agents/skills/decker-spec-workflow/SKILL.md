---
name: decker-spec-workflow
description: "Maintain DeckerEngine module designs and numbered development records before and during implementation, fixes, refactors, or build/dependency changes. Use in this repository; read-only questions do not require a new development record."
---

# DeckerEngine 开发留档

本 skill 适用于 DeckerEngine 仓库。以项目根目录为基准定位文件；
C++ 命名空间使用 dk，具体约定见根 AGENTS.md。

## 读取依据

先读 [spec 流程规范](../../../spec/README.md)，再读
[架构总览](../../../spec/design/architecture.md)、当前模块设计和最近关联开发记录。
流程细节、时间格式和编号规则以 spec/README.md 为唯一规范。

## 执行

1. 新模块在实现前从 [设计模板](../../../spec/templates/design.md) 创建
   spec/design/<module>.md；已有模块先检查并更新设计，覆盖当前边界、
   接口/数据、生命周期、取舍和验证计划。
2. 从 [开发模板](../../../spec/templates/development.md) 创建下一编号记录，
   或继续当前任务已有记录；填写创建时间、最后修改时间和设计链接，
   在 design/development 索引中登记后开始实现。
3. 开发过程中将实际文件变更、行为、原因、依赖调整、验证命令和结果落入记录；
   发现设计变化时先更新设计。保留创建时间，刷新实质修改时间。
4. 交付前同步设计、实现、文档状态和索引，明确通过/失败/未验证的项目，
   写下遗留事项和下一步，提供对应文档链接。

先设计不增加人工审批环节；设计足以支持当前已授权范围时继续开发。
不要为未开始的模块虚构完整设计或把预留能力记录为已实现。
只读分析不新增开发记录；纯文档修改按 spec 规范处理。

