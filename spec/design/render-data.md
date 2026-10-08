---
module: render-data
created_at: "2026-10-03T15:06:44+08:00"
updated_at: "2026-10-08T12:06:32+08:00"
status: accepted
---

# RenderScene 与 RenderView

## M8.2 临时变换预览

RenderScene::extract 可接受单个 LocalTransformOverride（EntityId、本地 TRS），仅覆盖候选提取的数据，
后代世界变换使用候选父级。输入 Snapshot、revision、SceneDocument 和历史均不变。
未找到实体或无效 TRS 返回错误，不发布部分候选。调用者用单独 preview 序号标记临时画面，
不能将保留的 revision 当作已提交该变换的证据。

## 目标与边界

M7.1 从不可变 SceneSnapshot 提取独立、只读的渲染数据；不修改 SceneDocument、
revision、dirty 或资产状态，不读取磁盘，不持有 ECS 句柄。M7.2 才实现绘制管线。
engine/render/data 提供 dk_render_data / dk::render_data，公开依赖 Scene 和 Memory，
不依赖 Vulkan、Assets Runtime、Framework。DK_BUILD_RENDER_DATA 默认 OFF。

## 数据与接口

RenderScene::extract(heap, snapshot) 返回共享不可变所有者：SceneId、revision、按 EntityId
排序的 RenderEntity、每实体的原始 AssetReference 顺序。RenderEntity 保存完整双精度世界
仿射变换，父级组合保留剪切、负缩放和零缩放；不重新分解 TRS。迭代解析父级，避免递归深度限制。
实体名称和编辑层级不进入结果；无网格实体仍保留，方便稳定 ID 查询和未来可见性筛选。
资产引用只是引用列表，不将 material/texture 引用猜测为 mesh override，也不隐式加载资源。

RenderView::create(scene, ViewDescription) 绑定该场景版本和调用方 frame 序号、非零像素尺寸、
相机世界变换及投影矩阵。相机须可逆，投影须有限且可逆；视图保存 world-to-view 和
projection * world-to-view。投影由调用者按 Vulkan 深度 [0,1] 及目标 Y 约定提供，
本阶段无 Camera 持久组件、投影生成、裁剪或精度降为 float。

## 寿命与错误

RenderScene 可廉价复制；span/引用随最后一个所有者销毁而失效。RenderView 保留 Scene，
后续编辑、源快照销毁和 Memory 进入 closing 不改变已有提取值。新提取要求 open heap；
校验和所有分配在候选状态完成，失败不更改源文档或既有提取值。持久和临时容器均显式使用
传入 Memory；失败返回 Result，系统 Error 文本分配沿用 Core 约定。源快照读取及输出发布
由调用者同步；发布后不同线程只读。

## 验证与记录

验证多级世界矩阵/剪切、资产顺序、空场景、旧版本与源对象寿命、相机/尺寸非法输入、
预算失败及关闭后的只读访问。关联 [Scene](scene.md)、[Memory](foundation-memory.md)、
[0061](../development/0061-render-data-resources.md)。
