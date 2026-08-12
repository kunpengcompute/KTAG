
# API参考

## 1. 现代三层 API

KTAG 新接口由以下公共头文件组成：

| 层 | 头文件 | 主要职责 |
|---|---|---|
| Capture | `ktag_capture.h` | PCAP/网口读取、报文所有权、双栈 Flow Key |
| Feature | `ktag_feature.h` | Provider 注册、Schema、双栈组流、Feature Batch |
| Plugin | `ktag_feature_plugin.h` | 动态 Feature Provider 插件 ABI |
| Inference | `ktag_inference.h` | 模型包校验、Schema-aware 批量推理 |

所有公开结构体在调用前都应填写 `struct_size` 和 `api_version`；当前
`api_version` 为 `KTAG_API_VERSION`。函数返回 `ktag_status`，0 表示成功，正数表示
Timeout/EOF 等状态，负数表示错误。

### 1.1 Capture

创建 PCAP 或网口输入：

```c
ktag_capture_context *ktag_capture_open(
    const ktag_capture_config *config);

ktag_status ktag_capture_next(
    ktag_capture_context *context,
    ktag_packet *packet);

void ktag_capture_interrupt(ktag_capture_context *context);
void ktag_capture_close(ktag_capture_context *context);
```

`ktag_capture_next` 可能返回 `KTAG_SUCCESS`、`KTAG_STATUS_TIMEOUT`、
`KTAG_STATUS_END_OF_STREAM` 或错误码。跨线程传递 borrowed 报文前，应调用
`ktag_packet_make_owned`；拥有所有权的报文视图使用
`ktag_packet_retain/ktag_packet_release` 管理引用。

支持的链路类型常量：

- `KTAG_CAPTURE_LINKTYPE_ETHERNET`；
- `KTAG_CAPTURE_LINKTYPE_RAW`；
- `KTAG_CAPTURE_LINKTYPE_LINUX_SLL`；
- `KTAG_CAPTURE_LINKTYPE_LINUX_SLL2`。

双栈 Flow Key 接口：

```c
ktag_status ktag_packet_get_flow_key(
    const ktag_packet *packet,
    ktag_flow_key *key);

uint64_t ktag_flow_hash(const ktag_flow_key *key, uint64_t seed);

uint32_t ktag_flow_select_worker(
    const ktag_flow_key *key,
    uint32_t worker_count,
    uint64_t seed);
```

`ktag_flow_key` 使用 16 字节地址槽保存 IPv6；IPv4 使用前 4 字节，其余字节为 0。
Flow Key 按双向端点规范化，不代表第一包的源/目的方向。

### 1.2 Feature、Provider 与 Schema

Feature Context 的核心调用顺序为：

```c
ktag_feature_context *context = ktag_feature_create(&config);
ktag_feature_process_packet(context, &packet, batch);
ktag_feature_expire(context, current_time_us, batch);
ktag_feature_flush(context, batch);
ktag_feature_destroy(context);
```

一个 Context 在第一次处理、过期或 Flush 后由单一线程拥有；多个 Context 可以并行
运行。`ktag_feature_flush` 用于在 PCAP EOF 或停止流程中输出尚未自然结束的流。

Schema 创建与查询接口：

```c
ktag_feature_schema *ktag_feature_schema_create(
    const char *schema_name,
    uint32_t schema_version);
ktag_status ktag_feature_schema_add(
    ktag_feature_schema *schema,
    const ktag_feature_descriptor *descriptor);
ktag_status ktag_feature_schema_add_registered(
    ktag_feature_schema *schema,
    const ktag_feature_registry *registry,
    const char *feature_id);
ktag_status ktag_feature_schema_freeze(ktag_feature_schema *schema);
uint64_t ktag_feature_schema_get_hash(
    const ktag_feature_schema *schema);
```

Schema Freeze 后不可修改。Feature ID、维度或顺序的变化会改变 Schema Hash；模型包
必须与运行时冻结 Schema 匹配。`ktag_feature_schema_create_legacy_v1()` 返回原
354 维应用契约。

Provider 通过 `ktag_feature_registry_register` 注册回调。动态插件必须导出：

```c
ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor);
```

代理插件 `libktag_proxy_features.so` 通过该 ABI 注册 Sequence、TCP Signature 和
QUIC 三个 Provider。插件描述符和注册数据由宿主校验，插件卸载必须晚于使用它的
Registry、Schema 和 Feature Context。

### 1.3 Inference

模型包在创建 ONNX Session 前可以单独校验：

```c
ktag_status ktag_inference_validate_model_package(
    const char *model_package_path,
    const ktag_feature_schema *feature_schema,
    ktag_model_package_validation *validation);
```

模型包目录必须包含 `manifest.json`、`model.onnx`、`scaler.json`。校验内容包括
Schema Hash、Feature ID/维度/顺序、Scaler 维度和预处理配置。

批量推理接口：

```c
ktag_inference_context *ktag_inference_create(
    const ktag_inference_config *config);

ktag_status ktag_inference_predict(
    ktag_inference_context *context,
    const ktag_feature_batch *features,
    ktag_prediction_batch *predictions);
```

Inference Context 在第一次 `predict` 后由单一线程拥有。输入 Feature Batch 的
Schema Hash 不匹配时返回 `KTAG_ERROR_SCHEMA_MISMATCH`。

