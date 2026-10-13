# DeckerEngine Python SDK

Python 3.11+ 标准库客户端，通过 Windows `dk-ctl` 连接已有 runner/editor。
没有 Python 三方依赖；将本目录加入 PYTHONPATH 后 `from decker import Client`。
源码分发，当前不发布 pip 包。Client连接已有服务；独立用例CLI可管理自己的runner生命周期。

完整配置、API、错误和可执行示例见 [Python 自动化指南](../../spec/guides/python.md)，
状态与失败契约见 [设计](../../spec/design/automation-python.md)。
记录/重放使用 `decker.replay`，见 [重放指南](../../spec/guides/replay.md)。

实验计划展开与产物来源核验使用 `decker.experiments` / `decker.experiment_index`；
`decker.experiment_batch` 提供串行执行CLI/API、持久进度和CPU数值/GPU图像自动收集，
见 [实验管理指南](../../spec/guides/experiments.md)。run创建新批次，resume核验成功项并接续原任务；
`resume --retry-failed`显式从零重试失败/取消项，保留旧尝试与产物。
CLI执行/恢复后自动发布JSON/CSV/HTML报告；`--baseline`比较独立重跑，CPU/GPU计划自动配对。
`decker.experiment_report`及write_report提供离线重新核验/报告，数值容差与计时范围见上述指南。

CPU/GPU 物理实验与 config.json 从零重放使用 [cloth_experiment.py](../../examples/automation/cloth_experiment.py)，见 [模拟指南](../../spec/guides/simulation.md)。它复用 Client.call，不改变场景 Recorder v1 格式。

云渲染用例使用 `python -m decker.render_cases`：版本化清单、独立序列、来源核验、PPM/线性RGBA产物与数值对比，见[云使用指南](../../spec/guides/clouds.md)。
