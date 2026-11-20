# 功能覆盖清单

运行以下命令从当前源码重新生成清单：

    python tests/catalog/generate_catalog.py

输出：

- function_catalog.json：供测试工具和后续覆盖率统计读取。
- function_catalog.md：供人工评审的功能点清单。

源码扫描只负责发现功能。每个条目的 planned 状态必须由真实 JSON 流程用例执行后，经过人工确认再更新为 passed。
