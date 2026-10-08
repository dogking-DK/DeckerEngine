---
module: geometry-query
created_at: "2026-10-08T11:35:00+08:00"
updated_at: "2026-10-08T12:06:32+08:00"
status: accepted
---

# CPU 几何查询

## 边界与数据

`engine/geometry` / `dk_geometry`（`dk::geometry`）仅依赖 Math/Core，不依赖 Scene、Assets、窗口或 GPU。
启用 Math 时构建。公开 Ray、Bounds、Triangle、MeshQuery；值/容器拥有数据，不借用资产内存。
MeshQuery 建立局部空间三角形 BVH（最长质心轴中位切分），保留原始三角形编号。
输入在候选构造时检查有限坐标；空网格合法，退化三角形不命中。

## 查询契约

Ray 的 direction 可以不归一化；参数 t 沿 origin+t*direction，范围为非负闭区间。
方向全零、非有限数据、倒置区间返回 invalid_argument。AABB slab 和双面三角形查询只读，
平行轴、盒内起点、边界命中有定义；最近命中相同 t 时按原三角形编号决胜。
实例查询将世界射线逆变换到局部，保留 direction 长度，保证非均匀/负缩放后的 t 可比较。
不可逆实例返回错误；编辑器跳过零缩放实例。世界射线归一化时 t 即世界距离。
首版不做纹理 alpha 测试或 GPU 可见性判定；使用双面几何最近命中。

## 验证

CPU 用例覆盖 slab 平行/边界、三角形正反面/退化、BVH 与暴力最近命中一致、
非均匀/负缩放实例的距离、不合法输入和空查询。原子构造失败不发布部分 BVH。
当前不承诺实时形变网格、BVH refit 或性能基线。

关联：[编辑器交互](editor-interaction.md)、[架构](architecture.md)、[0066](../development/0066-editor-interaction.md)。
