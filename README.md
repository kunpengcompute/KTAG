# KTAG

## 最新消息

\[2026-09-30\]: 增加 Capture、Feature、Inference 三层公共 API、IPv4/IPv6 双栈 Schema Pipeline 和代理 Feature Plugin。

\[2026-06-30\]: 项目更名为 KTAG。

\[2026-03-30\]: 发布 1.0.0。更新了特征提取及特征推理接口头文件及软件包。

## 项目介绍

KTAG（Kunpeng TAG，鲲鹏流量分类库）是鲲鹏数据分析开发套件的网络流分类库，包含流汇聚、协议检测、特征计算、AI引擎和训练工具包，针对鲲鹏特性进行极致亲和优化，在具备对明文流量分析能力的同时，亦能对加密流量进行实时分析，弥补传统基于规则检测的DPI方案无法分析加密流量的短板，为解决业界普遍难题提供端到端参考方案。

当前公共库按 Capture、Feature、Inference 三层交付。新架构支持 callbacks
自定义特征、Schema 化模型契约和由 demo 组织的单线程/多线程流水线。冻结的 `ktag_legacy_v1` 特征由 Traffic Feature Plugin 提供；旧提取接口已移除，
模型推理统一使用公共 Inference API，旧固定向量推理接口和独立兼容库已移除；产品版本查询独立保留。

项目命名已统一为 KTAG。使用早期版本的项目需要同步更新头文件、库名、
接口名称、构建参数和安装路径，并重新编译；命令行程序统一为 `ktag_demo`。
冻结特征计算统一由插件执行；历史文件格式的数据布局继续保留，提取和推理统一使用公共 API。

当前仓库还提供 IPv4/IPv6 双栈 Schema 特征提取和独立的应用/代理分类配置。代理
分类使用可选的 `libktag_proxy_features.so` 插件和 619 维 `ktag_proxy_v1` Schema。
仓库中的代理配置是两个 SSR 类别与两个 VMess 承载类别的四分类示例。完整范围、命令和限制请参见
《[用户指南](docs/zh/user_guide.md)》。

## 项目结构

项目目录结构如下：

```
|—— cmake                                                     # 工程编译配置
|—— docs                                                      # 文档资料
│   └── zh                                                    # 中文文档目录
│       ├── release_notes.md                                  # 版本说明书
│       ├── ub_user_guide                                     # UB使用手册
│       ├── installation_guide.md                             # 安装指南
│       ├── quick_start.md                                    # 快速入门
│       ├── user_guide.md                                     # 用户指南
│       └── api_reference.md                                  # API参考
|—— examples                                                  # 公共 API 示例
|—— include                                                   # 头文件目录
|—— plugins                                                   # 冻结流量特征与代理特征插件
|—— src                                                       # 源码目录
│   ├── core                                                  # 库级基础能力与版本查询
│   ├── capture                                               # 报文读取与流标识
│   ├── feature                                               # 组流与特征计算
│   └── inference                                             # 模型加载与批量推理
|—— test                                                      # 测试框架
|—— tools                                                     # 工具集
│   ├── demo                                                  # 训练/评估/推理命令行工具
│   ├── data                                                  # 数据过滤与预检工具
│   ├── capture                                               # 流量抓包脚本
│   ├── labeling                                              # 流量打标签工具
│   ├── model_package                                         # 模型包打包工具
│   └── validation                                            # 一致性验收脚本
|—— README.md                                                 # 说明文档
|—— ktag_appidver.h                                          # 项目版本信息头文件
|—— build.sh                                                  # 项目编译脚本
```

## 版本说明

每个版本的特性变更详细信息，具体请参见《[版本说明书](docs/zh/release_notes.md)》。

## 环境部署

KTAG当前适配的处理器和操作系统为鲲鹏920系列处理器，openEuler 22.03操作系统，若您在使用过程中遇到问题，请先检查使用的环境是否在已验证的环境范围内。

**表2** KTAG已验证环境

| 操作系统  | CPU类型  |
| ------------ | ------------ |
| openEuler 22.03 LTS SP4  | 鲲鹏920新型号处理器  |
| openEuler 24.03 LTS SP2  | 鲲鹏920新型号处理器  |

**表3** 软件要求

| 软件名称| 版本 |
| --- | --- |
| GCC | 10.3.1 |
| CMake | 3.22.0 |
| yaml-cpp | 0.8.0 |
| onnxruntime| 1.22.0|

详细编译方法参考《[编译指南](docs/zh/installation_guide.md#编译指南)》。

## 学习文档

本特性提供以下学习文档可供参考。

| 文档 | 介绍|
| -- | -- |
| [编译指南](docs/zh/installation_guide.md#编译指南) | 提供了项目编译方法。 |
| [快速上手](docs/zh/quick_start.md) | 按步骤指导完成推理、网口实时分类、特征生成、训练和评估。 |
| [用户指南](docs/zh/user_guide.md#用户指南) | 提供配置文件、Schema 契约、训练评估、推理模式、支持边界和常见问题的完整使用说明。 |
| [API参考](docs/zh/api_reference.md) | 提供 Capture、Feature、Inference 三层接口、插件 ABI 和旧接口兼容说明。 |
| [版本说明书](docs/zh/release_notes.md#版本说明书) | 提供了版本更新说明。 |

## 贡献声明

如果使用过程中有任何问题，或者需要反馈特性需求和bug报告，可以提交issues联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。

## 许可证书

本项目采用MIT修改版许可证。详见[LICENSE](LICENSE)。

本项目的文档适用CC-BY 4.0许可证，具体请参见[LICENSE](docs/LICENSE)文件。