`ktag_prediction_record.label` 是模型数字标签；Inference API 不理解业务类别名称。
`confidence` 在 `[0,1]` 内时表示模型概率，0 是合法值，不会抑制或改写标签；
`KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE`（-1）表示没有可用概率输出，但标签仍有效。
当前接口不应用 unknown 阈值。

现代三层 API 的完整字段和所有函数以 `include/ktag_*.h` 为准。应用/代理配置与命令
参见《[应用分类与代理分类使用指南](demo_pipeline.md)》。

## 2. 旧特征提取兼容接口

以下固定 `feature_vector` 接口继续保留用于旧应用和旧模型兼容；新集成推荐使用上面
的 Feature Schema API。

### extractor_init

**功能**

创建并初始化特征提取器。

**函数接口**

````bash
feature_extractor *extractor_init(int link_type);
````

**参数**

* `link_type`： 链路层类型，支持`DLT_EN10MB(1)`、`DLT_LINUX_SLL(113)`。

**返回值**

* 特征提取器句柄，失败返回NULL。

### extractor_destroy

**功能**

销毁特征提取器。

**函数接口**

````bash
void extractor_destroy(feature_extractor *extractor);
````

**参数**

* `extractor`：  特征提取器句柄。

**返回值**

* 无

### process_packet

**功能**

处理单个数据包，进行packet的协议解析及单个流的特征提取。

**函数接口**

````bash
int process_packet(feature_extractor *extractor, const unsigned char *packet, uint32_t packet_len,
                   const struct timeval *ts, feature_vector *features, int *has_feature);
````

**参数**

* `extractor`：  特征提取器句柄。
* `packet`：  数据包指针。
* `packet_len`：  数据包长度。
* `ts`： 时间戳。
* `features`：  输出特征向量（单个流）。
* `has_feature`： 输出标志，表示是否有特征提取（流结束才会进行特征提取）。

**返回值**

* 状态码：EXTRACTOR_SUCCESS表示成功，EXTRACTOR_ERROR表示失败。

### process_packet_with_rawbow

**功能**

处理单个数据包（提取特征向量和rawbow），进行packet的协议解析及单个流的特征提取，包含rawbow。

**函数接口**

````bash
int process_packet_with_rawbow(feature_extractor *extractor, const unsigned char *packet, uint32_t packet_len,
                               const struct timeval *ts, feature_vector *features, rawbow *rawbows, int *has_feature);
````

**参数**

* `extractor`：  特征提取器句柄。
* `packet`：  数据包指针。
* `packet_len`：  数据包长度。
* `ts`： 时间戳。
* `features`：  输出特征向量（单个流）。
* `rawbows`： 输出rawbow（单个流）。
* `has_feature`： 输出标志，表示是否有特征提取（流结束才会进行特征提取）。

**返回值**

* 状态码：EXTRACTOR_SUCCESS表示成功，EXTRACTOR_ERROR表示失败。

### extractor_finalize

**功能**

处理所有剩余的流（仅提取特征向量）。

**函数接口**

````bash
int extractor_finalize(feature_extractor *extractor, feature_vector_list *features);
````

**参数**

* `extractor`：  特征提取器句柄。
* `features`：   输出特征向量列表。

**返回值**

* 状态码：EXTRACTOR_SUCCESS表示成功，EXTRACTOR_ERROR表示失败。

### extractor_finalize_with_rawbow

**功能**

处理所有剩余的流（提取特征向量和rawbow）。

**函数接口**

````bash
int extractor_finalize_with_rawbow(feature_extractor *extractor, feature_vector_list *features, rawbow_list *rawbows);
````

**参数**

* `extractor`：  特征提取器句柄。
* `features`：   输出特征向量列表。
* `rawbows`： 输出rawbow列表。

**返回值**

* 状态码：EXTRACTOR_SUCCESS表示成功，EXTRACTOR_ERROR表示失败。

### extractor_get_statistics

**功能**

获取统计信息。

**函数接口**

````bash
int extractor_get_statistics(feature_extractor *extractor, statistics *stats);
````

**参数**

* `extractor`：  特征提取器句柄。
* `stats`：   stats 输出统计信息。  

**返回值**

* 状态码：EXTRACTOR_SUCCESS表示成功，EXTRACTOR_ERROR表示失败。

### extractor_reset_statistics

**功能**

重置统计信息。

**函数接口**

````bash
void extractor_reset_statistics(feature_extractor *extractor);
````

**参数**

* `extractor`：  特征提取器句柄。

**返回值**

* 无

## 3. 旧特征推理兼容接口

### inference_init

**功能**

创建并初始化推理引擎。

**函数接口**

````bash
inference_engine *inference_init(const inference_config *config);
````

**参数**

* `config_file`：推理配置（包含配置文件路径）。

**返回值**

* 推理引擎句柄，失败返回NULL。

### inference_destroy

**功能**

销毁推理引擎。

**函数接口**

````bash
void inference_destroy(inference_engine *engine);
````

**参数**

* `engine`：推理引擎句柄。

**返回值**

* 无

### inference_predict

**功能**

批量推理。

**函数接口**

````bash
int inference_predict(inference_engine *engine, const feature_vector_list *features, inference_result *result);
````

**参数**

* `engine`：推理引擎句柄。
* `features`： 特征向量列表。
* `result`：输出推理结果。

**返回值**

* 状态码：INFERENCE_SUCCESS表示成功，INFERENCE_ERROR表示失败。
