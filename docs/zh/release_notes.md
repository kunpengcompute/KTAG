# 版本说明书

## 修改记录

|文档版本|发布日期|修改说明|
|--|--|--|
|01|2026-03-30|第一次正式发布。更新了特征提取及特征推理接口头文件及软件包。|
|开发中|未发布|增加三层 API、双栈 Schema Pipeline 和代理 Feature Plugin；完整安全与性能验收尚未完成。|

## 版本配套说明

### 产品版本信息

<a name="table62675726"></a>
<table><tbody><tr id="row41561572"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.1.1"><p id="p11044137"><a name="p11044137"></a><a name="p11044137"></a>产品名称</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.1.1 "><p id="p1597721693713"><a name="p1597721693713"></a><a name="p1597721693713"></a>Kunpeng BoostKit</p>
</td>
</tr>
<tr id="row24726251"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.2.1"><p id="p56669300"><a name="p56669300"></a><a name="p56669300"></a>产品版本</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.2.1 "><p id="p11923034"><a name="p11923034"></a><a name="p11923034"></a><span id="text152431189308"><a name="text152431189308"></a><a name="text152431189308"></a>26.0.0</span></p>
</td>
</tr>
<tr id="row1930811171892"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.3.1"><p id="p2030912172097"><a name="p2030912172097"></a><a name="p2030912172097"></a>软件名称</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.3.1 "><p id="p1730912179911"><a name="p1730912179911"></a><a name="p1730912179911"></a>KTAG</p>
</td>
</tr>
<tr id="row5497143514612"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.4.1"><p id="p162251517551"><a name="p162251517551"></a><a name="p162251517551"></a>软件版本</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.4.1 "><p id="p6225131165519"><a name="p6225131165519"></a><a name="p6225131165519"></a>1.0.0</p>
</td>
</tr>
</tbody>
</table>

### 与操作系统、编译器和CPU配套说明<a name="ZH-CN_TOPIC_0000002518245458"></a>

|操作系统|CPU类型|编译器|
|--|--|--|
|openEuler 22.03 LTS SP4|鲲鹏920新型号处理器|GCC 10.3.1|
|openEuler 24.03 LTS SP2|鲲鹏920新型号处理器|GCC 10.3.1|

## V1.1.0

### 更新说明

**新增特性**

|特性名称| 更新说明                                     |
|--|------------------------------------------|
|加密流量处理| 更新了特征提取及特征推理接口头文件及软件包。 |

## 开发中版本（未发布）

以下内容反映当前开发分支代码，不代表已经完成正式发布验收。

### 已实现代码能力

- 增加 Capture、Feature、Inference 三层公共 API，旧固定向量接口作为兼容层保留。
- Capture/Feature 支持 IPv4/IPv6 TCP、UDP、规范化 Flow Key 和 DNS A/AAAA 关联。
- 增加 Schema 化特征 CSV、Schema Hash 和模型包输入契约校验。
- 保留 `ktag_legacy_v1` 应用 Schema：53 个 Feature Descriptor、354 个数值；训练
  排除 Flow ID 和源/目的 IP 后输入 351 个数值。
- 增加 `ktag_proxy_v1` 代理 Schema：118 个 Feature Descriptor、619 个数值。
- 增加 `libktag_proxy_features.so`，提供 Sequence/Burst、TCP/TLS/Carrier 和轻量级
  QUIC 特征。
- demo 支持 `single_thread`、`staged`、`feature_parallel`、`full_parallel` 四种
  新 Pipeline Mode，并保留 `legacy` 路径。
- 训练/评估允许 IPv4-only、IPv6-only 或混合数据；缺少地址族时告警但不阻止训练。
- 评估输出整体及可用地址族分层报告、混淆矩阵和逐流 confidence，不通过阈值改写
  RandomForest 数字标签。

### 当前交付边界

- `tools/demo/proxy_config.yaml` 当前实际启用两个 VMess 承载类别，用于验证代理训练
  和推理链路；普通流量及其余目标代理类别的数据和正式模型未随仓库提供。
- IPv6 Extension Header 采用有界解析；不做 IP 分片重组，不支持 IPv6 Jumbogram、
  ESP 解密或跨地址族会话合并。
- QUIC 为轻量级结构与统计特征解析，不执行解密、完整连接状态机或 HTTP/3 内容解析。
- TLS 1.3 ECH 下的视频/文本内容分类不属于当前代理协议分类范围。
- 完整双栈 Fuzz、ASan/UBSan、资源与吞吐性能对比尚未完成独立验收，因此本版本不
  声明相应安全或性能结论。

详细使用方法参见《[应用分类与代理分类使用指南](demo_pipeline.md)》。
