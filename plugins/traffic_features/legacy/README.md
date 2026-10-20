# 冻结的 Legacy V1 特征

此目录保存 `ktag_legacy_v1` 的 Provider 和特征算法：五元组、双向统计及直方图、
HTTP 到 RawBoW 的输入处理、DFA 分词、CRC 词袋和 Flow ID。

- 统计算法使用迁移前的样本方差、时间间隔对数变换及直方图规则。
- 分词使用已合入的单字符 DFA，保留过滤规则、长度与 token 容量限制。
- Provider 通过公共 Packet/Flow View 工作，不持有旧流表或重新组流。
- 只编译到上层 `libktag_traffic_features.so`，不生成额外兼容动态库。
- 核心保留通用流生命周期、DNS/Domain Store、Schema/Registry 和 Batch 管理。

旧提取器 C 接口已经移除。新增算法或改变冻结契约时应显式定义新的契约版本。
