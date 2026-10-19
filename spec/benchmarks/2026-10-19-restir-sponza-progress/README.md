# M19.8 换机前测量快照

这是用户要求于 2026-10-19 换电脑前保存的**未完成进度**，不是完整验收归档。
阶段及接续顺序见 [0141](../../development/0141-restir-sponza-acceptance.md)，性能结果与复现命令见[报告草稿](../2026-10-19-restir-sponza.md)。

- 已完成 40 个进程报告：12/12 性能（三轮、四模式），28/40 质量（classic 10、initial 10、temporal 8）。
- 尚缺 temporal/cloud-inside、temporal/cloud-above 和 spatial 的全部十组；停止时未完成的进程不计入证据。
- 首轮的静止 4 spp、移动最后一帧、停动恢复 16 spp 共 120 幅完整 640×360 RGB32F 图像，按内容去重为 119 个文件。
- 已保存等待方式的 8 链逐值对照。质量尺寸为 4×4，不认证全尺寸画质；完整 summary 和最终默认策略结论尚未形成。
- 当前来源为 RTX 4070 Laptop / 596.49，Release / Tracy OFF；详细硬件、物理输入和局限见报告草稿。

[progress.json](progress.json) 列出完成/缺失任务、文件名和压缩后字节的 SHA256。
[manifest.json](manifest.json) 保存程序、资产、Slang 与 RTXDI 头文件来源。
原始报告为 `<任务>.json.gz`；图像为 `<线性字节 SHA256>.rgb32f.gz`，解压后为小端 float32 RGB。
报告中的旧 `linear_file` 绝对路径仅作为原始来源保留；移机读取时用 `linear_sha256` 查 `progress.json.images`，不访问旧路径。

保存时已验证全部归档字节 hash、报告来源、120 幅图像的原始 hash/尺寸/有限非负值，以及等待对照的像素/矩/历史来源一致性。
本目录总量约 86 MiB；完整数据已进入 Git，原机器 `out` 不需要复制。

从仓库根离线检查所有归档字节：

```powershell
@'
import hashlib, json
from pathlib import Path
root = Path('spec/benchmarks/2026-10-19-restir-sponza-progress')
index = json.loads((root/'progress.json').read_text(encoding='utf-8'))
assert index['status'] == 'in-progress'
assert len(index['complete_runs']) == 40 and len(index['missing_runs']) == 12
for entry in [*index['files'].values(), *index['images'].values()]:
    assert Path(entry['file']).name == entry['file']
    assert hashlib.sha256((root/entry['file']).read_bytes()).hexdigest() == entry['sha256']
print('Progress archive byte hashes passed; full acceptance is still pending')
'@ | python -
```

换硬件/驱动或重建程序后，使用新的输出目录和 manifest 重采完整数据，不把本快照拼入新机器的计时。
同机同程序的原目录才可以 `--resume`；当前机器该目录为 `out/m19-8-sponza-poll`。
新电脑按构建指南准备环境，再使用报告草稿的正式采集命令。
`--summarize`、`--archive`、`--verify-archive` 要求完整的 52 个进程，本快照不能用于冒充完整通过。
最终验收完成后另建完整归档，保留此快照作为历史证据。
