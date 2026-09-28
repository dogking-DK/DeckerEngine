# 资产导入测试夹具

全部为本项目自制，无外部模型或图片内容。`generate.py` 使用 Python 标准库生成 PNG/glTF/GLB，
JPEG 使用 Pillow 11.1.0（无色度降采样、quality 95）；生成器不是引擎构建/测试依赖。

- rgba.png：2×2 RGBA，顶行红/绿，底行半透明蓝/白；检查通道、alpha 与行顺序。
- gray16.png：2×2 16 位灰度 PNG，检查拒绝隐式降精度。
- red.jpg：2×2 红色 JPEG，检查有损解码（允许舍入误差）。
- triangle.gltf + triangle.bin：交错 POSITION/UV 和 uint16 索引、基础材质、外部 rgba.png。
- triangle.glb：同样的网格，改为 bufferView 内嵌 red.jpg，单文件依赖。

`python tests/fixtures/assets/generate.py` 可重建全部夹具。测试复制到专用临时工程，源夹具不产生 meta。
关联：[0034 验收记录](../../../spec/development/0034-textures-assetc.md)。
