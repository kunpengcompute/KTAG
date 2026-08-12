# KTAG

## 最新消息

\[2026-06-30\]: KDADK-APPID更名为KTAG。

\[2026-03-30\]: 发布KDADK-APPID 1.0.0。更新了特征提取及特征推理接口头文件及软件包。

## 1. 项目介绍
KTAG是鲲鹏数据分析开发套件的网络流分类库，包含流汇聚、协议检测、特征计算、AI引擎和训练工具包，针对鲲鹏特性进行极致亲和优化，在具备对明文流量分析能力的同时，亦能对加密流量进行实时分析，弥补传统基于规则检测的DPI方案无法分析加密流量的短板，为解决业界普遍难题提供端到端参考方案。

当前公共库按 Capture、Feature、Inference 三层交付。新架构支持 callbacks
自定义特征、Schema 化模型契约和由 demo 组织的单线程/多线程流水线。旧固定
`feature_vector` 与旧模型接口继续作为 deprecated 兼容层保留。

当前仓库还提供 IPv4/IPv6 双栈 Schema 特征提取和独立的应用/代理分类配置。代理
分类使用可选的 `libktag_proxy_features.so` 插件和 619 维 `ktag_proxy_v1` Schema。
仓库中的代理配置目前是两个 VMess 承载类别的可运行示例，不代表已经随仓库交付
普通流量及全部目标代理类别的正式模型。完整范围、命令和限制参见
《[应用分类与代理分类使用指南](docs/zh/demo_pipeline.md)》。

## 2. 项目结构
项目目录结构如下：
```
|—— cmake                                                     # 项目工程编译目录
|—— docs                                                      # 文档资料
│   └── zh                                                    # 中文文档目录
│       ├── release_notes.md                                  # 版本说明书
│       ├── installation_guide.md                             # 安装指南
│       ├── user_guide.md                                     # 用户指南
│       ├── api_reference.md                                  # API参考
│       ├── demo_pipeline.md                                  # 应用/代理分类使用指南
|—— include                                                   # 头文件目录
|—— src                                                       # 源码目录
|—— test                                                      # 测试框架
|—— README.md                                                 # 说明文档
|—— kdadk_appidver.h                                          # 项目版本信息头文件
|—— build.sh                                                  # 项目编译脚本
```

## 版本说明
每个版本的特性变更详细信息，具体请参见《[版本说明书](docs/zh/release_notes.md)》。

## 4. 环境部署
KTAG当前适配的处理器和操作系统为鲲鹏920系列处理器，openEuler 22.03操作系统，若您在使用过程中遇到问题，请先检查使用的环境是否在已验证的环境范围内。

**表2** KTAG已验证环境
| 操作系统  | CPU类型  |
| ------------ | ------------ |
| openEuler 22.03 LTS SP4  | 鲲鹏920新型号处理器。  |
| openEuler 24.03 LTS SP2  | 鲲鹏920新型号处理器。  |

**表3** 软件要求
| 软件名称| 版本 |
| --- | --- |
| GCC | 10.3.1 |
| CMake | 3.22.0 |
| yaml-cpp | 0.8.0 |
| onnxruntime| 1.22.0|

详细编译方法参考《[编译指南](docs/zh/installation_guide.md#编译指南)》。

## 5. 学习文档
本特性提供以下学习文档可供参考。
| 文档 | 介绍|
| -- | -- |
| [编译指南](docs/zh/installation_guide.md#编译指南) | 提供了项目编译方法。 |
| [用户指南](docs/zh/user_guide.md#用户指南) | 提供了测试框架说明及用法。 |
| [API参考](docs/zh/api_reference.md#api参考) | 提供了特征提取和特征推理的接口说明。 |
| [应用分类与代理分类使用指南](docs/zh/demo_pipeline.md) | 说明配置、特征提取、训练、评估、推理、双栈范围和当前限制。 |
| [应用代理与加密代理流量分类功能设计](docs/zh/proxy_protocol_identification_design.md) | 说明应用与代理双栈分类、代理 Feature Plugin、轻量级 QUIC 与完整实施步骤。 |
| [版本说明书](docs/zh/release_notes.md#版本说明书) | 提供了版本更新说明。 |

## 6. 贡献声明
如果使用过程中有任何问题，或者需要反馈特性需求和bug报告，可以提交issues联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。

## 7. 许可证书
本项目采用MIT修改版许可证。详见[LICENSE](LICENSE)。

本项目的文档适用CC-BY 4.0许可证，具体请参见[LICENSE](docs/LICENSE)文件。
