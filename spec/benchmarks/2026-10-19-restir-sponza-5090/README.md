# M19.8 RTX 5090 D v2 完整验收归档

新机于 2026-10-19 22:15:54–23:35:26（+08:00）串行重采全部 52 个进程，未重试、删轮或混入旧机数据。
结论与测量边界见[报告](../2026-10-19-restir-sponza.md)，开发与环境恢复见[0141](../../development/0141-restir-sponza-acceptance.md)。

- 12 份性能报告：四模式 × 三轮独立进程，640×360；完整相机移动、取消及停动至 16 spp。
- 40 份质量报告：十场景 × 四模式，每组 8 条独立历史链；Classic 另外保存每链独立 N/4N 参考。
- 120 幅完整 RGB32F 图像，按内容去重为 119 文件；均为 640×360、小端 float32 RGB。
- [summary.json](summary.json) 保存严格聚合结果；[manifest.json](manifest.json) 保存二进制、资产、shader 与 RTXDI 头文件 SHA-256。
- [archive.json](archive.json) 保存报告和图像的压缩字节 SHA-256。报告为 `.json.gz`，图像为 `<原始字节 SHA256>.rgb32f.gz`。
- [environment.json](environment.json) 保存设备、工具版本、运行库与相关源文件哈希；[验证摘要](validation/summary.json)和[日志](validation/tests.txt)记录 14 项定向检查全通过，0 失败/0 跳过。
- [evidence-checksums.json](evidence-checksums.json) 校验附加环境、构建、测试及采集日志；核心报告/图像另由 archive.json 校验。

归档约 87.2 MiB，不包含可重建的程序和三方库。原始报告中的 `linear_file` 绝对路径保留来源记录，
离线复算按图像 SHA 从本目录读数据，不需要原机 out 目录。归档生成时已验证全部字节、尺寸/有限性、
模式/物理输入/来源、三轮身份与八条独立链，并逐值重算 summary。附加日志以 `.txt` 或 gzip 保存；带原始空白的构建/配置/发现输出使用 gzip 保留原始字节。未压缩的摘要文本使用 LF，并通过本目录 Git attributes 固定换行，避免 Git 忽略日志或换行转换破坏跨机校验。

```powershell
python scripts/benchmark-restir-sponza.py --directory spec/benchmarks/2026-10-19-restir-sponza-5090 --verify-archive
```

附加证据校验（从仓库根执行）：

```powershell
@'
import hashlib, json
from pathlib import Path
root = Path('spec/benchmarks/2026-10-19-restir-sponza-5090')
for name, expected in json.loads((root/'evidence-checksums.json').read_text()).items():
    assert hashlib.sha256((root/name).read_bytes()).hexdigest() == expected
print('Ancillary evidence hashes passed')
'@ | python -
```

完成测量不表示性能门槛全部通过：仅云内/云上方参考认证，其他八组未认证；已认证场景中 ReSTIR 未达 2× 等误差收益。
三项交互均达标的场景数为 6/10、7/10、2/10、0/10。默认保留 Classic MIS；4×4 质量不认证全尺寸画质。
