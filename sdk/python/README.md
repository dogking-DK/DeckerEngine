# DeckerEngine Python SDK

Python 3.11+ 标准库客户端，通过 Windows `dk-ctl` 连接已有 runner/editor。
没有 Python 三方依赖；将本目录加入 PYTHONPATH 后 `from decker import Client`。
源码分发，当前不发布 pip 包，也不负责启动或关闭服务。

完整配置、API、错误和可执行示例见 [Python 自动化指南](../../spec/guides/python.md)，
状态与失败契约见 [设计](../../spec/design/automation-python.md)。
记录/重放使用 `decker.replay`，见 [重放指南](../../spec/guides/replay.md)。

实验计划展开与产物来源核验使用 `decker.experiments` / `decker.experiment_index`，
见 [实验管理指南](../../spec/guides/experiments.md)；准备计划不执行模拟。

CPU/GPU 物理实验与 config.json 从零重放使用 [cloth_experiment.py](../../examples/automation/cloth_experiment.py)，见 [模拟指南](../../spec/guides/simulation.md)。它复用 Client.call，不改变场景 Recorder v1 格式。
