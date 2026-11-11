---
name: decker-spec-workflow
description: "Maintain DeckerEngine designs and development records for implementation, fixes, refactors, and build/dependency changes. For documentation maintenance, read only applicable rules. Skip read-only questions and planning."
---

# DeckerEngine 开发留档

本 skill 适用于 DeckerEngine 仓库。以项目根目录为基准定位文件；
C++ 命名空间使用 dk，具体约定见根 AGENTS.md。

## 读取依据

实现类任务先读 spec 规范的[开发记录粒度](../../../spec/README.md#开发记录粒度)和
[执行顺序](../../../spec/README.md#每次开发的执行顺序)，不要求整读 spec/README。
新建/更新文档元数据时补读[文件和元数据](../../../spec/README.md#文件和元数据)，
命令变更补读[命令文档](../../../spec/README.md#命令文档)；纯文档维护只取本次适用规则。

已知模块直接进入相关设计；未知时用关键词搜索[设计索引](../../../spec/design/README.md)对应行。
长文档先用 `rg -n` 找标题/关键词，再读取完整相关章节，默认不整读 Roadmap 或各类索引。
阶段任务只读目标 Mx.y 和必要前置；“下一步”先定位 Roadmap“当前执行边界与下一步”。
架构、关联历史和其他 skill 按[读取范围](../../../spec/README.md#文档职责与读取范围)中的触发条件补读，
不因入口存在链接而递归加载。已读且未变化的内容复用；范围、契约与验证入口明确后开始工作。
流程细节、时间格式和编号规则以 spec/README.md 为唯一规范；输出截断时缩小范围补读缺失部分。

## 执行

1. 新模块在实现前从 [设计模板](../../../spec/templates/design.md) 创建
   spec/design/<module>.md；已有模块先检查并更新设计，覆盖当前边界、
   接口/数据、生命周期、取舍和验证计划。
2. 按 spec 规范决定留档粒度：重要开发从 [开发模板](../../../spec/templates/development.md)
   创建编号记录；相关小改可合并到已有记录，简单补丁可免写开发日志。
   使用记录时维护创建/修改时间、设计链接和索引；不为每次小改机械新增编号。
3. 需要留档时将实际文件变更、行为、原因、依赖调整、验证命令和结果落入记录；
   发现设计变化时先更新设计。保留创建时间，刷新实质修改时间。
4. 交付前同步设计、实现、文档状态和索引；当前契约合入主题，历史证据留在开发记录，阶段状态由 Roadmap 维护。
   执行文档检查，明确通过/失败/未验证的项目，
   写下遗留事项和下一步；免写日志的补丁在提交说明和交付回复中说明变化及验证。

先设计不增加人工审批环节；设计足以支持当前已授权范围时继续开发。
不要为未开始的模块虚构完整设计或把预留能力记录为已实现。
只读分析不新增开发记录；纯文档修改按 spec 规范处理。

