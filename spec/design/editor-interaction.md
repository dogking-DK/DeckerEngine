---
module: editor-interaction
created_at: "2026-10-08T11:35:00+08:00"
updated_at: "2026-10-09T20:40:16+08:00"
status: accepted
---

# M8.2 选择、Gizmo 与编辑器相机

## 模拟取景（M12.2）

模拟视图拥有独立Camera；首次按规则布片宽深、初始高度和地面范围自动取景，不读取GPU粒子。
Frame、F、Home重新取景；右键环绕、中键平移、滚轮缩放沿用Camera限制。
输入仅响应模拟Image或已经捕获的导航，文本输入/模态框/失焦/最小化取消导航。
图像视图序号用于识别相机和尺寸；旧图可暂时显示但明确标记更新中。模拟图像不做实体选择或Gizmo。
切回Edit保留原相机、选择和历史，Stop后自然恢复编辑操作。

## 边界

CPU Camera 与变换手势进入 `dk_editor_model`；Geometry 提供独立射线/BVH。
UI 私有适配负责 ImGui 图像矩形、轴/环绘制、输入路由和 DiskScene 网格到 BVH 的转换。
沿用已有 ImGui/Eigen，不新增三方库。Gizmo 首版提供本地轴平移、旋转、缩放，
使用原始 TRS 及父级仿射变换计算，不对含 shear 的世界矩阵强行 TRS 分解。

## 选择与相机

鼠标坐标相对实际 Image 矩形换算为 [0,1]；投影复用 Vulkan Y 翻转、Z [0,1] 约定。
左键最近三角形命中选择 EntityId，空白清空；Hierarchy 与 Inspector 共用同一选择。
拾取使用成功发布图像对应的 Scene/CPU 网格/相机，过期或失败预览禁止交互。
实例世界变换包含父级和资产导入后的网格坐标；资产 BVH 按会话复用。
选中实体用包围盒和轴标记；没有网格的实体仍可从 Hierarchy 选择。

编辑器相机只属于 UI 会话，不写 Scene Camera、revision、dirty 或历史。
右键拖动环绕、滚轮缩放、中键平移；F 聚焦选择、Home/Reset 恢复默认相机。
距离和俯仰限制避免奇点；尺寸/相机改变使视口失效，绘制与拾取共享相机矩阵。
输入仅在图像区域或已捕获的拖动中处理；文本/模态框不触发快捷键。

## 手势状态与提交

Workspace::begin_transform 返回由 UI 拥有的独立 TransformEdit 候选：捕获 EntityId、document_id/revision、原始本地 TRS 和父世界变换。
Inspector 有未应用草稿时不能开始手势；手势期间禁止 Inspector/选择/历史/保存/换场景。
UI 在手势期间禁用其他编辑入口；Workspace 提交仍复查草稿/选择/guard，其他调用者不依赖 UI 禁用保证。
移动只更新候选和预览序号；RenderScene 提取支持单实体只读 local override，后代也继承预览变换。
这不修改 Runtime 文档、revision、dirty 或历史；UI 明示 Transform preview。
松开一次提交已有 entity.set_transform（guard），产生一个撤销单元；原值无变化不发命令。
Esc、焦点丢失、最小化、图像矩形变化、关闭请求、图像不可用取消候选；原生事件在暂停 NewFrame 前取消，避免恢复时误提交。
失败提交清理候选、保留原文档并显示错误。
过期 guard 不覆盖后来编辑。父级不可逆、非有限/非法 TRS 在开始或更新时拒绝。
旋转在本地轴后乘四元数，缩放只改变该轴数值；平移通过父级逆变换转换世界位移。
平移/缩放使用投影轴屏幕位移，旋转使用射线/局部环平面交点；端视不足 15px 的平移/缩放轴隐藏，环绕后可操作。

## 验证与范围

CPU 验证投影/射线对应、相机奇点/无文档副作用、父级缩放旋转、取消/无变化、
过期提交、单次历史、撤销重做、保存重载；Geometry 单独测试。
真实窗口 smoke 注入拾取、三种 Gizmo、取消与相机输入，检查 preview/commit 像素与 revision，
最后保存、重载和 runner 读取同一状态；保留 M8.1 smoke。
默认 Sponza 做实际窗口预览，GPU validation 必须零告警/错误；CPU-only 模型独立构建。
M8.3 IPC、纹理 alpha 精确拾取、多选、世界轴 Gizmo、吸附和飞行相机不在本阶段。

关联：[工作台](editor.md)、[几何查询](geometry-query.md)、[Render 数据](render-data.md)、[0066](../development/0066-editor-interaction.md)。
