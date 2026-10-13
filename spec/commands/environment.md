---
module: command-reference-environment
created_at: "2026-10-12T15:10:00+08:00"
updated_at: "2026-10-13T13:12:00+08:00"
status: accepted
---

# 场景环境命令

[返回命令目录](README.md)。环境为场景级CPU状态，默认disabled，保存、事务与历史均包含它。
启用环境后，已开启大气模块的Edit视口和render.capture显示天空、Lambert日光/天空漫射和空气透视。
编辑器Environment面板复用这两条命令及同一撤销历史；参数/渲染支持域见[大气指南](../guides/atmosphere.md)。
实现：[环境模型](../../engine/scene/include/dk/scene/Environment.hpp)、
[Operations](../../engine/framework/operations/src/SceneOperations.cpp)。

## environment.query

参数为`{}`，可省略；需要活动场景，无需guard。effect=query、undoable=false。
返回`{state,environment}`，state使用[公共State](README.md)；环境完整对象见下表。
查询不修改revision/dirty/历史；无活动场景返回invalid_state。

```json
{"jsonrpc":"2.0","id":1,"method":"environment.query"}
```

## environment.set

必填`guard`和完整`environment`，不支持局部patch；先query、修改需要的字段，再提交整个对象。
需要活动场景；effect=memory_edit、undoable=true，可放入`scene.transaction`，批内params不带guard。
返回`{state,created_id:null}`；成功变化只增加一次revision，dirty=true，建立一个撤销单元。
规范化后完全相同的输入保持revision/dirty/redo；环境和实体混合事务最终无变化时也如此。
保存需另外调用`scene.save`；保存不清历史。new/load重置历史，load更换document_id。

错误：无活动文档为invalid_state；过期guard为conflict；缺失/未知字段、错误版本或参数越界为
invalid_argument；revision耗尽或历史单元超预算为invalid_state。失败保留内容、dirty及undo/redo。
disabled仍须通过全部参数校验，避免之后启用时出现无效状态。

环境JSON顶层恰含`version:2`（整数）、`enabled:boolean`、`atmosphere:object`、`sun:object`和`clouds:object`，
均必填、无隐含缺省字段；命令拒绝旧v1对象，请先query再修改。下面默认值用于新场景；
旧磁盘environment v1会迁移disabled云。大气/太阳向量为3个有限数值，风为2个。

| atmosphere字段 | 默认值 | 单位/支持范围 |
| --- | --- | --- |
| planet_center_m | [0,-6360000,0] | 米，分量绝对值≤1e9 |
| ground_radius_m | 6360000 | 米，[1,1e9] |
| atmosphere_height_m | 100000 | 地面至大气顶厚度，米，[1,1e7] |
| rayleigh_scattering_per_m | [5.802e-6,13.558e-6,33.1e-6] | RGB，m^-1，[0,1]，Rayleigh散射即消光 |
| rayleigh_scale_height_m | 8000 | 米，[1,atmosphere_height_m] |
| mie_scattering_per_m | [3.996e-6,3.996e-6,3.996e-6] | RGB，m^-1，[0,1]，逐通道≤mie_extinction_per_m |
| mie_extinction_per_m | [4.44e-6,4.44e-6,4.44e-6] | RGB，m^-1，[0,1] |
| mie_scale_height_m | 1200 | 米，[1,atmosphere_height_m] |
| mie_anisotropy | 0.8 | 无量纲，[-0.99,0.99] |
| absorption_extinction_per_m | [0.65e-6,1.881e-6,0.085e-6] | RGB，m^-1，[0,1] |
| absorption_center_height_m | 25000 | 米，[0,atmosphere_height_m] |
| absorption_half_width_m | 15000 | 米，[1,atmosphere_height_m] |
| ground_albedo | [0.3,0.3,0.3] | 线性RGB反照率，[0,1] |

Rayleigh/Mie采用指数密度`exp(-height/scale_height)`；吸收采用峰值为1的三角层，
`max(0,1-abs(height-center)/half_width)`。系数允许全零以表示真空；这些参数边界不是气象拟合承诺。

| sun字段 | 默认值 | 单位/支持范围 |
| --- | --- | --- |
| direction | [0,1,0] | 采样点指向太阳；有限非零向量，存储时归一化 |
| irradiance | [1,1,1] | 线性RGB场景归一化法向辐照度，各通道[0,1e6] |
| angular_radius_rad | 0.004675 | 角半径，弧度，[1e-6,0.1] |

