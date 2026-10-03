---
created_at: "2026-10-03T17:07:43+08:00"
updated_at: "2026-10-03T17:31:34+08:00"
---

# 默认测试场景素材

本目录集中保存默认 Sponza 场景、HDR 环境贴图和六面天空盒。
[defaults.json](defaults.json) 记录默认输入；其中所有路径均相对于本目录。
它是资源清单，不是引擎 Scene/Project 文件，也尚未被运行时自动加载。

| 用途 | 本地路径 |
| --- | --- |
| 默认 glTF 场景 | `gltf/Sponza/glTF/Sponza.gltf` |
| HDR 环境贴图 | `hdr/citrus_orchard_road_puresky_4k.hdr` |
| 天空盒六面与布局参考 | `skybox/sky_clouds_12_cubemap_(roblox)_2k/` |

Sponza 整个源目录保留，包含 .gltf、.bin、纹理、README 来源说明和截图。
天空盒保留 px/nx/py/ny/pz/nz.png 与 cubemap_layout.png，不修改像素、面命名或方向；
引擎采样时的面朝向转换由后续接入验证。
磁盘场景示例现使用 [project.json](../project.json) 和 [Sponza 场景](../scenes/sponza.scene.json)，
显式无光照预览并支持 alpha mask；HDR 与天空盒渲染尚未实现。现有程序化回归保持原入口，
详见 [Render 指南](../../../spec/guides/render.md)。

## 本机原始来源

- Sponza：`C:\code\data\glTF-Sample-Models-main\2.0\Sponza`
- HDR：`C:\code\data\texture\hdr\citrus_orchard_road_puresky_4k.hdr`
- 天空盒：`C:\code\data\texture\skybox\sky_clouds_12_2k\sky_clouds_12_cubemap_(roblox)_2k`

素材目录由本地 .gitignore 排除；Git 保存本说明、默认清单和忽略规则。
新 checkout 需要按上述对应关系复制素材，并保留目录内相对路径。
Sponza 的原始来源与许可说明保留在 `gltf/Sponza/README.md`；其他素材来源按用户给定本机文件记录。

## 本次完整性检查

共复制 82 个文件、74,106,764 字节（约 70.7 MiB），源与目标 SHA-256 全部一致。
Sponza 的 70 个外部 buffer/image URI 均在复制目录内解析成功，buffer 长度符合声明。
HDR 头部尺寸为 4096x2048；六张天空盒面各为 512x512，PNG 完整性检查通过。
复制校验明细保存于本机 `out/default-test-assets-copy.json`（从仓库根解析）。
本节只记录素材复制核对；磁盘导入和渲染验收另见 [M7.3 记录](../../../spec/development/0063-render-disk.md)。
