# 版本说明书

## 修改记录

|文档版本|发布日期|修改说明|
|--|--|--|
|02|2026-09-15|增加 Capture、Feature、Inference 三层公共 API、IPv4/IPv6 双栈 Schema Pipeline 和代理 Feature Plugin；新增《快速上手指南》《用户指南》，测试说明移至 `test/README.md`。|
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

### 与操作系统、编译器和CPU配套说明<a name="ZH-CN_TOPIC_0000002518245458"></a>

|操作系统|CPU类型|编译器|
|--|--|--|
|openEuler 22.03 LTS SP4|鲲鹏920新型号处理器|GCC 10.3.1|
|openEuler 24.03 LTS SP2|鲲鹏920新型号处理器|GCC 10.3.1|

## V1.1.0

### 更新说明

**新增特性**

|特性名称|更新说明|
|--|--|
|三层公共 API|Capture、Feature、Inference 三层公共 API 统一承载提取与推理；旧固定向量提取、推理接口及独立兼容库已移除。|
|插件化特征提取|冻结算法位于 `plugins/traffic_features/legacy/`，由 `libktag_traffic_features.so` 提供；Traffic 与 Proxy 插件默认构建。|
|IPv4/IPv6 双栈|Capture/Feature 支持 IPv4/IPv6 TCP、UDP、规范化 Flow Key 和 DNS A/AAAA 关联。|
|Schema Pipeline|增加 Schema 化特征 CSV、Schema Hash 和模型包输入契约校验；保留 `ktag_legacy_v1` 应用 Schema：53 个 Feature Descriptor、354 个数值，训练排除 Flow ID 和源/目的 IP 后输入 351 个数值。|
|代理分类|增加 `ktag_proxy_v1` 代理 Schema：118 个 Feature Descriptor、619 个数值；增加 `libktag_proxy_features.so`，提供 Sequence/Burst、TCP/TLS/Carrier 和轻量级 QUIC 特征。|
|并行模式|demo 支持 `single_thread`、`staged`、`feature_parallel`、`full_parallel` 四种 Pipeline Mode，`legacy` 仅保留历史调度和输出格式，推理同样使用公共 Inference API 和模型包。|
|版本查询|版本查询实现位于 `src/core/`，通过 `ENABLE_BUILD_LIB=ON` 或 `sh build.sh -lib` 生成独立的 `libktag_appidlib.so`。|
|双栈训练评估|训练/评估支持 IPv4-only、IPv6-only 或混合数据；评估输出整体及可用地址族分层报告、混淆矩阵和逐流 confidence。|

## V1.0.0

### 更新说明

**新增特性**

|特性名称| 更新说明                                     |
|--|------------------------------------------|
|加密流量处理| 更新了特征提取及特征推理接口头文件及软件包。 |
