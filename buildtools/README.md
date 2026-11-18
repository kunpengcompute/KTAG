# 构建脚本

| 文件 | 用途 |
| --- | --- |
| `build.sh` | 构建入口，读取根目录 `dependence.xml`，支持编译、清理、安装、卸载和打包 |
| `function.sh` | 由 `build.sh` 加载的辅助函数，负责调用 CMake、检查产物及打包，不直接运行 |

在仓库根目录使用 Bash 执行：

```bash
bash buildtools/build.sh                # 默认构建
bash buildtools/build.sh -lib           # 额外构建版本查询库和测试插件
bash buildtools/build.sh -perf          # 启用性能分析构建选项
bash buildtools/build.sh dpdk           # 启用 DPDK 收包
KTAG_ENABLE_DPDK_UB=ON bash buildtools/build.sh dpdk  # 同时启用 UB
bash buildtools/build.sh clean          # 清理根目录 build/ 和 output/
```

其余参数为 `-sve256`、`-coverage`、`install`、`uninstall`、`deb`、`util`，沿用原有行为和依赖要求。使用 Bash，避免其他系统的 `sh` 不支持脚本中的 Bash 语法。

脚本根据自身位置定位仓库，可以从其他目录通过绝对路径执行，例如：

```bash
bash /opt/KTAG/buildtools/build.sh
```

构建目录仍是仓库根目录的 `build/`，产物仍在根目录的 `output/`；`CMakeLists.txt`、`config.cmake.in`、`dependence.xml` 和 `cmake/` 仍在原处。

根目录另有 `ci-pre-commit-pr.sh`，用于 CI 拉取 PR 并运行 pre-commit 代码检查，不是编译入口。本次保留其位置；`tools/capture/` 下的 Shell 脚本用于抓包，也不属于构建脚本。
