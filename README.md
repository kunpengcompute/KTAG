# KDADK-APPID
## 1. 项目介绍
KDADK-APPID是鲲鹏数据分析开发套件的网络流分类库，包含流汇聚、协议检测、特征计算、AI引擎和训练工具包，针对鲲鹏特性进行极致亲和优化，在具备对明文流量分析能力的同时，亦能对加密流量进行实时分析，弥补传统基于规则检测的DPI方案无法分析加密流量的短板，为解决业界普遍难题提供端到端参考方案。

## 2. 项目结构
项目目录结构如下：
```
|—— cmake             # 项目工程编译目录
|—— docs              # 文档资料
|—— include           # 头文件目录
|—— src               # 源码目录
|—— test              # 测试框架
|—— README.md         # 说明文档
|—— kdadk_appidver.h  # 项目版本信息头文件
|—— build.sh          # 项目编译脚本
```

## 3. 版本说明
**表1** 版本说明
| KDADK-APPID  | 特性  |
| ------------ | ------------ |
| 1.1.0  | 鲲鹏数据分析开发套件的网络流分类库,包括特征提取和特征推理接口及源码 |

## 4. 环境部署
KDADK-APPID当前适配的处理器和操作系统为鲲鹏920系列处理器，openEuler 22.03操作系统，若您在使用过程中遇到问题，请先检查使用的环境是否在已验证的环境范围内。

**表2** KDADK-APPID已验证环境
| 操作系统  | CPU类型  |
| ------------ | ------------ |
| openEuler 22.03 LTS SP4  | 鲲鹏920系列处理器  |

**表3** 软件要求
| 软件名称| 版本 |
| --- | --- |
| GCC | 10.3.1 |
| CMake | 3.22.0 |
| yaml-cpp | 0.8.0 |
| onnxruntime| 1.22.0|

详细编译方法参考《[编译指南](docs/zh/compile_guide.md#编译指南)》

## 5. 学习文档
本特性提供以下学习文档可供参考。
| 文档 | 介绍|
| -- | -- |
| [编译指南](docs/zh/compile_guide.md#编译指南) | 提供了项目编译方法 |
| [用户指南](docs/zh/user_guide.md#用户指南) | 提供了测试框架说明及用法 |
| [API参考](docs/zh/api_reference.md#api参考) | 提供了特征提取和特征推理的接口说明 |

## 6. 贡献声明
如果使用过程中有任何问题，或者需要反馈特性需求和bug报告，可以提交issues联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。

## 7. 许可证书
本项目采用Apache License 2.0许可证。详见[LICENSE](LICENSE)。