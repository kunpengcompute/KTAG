# 版本说明书

## 修改记录

|文档版本|发布日期|修改说明|
|--|--|--|
|03|2026-12-30|第三次增加VPN识别能力，开放用户自定义特征能力|
|02|2026-09-30|第二次增加Capture、Feature、Inference三层公共API、IPv4/IPv6双栈Schema Pipeline和代理Feature Plugin；新增《[快速入门](quick_start.md)》、《[用户指南](user_guide.md)》，测试说明移至[测试说明](../../test/README.md)。|
|01|2026-03-30|第一次正式发布。更新了特征提取及特征推理接口头文件及软件包。|

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
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.2.1 "><p id="p11923034"><a name="p11923034"></a><a name="p11923034"></a><span id="text152431189308"><a name="text152431189308"></a><a name="text152431189308"></a>26.2.0</span></p>
</td>
</tr>
<tr id="row1930811171892"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.3.1"><p id="p2030912172097"><a name="p2030912172097"></a><a name="p2030912172097"></a>软件名称</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.3.1 "><p id="p1730912179911"><a name="p1730912179911"></a><a name="p1730912179911"></a>KTAG</p>
</td>
</tr>
<tr id="row5497143514612"><th class="firstcol" valign="top" width="42.17%" id="mcps1.1.3.4.1"><p id="p162251517551"><a name="p162251517551"></a><a name="p162251517551"></a>软件版本</p>
</th>
<td class="cellrowborder" valign="top" width="57.830000000000005%" headers="mcps1.1.3.4.1 "><p id="p6225131165519"><a name="p6225131165519"></a><a name="p6225131165519"></a>1.1.0</p>
</td>
</tr>
</tbody>
</table>

### 操作系统、编译器和CPU配套说明<a name="ZH-CN_TOPIC_0000002518245458"></a>

|操作系统|CPU类型|编译器|
|--|--|--|
|openEuler 22.03 LTS SP4|鲲鹏920新型号处理器|GCC 10.3.1|
|openEuler 24.03 LTS SP2|鲲鹏920新型号处理器|GCC 10.3.1|

## V1.1.0

### 新增特性

|特性名称|更新说明|
|--|--|
|三层公共API|Capture、Feature、Inference统一承载采集、提取与推理，便于客户按层集成。旧接口的兼容性影响见下方“删除特性”。|
|插件化特征提取|冻结算法位于 plugins/traffic_features/legacy/，由 libktag_traffic_features.so 提供；Traffic与Proxy插件默认构建。|
|IPv4/IPv6双栈|Capture/Feature支持IPv4/IPv6 TCP、UDP、规范化Flow Key和DNS A/AAAA关联。|
|Schema Pipeline|增加Schema CSV、Schema Hash和模型输入契约校验，避免错误组合特征与模型。保留 ktag_legacy_v1：53个描述符、354个数值；训练排除Flow ID和源/目的IP后，输入351个数值。|
|代理分类|增加 ktag_proxy_v1：118个描述符、619个数值。libktag_proxy_features.so 提供Sequence/Burst、TCP/TLS/Carrier和轻量级QUIC特征，用于训练代理流量分类模型；分类范围由训练数据决定。|
|并行模式|KFlowLens支持串行执行 single_thread、分阶段执行 staged、特征并行 feature_parallel 和全流水线并行 full_parallel，用于按负载配置线程。CLI的 legacy 保留历史调度和输出格式，仍使用公共Inference API和模型包；约束见[模式表](user_guide.md#pipeline-mode-与线程配置)。|
|版本查询|版本查询实现位于 src/core/；需要独立查询产品/库版本时，以 ENABLE_BUILD_LIB=ON 或 bash buildtools/build.sh -lib 生成 libktag_appidlib.so。新构建中该选项未设置等同OFF，此时不额外生成该库。|
|双栈训练评估|训练/评估支持IPv4-only、IPv6-only或混合数据；评估输出整体及可用地址族分层报告、混淆矩阵和逐流confidence。|

### 修改特性

无

### 删除特性

旧固定向量提取、推理接口及其独立兼容库已移除。直接调用这些接口或链接旧库的客户程序，需要修改调用及链接配置，不能仅替换动态库。

迁移到Capture → Feature → Inference公共API，调用顺序见[API参考](api_reference.md#端到端调用顺序)，完整样例见[三层示例](../../examples/three_layer/three_layer_example.c)。旧ONNX/Scaler可按[数据桥接说明](api_reference.md#统一公共推理-api)转换为模型包；历史文件格式的数据类型与转换说明见[固定向量数据桥接](api_reference.md#固定向量数据桥接)。

### 已解决的问题

无

### 遗留问题

无

## V1.0.0

### 更新说明

|特性名称| 更新说明                                     |
|--|------------------------------------------|
|加密流量处理| 更新了特征提取及特征推理接口头文件及软件包。 |
