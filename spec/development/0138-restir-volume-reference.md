---
id: "0138"
created_at: "2026-10-19T15:16:00+08:00"
updated_at: "2026-10-19T15:33:00+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# 0138 M19.5 Scene media 首表面 RIS 条件参考

依据 [M19.5 契约](../design/render-restir.md#m195-cpu-scene-media-条件采样契约)，
先固定几何首表面和完整 RGB reservoir，再抽 hero/analog 自由程；仅无真实散射到达首表面时应用 DI。
先实现 CPU 参考及条件概率/光能分工验收，GPU 介质复用、Editor/capture 和 DLSS 留给后续阶段。

## 设计与验证计划

- 候选独立于 hero/存活，有限太阳/环境/面积灯标签分离，独立新阴影流；真实体积散射解除首段 emission 抑制。
- 网格与行星边界单独记录，首次几何查询复用；预算预检、tracking 超限、取消和图像 owner 原子返回。
- 增加独立估计/图像入口与来源统计，默认旧体积入口不变，CPU 无 SDK/GPU 依赖。
- 用离散条件恒等式、独立积分/普通体积 MIS、空气/云/行星/材质/状态和直接受影响旧体积链路定向验证。
- 本记录按实际结果验收 CPU 阶段，不扩大为 M19.6。

## 实际实现

- [公开入口](../../engine/render/pathtracing/include/dk/render/VolumePathReference.hpp)增加 `estimate_restir_volume_reference` 和 `render_restir_volume_reference`。
  固定射线结果携带配置、区间和统计；图像沿用不可变 `VolumePathImage` owner，来源增加可选 RIS 配置。
  旧入口该配置为空；沿用 model 4/sampler 2，不更改旧随机流 0..10，新的 RIS 阴影流仅追加为 11。
- [积分路径](../../engine/render/pathtracing/src/VolumePathReference.cpp)先准备几何接收点和 RGB reservoir，
  复用同一次主几何查询与介质区间，再进行 hero/analog 自由程；准备与实际到达的数量分别记录。
  主段发生真实散射/吸收时不应用 reservoir；真实散射后的普通路径逐样本保持一致。
- [初始 RIS](../../engine/render/pathtracing/src/VolumeRestirReference.inl)使用独立 RIS1 候选维度，
  目标不含遮挡或随机透射，太阳/环境采用分离标签的立体角测度，面积灯使用面积测度和完整类概率。
  当前着色重新查询网格/mask/行星与 ratio-tracking 透射。地面使用球面法线和 Lambert 材质，无伪造 mesh 索引。
- 首表面只替换其直接项；下一 BSDF 段的受支持直达 emission 被抑制，真实介质散射立即解除抑制。
  mask 发光、相机直见、第二表面反射、普通后续 NEE/MIS/RR 保留。
- 参数、环境匹配和候选/几何预算在图像分配前拒绝；tracking 和取消失败释放私有结果，旧图与场 owner 不变。
  首查询复用使几何上界保持 `(2*bounces+1)*pixels*spp`。temporal/spatial 和 BSDF-only 组合明确拒绝。
- 无新增依赖、无 SDK 布局外泄。CPU-only 配置的 Graphics、GPU Pathtracing、Editor 均为 OFF，参考 target 为 ON。

## 数值与状态证据

[10 个新用例](../../tests/unit/RestirVolumeTests.cpp)与 36 个已有体积/RIS 回归用例，共 46 个唯一用例通过。
每个随机均值使用固定 8 seeds `101+7919*i`，以 seed 均值求标准误差；
逐 RGB 同时要求误差不超过 `2%*abs(reference)+1e-4` 和 `5*combined_SE+1e-4`，未调整阈值。
面积/半球与云透射参考分别先通过 256/512 或 4096/8192 积分分辨率认证。

| 证据 | 结果 |
| --- | --- |
| 条件期望 | 枚举两个 proposal、替换、RGB hero、极低存活率与独立随机阴影；与完整 RGB 积分差小于 1e-12，包含单通道零贡献 |
| 真空与发光分工 | Lambert、M=1、GGX、环境与有限太阳重叠、面积灯、mask emitter 和二次反射/RR 满足双门槛 |
| 彩色纯吸收 | 网格参考 `(0.165440,0.0172396,0.00875338)`，实测 `(0.165218,0.0172596,0.00876621)`；地面同样通过，全部候选在主段吸收之前生成 |
| 主段散射 | 1024 条单样本路径中，真实先散射分支的均值、二阶矩、rays/tracking 与普通体积路径完全相等；存活路径仍应用首表面 RIS |
| 首表面后散射 | 32 bounce 白炉 16384/65536 spp 的 8-seed MSE 下降，均值均满足双门槛；继续散射和多次散射计数为正，RR 同样通过 |
| 真实云阴影 | time=17 参考每通道 0.0706546，实测 `(0.0705040,0.0707243,0.0709406)`；time=2017 风移到晴空间隙，参考 0.159155、实测 `(0.158802,0.159444,0.159219)`，固定时间可重放 |
| 云与空气多次散射 | 云白炉实测 `(0.997789,1.00407,1.00288)`，混合空气/云 `(0.998789,1.00218,0.999088)`，参考均为 1；真实延伸/多次散射均发生 |
| 状态与来源 | 地面/网格排序、地下拒绝、样本分区、透视/正交 jitter 与逐射线一致；配置/区间/场 hash/风时间和 owner 保活；候选预算预检、主动取消、部分分配失败、总/逐路径 tracking 失败均不返回部分图 |

原体积 sampler 的 11 个 golden 流、有限太阳/纯吸收/单次散射独立积分、空气/云多散射、图像失败保护，
以及原 RIS 数学/时空支持参考/表面能量用例全部通过。

## 验证命令与结果

使用已有 CPU-only `out/build/windows-path-media-cpu`、Release，显式 target 为 `dk_pathtracing_reference_tests`。
没有变更实现后重复全量验证，失败修正后只重跑受影响用例。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.RESTIR volume ' `
  -Reason 'M19.5 CPU conditional DI energy, media branches and atomic image contracts' -Parallel 2

& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.RESTIR volume real cloud ' `
  -Reason 'M19.5 cloud shadow with wind-driven clearing and conservative multiple scattering' -Parallel 2

& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests `
  -TestRegex '^dk\.pathtracing\.(volume |RESTIR (reservoir|temporal|spatial|CPU|counter|merge|light))' `
  -Reason 'M19.5 shared volume trace and RIS math regression without GPU or unrelated modules' -Parallel 2
```

- `out/verify/20261019-152054-3966ea50`：初始 4 例中 2 通过、2 失败。测试夹具在切回 Lambert 后保留 metallic=.7，
  且缩短大气高度后未同步吸收剖面范围，均由既有校验拒绝；修正夹具后通过。
- `out/verify/20261019-152630-bfa1e0df`：测试编译失败，`RenderView` 无默认构造；显式创建合法视图后修正，没有运行旧二进制。
- `out/verify/20261019-152646-328db491`：10 例中 9 通过；云用例错误要求两个风时间都覆盖云。
  实际收敛参考显示后一个时间为晴空间隙；改为首时间必须有云、两时间透射不同，保留两时间各自均值门槛。
- `out/verify/20261019-152734-b8bd7f2e`：唯一受影响云用例 1/1 通过，包含云/空气白炉与 RR。
- `out/verify/20261019-152831-c3c2a865`：原体积 15 例与原 RIS 21 例，共 36/36 通过，零跳过。
- 文档定向检查通过：`check-spec.ps1 -Path` 覆盖本次 9 个 Markdown，校验 791 个本地链接、元数据、表格、索引、测试入口和 JSON manifests；`git diff --check` 通过。

## 限制与交接

本阶段为 CPU 初始参考，不提供介质 temporal/spatial 会话，不复用体积散射点。
未运行 GPU、Editor/capture、DLSS 或性能基准，本次没有修改它们的实现；不宣称 GPU 介质已支持或性能收益已达标。
低粗糙度 light-only proposal 和主段先散射时仍准备候选的成本保持可见，不用夹取或提前依赖存活筛选掩盖。
下一项 [M19.6](../roadmap.md#m19restir-直接光重采样)应将同一条件独立性与当前透射规则迁入 GPU，
重新验证复用来源支持、tracking 预算、风时间/介质失效和失败发布；RTXDI shader 布局由私有适配层负责，DLSS 仍是独立重建边界。