右手Y-up局部坐标，距离全部为米；默认原点位于地面。CPU使用double，球面高度为
`length(position-planet_center_m)-ground_radius_m`，与现有平面网格不是同一种地形。
RGB使用线性sRGB/D65通道，irradiance不是lux或原始W/m²；正入射Lambert输出为`albedo*E/pi`，
均匀太阳盘radiance为`E/(pi*sin(angular_radius_rad)^2)`，与材质emissive共享曝光前标度。
曝光属于渲染请求，不进入环境。

| clouds字段 | 默认值 | 单位/支持范围 |
| --- | --- | --- |
| enabled | false | bool；云参数可CPU编辑，当前云渲染未实现，true时环境截图/预览返回not_supported |
| seed | 1 | uint32整数[0,4294967295]；固定seed重建一致 |
| generator_version | 1 | 整数，仅支持1 |
| weather_resolution | 128 | 2D方形边长，2的幂[8,256] |
| base_resolution | 128 | 基础3D体边长，2的幂[8,128] |
| detail_resolution | 32 | 细节3D体边长，2的幂[4,64] |
| bottom_height_m / top_height_m | 1500 / 4000 | 球面高度，米；0≤bottom<top<atmosphere_height_m |
| coverage | 0.5 | [0,1]；0精确无云 |
| density | 1 | 无量纲[0,10]；0精确无云 |
| extinction_per_m | 0.001 | m^-1，[0,1]；供后续积分使用 |
| weather_scale_m / shape_scale_m / detail_scale_m | 100000 / 10000 / 1000 | 周期长度，米；各[1,1e7] |
| bottom_fade_fraction / top_fade_fraction | 0.1 / 0.2 | 垂直渐变相对壳厚，各[.001,.5] |
| wind_velocity_mps | [10,0] | 世界X/Z平移速度，m/s；各[-1000,1000] |

云字段也全部必填。生成器key只有seed、generator_version和分辨率；调coverage/密度/风不会重建纹理。
时间是采样请求的显式秒数，不写入场景，也不由帧率累计；当前命令未提供云积分或序列截图。
旧environment v1迁移时，top=min(4000,大气厚度/2)，bottom=min(1500,top/2)，保持旧小尺度大气可读。

完整set调用示例（替换guard为query取得的最新state）：

```json
{"jsonrpc":"2.0","id":2,"method":"environment.set","params":{
  "guard":{"document_id":"<document_id>","revision":0},
  "environment":{
    "version":2,"enabled":true,
    "clouds":{
      "enabled":false,"seed":1,"generator_version":1,
      "weather_resolution":128,"base_resolution":128,"detail_resolution":32,
      "bottom_height_m":1500,"top_height_m":4000,"coverage":0.5,"density":1,"extinction_per_m":0.001,
      "weather_scale_m":100000,"shape_scale_m":10000,"detail_scale_m":1000,
      "bottom_fade_fraction":0.1,"top_fade_fraction":0.2,"wind_velocity_mps":[10,0]
    },
    "atmosphere":{
      "planet_center_m":[0,-6360000,0],"ground_radius_m":6360000,"atmosphere_height_m":100000,
      "rayleigh_scattering_per_m":[5.802e-6,13.558e-6,33.1e-6],"rayleigh_scale_height_m":8000,
      "mie_scattering_per_m":[3.996e-6,3.996e-6,3.996e-6],"mie_extinction_per_m":[4.44e-6,4.44e-6,4.44e-6],
      "mie_scale_height_m":1200,"mie_anisotropy":0.8,
      "absorption_extinction_per_m":[0.65e-6,1.881e-6,0.085e-6],
      "absorption_center_height_m":25000,"absorption_half_width_m":15000,"ground_albedo":[0.3,0.3,0.3]
    },
    "sun":{"direction":[1,0.1,0],"irradiance":[1,1,1],"angular_radius_rad":0.004675}
  }
}}
```

可直接执行[CPU环境示例](../../examples/automation/environment.jsonl)，包含创建、完整设置、查询、
undo/redo及保存；重启后load保留规范化环境。DeckerScene保存为v2，读取v1自动补disabled默认环境，
嵌套environment写v2（clouds完整对象），environment v1读取后补disabled云。
Project清单仍为v1；未知场景/环境版本和未知字段拒绝。

```powershell
New-Item -ItemType Directory -Force out/environment-demo | Out-Null
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/environment-demo --batch examples/automation/environment.jsonl --auto-guard
```
