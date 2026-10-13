---
id: "0101"
created_at: "2026-10-13T12:48:00+08:00"
updated_at: "2026-10-13T13:12:00+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/scene.md
  - ../design/application-services.md
  - ../design/graphics-resources.md
  - ../design/graphics-vulkan.md
  - ../design/render-pipeline.md
---

# 0101 M15.1 确定性云场与版本化参数

## 目标与设计依据

完成[云场设计](../design/render-clouds.md)的CPU参数、持久化、seed天气图和3D噪声、
显式时间风平移及GPU纹理核验。积分/云光照/合成属于后续阶段。

## 实际变更

- [Environment](../../engine/scene/include/dk/scene/Environment.hpp)增加固定大小CloudParameters，
  全字段有限范围/球壳约束、seed/version/resolution整数约束；Scene快照、RenderScene、历史和保存继承完整值。
  DeckerScene保持v2容器，environment写v2，clouds字段全部必填；旧Scene/environment v1迁移disabled云。
  小尺度旧大气迁移时收缩默认云壳。命令仅接受v2完整对象，避免旧完整替换静默清除云。
- [CloudFields](../../engine/render/clouds/src/CloudFields.cpp)提供整数确定性生成器v1、周期RG8天气coverage/type、
  R8基础/细节3D噪声、FNV-1a64摘要、CPU过滤采样与球壳密度剖面。风由显式秒数解析平移；
  coverage=0/density=0/disabled/壳边界精确零。有限坐标先做周期归约，覆盖最大风/时间与最小周期组合。
  默认128² RG8、128³ R8、32³ R8，总计2,162,688字节。
- [CloudTextures](../../engine/render/clouds/src/CloudTextures.cpp)上传天气和3D纹理，使用repeat/linear单mip采样；
  硬件format/filter/type能力由Device核验。纯CPU `dk::render_cloud_fields`只依赖Scene，
  可选GPU `dk::render_clouds`额外依赖Device，默认选项OFF，graphics/editor预设开启GPU模块。
- 两个cache先检查预算，再构建候选，最后发布；GPU上传完成才更新current/generation。
  默认最多2个活代，CPU 8MiB、GPU 16MiB逻辑预算（GPU含候选staging）。外部快照保活旧代；
  GPU cache还保留退休代次，直到队列无在途工作/其他录制批次且外部快照释放，避免只保活Image时漏计预算。
  key仅含seed/generator/resolution；密度、coverage、风和时间不触发纹理重建。
- [ResourcePolicy](../../engine/graphics/device/src/ResourcePolicy.hpp)增加R8/RG8 UNORM 1/2字节格式支持，
  复用现有带pitch、3D slice、mip和readback路径；不扩展格式替换或新导入器。
- 命令schema/参考/静态CPU示例与截图provenance同步environment v2。
  编辑器大气草稿操作保留云字段。ScenePipeline::render_environment在clouds.enabled时返回not_supported，
  避免把未合成云的截图声明为完整环境结果；显式unlit/大气诊断入口保持原语义。

## 验证记录

Windows/MSVC Debug；本次不更改vcpkg baseline或依赖。两个构建树配置：

```powershell
cmake --preset windows-scripting -DDK_BUILD_RENDER_CLOUD_FIELDS=ON
cmake --preset windows-editor
```

scripting配置Graphics Device/Graph/Shaders及GPU renderer均OFF，CPU环境命令和云场可独立构建。
GPU验证使用已有RTX 4070 Laptop，required validation与sync validation开启；仅对子进程临时设置
`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`规避已知AMD隐式层冲突，finally恢复环境变量。
没有启动前台编辑器窗口。

实际执行下列verify范围（均经scripts/verify.ps1；省略的Configuration默认为Debug）：

