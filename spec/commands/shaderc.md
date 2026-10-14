---
created_at: "2026-09-28T19:03:00+08:00"
updated_at: "2026-10-14T12:15:10+08:00"
---

# dk-shaderc 离线工具

[返回命令目录](README.md)。这是独立进程，不是 JSON-RPC 方法，不注册到 `commands.list`，
无需活动文档、guard 或 TaskId。构建与完整示例见 [Shader 指南](../guides/shaders.md)。

## compile

将一个 Slang 具名入口编译为原始 SPIR-V 1.5，并输出最小反射 JSON。

```text
dk-shaderc compile --source FILE --entry NAME --stage STAGE --output FILE.spv
                   [--include DIR ...] [--define NAME=VALUE ...]
dk-shaderc --help
```

| 参数 | 要求与默认值 |
| --- | --- |
| `--source FILE` | 必填；UTF-8 Slang 源文件，最多 64 MiB；拒绝 NUL |
| `--entry NAME` | 必填；ASCII 标识符，具名入口需能由 Slang 识别阶段 |
| `--stage` | 必填；vertex、fragment、compute、raygeneration、miss、closesthit、anyhit；必须与源码实际阶段相同 |
| `--output FILE.spv` | 必填；父目录已存在且可写，不能覆盖源/include/import 文件或其硬链接别名 |
| `--include DIR` | 可重复；按参数顺序提供 include/import 搜索目录，默认无额外目录 |
| `--define NAME=VALUE` | 可重复；宏名是 ASCII 标识符且不能重复；VALUE 可为空，默认无额外宏 |

相对文件和 include 路径以进程工作目录为基准；源码相邻 include 同样由 Slang 解析。
除 include/define 外不接受重复参数；未知参数、缺值和非法 CLI stage 为用法错误。
推荐在源码显式写 `[shader("...")]`；Slang 也能从 `[numthreads]` 识别 compute。
无阶段标记的普通函数不能由 `--stage` 强行变成入口。
固定 row-major matrix、默认优化、无 debug 信息、直接生成 SPIR-V，入口符号保留请求名称。

## 返回、错误与副作用

成功 stdout 是一个 JSON 对象（schema_version=3），stderr 可包含 Slang 警告：

| JSON 字段 | 内容 |
| --- | --- |
| compiler / target / matrix_layout | 编译器 build tag、spirv_1_5、row_major |
| entry / stage / spirv_bytes | SPIR-V 入口名、阶段、输出字节数 |
| thread_group_size | compute 的三个本地维度；其他阶段为 `[0,0,0]` |
| bindings | 按 set/binding 排序；每项含 name、set、binding、type、count、block_size、image_shape |
| push_constants | offset/size 字节范围；当前最多一个 offset=0 的块 |
| requires_ray_tracing | 四个RT阶段为true，基线阶段为false；执行时必须启用RT设备能力组 |
| source_fingerprint | 编译器/profile/matrix、入口/阶段、宏与依赖路径及内容的FNV-1a 64-bit标记；不是安全散列或跨机器缓存key |
| ray_payloads | kind（outgoing/incoming/hit_attribute）、location、bytes、signature；基线为空 |
| ray_traces | TraceRay的静态offset、stride、miss、payload_location；基线为空 |

binding的image_shape为none/2d/2d_array/3d；非图像使用none，图像维度与descriptor count独立。
descriptor type 为 uniform_buffer、storage_buffer、sampled_image、storage_image、sampler 或 acceleration_structure；
block_size 仅 uniform_buffer 有效，其余为 0。反射包含组合程序声明的全部全局资源，
不表示优化后的实际访问集合；不包含成员偏移、顶点输入布局、跨阶段合并或设备能力检查。
ParameterBlock、嵌套资源、未定长 descriptor 数组、入口 uniform、typed buffer、1D/cube/MSAA图像等当前无法表达的布局明确拒绝。
RT payload支持32-bit标量/向量/结构体/固定数组；bytes是逻辑字节数，signature表达结构类型，不能用同字节数替代类型匹配。
incoming payload无Location时记0，callee按类型匹配调用；三角hit attribute须为float2。
TraceRay限raygeneration并使用静态SBT选择器；动态选择、callee递归TraceRay、callable/intersection不在首版范围。
RT阶段能力白名单只追加RayTracingKHR，其他未启用的可选SPIR-V能力在使用层拒绝。

| 退出码 | 含义 |
| --- | --- |
| 0 | 编译和输出成功；或 --help 成功 |
| 1 | 输入/编译/入口/反射错误或文件输出失败；stderr 给出 dk 错误、Slang 原文与 source/entry/stage 上下文（适用时） |
| 2 | CLI 用法或参数结构错误 |
| 3 | Memory 初始化、分配或其他基础设施异常 |

成功会替换一个输出文件，不可进入 Runtime 撤销历史/事务。先完成编译、反射及 JSON 序列化，
再经 IO 同目录临时文件原子替换 SPIR-V；提交前失败保留已有输出，失败不打印成功 JSON。
Windows 以外的原子发布目前返回 not_supported。不自动创建父目录、不承诺断电耐久或并发路径隔离。
SPIR-V 提交后才写 stdout，stdout 失败不能回滚文件；shell 重定向的 JSON 文件由调用者管理，
不与 SPIR-V 组成双文件事务。输出路径应专用于编译产物，调用者须保证编译期间输入不变。

源码语法/类型错误保留文件、位置和原始诊断；缺失入口报告 not_found，阶段不符报告 invalid_argument，
不支持的布局报告 not_supported。`--define` 宏名/重复项由编译请求校验并返回 1。
缺失 slang-glslang/SPIR-V 优化器时在编译前拒绝，报告 not_supported；不会静默省略该编译步骤。
字节可重复性只覆盖相同输入、参数和固定编译器版本，不承诺跨版本或跨机器一致。