| 构建目录 | Target | TestRegex | 结果与证据目录（out/verify/） |
| --- | --- | --- | --- |
| windows-scripting | dk_cloud_field_tests, dk_scene_tests, dk_service_tests, dk_run | `^dk\.(clouds\.\|scene\.environment\|services\.environment\|environment\.process$)` | 11/11通过；20261013-125557-241941dc |
| windows-editor | dk_cloud_field_probe, dk_environment_preview_tests, dk_editor_tests, dk_render_data_tests, dk_protocol_tests | `^dk\.(clouds\.fields_gpu_validation$\|editor\.(workspace environment\|environment_worker_gpu_validation)\|render\.data\.render environment\|protocol\.capture import failure)` | 首轮5通过/1失败；云场缺R8/RG8格式，已修复；20261013-125918-95ecdb57 |
| windows-editor | dk_cloud_field_probe, dk_graphics_resource_tests, dk_volume_image_probe | `^dk\.(clouds\.fields_gpu_validation$\|graphics\.(unit\.(cloud UNORM\|image descriptions\|image subresource\|volume descriptions)\|volume_validation$))` | 修复后6/6通过；20261013-130400-e4ff7107 |
| windows-editor | dk_cloud_field_probe | `^dk\.clouds\.fields_gpu_validation$` | 退休代计费补强后1/1通过；20261013-130637-f042db0e |
| windows-scripting | dk_cloud_field_tests, dk_run | `^dk\.(clouds\.cloud \|environment\.process$)` | 固定摘要、coverage单调、启用云跨进程保存重载4/4通过；20261013-130637-7ec1818b |
| windows-scripting | dk_cloud_field_tests | `^dk\.clouds\.cloud ` | 最大风/时间/最小周期组合3/3通过；20261013-130904-4084b36f |

表格中的正则竖线为alternation，完整可复制命令参数保存在对应summary.json。
最终22个不同用例通过、零跳过。保留首次失败结果，不用旧二进制替代失败后的验证。

主要证据：

- 小尺寸generator v1独立整数oracle摘要`873bb341ee4ddebd`；默认seed=1摘要`c859ef6a9ec30856`，
  seed=3摘要`1a0210f923a560e9`。生成器固定字节有golden门槛，变更需升级版本。
- 默认全部天气/3D字节GPU readback一致；GPU bilinear/trilinear/repeat样本最大绝对误差
  `0.000069868`（seed=1）、`0.000045211`（seed=3），均低于事先测试门槛0.002。
- required GPU验证errors=0、warnings=0，VMA资源及Memory live allocations均归零；
  提交失败注入保持旧current/hash/generation及分配数，旧快照仍可采样；预算拒绝、外部释放但仍录制引用、
  外部/录制释放后重试、跨队列cache hit拒绝均通过。
- CPU检查非法disabled值、整数溢出/非整数/未知字段、旧格式、snapshot/事务/guard/undo/redo/no-op/历史预算，
  runner真实commands.list/describe，修改并独立进程重载enabled云；截图provenance与query环境完全相同。
- 编辑器草稿保存/重载保留云字段；环境后台预览、旧帧/失败/取消/关闭及enabled云明确拒绝通过。
- `scripts/check-spec.ps1`检查修改文档的元数据、链接、表格、索引和测试入口；`git diff --check`通过。

未运行：全量套件、Release矩阵和云性能基线，本次未改变优化算法或声明实时性能达标。
GPU纹理数值测试不是云光照/积分质量验收，未声称已有云画面。

## 偏差与决策

不新增三方库。沿用Scene v2容器，由environment v2描述新增内容；命令不接受旧完整对象，磁盘保留迁移。
原底层支持RGBA类和R32格式，真实上传发现R8/RG8缺口后同步设计并补齐所需格式及传输测试。
CPU场和GPU纹理分拆，保持无GPU参数/场测试可用；相函数和光照参数由实际积分阶段定义。
原弱引用代次账本补强为GPU退休代保活，覆盖调用者只让录制/提交保活Image的情况。
逻辑预算不等同显存硬上限；队列GPU工作/临时分配由底层拥有和验证，不承诺OOM或设备丢失后设备可继续使用。

## 遗留问题与下一步

M15.1无遗留阻塞。M15.2实现无历史球壳区间、Beer–Lambert积分、太阳散射/自遮蔽和低阶多散射参考；
随后M15.3接入云/空气/几何合成。当前程序化场为受控value-noise近似，尚未做云图像质量/性能验收。

## 修改记录

- 2026-10-13T12:48:00+08:00：先冻结参数、生成器、预算/发布契约及验证边界，创建记录。
- 2026-10-13T13:10:00+08:00：完成M15.1、R8/RG8修复及定向验证，更新Roadmap和命令参考。
