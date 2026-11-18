# API 参考

本文以 `include/` 下发布的头文件为准，按模块分章介绍KTAG的公开C API。接口按功能说明、函数原型、参数说明和返回值说明组织；关键流程在章末补充最小用例。所有推理入口统一使用Capture、Feature、Inference三层API；历史文件格式的数据桥接见[冻结特征契约与数据桥接](#冻结特征契约与数据桥接)。

头文件路径以仓库根目录为基准；集成时将 `include/` 加入头文件搜索路径。

应用/代理分类配置和命令行操作见《[用户指南](user_guide.md)》；快速执行demo见《[快速入门](quick_start.md)》。

## 文档范围与公共约定

### 文档范围

| 头文件 | 职责 | 使用建议 |
|---|---|---|
| ktag_types.h | 状态码、时间戳、IP地址和Flow Key | 必需 |
| ktag_capture.h | PCAP/网口读取、报文生命周期、Flow Key、设备发现 | 推荐 |
| ktag_capture_dpdk.h | 可选DPDK轮询采集，输出公共报文对象 | 按需，需启用 KTAG_ENABLE_DPDK |
| ktag_feature.h | 组流、Schema、Provider、Domain Store、Feature Batch | 推荐 |
| ktag_feature_plugin.h | 动态Feature Provider插件ABI、公共加载与卸载接口 | 按需 |
| ktag_traffic_features.h | Traffic Provider直接注册入口与RawBoW注解键 | 按需 |
| ktag_inference.h | 模型包校验和Schema-aware批量推理 | 推荐 |
| ktag_legacy_types.h | 历史Demo文件格式的数据类型，不提供推理接口 | 按需 |
| ktag_core.h、ktag_type.h | 产品版本查询及其返回类型 | 按需 |

运行时对应三个共享库：`libktag_capture.so`、`libktag_feature.so`、`libktag_inference.so`。构建产物位于仓库 `output/` 目录，特征计算由 `output/plugins/` 中的Traffic/Proxy插件提供；可选DPDK采集库为 `libktag_capture_dpdk.so`，可选版本查询库为 `libktag_appidlib.so`。构建方式见《[编译指南](installation_guide.md)》。

### 公共约定

以下约定适用于Capture、DPDK Capture、Feature、Inference及插件加载API；产品版本接口使用独立的数据类型与状态码。

#### ABI 版本与版本化结构体

`KTAG_API_VERSION` 当前为 `1U`。作为输入传入API的**版本化结构体**（含 `struct_size` 和 `api_version`）应先清零，再设置这两个字段。需要校验版本头的输出结构体（如统计、模型包校验、传输层视图）也必须预先设置。

```c
ktag_capture_config config = {0};
config.struct_size = sizeof(config);
config.api_version = KTAG_API_VERSION;
```

输入配置、Feature描述符、Provider注册信息和追加记录的元数据均遵守上述初始化方式。`ktag_packet` 使用 `ktag_packet_init()` 初始化。Schema描述符查询、Flow Key查询及Batch记录查询等接口会直接填写输出版本头，不要求预设输出版本头；各接口的参数说明会区分这两种情况。没有版本头的类型（如 `ktag_timestamp`）不适用。返回状态码的接口通常以 `KTAG_ERROR_INVALID_ARGUMENT` 报告不合法的版本头，指针返回型创建接口则返回 `NULL`。

#### 状态码

所有返回 `ktag_status` 的API使用以下状态码。

| 状态码 | 值 | 含义 |
|---|---:|---|
| KTAG_SUCCESS | 0 | 成功 |
| KTAG_STATUS_TIMEOUT | 1 | 实时读取超时，无报文，不是错误 |
| KTAG_STATUS_END_OF_STREAM | 2 | PCAP读完或读取被中断 |
| KTAG_ERROR_INVALID_ARGUMENT | -1 | 指针、版本化结构体、枚举或参数不合法 |
| KTAG_ERROR_OUT_OF_MEMORY | -2 | 内存分配失败 |
| KTAG_ERROR_ALREADY_EXISTS | -3 | 对象或注册项已存在 |
| KTAG_ERROR_NOT_FOUND | -4 | 查找对象不存在 |
| KTAG_ERROR_INVALID_STATE | -5 | 生命周期或线程归属不符合要求 |
| KTAG_ERROR_CAPACITY | -6 | 容量、批次大小或资源不足 |
| KTAG_ERROR_SCHEMA_MISMATCH | -7 | Schema、模型包或Feature Batch不一致 |
| KTAG_ERROR_INTERNAL | -8 | 内部错误 |
| KTAG_ERROR_NOT_SUPPORTED | -9 | 不支持的操作 |
| KTAG_ERROR_IO | -10 | I/O错误 |

#### 错误详情

Capture、DPDK Capture、插件加载和Inference分别提供线程本地错误详情：`ktag_capture_get_last_error()`、`ktag_dpdk_capture_get_last_error()`、`ktag_feature_plugin_get_last_error()` 和 `ktag_inference_get_last_error()`。返回字符串由库管理，应在下一次同类调用前记录，不能释放或跨线程保存。

#### 对象所有权

- 文中标注为opaque的对象只能通过对应API创建、传递和释放，调用者不得访问其内部布局。
- 由 `*_create`/`*_open` 返回的对象，使用对应 `*_destroy`/`*_close` 释放；`NULL` 参数对destroy类接口安全。
- 除非接口明确转移所有权，输入对象的所有权仍归调用者。
- 返回 `const char *` 的查询接口，如无特别说明，字符串由被查询对象持有，在该对象销毁前有效。

#### 线程安全

- Feature Context：首次调用 `process`/`expire`/`flush`/统计接口后绑定到单一线程；不同Context可并行。
- Inference Context：首次 `predict` 后绑定到单一线程；不同Context可并行。
- Domain Store：变更和统计接口线程安全，可被多个Context共享。
- Domain Cache、Feature Batch、Prediction Batch：单线程对象。
- `get_last_error` 系列接口：返回线程本地数据。

## 基础类型（ktag_types.h）

`ktag_types.h` 只定义类型和常量，没有函数。所有现代API都间接依赖它。

| 类型 | 说明 |
|---|---|
| ktag_status | int32_t，状态码，见[状态码](#状态码) |
| ktag_struct_header | 版本化结构体公共头：struct_size + api_version |
| ktag_timestamp | seconds（int64）+ microseconds（int32） |
| ktag_ip_address | bytes[16] + version（4或6）+ reserved[3] |
| ktag_flow_key | 双向规范化五元组，见下文 |

`ktag_ip_address` 使用 `version` 标记IPv4/IPv6；IPv4地址位于 `bytes[0..3]`，其余字节为0。IPv6使用全部16字节，网络字节序。

`ktag_flow_key` 是**双向规范化**五元组。

```c
typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    ktag_ip_address address_a;
    ktag_ip_address address_b;
    uint16_t port_a;
    uint16_t port_b;
    uint8_t protocol;
    uint8_t reserved[3];
} ktag_flow_key;
```

端点A按地址/端口排序后不大于端点B，因此同一业务流的正向和反向报文归入同一个Key。它适合哈希、分流和关联，但**不表示首包的源/目的方向**。需要首包方向五元组时，使用 `ktag_flow_view_get_endpoint()`（见[Flow View 查询 API](#flow-view-查询-api)）或 `ktag_feature_record_details`（见[记录结构](#记录结构)）。

## Capture API（ktag_capture.h）

Capture层负责从PCAP文件或网口读取报文，管理报文内存生命周期，并提供轻量L2/L3/L4解析、Flow Key构造、稳定哈希与worker分流。本层不创建线程。

### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 打开/关闭 | ktag_capture_open | 打开PCAP文件或网口。 |
| 打开/关闭 | ktag_capture_close | 关闭Capture上下文。 |
| 打开/关闭 | ktag_capture_interrupt | 唤醒阻塞中的读取。 |
| 打开/关闭 | ktag_capture_get_link_type | 查询链路层类型。 |
| 读取 | ktag_packet_init | 初始化报文存储对象。 |
| 读取 | ktag_capture_next | 读取下一个报文。 |
| 读取 | ktag_capture_get_last_error | 查询最近错误详情。 |
| 报文所有权 | ktag_packet_make_owned | 借用报文转拥有副本。 |
| 报文所有权 | ktag_packet_retain | 增加一个引用。 |
| 报文所有权 | ktag_packet_release | 释放一个引用。 |
| 报文所有权 | ktag_packet_is_owned | 查询报文是否为拥有型。 |
| Flow Key | ktag_packet_get_flow_key | 解析报文并构造规范化Flow Key。 |
| Flow Key | ktag_flow_hash | Flow Key稳定哈希。 |
| Flow Key | ktag_flow_select_worker | 按Flow Key选择worker。 |
| 设备发现 | ktag_capture_list_devices | 枚举本机抓包设备。 |
| 设备发现 | ktag_capture_device_list_destroy | 释放设备列表。 |
| 设备发现 | ktag_capture_device_list_get_count | 设备数量。 |
| 设备发现 | ktag_capture_device_list_get_name | 设备名。 |
| 设备发现 | ktag_capture_device_list_get_description | 设备描述。 |
| 设备发现 | ktag_capture_device_list_get_ipv4_count | 设备IPv4地址数。 |
| 设备发现 | ktag_capture_device_list_get_ipv4 | 设备IPv4地址。 |
| 设备发现 | ktag_capture_device_list_get_ipv6_count | 设备IPv6地址数。 |
| 设备发现 | ktag_capture_device_list_get_ipv6 | 设备IPv6地址。 |

### 对象与常量

**链路层类型常量**。

| 常量 | 值 | 说明 |
|---|---:|---|
| KTAG_CAPTURE_LINKTYPE_ETHERNET | 1 | 以太网 |
| KTAG_CAPTURE_LINKTYPE_RAW | 12 | RAW IP |
| KTAG_CAPTURE_LINKTYPE_LINUX_SLL | 113 | Linux SLL |
| KTAG_CAPTURE_LINKTYPE_LINUX_SLL2 | 276 | Linux SLL2 |

**输入源类型 `ktag_capture_source_type`**。

| 枚举值 | 说明 |
|---|---|
| KTAG_CAPTURE_SOURCE_PCAP_FILE（1） | 离线PCAP文件 |
| KTAG_CAPTURE_SOURCE_LIVE_INTERFACE（2） | 实时网口 |

**报文模式 `ktag_capture_packet_mode`**。

| 枚举值 | 说明 |
|---|---|
| KTAG_CAPTURE_PACKET_BORROWED（0） | data 仅在下一次 next() 或Capture关闭前有效，低拷贝。 |
| KTAG_CAPTURE_PACKET_OWNED（1） | data 复制到引用计数PacketBuffer，可跨线程排队。 |

**`ktag_capture_config` 字段**。

| 字段 | 说明 |
|---|---|
| source_type | 输入源类型，见上表 |
| source | PCAP文件路径或网口名，不能为空 |
| packet_mode | 报文模式，见上表 |
| snapshot_length | 抓包长度，0 使用库默认值 |
| promiscuous | 非0时开启混杂模式（仅网口） |
| read_timeout_ms | 网口读取超时（毫秒），0 使用库默认值 |
| live_buffer_size | 网口缓冲区字节数，0 使用libpcap默认值 |
| packet_buffer_capacity | OWNED 模式报文池容量，0 使用库默认值 |

**`ktag_packet` 字段**（版本化结构体）。

| 字段 | 说明 |
|---|---|
| data | 报文数据指针。有效范围由报文模式决定 |
| captured_length | 实际捕获的字节数 |
| original_length | 线路上的原始字节数（可能大于 captured_length） |
| link_type | 链路层类型 |
| sequence | 本次会话内的报文序号 |
| timestamp | 报文时间戳 |
| owner | 拥有型报文的PacketBuffer引用，借用型为 NULL |

### `ktag_capture_open`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

打开一个离线PCAP文件或实时网口，返回Capture上下文。本函数不创建任何工作线程。

#### 函数原型

```c
KTAG_API ktag_capture_context *
ktag_capture_open(const ktag_capture_config *config);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| config | 输入 | 配置结构体，必须按[ABI 版本与版本化结构体](#abi-版本与版本化结构体) 初始化版本头；source 不能为空。 |

#### 返回值说明

| 返回值 | 说明 |
|---|---|
| 非 NULL | 打开成功，调用者用 ktag_capture_close() 释放。 |
| NULL | 打开失败，调用 ktag_capture_get_last_error() 获取详情。 |

### `ktag_packet_init`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

初始化一个报文存储对象，使其可以首次传入 `ktag_capture_next()`。

**使用约束**

全新的 `ktag_packet` 存储必须先调用本函数才能用于 `ktag_capture_next()`；`ktag_packet_release()` 之后对象自动恢复为可复用状态，无需重新init。

#### 函数原型

```c
KTAG_API void ktag_packet_init(ktag_packet *packet);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入/输出 | 待初始化的报文对象。 |

#### 返回值说明

无。

### `ktag_capture_next`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

从Capture上下文读取下一个报文。

**使用约束**

- `BORROWED` 报文的 `data` 仅在下一次 `next()` 或Capture关闭前有效，不能跨线程排队；需要跨线程时调用 `ktag_packet_make_owned()`。
- `OWNED` 报文在重新传入同一输出对象前必须先 `ktag_packet_release()`。

#### 函数原型

```c
KTAG_API ktag_status ktag_capture_next(ktag_capture_context *context,
                                       ktag_packet *packet);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 已打开的Capture上下文。 |
| packet | 输出 | 输出参数。必须先用 ktag_packet_init() 初始化；上一个拥有型报文必须先 ktag_packet_release() 释放后才能再次传入同一对象。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 读取成功，packet 已填充。 |
| KTAG_STATUS_TIMEOUT | 实时读取超时，无报文，可继续调用。 |
| KTAG_STATUS_END_OF_STREAM | PCAP已读完，或读取被中断。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_INVALID_STATE | 输出报文仍持有尚未释放的拥有型引用。 |
| KTAG_ERROR_CAPACITY | 报文池耗尽或报文序号空间耗尽。 |
| KTAG_ERROR_IO | libpcap读取失败或返回不合法报文。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

### `ktag_capture_get_link_type`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询Capture上下文的链路层类型。

#### 函数原型

```c
KTAG_API uint32_t
ktag_capture_get_link_type(const ktag_capture_context *context);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 已打开的Capture上下文。 |

#### 返回值说明

返回链路层类型常量，见“对象与常量”；`context` 为 `NULL` 时返回 `0`。

### `ktag_capture_interrupt`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

从另一线程唤醒阻塞中的 `ktag_capture_next()` 调用，该读取随后返回 `KTAG_STATUS_END_OF_STREAM`。

**使用约束**

停止读取后再关闭Capture；中断后不要再访问其产生的借用型报文。

#### 函数原型

```c
KTAG_API void ktag_capture_interrupt(ktag_capture_context *context);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 已打开的Capture上下文。 |

#### 返回值说明

无。

### `ktag_capture_close`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

关闭Capture上下文并释放全部关联资源。

#### 函数原型

```c
KTAG_API void ktag_capture_close(ktag_capture_context *context);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待关闭的上下文；可为 NULL。 |

#### 返回值说明

无。

### `ktag_capture_get_last_error`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

返回当前线程最近一次Capture API失败的详情字符串。

#### 函数原型

```c
KTAG_API const char *ktag_capture_get_last_error(void);
```

#### 参数说明

无。

#### 返回值说明

错误详情字符串；无错误时可能为空字符串。字符串由库管理，在下一次同类调用前有效，不能释放或跨线程保存。

### `ktag_packet_make_owned`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

把借用型报文转换为独立的拥有型副本，此后可以安全地跨线程排队，并可以继续调用 `ktag_capture_next()`。

#### 函数原型

```c
KTAG_API ktag_status ktag_packet_make_owned(ktag_packet *packet);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入/输出 | 待转换的报文对象。转换成功后原 data 指针由PacketBuffer支撑。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

### `ktag_packet_retain`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

为拥有型报文增加一个引用。多线程共享同一报文时：先复制 `ktag_packet` 值，再对每个副本调用本函数；每个视图最后各调用一次 `ktag_packet_release()`。

#### 函数原型

```c
KTAG_API ktag_status ktag_packet_retain(ktag_packet *packet);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入/输出 | 拥有型报文视图。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_STATE | 报文为空、版本头不合法或报文不是拥有型。 |
| KTAG_ERROR_CAPACITY | 引用计数为零或已达到 uint32_t 上限。 |

### `ktag_packet_release`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

释放拥有型报文的一个引用，并清空该视图。引用归零时报文内存归还报文池。

#### 函数原型

```c
KTAG_API void ktag_packet_release(ktag_packet *packet);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入/输出 | 待释放的报文视图。释放后对象保持已初始化状态，可再次用于 ktag_capture_next()。 |

#### 返回值说明

无。

### `ktag_packet_is_owned`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询报文当前是否为拥有型。

#### 函数原型

```c
KTAG_API uint32_t ktag_packet_is_owned(const ktag_packet *packet);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入 | 待查询的报文。 |

#### 返回值说明

非0表示拥有型，`0` 表示借用型。

### `ktag_packet_get_flow_key`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

对报文做轻量只读L2/L3/L4解析，输出双向规范化的Flow Key。

**使用约束**

只有本函数成功返回的Key才能用于 `ktag_flow_hash` / `ktag_flow_select_worker`。

#### 函数原型

```c
KTAG_API ktag_status ktag_packet_get_flow_key(const ktag_packet *packet,
                                              ktag_flow_key *key);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入 | 已初始化且有效的报文对象。 |
| key | 输出 | 非空的Flow Key存储；成功时由接口填写全部字段和版本头，无需预设输出版本头。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 解析成功，输出规范化Flow Key。 |
| KTAG_ERROR_INVALID_ARGUMENT | 指针、报文版本头、报文长度或报文内容不合法。 |
| KTAG_ERROR_NOT_SUPPORTED | 链路层、网络层或传输层协议不受支持。 |
| KTAG_ERROR_NOT_FOUND | 报文不能提供所需的传输层五元组，例如非首分片。 |
| KTAG_ERROR_CAPACITY | 报文解析超过实现允许的限制。 |

### `ktag_flow_hash`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

计算Flow Key的稳定哈希。只哈希逻辑字段，不受C结构体填充影响，同一Key在不同进程/平台得到相同结果。

#### 函数原型

```c
KTAG_API uint64_t ktag_flow_hash(const ktag_flow_key *key, uint64_t seed);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| key | 输入 | 有效的Flow Key（来自 ktag_packet_get_flow_key() 成功返回）。 |
| seed | 输入 | 哈希种子。相同Key与不同seed得到不同结果。 |

#### 返回值说明

返回64位稳定哈希值；Key不合法时返回 `0`。哈希值本身不是错误码。

### `ktag_flow_select_worker`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

按Flow Key哈希选择一个worker下标，用于多线程分流。同一流的所有报文（正反方向）映射到同一worker。

#### 函数原型

```c
KTAG_API uint32_t ktag_flow_select_worker(const ktag_flow_key *key,
                                          uint32_t worker_count, uint64_t seed);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| key | 输入 | 有效的Flow Key。 |
| worker_count | 输入 | worker数量。 |
| seed | 输入 | 哈希种子，与 ktag_flow_hash 语义一致。 |

#### 返回值说明

| 返回值 | 说明 |
|---|---|
| 0 .. worker_count-1 | 选中的worker下标。 |
| KTAG_WORKER_INDEX_INVALID（UINT32_MAX） | worker_count 为0或Key不合法。 |

<a id="设备发现-api"></a>

**设备发现API**

### `ktag_capture_list_devices`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

枚举libpcap可见的本机抓包设备，不暴露libpcap类型。

#### 函数原型

```c
KTAG_API ktag_capture_device_list *ktag_capture_list_devices(void);
```

#### 参数说明

无。

#### 返回值说明

成功返回设备列表，由调用方使用 `ktag_capture_device_list_destroy()` 释放；失败返回 `NULL`，可查询 `ktag_capture_get_last_error()`。

### `ktag_capture_device_list_destroy`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

销毁抓包设备列表，释放列表及其名称、描述和地址字符串。

#### 函数原型

```c
KTAG_API void
ktag_capture_device_list_destroy(ktag_capture_device_list *devices);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 待释放的对象；可为 NULL。 |

#### 返回值说明

无。

### `ktag_capture_device_list_get_count`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询抓包设备数量。

#### 函数原型

```c
KTAG_API uint32_t
ktag_capture_device_list_get_count(const ktag_capture_device_list *devices);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 设备列表。 |

#### 返回值说明

返回设备数量；`devices` 为 `NULL` 时返回 `0`。

### `ktag_capture_device_list_get_name`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询指定设备的名称。

#### 函数原型

```c
KTAG_API const char *
ktag_capture_device_list_get_name(const ktag_capture_device_list *devices,
                                  uint32_t index);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 设备列表。 |
| index | 输入 | 设备下标，范围为 [0, count)。 |

#### 返回值说明

返回设备名称；列表为空或下标越界时返回 `NULL`。字符串由设备列表持有，在列表销毁前有效。

### `ktag_capture_device_list_get_description`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询指定设备的描述。

#### 函数原型

```c
KTAG_API const char *ktag_capture_device_list_get_description(
    const ktag_capture_device_list *devices, uint32_t index);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 设备列表。 |
| index | 输入 | 设备下标，范围为 [0, count)。 |

#### 返回值说明

返回设备描述；列表为空、下标越界或描述为空时返回 `NULL`。字符串由设备列表持有，在列表销毁前有效。

### `ktag_capture_device_list_get_ipv4_count`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询指定设备的IPv4地址数量。

#### 函数原型

```c
KTAG_API uint32_t ktag_capture_device_list_get_ipv4_count(
    const ktag_capture_device_list *devices, uint32_t index);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 设备列表。 |
| index | 输入 | 设备下标，范围为 [0, count)。 |

#### 返回值说明

返回IPv4地址数量；列表为空或设备下标越界时返回 `0`。

### `ktag_capture_device_list_get_ipv4`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询指定设备的IPv4地址字符串。

#### 函数原型

```c
KTAG_API const char *
ktag_capture_device_list_get_ipv4(const ktag_capture_device_list *devices,
                                  uint32_t device_index,
                                  uint32_t address_index);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 设备列表。 |
| device_index | 输入 | 设备下标，范围为 [0, count)。 |
| address_index | 输入 | IPv4地址下标，范围为 [0, 对应地址数量)。 |

#### 返回值说明

返回地址字符串，由设备列表持有，在列表销毁前有效；列表为空、设备或地址下标越界时返回 `NULL`。

### `ktag_capture_device_list_get_ipv6_count`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询指定设备的IPv6地址数量。

#### 函数原型

```c
KTAG_API uint32_t ktag_capture_device_list_get_ipv6_count(
    const ktag_capture_device_list *devices, uint32_t index);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 设备列表。 |
| index | 输入 | 设备下标，范围为 [0, count)。 |

#### 返回值说明

返回IPv6地址数量；列表为空或设备下标越界时返回 `0`。

### `ktag_capture_device_list_get_ipv6`

#### 功能说明

头文件路径为：`"include/ktag_capture.h"`。

查询指定设备的IPv6地址字符串。

#### 函数原型

```c
KTAG_API const char *
ktag_capture_device_list_get_ipv6(const ktag_capture_device_list *devices,
                                  uint32_t device_index,
                                  uint32_t address_index);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| devices | 输入 | 设备列表。 |
| device_index | 输入 | 设备下标，范围为 [0, count)。 |
| address_index | 输入 | IPv6地址下标，范围为 [0, 对应地址数量)。 |

#### 返回值说明

返回地址字符串，由设备列表持有，在列表销毁前有效；列表为空、设备或地址下标越界时返回 `NULL`。

### 最小用例

读取一个PCAP文件，解析每个报文的Flow Key，并统计报文总数和可解析报文数；示例不去重统计流数。

```c
#include <stdio.h>
#include <stdint.h>
#include "ktag_capture.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s INPUT.pcap\n", argv[0]);
        return 1;
    }

    ktag_capture_config config = {0};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.source_type = KTAG_CAPTURE_SOURCE_PCAP_FILE;
    config.source = argv[1];
    config.packet_mode = KTAG_CAPTURE_PACKET_BORROWED;

    /* 打开采集源；失败时读取线程本地错误详情。 */
    ktag_capture_context *capture = ktag_capture_open(&config);
    if (capture == NULL) {
        fprintf(stderr, "open failed: %s\n", ktag_capture_get_last_error());
        return 1;
    }

    uint64_t total = 0U, parsed = 0U;
    ktag_packet packet;
    /* 初始化可重复使用的报文存储。 */
    ktag_packet_init(&packet);
    for (;;) {
        /* 读取报文；TIMEOUT可重试，EOF结束循环。 */
        ktag_status status = ktag_capture_next(capture, &packet);
        if (status == KTAG_STATUS_TIMEOUT) {
            continue;
        }
        if (status != KTAG_SUCCESS) {
            break; /* END_OF_STREAM 或错误 */
        }
        ++total;

        ktag_flow_key key = {0};
        key.struct_size = sizeof(key);
        key.api_version = KTAG_API_VERSION;
        if (ktag_packet_get_flow_key(&packet, &key) == KTAG_SUCCESS) {
            ++parsed;
        }
        /* 释放本次报文引用后再读取下一包。 */
        ktag_packet_release(&packet);
    }

    /* 停止读取后关闭采集源。 */
    ktag_capture_close(capture);
    printf("packets=%llu parsed=%llu\n",
           (unsigned long long)total, (unsigned long long)parsed);
    return 0;
}
```

将代码保存为 `capture_example.c`，在仓库根目录执行。

```bash
bash /opt/KTAG/buildtools/build.sh   # 若尚未构建，产物位于 /opt/KTAG/output/
cc -std=c11 -I/opt/KTAG/include /opt/KTAG/capture_example.c \
   -L/opt/KTAG/output -lktag_capture -Wl,-rpath,/opt/KTAG/output \
   -o /opt/KTAG/output/capture_example
/opt/KTAG/output/capture_example sample.pcap
```

正确输出（数字随输入文件变化）。

```text
packets=128 parsed=126
```

## DPDK Capture API（ktag_capture_dpdk.h）

可选的DPDK轮询收包接口，输出与libpcap Capture层相同的 `ktag_packet`，可继续传给Feature/Inference。启用 `KTAG_ENABLE_DPDK` 后构建 `libktag_capture_dpdk.so`，需链接该库及其依赖；DPDK至少为24.11，并准备支持的网卡驱动和EAL所需内存。该采集后端与UB分布式传输分别启用。

每个采集Context由一个线程驱动；`ktag_dpdk_capture_interrupt()` 可从其他线程调用。EAL初始化应在主线程进行，首次成功初始化的参数生效，后续调用不重新配置EAL。

### 配置结构体

`ktag_dpdk_capture_config` 必须初始化 `struct_size` 和 `api_version`。

| 字段 | 说明 |
| :--- | :--- |
| device_name | PCI地址或vdev名称；空指针或空字符串时自动选择可用端口，优先PCI端口 |
| rx_queue_count | 接收队列数，0使用1，最大256；多队列时轮询读取，在设备支持时启用RSS |
| rx_queue_depth | 接收队列深度，0使用1024，最大65535；DPDK可按设备能力调整实际深度 |
| mbuf_pool_size | mbuf池大小；0时按传入的 rx_queue_depth × 2 计算，此时应显式设置非零 rx_queue_depth |
| burst_size | 每次burst最大包数，0使用64，最大64 |
| packet_buffer_capacity | 保留字段，当前实现不读取，不限制OWNED模式的报文副本数量 |
| packet_mode | KTAG_DPDK_PACKET_BORROWED（0）或 KTAG_DPDK_PACKET_OWNED（1） |
| rx_idle_eof_us | 持续无报文达到该微秒数后返回EOF；0禁用 |
| poll_interval_us | 空轮询后的延时，单位为微秒；0为忙轮询 |

BORROWED模式的数据只在下一次 `next()` 或Context关闭前有效；OWNED模式复制数据后立即释放mbuf，调用方通过 `ktag_packet_release()` 释放报文副本。

### `ktag_dpdk_eal_init`

#### 功能说明

头文件路径为：`"include/ktag_capture_dpdk.h"`。

显式初始化DPDK EAL。调用方不需要在参数串中提供程序名。已初始化时直接成功，不会应用新的参数。

#### 函数原型

```c
KTAG_API ktag_status ktag_dpdk_eal_init(const char *eal_args);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| eal_args | 输入 | 空格分隔的EAL参数，支持引号和反斜杠转义；可为 NULL，表示不额外传入参数。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | EAL参数分词后超过64项。 |
| KTAG_ERROR_IO | DPDK EAL初始化失败。 |

### `ktag_dpdk_capture_open`

#### 功能说明

头文件路径为：`"include/ktag_capture_dpdk.h"`。

打开DPDK接收端口、创建mbuf池和接收队列。若尚未初始化EAL，则以空参数初始化；需要自定义EAL参数时先调用 `ktag_dpdk_eal_init()`。

#### 函数原型

```c
KTAG_API void *ktag_dpdk_capture_open(const ktag_dpdk_capture_config *config);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| config | 输入 | 非空的版本化DPDK配置，字段及范围见“配置结构体”。 |

#### 返回值说明

成功返回不透明Context指针；配置、EAL、设备、内存池或队列初始化失败时返回 `NULL`，使用 `ktag_dpdk_capture_get_last_error()` 查询详情。

### `ktag_dpdk_capture_next`

#### 功能说明

头文件路径为：`"include/ktag_capture_dpdk.h"`。

轮询读取一个报文。需先以 `ktag_packet_init()` 初始化输出报文，并在复用前释放其拥有型引用。单个Context不支持多线程并发调用。

#### 函数原型

```c
KTAG_API ktag_status ktag_dpdk_capture_next(void *context, ktag_packet *packet);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | ktag_dpdk_capture_open() 返回的Context。 |
| packet | 输出 | 已初始化且不持有拥有型引用的报文对象。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_STATUS_TIMEOUT | 本次轮询无报文。 |
| KTAG_STATUS_END_OF_STREAM | Context已中断或持续空闲达到配置阈值。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_INVALID_STATE | 输出报文仍持有拥有型引用。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

### `ktag_dpdk_capture_get_link_type`

#### 功能说明

头文件路径为：`"include/ktag_capture_dpdk.h"`。

查询DPDK采集Context的链路层类型。

#### 函数原型

```c
KTAG_API uint32_t ktag_dpdk_capture_get_link_type(const void *context);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待查询的对象。 |

#### 返回值说明

返回链路层类型；`context` 为 `NULL` 时返回 `0`。

### `ktag_dpdk_capture_interrupt`

#### 功能说明

头文件路径为：`"include/ktag_capture_dpdk.h"`。

设置停止标志，可从其他线程调用；后续 `ktag_dpdk_capture_next()` 返回EOF。

#### 函数原型

```c
KTAG_API void ktag_dpdk_capture_interrupt(void *context);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | DPDK采集Context；可为 NULL。 |

#### 返回值说明

无。

### `ktag_dpdk_capture_close`

#### 功能说明

头文件路径为：`"include/ktag_capture_dpdk.h"`。

停止并关闭DPDK采集，释放待处理mbuf、端口和内存池。须先停止使用该Context的采集线程。

#### 函数原型

```c
KTAG_API void ktag_dpdk_capture_close(void *context);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待释放的对象；可为 NULL。 |

#### 返回值说明

无。

### `ktag_dpdk_capture_get_last_error`

#### 功能说明

头文件路径为：`"include/ktag_capture_dpdk.h"`。

查询当前线程最近一次DPDK采集API错误详情。

#### 函数原型

```c
KTAG_API const char *ktag_dpdk_capture_get_last_error(void);
```

#### 参数说明

无。

#### 返回值说明

返回库持有的错误字符串，无错误时为空字符串；应在下一次相关调用前使用，不得释放或跨线程保存。

## Feature API（ktag_feature.h）

Feature层先将报文组成流，再调度已注册的Provider计算特征，最后按Schema契约输出Feature Batch。

本章按对象族说明：Schema（[Schema API](#schema-api)）、Registry与Provider（[Registry 与 Provider API](#registry-与-provider-api)）、Provider视图与输出（[Provider 回调、视图与输出 API](#provider-回调视图与输出-api)）、Feature Context（[Feature Context API](#feature-context-api)）、Feature Batch（[Feature Batch API](#feature-batch-api)）、Domain Store（[Domain Store API](#domain-store-api)）、Domain Cache（[Domain Cache API](#domain-cache-api)）。

### 对象关系总览

```text
ktag_feature_registry ──register──▶ Provider（自定义/插件）
        │
        ▼
ktag_feature_schema（freeze 后不可变，绑定 Feature ID/维度/顺序）
        │
        ▼
ktag_feature_context（组流 + Provider Pipeline，单线程拥有）
        │  process/expire/flush
        ▼
ktag_feature_batch（动态记录数组，Schema Hash 必须与 Context 一致）

ktag_domain_store（IP→域名权威存储，线程安全，可跨 Context 共享）
        │ sync
        ▼
ktag_domain_cache（每 Context 本地缓存，无锁热路径）
```

典型创建顺序：创建Registry → 注册Provider → 创建/组合Schema → Freeze → 创建Feature Context（+可选共享Domain Store）→ 创建Feature Batch。

### Schema API

Schema固化模型输入的Feature ID、类型、维度和顺序；创建后可修改，Freeze后不可变。Schema Hash是Feature Batch、模型包与Context之间的一致性凭证。

#### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | ktag_feature_schema_create | 创建Schema。 |
| 生命周期 | ktag_feature_schema_destroy | 销毁Schema。 |
| 组合 | ktag_feature_schema_add | 添加Feature描述符。 |
| 组合 | ktag_feature_schema_add_registered | 从Registry添加已注册Feature。 |
| 组合 | ktag_feature_schema_remove | 移除Feature。 |
| 组合 | ktag_feature_schema_freeze | 冻结Schema。 |
| 组合 | ktag_feature_schema_create_legacy_v1 | 旧354值契约工厂。 |
| 查询 | ktag_feature_schema_is_frozen | 是否已冻结。 |
| 查询 | ktag_feature_schema_get_feature_count | Feature数量。 |
| 查询 | ktag_feature_schema_get_value_count | 值总数量。 |
| 查询 | ktag_feature_schema_get_hash | Schema Hash。 |
| 查询 | ktag_feature_schema_get_name | Schema名称。 |
| 查询 | ktag_feature_schema_get_version | Schema版本。 |
| 查询 | ktag_feature_schema_get_descriptor | 按下标取描述符。 |
| 查询 | ktag_feature_schema_find | 按Feature ID查找。 |

#### `ktag_feature_descriptor`

Feature描述符是Schema的组成单元（版本化结构体）。

| 字段 | 说明 |
|---|---|
| feature_id | Feature唯一标识，如 "customer.packet_counts"，最长255字节 |
| display_name | 展示名，最长255字节 |
| provider_id | 提供方标识，最长255字节 |
| provider_version | 提供方版本，必须大于 0 |
| value_type | 值类型，当前仅有 KTAG_FEATURE_FLOAT32（1） |
| dimension | 该Feature输出的float值个数，必须大于 0 |

#### `ktag_feature_schema_create`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

创建一个可修改的空Schema。

**使用约束**

Feature ID、维度或顺序发生变更时，必须使用新的Schema名称/版本并重新训练模型，不能复用旧Hash。

##### 函数原型

```c
KTAG_API ktag_feature_schema *
ktag_feature_schema_create(const char *schema_name, uint32_t schema_version);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema_name | 输入 | 非空Schema名称，长度为1～255字节。 |
| schema_version | 输入 | Schema版本号，必须大于 0。 |

##### 返回值说明

成功返回新Schema，失败返回 `NULL`。

#### `ktag_feature_schema_add`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

向Schema添加一个Feature描述符。仅可在Freeze前调用。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_schema_add(
    ktag_feature_schema *schema, const ktag_feature_descriptor *descriptor);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |
| descriptor | 输入 | 版本化描述符，按[ABI 版本与版本化结构体](#abi-版本与版本化结构体) 初始化。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_INVALID_STATE | Schema已冻结。 |
| KTAG_ERROR_ALREADY_EXISTS | Feature ID已存在。 |
| KTAG_ERROR_CAPACITY | 值总数超出 uint32_t 范围。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_schema_add_registered`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按Feature ID把Registry中已注册Provider的Feature加入Schema，保证描述符与Provider声明一致。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_schema_add_registered(
    ktag_feature_schema *schema, const ktag_feature_registry *registry,
    const char *feature_id);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |
| registry | 输入 | 已注册目标Feature的Registry。 |
| feature_id | 输入 | 要添加的Feature ID。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_NOT_FOUND | Registry中不存在该Feature。 |
| KTAG_ERROR_INVALID_STATE | Schema已冻结。 |
| KTAG_ERROR_ALREADY_EXISTS | Schema中已存在该Feature ID。 |
| KTAG_ERROR_CAPACITY | 值总数超出 uint32_t 范围。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_schema_remove`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按Feature ID从Schema移除一个Feature。仅可在Freeze前调用。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_schema_remove(ktag_feature_schema *schema,
                                                const char *feature_id);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |
| feature_id | 输入 | 要移除的Feature ID。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_NOT_FOUND | Feature不存在。 |
| KTAG_ERROR_INVALID_STATE | Schema已冻结。 |

#### `ktag_feature_schema_freeze`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

冻结Schema。冻结后不可再add/remove，Schema Hash最终确定。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_schema_freeze(ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_INVALID_STATE | Schema为空，未添加任何Feature。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_schema_destroy`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

销毁Schema并释放全部关联内存。

##### 函数原型

```c
KTAG_API void ktag_feature_schema_destroy(ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 待销毁的Schema；可为 NULL。 |

##### 返回值说明

无。

<a id="schema-查询-api"></a>

**Schema查询API**

#### `ktag_feature_schema_is_frozen`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Schema是否已冻结。

##### 函数原型

```c
KTAG_API uint32_t
ktag_feature_schema_is_frozen(const ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |

##### 返回值说明

已冻结返回 `1`，未冻结或 `schema` 为 `NULL` 时返回 `0`。

#### `ktag_feature_schema_get_feature_count`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Schema中的Feature描述符数量。

##### 函数原型

```c
KTAG_API uint32_t
ktag_feature_schema_get_feature_count(const ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |

##### 返回值说明

返回Feature数量；`schema` 为 `NULL` 时返回 `0`。

#### `ktag_feature_schema_get_value_count`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询全部Feature的维度之和，即每条记录的float值数量。

##### 函数原型

```c
KTAG_API uint32_t
ktag_feature_schema_get_value_count(const ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |

##### 返回值说明

返回值总数；`schema` 为 `NULL` 时返回 `0`。

#### `ktag_feature_schema_get_hash`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询冻结Schema的契约Hash。

##### 函数原型

```c
KTAG_API uint64_t
ktag_feature_schema_get_hash(const ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |

##### 返回值说明

返回Schema Hash；Schema未冻结或为 `NULL` 时返回 `0`。

#### `ktag_feature_schema_get_name`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Schema名称。

##### 函数原型

```c
KTAG_API const char *
ktag_feature_schema_get_name(const ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |

##### 返回值说明

返回Schema持有的名称字符串，在Schema销毁前有效；`schema` 为 `NULL` 时返回 `NULL`。

#### `ktag_feature_schema_get_version`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Schema版本号。

##### 函数原型

```c
KTAG_API uint32_t
ktag_feature_schema_get_version(const ktag_feature_schema *schema);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 目标Schema。 |

##### 返回值说明

返回版本号；`schema` 为 `NULL` 时返回 `0`。

#### `ktag_feature_schema_get_descriptor`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按下标取出Feature描述符及其在值数组中的偏移。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_schema_get_descriptor(
    const ktag_feature_schema *schema, uint32_t index,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 已冻结的Schema。 |
| index | 输入 | Feature下标，范围为 [0, feature_count)。 |
| descriptor | 输出 | 非空描述符存储；接口填写版本头，字符串由Schema持有并在其销毁前有效。 |
| value_offset | 输出 | 必须非空，Feature首个值在记录值数组中的偏移。 |

##### 返回值说明

| 返回值 | 说明 |
|---|---|
| KTAG_SUCCESS | 取得描述符。 |
| KTAG_ERROR_INVALID_ARGUMENT | index 越界、Schema未冻结或参数为空。 |

#### `ktag_feature_schema_find`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按Feature ID查找描述符和值偏移。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_schema_find(
    const ktag_feature_schema *schema, const char *feature_id,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 已冻结的Schema。 |
| feature_id | 输入 | 非空Feature ID，长度为1～255字节。 |
| descriptor | 输出 | 非空描述符存储；接口填写版本头，字符串由Schema持有。 |
| value_offset | 输出 | 必须非空，Feature首个值在记录值数组中的偏移。 |

##### 返回值说明

| 返回值 | 说明 |
|---|---|
| KTAG_SUCCESS | 找到并填充输出。 |
| KTAG_ERROR_NOT_FOUND | Feature不存在。 |
| KTAG_ERROR_INVALID_ARGUMENT | Schema未冻结或参数不合法。 |

#### `ktag_feature_schema_create_legacy_v1`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

重构前固定354值CSV/模型契约的Schema工厂，返回**已冻结**的 `ktag_legacy_v1` v1 Schema（53个Feature、354个值）。该工厂只创建Schema；配套Provider由Traffic插件提供，应先通过 `ktag_feature_plugin_load()` 加载并注册（见[Traffic Provider 注册](#traffic-provider-注册)）。

##### 函数原型

```c
KTAG_API ktag_feature_schema *ktag_feature_schema_create_legacy_v1(void);
```

##### 参数说明

无。

##### 返回值说明

成功返回冻结Schema，失败返回 `NULL`。

### Registry 与 Provider API

Registry管理Provider注册项；Feature Context创建时把Registry和Schema复制进自己的Provider Pipeline。

#### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | ktag_feature_registry_create | 创建Registry。 |
| 生命周期 | ktag_feature_registry_destroy | 销毁Registry。 |
| 注册 | ktag_feature_registry_register | 注册Provider。 |
| 注册 | ktag_feature_registry_unregister | 注销Provider。 |
| 插件注册 | ktag_feature_plugin_load | 加载Traffic等插件并注册其Provider，声明位于 ktag_feature_plugin.h。 |
| 查询 | ktag_feature_registry_get_provider_count | 已注册Provider数量。 |

注册标志。

| 常量 | 值 | 说明 |
|---|---:|---|
| KTAG_FEATURE_REGISTER_DEFAULT | 0 | 常规注册；Provider ID冲突时返回 KTAG_ERROR_ALREADY_EXISTS。 |
| KTAG_FEATURE_REGISTER_REPLACE | 1 << 0 | 替换同ID的旧Provider。替换前必须确保旧Provider未被运行中的Context使用。 |

#### `ktag_feature_registry_create`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

创建空的Feature Provider Registry。

##### 函数原型

```c
KTAG_API ktag_feature_registry *ktag_feature_registry_create(void);
```

##### 参数说明

无。

##### 返回值说明

成功返回Registry，失败返回 `NULL`。

#### `ktag_feature_registry_destroy`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

销毁Registry及其深拷贝的注册数据。由该Registry创建的Context持有独立Pipeline副本；借用的 `user_data` 和插件代码仍须保持有效直到相关Context全部销毁。

##### 函数原型

```c
KTAG_API void ktag_feature_registry_destroy(ktag_feature_registry *registry);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| registry | 输入 | 待释放的对象；可为 NULL。 |

##### 返回值说明

无。

#### `ktag_feature_registry_register`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

注册一个Feature Provider。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_registry_register(
    ktag_feature_registry *registry,
    const ktag_feature_provider_registration *registration, uint32_t flags);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| registry | 输入 | 目标Registry。 |
| registration | 输入 | 版本化注册结构体，字段见下。 |
| flags | 输入 | KTAG_FEATURE_REGISTER_DEFAULT 或 KTAG_FEATURE_REGISTER_REPLACE。 |

`ktag_feature_provider_registration` 字段。

| 字段 | 说明 |
|---|---|
| provider_id | Provider标识，最长255字节 |
| provider_version | Provider版本 |
| features | Feature描述符数组 |
| feature_count | 描述符数量 |
| callbacks | 回调集合，见[回调结构 ktag_feature_callbacks](#回调结构-ktag_feature_callbacks) |
| callback_error_policy | 回调失败策略，见下 |
| user_data | 原样传给各回调的用户数据，借用语义 |
| contract_provider_id | 可选的模型契约Provider标识；NULL 时使用 provider_id，用于解耦运行实现与冻结契约身份 |

回调失败策略。

| 策略 | 说明 |
|---|---|
| KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW（0） | 单流回调失败时丢弃该流。 |
| KTAG_FEATURE_CALLBACK_ERROR_FILL_ZERO（1） | 可恢复的失败以0填充该Provider的输出。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_ALREADY_EXISTS | Provider ID已存在且未指定REPLACE，或Feature ID与其他Provider冲突。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_registry_unregister`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按Provider ID注销注册项。正在运行的Context不受影响（它们持有自己的Pipeline副本），但新Context不再包含该Provider。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_registry_unregister(
    ktag_feature_registry *registry, const char *provider_id);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| registry | 输入 | 目标Registry。 |
| provider_id | 输入 | Provider标识。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_NOT_FOUND | Provider不存在。 |

#### `ktag_feature_registry_get_provider_count`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Registry中已注册的Provider数量。

##### 函数原型

```c
KTAG_API uint32_t
ktag_feature_registry_get_provider_count(const ktag_feature_registry *registry);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| registry | 输入 | 目标Registry。 |

##### 返回值说明

返回Provider数量；`registry` 为 `NULL` 时返回 `0`。

<a id="traffic-provider-注册"></a>

**Traffic Provider注册**

冻结应用Schema所需的Provider由 `libktag_traffic_features.so` 提供。动态宿主使用 [`ktag_feature_plugin_load`](#ktag_feature_plugin_load)；直接链接Traffic插件的宿主使用以下注册接口，两种方式不能对同一Registry重复注册同一组Provider。

#### `ktag_traffic_features_register`

##### 功能说明

头文件路径为：`"include/ktag_traffic_features.h"`。

直接向Registry注册Traffic插件提供的11个Provider，供直接链接插件的宿主使用。注册失败时撤销本次已成功注册的Provider。

##### 函数原型

```c
KTAG_TRAFFIC_FEATURES_API ktag_status
ktag_traffic_features_register(ktag_feature_registry *registry);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| registry | 输入 | 接收Traffic Provider的Registry，不能为空。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_ALREADY_EXISTS | Provider ID或Feature ID冲突。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

### Provider 回调、视图与输出 API

Provider通过回调参与组流；回调内通过Flow View、Packet View读取流与报文信息，通过Feature Output写出特征值。

#### 回调结构 `ktag_feature_callbacks`

```c
typedef struct {
  uint32_t struct_size;
  uint32_t api_version;

  ktag_status (*provider_create)(const ktag_feature_provider_config *config,
                                 void **provider_state);
  ktag_status (*flow_create)(void *provider_state, const ktag_flow_view *flow,
                             void **flow_state);
  ktag_status (*on_packet)(void *provider_state, void *flow_state,
                           const ktag_packet_view *packet,
                           const ktag_flow_view *flow);
  ktag_status (*flow_finish)(void *provider_state, void *flow_state,
                             const ktag_flow_view *flow,
                             ktag_feature_output *output);
  void (*flow_destroy)(void *provider_state, void *flow_state);
  void (*provider_destroy)(void *provider_state);
} ktag_feature_callbacks;
```

| 回调 | 必需性 | 调用时机 |
|---|---|---|
| provider_create | 可选 | Feature Context创建时初始化Provider状态。 |
| flow_create | 可选 | 新流建立时初始化流状态。 |
| on_packet | 可选 | 当前流收到每个可解析报文时。 |
| flow_finish | **必需** | 流结束时输出特征；在此调用 ktag_feature_output_set()。 |
| flow_destroy | 可选 | 流状态销毁时释放流级资源。 |
| provider_destroy | 可选 | Provider状态销毁时释放Provider级资源。 |

`ktag_feature_provider_config`（传给 `provider_create`）字段：`provider_id`、`provider_version`、`schema`（借用）、`user_data`。

使用约束。

- Provider不得把 `ktag_flow_view`、`ktag_packet_view` 或其返回指针保留到回调返回之后。
- `provider_create`/`flow_create` 失败时与回调失败同样受 `callback_error_policy` 支配（可恢复场景下按策略处理）。

#### `provider_create` 回调

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。本项为结构体中的函数指针，由调用方实现。

可选，在Feature Context创建期间初始化Provider状态。返回失败前应自行清理本次分配；失败状态不会交给 `provider_destroy`。

##### 函数原型

```c
ktag_status (*provider_create)(const ktag_feature_provider_config *config,
                                   void **provider_state);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| config | 输入 | Provider ID、版本、Context内部Schema与借用的 user_data。 |
| provider_state | 输出 | 保存Provider私有状态指针，成功后由Context持有至销毁回调。 |

##### 返回值说明

`KTAG_SUCCESS` 表示初始化成功；其他值按 `callback_error_policy` 处理：FAIL_FLOW使Context创建失败，FILL_ZERO将该Provider标记为不可用并对其输出填零。

#### `flow_create` 回调

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。本项为结构体中的函数指针，由调用方实现。

可选，在新流建立时初始化流状态。返回失败前自行清理本次分配；失败状态不会交给 `flow_destroy`。

##### 函数原型

```c
ktag_status (*flow_create)(void *provider_state, const ktag_flow_view *flow,
                               void **flow_state);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| provider_state | 输入 | Provider私有状态；未提供创建回调时可为 NULL。 |
| flow | 输入 | 本次回调有效的只读流视图。 |
| flow_state | 输出 | 流私有状态指针，成功后由对应销毁回调释放。 |

##### 返回值说明

`KTAG_SUCCESS` 表示成功；其他值按失败策略丢弃该流或对该Provider填零。

#### `on_packet` 回调

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。本项为结构体中的函数指针，由调用方实现。

可选，处理分发给该Provider的报文。生命周期模式可能过滤不再参与特征累计的报文，不保证每个捕获报文都触发回调。

##### 函数原型

```c
ktag_status (*on_packet)(void *provider_state, void *flow_state,
                             const ktag_packet_view *packet,
                             const ktag_flow_view *flow);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| provider_state | 输入 | Provider私有状态。 |
| flow_state | 输入/输出 | 当前流的私有状态。 |
| packet | 输入 | 仅本次回调有效的报文视图。 |
| flow | 输入 | 仅本次回调有效的流视图。 |

##### 返回值说明

`KTAG_SUCCESS` 表示成功；其他值按失败策略丢弃该流或对该Provider填零。

#### `flow_finish` 回调

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。本项为结构体中的函数指针，由调用方实现。

必需，在流结束时调用。通过 `ktag_feature_output_set()` 写完当前Schema中属于该Provider的全部数值输出；需要时写入文本注解。

##### 函数原型

```c
ktag_status (*flow_finish)(void *provider_state, void *flow_state,
                               const ktag_flow_view *flow,
                               ktag_feature_output *output);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| provider_state | 输入 | Provider私有状态。 |
| flow_state | 输入/输出 | 当前流的私有状态。 |
| flow | 输入 | 本次回调有效的只读流视图。 |
| output | 输出 | 当前Provider的输出writer，不得保存到回调之外。 |

##### 返回值说明

`KTAG_SUCCESS` 且全部声明输出已写入时成功；非成功返回或漏写输出按失败策略处理。

#### `flow_destroy` 回调

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。本项为结构体中的函数指针，由调用方实现。

可选，释放成功创建的流状态。不能只提供销毁回调而不提供对应的 `flow_create`。

##### 函数原型

```c
void (*flow_destroy)(void *provider_state, void *flow_state);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| provider_state | 输入 | Provider私有状态。 |
| flow_state | 输入 | 待释放的流状态。 |

##### 返回值说明

无。

#### `provider_destroy` 回调

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。本项为结构体中的函数指针，由调用方实现。

可选，释放成功创建的Provider状态。不能只提供销毁回调而不提供对应的 `provider_create`。

##### 函数原型

```c
void (*provider_destroy)(void *provider_state);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| provider_state | 输入 | 待释放的Provider状态。 |

##### 返回值说明

无。

#### `ktag_feature_output_set`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

在 `flow_finish` 回调中写入一个Feature的输出值。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_output_set(ktag_feature_output *output,
                                             const char *feature_id,
                                             const float *values,
                                             uint32_t value_count);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| output | 输出 | flow_finish 回调传入的输出对象。 |
| feature_id | 输入 | Provider注册时声明的Feature ID。 |
| values | 输入 | 值数组，调用期间被复制。 |
| value_count | 输入 | 值个数，必须等于该Feature声明的 dimension。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_NOT_FOUND | Feature ID不在Schema中。 |
| KTAG_ERROR_SCHEMA_MISMATCH | Feature属于其他Provider，或值数量与声明的维度不符。 |
| KTAG_ERROR_CAPACITY | 输出值存储空间不足。 |
| KTAG_ERROR_INVALID_STATE | 同一Feature在本流已写入过。 |

#### `ktag_feature_output_annotations_enabled`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询当前输出对象是否启用了文本注解收集。注解仅用于诊断，不计入Schema数值或模型输入。

##### 函数原型

```c
KTAG_API uint32_t
ktag_feature_output_annotations_enabled(const ktag_feature_output *output);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| output | 输入 | flow_finish 回调收到的输出对象。 |

##### 返回值说明

启用返回 `1`；未启用或 `output` 为 `NULL` 时返回 `0`。

#### `ktag_feature_output_set_annotation`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

在 `flow_finish` 回调中写入文本注解。文本会被复制；必须在Feature配置中启用 `collect_annotations`。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_output_set_annotation(
    ktag_feature_output *output, const char *key, const char *value);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| output | 输入 | 当前Provider的输出对象。 |
| key | 输入 | 非空注解键，长度为1～255字节。 |
| value | 输入 | 注解文本，可为空字符串，最长4095字节。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_NOT_SUPPORTED | 未启用注解收集。 |
| KTAG_ERROR_INVALID_STATE | 当前流中该注解键已写入。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

<a id="flow-view-查询-api"></a>

**Flow View查询API**

#### `ktag_flow_view_get_flow_id`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询流ID。Flow View及其返回的借用数据只在回调期间有效。

##### 函数原型

```c
KTAG_API uint64_t ktag_flow_view_get_flow_id(const ktag_flow_view *flow);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| flow | 输入 | 待查询的对象。 |

##### 返回值说明

返回流ID；`flow` 为 `NULL` 时返回 `0`。

#### `ktag_flow_view_get_flow_key`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取双向规范化Flow Key。

##### 函数原型

```c
KTAG_API ktag_status ktag_flow_view_get_flow_key(const ktag_flow_view *flow,
                                                 ktag_flow_key *key);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| flow | 输入 | 包含Flow Key的流视图。 |
| key | 输出 | 非空输出存储；接口填写全部字段和版本头。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

#### `ktag_flow_view_get_endpoint`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按首包方向读取端点。FORWARD返回首包源端点，REVERSE返回首包目的端点，与规范化A/B排序不同。

##### 函数原型

```c
KTAG_API ktag_status ktag_flow_view_get_endpoint(const ktag_flow_view *flow,
                                                 ktag_direction direction,
                                                 ktag_ip_address *address,
                                                 uint16_t *port);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| flow | 输入 | 流视图。 |
| direction | 输入 | KTAG_DIRECTION_FORWARD（0）或 KTAG_DIRECTION_REVERSE（1）。 |
| address | 输出 | 非空IP地址存储。 |
| port | 输出 | 非空端口存储。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

#### `ktag_flow_view_get_packet_count`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取指定方向的累计包数。

##### 函数原型

```c
KTAG_API uint32_t ktag_flow_view_get_packet_count(const ktag_flow_view *flow,
                                                  ktag_direction direction);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| flow | 输入 | 只读流视图。 |
| direction | 输入 | KTAG_DIRECTION_FORWARD（首包方向）或 KTAG_DIRECTION_REVERSE。 |

##### 返回值说明

返回累计包数；流为空或方向不合法时返回 `0`。

#### `ktag_flow_view_get_total_bytes`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取指定方向的累计字节数。

##### 函数原型

```c
KTAG_API uint64_t ktag_flow_view_get_total_bytes(const ktag_flow_view *flow,
                                                 ktag_direction direction);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| flow | 输入 | 只读流视图。 |
| direction | 输入 | KTAG_DIRECTION_FORWARD（首包方向）或 KTAG_DIRECTION_REVERSE。 |

##### 返回值说明

返回累计字节数；流为空或方向不合法时返回 `0`。

#### `ktag_flow_view_get_domain`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取指定方向的端点域名。

##### 函数原型

```c
KTAG_API const char *ktag_flow_view_get_domain(const ktag_flow_view *flow,
                                               ktag_direction direction);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| flow | 输入 | 只读流视图。 |
| direction | 输入 | KTAG_DIRECTION_FORWARD（首包方向）或 KTAG_DIRECTION_REVERSE。 |

##### 返回值说明

返回借用的域名字符串；有效视图未命中时返回空字符串，流为空或方向不合法时返回 `NULL`。字符串只在回调期间有效。

<a id="packet-view-查询-api"></a>

**Packet View查询API**

`ktag_packet_transport_view` 字段。

| 字段 | 说明 |
|---|---|
| direction | 本报文方向（KTAG_DIRECTION_*） |
| network_version | 4或6 |
| transport_protocol | 传输层协议号 |
| tcp_flags | TCP标志位 |
| fragment_state | KTAG_IP_FRAGMENT_NONE/ATOMIC/FIRST/NON_INITIAL |
| network_header_length / transport_header_length | 头部长度 |
| ipv6_extension_header_count / ipv6_extension_header_bytes | IPv6扩展头计数/字节数 |
| tcp_sequence / tcp_acknowledgment | TCP序号/确认号 |

#### `ktag_packet_view_get_data`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取完整捕获报文及其长度。返回的数据只在 `on_packet` 回调期间有效。

##### 函数原型

```c
KTAG_API const uint8_t *
ktag_packet_view_get_data(const ktag_packet_view *packet, uint32_t *length);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入 | 只读报文视图。 |
| length | 输出 | 非空长度存储，单位为字节。 |

##### 返回值说明

返回借用的数据指针；`packet` 或 `length` 为 `NULL` 时返回 `NULL`。无数据时长度可为 `0`，不能仅凭指针判断是否存在有效载荷。

#### `ktag_packet_view_get_timestamp`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取报文时间戳。

##### 函数原型

```c
KTAG_API ktag_status ktag_packet_view_get_timestamp(
    const ktag_packet_view *packet, ktag_timestamp *timestamp);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入 | 只读报文视图。 |
| timestamp | 输出 | 非空时间戳存储。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

#### `ktag_packet_view_get_transport`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取报文方向及网络层、传输层解析结果，字段见 `ktag_packet_transport_view`。

##### 函数原型

```c
KTAG_API ktag_status ktag_packet_view_get_transport(
    const ktag_packet_view *packet, ktag_packet_transport_view *transport);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入 | 只读报文视图。 |
| transport | 输出 | 非空版本化结构体，调用前必须初始化 struct_size 和 api_version。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

#### `ktag_packet_view_get_payload`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取传输层载荷及其长度。返回的数据只在 `on_packet` 回调期间有效。

##### 函数原型

```c
KTAG_API const uint8_t *
ktag_packet_view_get_payload(const ktag_packet_view *packet, uint32_t *length);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入 | 只读报文视图。 |
| length | 输出 | 非空长度存储，单位为字节。 |

##### 返回值说明

返回借用的数据指针；`packet` 或 `length` 为 `NULL` 时返回 `NULL`。无数据时长度可为 `0`，不能仅凭指针判断是否存在有效载荷。

#### `ktag_packet_view_get_capture_lengths`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

读取捕获报文长度、头部长度和捕获载荷长度，保留链路层填充统计语义。载荷长度为报文长度减头部长度，可能不同于解析出的L4载荷长度。

##### 函数原型

```c
KTAG_API ktag_status ktag_packet_view_get_capture_lengths(
    const ktag_packet_view *packet, uint32_t *packet_length,
    uint32_t *header_length, uint32_t *payload_length);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packet | 输入 | 只读报文视图。 |
| packet_length | 输出 | 非空，捕获报文长度。 |
| header_length | 输出 | 非空，链路层、网络层、传输层头部总长度。 |
| payload_length | 输出 | 非空，捕获报文长度减头部长度。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

### Feature Context API

Feature Context持有组流状态和Provider Pipeline，是特征提取的执行主体。

#### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | ktag_feature_create | 创建Feature Context。 |
| 生命周期 | ktag_feature_destroy | 销毁Feature Context。 |
| 处理 | ktag_feature_process_packet | 处理一个报文。 |
| 处理 | ktag_feature_expire | 按空闲超时结束流。 |
| 处理 | ktag_feature_flush | 输出全部剩余流。 |
| 统计 | ktag_feature_get_statistics | 查询运行统计。 |

#### `ktag_feature_config`

| 字段 | 说明 |
|---|---|
| schema | 已冻结Schema，必填 |
| registry | 已注册所需Provider的Registry，必填 |
| shared_domain_store | 可选的跨Context共享域名权威存储 |
| flow_capacity | 活跃流上限；0 在MODERN模式使用65536，在LEGACY_COMPAT模式使用 UINT32_MAX |
| tcp_idle_timeout_us | TCP空闲超时（微秒）；0 使用默认值300秒 |
| udp_idle_timeout_us | UDP空闲超时（微秒）；0 使用默认值30秒 |
| udp_packet_limit | UDP包数上限；0 使用默认值100 |
| domain_authority_lookup_on_miss | Cache未命中时是否回查Domain Store |
| lifecycle_mode | 生命周期模式，见下 |
| collect_annotations | 仅接受 0（关闭）或 1（收集Provider文本注解）；注解只供诊断，不进入模型输入 |

生命周期模式。

| 模式 | 说明 |
|---|---|
| KTAG_FEATURE_LIFECYCLE_MODERN（0） | UDP达到 udp_packet_limit 后结束流；expire() 按空闲超时结束TCP/UDP流。 |
| KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT（1） | 保持历史固定向量行为：UDP只保留前N个包，通常由 flush() 一次性输出；expire() 仅执行DomainStore维护，不因超时结束流。 |

TCP协议关闭处理两种模式一致。训练与部署必须使用相同的Schema与生命周期模式。

#### `ktag_feature_create`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

创建Feature Context。Schema与Registry被复制进Context的Provider Pipeline，创建后修改Registry不影响已创建的Context。

##### 函数原型

```c
KTAG_API ktag_feature_context *
ktag_feature_create(const ktag_feature_config *config);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| config | 输入 | 版本化配置结构体，见[ktag_feature_config](#ktag_feature_config)。 |

##### 返回值说明

成功返回新Context，失败返回 `NULL`。

#### `ktag_feature_process_packet`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

处理一个报文；若因此有流结束，完成的流被写入输出Batch。

无法解析或不受支持的报文可被跳过并计入 `rejected_packets`，此时仍返回 `KTAG_SUCCESS`；成功不保证输出记录。只有参数、线程和Schema检查通过后才会进入正常输出处理。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_process_packet(ktag_feature_context *context,
                                                 const ktag_packet *packet,
                                                 ktag_feature_batch *output);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | Feature Context。 |
| packet | 输入 | 来自 ktag_capture_next() 的报文。 |
| output | 输出 | 输出Batch。调用前会被清空；Schema Hash必须与Context一致。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_SCHEMA_MISMATCH | 输出Batch的Schema Hash或值数量与Context不一致。 |
| KTAG_ERROR_INVALID_STATE | 线程归属违规，或Provider未完整写出所声明的特征。 |
| KTAG_ERROR_CAPACITY | 流表、标识或相关存储容量不足。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_expire`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

驱动域名过期维护；MODERN模式还会结束空闲超时的TCP/UDP流并输出。LEGACY_COMPAT模式只维护Domain Store，不因空闲超时结束流。应周期性调用。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_expire(ktag_feature_context *context,
                                         uint64_t current_time_us,
                                         ktag_feature_batch *output);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | Feature Context，遵守单线程归属。 |
| current_time_us | 输入 | 当前微秒时间戳，应与报文时间戳采用同一时间基准。 |
| output | 输出 | Schema Hash和值数量必须与Context一致；有效调用先清空，再写入本次结束的流。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_SCHEMA_MISMATCH | 输出Batch的Schema Hash或值数量与Context不一致。 |
| KTAG_ERROR_INVALID_STATE | 线程归属违规，或Provider未完整写出所声明的特征。 |
| KTAG_ERROR_CAPACITY | 流表、标识或相关存储容量不足。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_flush`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

结束并输出全部剩余流。PCAP EOF或停止处理时必须调用，否则尚未结束的流不会输出。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_flush(ktag_feature_context *context,
                                        ktag_feature_batch *output);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | Feature Context，遵守单线程归属。 |
| output | 输出 | Schema Hash和值数量必须与Context一致；有效调用先清空，再写入剩余流。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_SCHEMA_MISMATCH | 输出Batch的Schema Hash或值数量与Context不一致。 |
| KTAG_ERROR_INVALID_STATE | 线程归属违规，或Provider未完整写出所声明的特征。 |
| KTAG_ERROR_CAPACITY | 流表、标识或相关存储容量不足。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_get_statistics`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询运行统计。该调用会将Context绑定到当前线程（与其他处理接口一致）。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_get_statistics(
    const ktag_feature_context *context, ktag_feature_statistics *statistics);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | Feature Context。 |
| statistics | 输出 | 输出参数。版本化结构体。 |

`ktag_feature_statistics` 字段。

| 字段 | 说明 |
|---|---|
| received_packets / received_bytes | 收到的报文数/字节数 |
| decoded_packets / rejected_packets | 成功解析/被拒绝的报文数 |
| created_flows / completed_flows / emitted_flows | 建立/完成/输出的流数 |
| expired_tcp_flows / expired_udp_flows | 因超时结束的TCP/UDP流数 |
| flushed_flows / dropped_flows | Flush输出/丢弃的流数 |
| capacity_rejections | 因容量满被拒绝的报文数 |
| callback_failures | Provider回调失败次数 |
| domain_expired_entries | DomainStore过期条目数 |
| thread_violations | 线程归属违规次数 |
| active_flows / peak_active_flows | 当前/峰值活跃流数 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_INVALID_STATE | 调用线程不是Context的所属线程。 |

#### `ktag_feature_destroy`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

销毁Feature Context。活跃流的回调状态被直接销毁，**不会补发结果**；需要输出时先调用 `ktag_feature_flush()`。

##### 函数原型

```c
KTAG_API void ktag_feature_destroy(ktag_feature_context *context);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待销毁的Context；可为 NULL。 |

##### 返回值说明

无。

### Feature Batch API

Feature Batch是动态记录数组，承载流完成后的特征记录。记录的值在 `append` 时被复制。

#### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | ktag_feature_batch_create | 创建Batch。 |
| 生命周期 | ktag_feature_batch_destroy | 销毁Batch。 |
| 容量 | ktag_feature_batch_reserve | 预留容量。 |
| 写入 | ktag_feature_batch_append | 追加一条记录。 |
| 写入 | ktag_feature_batch_clear | 清空全部记录。 |
| 查询 | ktag_feature_batch_get_count | 记录数量。 |
| 查询 | ktag_feature_batch_get_value_count | 每条记录的值数量。 |
| 查询 | ktag_feature_batch_get_schema_hash | Schema Hash。 |
| 查询 | ktag_feature_batch_get_record | 按下标取记录。 |
| 查询 | ktag_feature_batch_get_record_details | 按下标取审计详情。 |
| 查询 | ktag_feature_batch_get_annotation | 按记录下标和键读取文本注解。 |

#### 记录结构

`ktag_feature_record_metadata`（用于记录关联、结果回溯，字段本身不作为模型特征）。

| 字段 | 说明 |
|---|---|
| flow_id | 流ID |
| flow_key | 规范化Flow Key |
| first_packet_sequence / last_packet_sequence | 首/末包序号 |

`ktag_feature_record`（`get_record` 输出，值指针为**借用**）。

| 字段 | 说明 |
|---|---|
| schema_hash | Schema Hash |
| metadata | 核心元数据，见上 |
| values | 连续float值指针，在Batch再次写入/清空/销毁前有效 |
| value_count | 值数量，等于Schema value count |

`ktag_feature_record_details`（`get_record_details` 输出，仅供审计/CSV，**不进入模型输入**）：在 `metadata` 基础上增加首包方向的 `source_address`/`destination_address`/`source_port`/`destination_port`、`ipv6_extension_packet_count`、`ip_fragment_first_packet_count`。

#### `ktag_feature_batch_create`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

创建绑定已冻结Schema的空Feature Batch。Batch保存Schema Hash和值数量，后续不借用Schema对象。

##### 函数原型

```c
KTAG_API ktag_feature_batch *
ktag_feature_batch_create(const ktag_feature_schema *schema,
                          uint32_t initial_capacity);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| schema | 输入 | 已冻结且至少有一个值的Schema。 |
| initial_capacity | 输入 | 预留记录数；0 表示不预留，后续按需扩容，不表示固定的默认容量。 |

##### 返回值说明

成功返回Batch；Schema为空、未冻结、值数量为零或分配失败时返回 `NULL`。

#### `ktag_feature_batch_destroy`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

销毁Feature Batch及全部记录、数值和文本注解；已取得的借用指针随之失效。

##### 函数原型

```c
KTAG_API void ktag_feature_batch_destroy(ktag_feature_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 待释放的对象；可为 NULL。 |

##### 返回值说明

无。

#### `ktag_feature_batch_reserve`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

预留至少 `capacity` 条记录的存储，避免追加时反复扩容。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_batch_reserve(ktag_feature_batch *batch,
                                                uint32_t capacity);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Batch。 |
| capacity | 输入 | 需要容纳的记录数。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_batch_append`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

追加一条记录，值和元数据被复制进Batch。

本接口没有首包方向扩展字段，审计详情的源/目的地址与端口按元数据Flow Key的A/B端点填充。需要真实首包方向的详情时应读取Feature Context生成的记录。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_batch_append(
    ktag_feature_batch *batch, const ktag_feature_record_metadata *metadata,
    const float *values, uint32_t value_count);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Batch。 |
| metadata | 输入 | 记录元数据。 |
| values | 输入 | 值数组。 |
| value_count | 输入 | 必须等于Batch的Schema value count。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_SCHEMA_MISMATCH | 值数量不等于Batch的Schema值数量。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_feature_batch_clear`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

清空全部记录，不改变已分配容量。

##### 函数原型

```c
KTAG_API void ktag_feature_batch_clear(ktag_feature_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Batch。 |

##### 返回值说明

无。

<a id="batch-查询-api"></a>

**Batch查询API**

#### `ktag_feature_batch_get_count`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Feature Batch的当前记录数。

##### 函数原型

```c
KTAG_API uint32_t ktag_feature_batch_get_count(const ktag_feature_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 待查询的对象。 |

##### 返回值说明

返回当前记录数；`batch` 为 `NULL` 时返回 `0`。

#### `ktag_feature_batch_get_value_count`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Feature Batch的每条记录的值数量。

##### 函数原型

```c
KTAG_API uint32_t
ktag_feature_batch_get_value_count(const ktag_feature_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 待查询的对象。 |

##### 返回值说明

返回每条记录的值数量；`batch` 为 `NULL` 时返回 `0`。

#### `ktag_feature_batch_get_schema_hash`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Feature Batch的绑定的Schema Hash。

##### 函数原型

```c
KTAG_API uint64_t
ktag_feature_batch_get_schema_hash(const ktag_feature_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 待查询的对象。 |

##### 返回值说明

返回绑定的Schema Hash；`batch` 为 `NULL` 时返回 `0`。

#### `ktag_feature_batch_get_annotation`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按记录下标和键读取文本注解。RawBoW的键为 `KTAG_TRAFFIC_RAWBOW_ANNOTATION`（`traffic.rawbow.text`）。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_batch_get_annotation(
    const ktag_feature_batch *batch, uint32_t index, const char *key,
    const char **value);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Feature Batch。 |
| index | 输入 | 记录下标，范围为 [0, count)。 |
| key | 输入 | 非空注解键指针。 |
| value | 输出 | 非空字符串指针存储；通过参数检查后先设为 NULL，命中后返回借用文本，在Batch再次修改或销毁前有效。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_NOT_FOUND | 记录没有注解或指定键不存在。 |

#### `ktag_feature_batch_get_record`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按下标取出只读记录视图。

##### 函数原型

```c
KTAG_API ktag_status
ktag_feature_batch_get_record(const ktag_feature_batch *batch, uint32_t index,
                              ktag_feature_record *record);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Feature Batch。 |
| index | 输入 | 记录下标，范围为 [0, count)。 |
| record | 输出 | 非空记录存储；接口填写版本头，values 借用至Batch再次修改或销毁。 |

##### 返回值说明

| 返回值 | 说明 |
|---|---|
| KTAG_SUCCESS | 取得记录。 |
| KTAG_ERROR_INVALID_ARGUMENT | index 越界或参数为空。 |

#### `ktag_feature_batch_get_record_details`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

按下标取出审计详情（含首包方向五元组等扩展信息）。

##### 函数原型

```c
KTAG_API ktag_status ktag_feature_batch_get_record_details(
    const ktag_feature_batch *batch, uint32_t index,
    ktag_feature_record_details *details);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Feature Batch。 |
| index | 输入 | 记录下标，范围为 [0, count)。 |
| details | 输出 | 非空审计详情存储；接口直接填写全部字段和版本头。 |

##### 返回值说明

| 返回值 | 说明 |
|---|---|
| KTAG_SUCCESS | 取得详情。 |
| KTAG_ERROR_INVALID_ARGUMENT | index 越界或参数为空。 |

### Domain Store API

Domain Store维护IP→域名的权威映射，供多个Feature Context共享。Store的变更和统计接口线程安全；Store引用可在其Cache之前释放，对象存活到最后一个已注册Cache销毁。

#### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | ktag_domain_store_create | 创建Store。 |
| 生命周期 | ktag_domain_store_destroy | 释放Store引用。 |
| 变更 | ktag_domain_store_put | 写入/更新映射。 |
| 变更 | ktag_domain_store_delete | 删除映射。 |
| 变更 | ktag_domain_store_expire | 发布到期映射的删除事件。 |
| 查询 | ktag_domain_store_get_version | 当前发布版本号。 |
| 查询 | ktag_domain_store_get_stats | 查询统计。 |

`ktag_domain_store_config` 字段。

| 字段 | 说明 |
|---|---|
| max_entries | 最大条目数；0 使用默认值10000 |
| journal_capacity | 增量同步日志容量；0 使用默认值4096 |

#### `ktag_domain_store_create`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

创建共享IP→域名权威存储；配置结构体必须初始化版本头。

##### 函数原型

```c
KTAG_API ktag_domain_store *
ktag_domain_store_create(const ktag_domain_store_config *config);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| config | 输入 | 非空版本化配置；max_entries=0 使用10000，journal_capacity=0 使用4096。 |

##### 返回值说明

成功返回Store；配置不合法或分配失败时返回 `NULL`。

#### `ktag_domain_store_destroy`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

释放调用方持有的Store引用。Cache继续持有引用，对象在最后一个引用释放后销毁。

##### 函数原型

```c
KTAG_API void ktag_domain_store_destroy(ktag_domain_store *store);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| store | 输入 | 待释放的对象；可为 NULL。 |

##### 返回值说明

无。

#### `ktag_domain_store_put`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

写入或更新IP→域名映射。域名和到期时间均未变化时直接成功并返回当前版本，不发布新版本。

##### 函数原型

```c
KTAG_API ktag_status ktag_domain_store_put(ktag_domain_store *store,
                                           const ktag_ip_address *address,
                                           const char *domain,
                                           uint64_t expire_at_us,
                                           uint64_t *published_version);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| store | 输入 | 目标Store。 |
| address | 输入 | IP地址。 |
| domain | 输入 | 域名字符串，最长255字节。 |
| expire_at_us | 输入 | 到期时间（微秒）；0 表示不自动过期。 |
| published_version | 输出 | 可选输出。本次变更发布后的版本号。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_CAPACITY | 新条目超过容量、域名超过255字节或版本号已耗尽。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_domain_store_delete`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

删除一个映射并发布新版本。

地址不存在时也返回成功，`published_version` 返回当前版本号，不发布新版本。

##### 函数原型

```c
KTAG_API ktag_status ktag_domain_store_delete(ktag_domain_store *store,
                                              const ktag_ip_address *address,
                                              uint64_t *published_version);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| store | 输入 | 目标Store。 |
| address | 输入 | 要删除的IP地址。 |
| published_version | 输出 | 可选输出。本次变更发布后的版本号。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_CAPACITY | 需要删除现有映射，但版本号已耗尽。 |

#### `ktag_domain_store_expire`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

为TTL到达（`expire_at_us` 不晚于 `now_us`）的条目发布DELETE变更。

##### 函数原型

```c
KTAG_API ktag_status ktag_domain_store_expire(ktag_domain_store *store,
                                              uint64_t now_us,
                                              uint32_t *deleted_count);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| store | 输入 | 目标Store。 |
| now_us | 输入 | 当前微秒时间戳。 |
| deleted_count | 输出 | 必须非空，本次实际删除的到期条目数量。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_CAPACITY | 版本号已耗尽；此前完成的删除仍生效，deleted_count 返回已完成数量。 |

#### `ktag_domain_store_get_version`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Store当前发布的版本号，供Cache判断是否需要同步。

##### 函数原型

```c
KTAG_API uint64_t ktag_domain_store_get_version(const ktag_domain_store *store);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| store | 输入 | 目标Store。 |

##### 返回值说明

返回版本号；`store` 为 `NULL` 时返回 `0`。

#### `ktag_domain_store_get_stats`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Store统计信息；查询时持有Store内部锁。

##### 函数原型

```c
KTAG_API ktag_status ktag_domain_store_get_stats(
    const ktag_domain_store *store, ktag_domain_store_stats *stats);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| store | 输入 | 目标Store。 |
| stats | 输出 | 非空统计结构体，调用前初始化版本头。 |

`ktag_domain_store_stats` 字段。

| 字段 | 说明 |
|---|---|
| version | 当前发布版本号 |
| entry_count | 存活条目数 |
| journal_count | 增量日志中的变更数 |
| journal_oldest_version | 日志中最旧的版本号 |
| registered_cache_count | 已注册的Cache数量 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

### Domain Cache API

Domain Cache是每个Feature Context的本地缓存，由单线程使用；版本未变化且本地命中时，查找只做acquire版本加载和本地map访问。同步或权威回查时会访问Store的锁。

#### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | ktag_domain_cache_create | 创建Cache并注册到Store。 |
| 生命周期 | ktag_domain_cache_destroy | 销毁Cache。 |
| 同步 | ktag_domain_cache_sync | 从Store增量/快照同步。 |
| 查询 | ktag_domain_cache_lookup | 查询IP对应域名。 |
| 查询 | ktag_domain_cache_get_applied_version | 已应用的版本号。 |
| 查询 | ktag_domain_cache_get_stats | 查询统计。 |

`ktag_domain_cache_config` 字段。

| 字段 | 说明 |
|---|---|
| authority_lookup_on_miss | 非0时本地未命中回查权威Store。 |

#### `ktag_domain_cache_create`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

创建单线程使用的本地Cache，从Store复制初始快照并登记版本，同时持有Store引用。

##### 函数原型

```c
KTAG_API ktag_domain_cache *
ktag_domain_cache_create(ktag_domain_store *store,
                         const ktag_domain_cache_config *config);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| store | 输入 | 非空权威Store。 |
| config | 输入 | 非空版本化配置；authority_lookup_on_miss 非零时启用权威回查。 |

##### 返回值说明

成功返回Cache；Store/配置不合法、缓存登记失败或分配失败时返回 `NULL`。

#### `ktag_domain_cache_destroy`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

销毁本地Cache，注销其同步版本并释放它持有的Store引用。

##### 函数原型

```c
KTAG_API void ktag_domain_cache_destroy(ktag_domain_cache *cache);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| cache | 输入 | 待释放的对象；可为 NULL。 |

##### 返回值说明

无。

#### `ktag_domain_cache_sync`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

把Store自上次同步以来的增量变更应用到本地；增量日志不足时自动执行快照恢复。

##### 函数原型

```c
KTAG_API ktag_status ktag_domain_cache_sync(ktag_domain_cache *cache);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| cache | 输入 | 目标Cache。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_INVALID_STATE | Cache在Store中的登记已不存在。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_domain_cache_lookup`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询IP对应的域名。

##### 函数原型

```c
KTAG_API ktag_status ktag_domain_cache_lookup(ktag_domain_cache *cache,
                                              const ktag_ip_address *address,
                                              uint64_t now_us, char *domain,
                                              uint32_t domain_capacity,
                                              uint32_t *required_capacity);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| cache | 输入 | 目标Cache；本次查询会先执行必要的同步。 |
| address | 输入 | 有效IPv4/IPv6地址；IPv4未使用的12个字节必须为零。 |
| now_us | 输入 | 当前微秒时间戳，用于TTL判断。 |
| domain | 输出 | 域名缓冲区；仅在 domain_capacity=0 时可为 NULL，可用于探测所需容量。 |
| domain_capacity | 输入 | 缓冲区容量，单位为字节。 |
| required_capacity | 输出 | 必须非空；命中时写入包含末尾NUL的所需容量，未命中时为 0。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_NOT_FOUND | 映射不存在或已过期，启用回查时权威Store也未命中。 |
| KTAG_ERROR_CAPACITY | 输出缓冲区不足，按所需容量分配后重试。 |
| KTAG_ERROR_INVALID_STATE | Cache同步登记状态不合法。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_domain_cache_get_applied_version`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询Cache已应用的Store版本号。

##### 函数原型

```c
KTAG_API uint64_t
ktag_domain_cache_get_applied_version(const ktag_domain_cache *cache);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| cache | 输入 | 目标Cache。 |

##### 返回值说明

返回已应用版本号；`cache` 为 `NULL` 时返回 `0`。

#### `ktag_domain_cache_get_stats`

##### 功能说明

头文件路径为：`"include/ktag_feature.h"`。

查询本地Cache的命中、同步和回查统计。应由使用该Cache的线程调用。

##### 函数原型

```c
KTAG_API ktag_status ktag_domain_cache_get_stats(
    const ktag_domain_cache *cache, ktag_domain_cache_stats *stats);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| cache | 输入 | 目标Cache。 |
| stats | 输出 | 非空统计结构体，调用前初始化版本头。 |

`ktag_domain_cache_stats` 字段。

| 字段 | 说明 |
|---|---|
| applied_version | 已应用版本号 |
| local_hits / local_misses | 本地命中/未命中次数 |
| sync_count | 同步次数 |
| incremental_changes / snapshot_recoveries | 增量变更/快照恢复次数 |
| authority_lookups / authority_hits | 回查Store次数/命中次数 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

### 最小用例

使用Legacy V1契约处理PCAP：创建Schema/Registry/Context，逐包处理，EOF后flush，并打印每条输出记录的Flow ID与值数量。

```c
#include <inttypes.h>
#include <stdio.h>
#include "ktag_capture.h"
#include "ktag_feature.h"
#include "ktag_feature_plugin.h"

static void print_feature_batch(const ktag_feature_batch *batch)
{
    uint32_t index;
    for (index = 0U;
         index < ktag_feature_batch_get_count(batch); ++index) {
        ktag_feature_record record = {0};
        record.struct_size = sizeof(record);
        record.api_version = KTAG_API_VERSION;
        if (ktag_feature_batch_get_record(batch, index, &record) ==
            KTAG_SUCCESS) {
            printf("flow=%" PRIu64 " values=%u\n",
                   record.metadata.flow_id, record.value_count);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s INPUT.pcap\n", argv[0]);
        return 1;
    }

    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_loaded_feature_plugin *plugin = registry == NULL ? NULL :
        ktag_feature_plugin_load(
            "/opt/KTAG/output/plugins/libktag_traffic_features.so", registry);
    if (schema == NULL || plugin == NULL) {
        fprintf(stderr, "Could not create the Legacy V1 feature contract\n");
        return 1;
    }

    ktag_feature_config config = {0};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.schema = schema;
    config.registry = registry;
    config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
    ktag_feature_context *feature = ktag_feature_create(&config);
    ktag_feature_batch *batch = ktag_feature_batch_create(schema, 16U);
    if (feature == NULL || batch == NULL) {
        fprintf(stderr, "Could not create Feature Context\n");
        return 1;
    }

    ktag_capture_config cap_config = {0};
    cap_config.struct_size = sizeof(cap_config);
    cap_config.api_version = KTAG_API_VERSION;
    cap_config.source_type = KTAG_CAPTURE_SOURCE_PCAP_FILE;
    cap_config.source = argv[1];
    cap_config.packet_mode = KTAG_CAPTURE_PACKET_BORROWED;
    ktag_capture_context *capture = ktag_capture_open(&cap_config);
    if (capture == NULL) {
        fprintf(stderr, "open failed: %s\n", ktag_capture_get_last_error());
        return 1;
    }

    ktag_packet packet;
    ktag_packet_init(&packet);
    for (;;) {
        ktag_status status = ktag_capture_next(capture, &packet);
        if (status != KTAG_SUCCESS) {
            break;
        }
        if (ktag_feature_process_packet(feature, &packet, batch) !=
            KTAG_SUCCESS) {
            ktag_packet_release(&packet);
            break;
        }
        print_feature_batch(batch);
        ktag_packet_release(&packet);
    }
    ktag_feature_flush(feature, batch);

    print_feature_batch(batch);

    ktag_capture_close(capture);
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(feature);
    ktag_feature_registry_destroy(registry);
    ktag_feature_schema_destroy(schema);
    ktag_feature_plugin_unload(plugin);
    return 0;
}
```

将代码保存为 `feature_example.c`，在仓库根目录执行。

```bash
bash /opt/KTAG/buildtools/build.sh   # 若尚未构建
cc -std=c11 -I/opt/KTAG/include /opt/KTAG/feature_example.c \
   -L/opt/KTAG/output -lktag_capture -lktag_feature \
   -Wl,-rpath,/opt/KTAG/output -o /opt/KTAG/output/feature_example
/opt/KTAG/output/feature_example sample.pcap
```

正确输出（行数与内容随输入文件变化，Legacy V1每条记录354个值）。

```text
flow=1 values=354
flow=2 values=354
```

自定义Feature Provider与动态插件的完整示例见 `examples/feature_provider/custom_feature_provider.c` 和 `examples/feature_plugin/custom_feature_plugin.c`，说明见 `examples/README.md`。

## 动态 Feature 插件 ABI（ktag_feature_plugin.h）

Feature Provider可以编译为独立共享库，由宿主通过 `ktag_feature_plugin_load()` 在运行期加载。本章定义插件必须满足的C ABI契约；宿主负责动态库生命周期、ABI版本协商和卸载顺序，插件不创建流水线线程。

### 常量与入口符号

| 常量 | 值 | 说明 |
|---|---:|---|
| KTAG_FEATURE_PLUGIN_ABI_VERSION | 1U | 插件ABI版本。宿主与插件必须一致。 |
| KTAG_FEATURE_PLUGIN_ENTRYPOINT | "ktag_feature_plugin_query" | 插件必须导出的入口符号名。 |
| KTAG_FEATURE_PLUGIN_EXPORT | 平台相关 | 导出宏：MSVC为 __declspec(dllexport)，GCC 4+ 为 __attribute__((visibility("default")))。 |

插件必须在共享库中导出精确的C符号 `ktag_feature_plugin_query`，不能发生C++ 名称改编（C++ 源码需包裹 `extern "C"`）。

### `ktag_feature_plugin_descriptor`

```c
typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t plugin_abi_version;
    uint32_t reserved; /* Must be zero for ABI v1. */
    const char *plugin_id;
    const char *plugin_version;

    /* Optional. State remains owned by the plugin until plugin_destroy. */
    ktag_status (*plugin_create)(void **plugin_state);

    /* Required. Providers are registered through the core callbacks API. */
    ktag_status (*register_providers)(
        void *plugin_state,
        ktag_feature_registry *registry);

    /* Optional, but required when plugin_create is supplied. */
    void (*plugin_destroy)(void *plugin_state);
} ktag_feature_plugin_descriptor;
```

| 字段 | 说明 |
|---|---|
| struct_size / api_version | 宿主调用入口前预先设置；插件按[ABI 版本与版本化结构体](#abi-版本与版本化结构体) 语义处理。 |
| plugin_abi_version | 插件填写的ABI版本，必须等于 KTAG_FEATURE_PLUGIN_ABI_VERSION。 |
| reserved | ABI v1必须置0。 |
| plugin_id / plugin_version | 非空字符串，最长255字节，仅允许字母、数字、点、下划线和连字符。 |
| plugin_create | 可选。初始化插件状态。 |
| register_providers | **必需**。以事务方式向Registry注册Provider；返回失败前应自行撤销本次已注册的Provider。 |
| plugin_destroy | 可选；但提供 plugin_create 时**必需**。释放插件状态。 |

### `plugin_create` 回调

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。本项为结构体中的函数指针，由调用方实现。

可选，宿主加载器完成ABI校验后调用，用于创建插件状态。提供此项时必须同时提供 `plugin_destroy`。

#### 函数原型

```c
ktag_status (*plugin_create)(void **plugin_state);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| plugin_state | 输出 | 保存插件状态指针。 |

#### 返回值说明

`KTAG_SUCCESS` 表示成功；其他值使插件加载失败。失败时加载器也会调用已提供的 `plugin_destroy`，因此销毁回调须能处理失败后的状态。

### `register_providers` 回调

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。本项为结构体中的函数指针，由调用方实现。

必需，向宿主Registry注册Provider。返回失败前须撤销本次已经完成的注册；宿主加载器不会自动回滚Registry。

#### 函数原型

```c
ktag_status (*register_providers)(void *plugin_state,
                                      ktag_feature_registry *registry);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| plugin_state | 输入 | 插件私有状态；未提供创建回调时为 NULL。 |
| registry | 输入 | 接收Provider的Registry。 |

#### 返回值说明

`KTAG_SUCCESS` 表示注册成功；其他值使加载失败，加载器随后销毁插件状态并关闭动态库。

### `plugin_destroy` 回调

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。本项为结构体中的函数指针，由调用方实现。

释放插件状态；在卸载或插件初始化、注册失败的清理阶段调用。

#### 函数原型

```c
void (*plugin_destroy)(void *plugin_state);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| plugin_state | 输入 | 待释放的插件状态，可能为 NULL。 |

#### 返回值说明

无。

### `ktag_feature_plugin_load`

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。

加载Provider动态库，检查文件结构及ABI描述符，初始化可选插件状态，并向Registry注册Provider。ELF结构预检用于拒绝截断等异常文件，不验证插件任意代码的运行安全性。

插件的注册回调须在失败时自行撤销本次已注册的Provider；加载器不会统一回滚Registry。相关Registry与全部Feature Context销毁后，方可卸载插件。

#### 函数原型

```c
KTAG_API ktag_loaded_feature_plugin *
ktag_feature_plugin_load(const char *path, ktag_feature_registry *registry);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| path | 输入 | 非空插件动态库路径。 |
| registry | 输入 | 非空Registry，接收插件注册的Provider。 |

#### 返回值说明

成功返回持有动态库与插件状态的句柄；路径、文件、动态依赖、入口、ABI、初始化或注册失败时返回 `NULL`，立即使用 `ktag_feature_plugin_get_last_error()` 取得详情。

### `ktag_feature_plugin_unload`

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。

销毁插件状态并关闭动态库。必须先销毁相关Registry和全部Feature Context，确保不再有回调、对象或数据引用插件。

#### 函数原型

```c
KTAG_API void ktag_feature_plugin_unload(ktag_loaded_feature_plugin *plugin);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| plugin | 输入 | 待释放的对象；可为 NULL。 |

#### 返回值说明

无。

### `ktag_feature_plugin_get_id`

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。

查询已加载插件的插件标识。

#### 函数原型

```c
KTAG_API const char *
ktag_feature_plugin_get_id(const ktag_loaded_feature_plugin *plugin);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| plugin | 输入 | 待查询的对象。 |

#### 返回值说明

返回句柄持有的字符串，在插件卸载前有效；句柄为 `NULL` 时返回 `NULL`。

### `ktag_feature_plugin_get_version`

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。

查询已加载插件的插件版本。

#### 函数原型

```c
KTAG_API const char *
ktag_feature_plugin_get_version(const ktag_loaded_feature_plugin *plugin);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| plugin | 输入 | 待查询的对象。 |

#### 返回值说明

返回句柄持有的字符串，在插件卸载前有效；句柄为 `NULL` 时返回 `NULL`。

### `ktag_feature_plugin_get_last_error`

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。

查询当前线程最近一次宿主侧插件加载错误。

#### 函数原型

```c
KTAG_API const char *ktag_feature_plugin_get_last_error(void);
```

#### 参数说明

无。

#### 返回值说明

返回库持有的错误字符串，无错误时为空字符串；应在下一次插件加载前使用，不得释放或跨线程保存。

### `ktag_feature_plugin_query`

#### 功能说明

头文件路径为：`"include/ktag_feature_plugin.h"`。

插件导出的入口，由插件实现而非核心库实现。宿主预先初始化描述符版本头，通过该入口协商ABI并取得插件描述符。

#### 函数原型

```c
KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version, ktag_feature_plugin_descriptor *descriptor);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| host_abi_version | 输入 | 宿主支持的ABI版本，应为 KTAG_FEATURE_PLUGIN_ABI_VERSION。 |
| descriptor | 输入/输出 | 非空插件描述符；宿主初始化 struct_size、api_version，插件校验后填写自身信息与回调。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_NOT_SUPPORTED | 当前Traffic/Proxy插件在宿主ABI不匹配、描述符为空、尺寸不足或API版本不匹配时返回此值。自定义插件的错误码由其实现约定；加载器拒绝所有非成功返回值。 |

### 生命周期约束

宿主加载器由 `libktag_feature.so` 提供，加载时完成ABI校验和Provider注册。`ktag_feature_plugin_get_id()` / `ktag_feature_plugin_get_version()` 查询句柄信息；`ktag_feature_plugin_get_last_error()` 返回线程本地加载错误。插件卸载使用 `ktag_feature_plugin_unload()`，不能直接释放句柄。

- 插件状态由插件管理到 `plugin_destroy`；Provider注册数据由Registry深拷贝（见[ktag_feature_registry_create](#ktag_feature_registry_create)）。
- 在Registry、Schema、Feature Context或任何回调仍引用插件代码/数据时，**不得卸载**动态库；卸载必须发生在所有Context销毁之后。
- 官方 `libktag_traffic_features.so` 提供通用冻结特征。应用分类加载Traffic插件；`ktag_proxy_v1` 还需要 `libktag_proxy_features.so` 提供完整的68个 `proxy.*` Feature，两者必须同时加载。配置方式见《[用户指南](user_guide.md)》。

### 最小用例

以下插件导出一个输出正/反向包数的二维Feature（节选自 `examples/feature_plugin/custom_feature_plugin.c`，可直接编译为共享库）。

```c
/* 前提：宿主已提供公共Feature API并初始化Registry。
 * 流程：实现flow_finish → 注册描述符与回调 → 导出ABI查询入口。
 * feature_id是契约标识，display_name是展示名，二者可采用不同命名。 */
#include "ktag_feature_plugin.h"

static ktag_status packet_counts_finish(
    void *provider_state, void *flow_state,
    const ktag_flow_view *flow, ktag_feature_output *output)
{
    (void)provider_state;
    (void)flow_state;
    const float values[2] = {
        (float)ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_FORWARD),
        (float)ktag_flow_view_get_packet_count(flow, KTAG_DIRECTION_REVERSE)};
    return ktag_feature_output_set(
        output, "customer.packet_counts", values, 2U);
}

static ktag_status register_customer_provider(
    void *plugin_state, ktag_feature_registry *registry)
{
    (void)plugin_state;
    ktag_feature_descriptor feature = {0};
    feature.struct_size = sizeof(feature);
    feature.api_version = KTAG_API_VERSION;
    feature.feature_id = "customer.packet_counts";
    feature.display_name = "customer_packet_counts";
    feature.provider_id = "customer.example.plugin";
    feature.provider_version = 1U;
    feature.value_type = KTAG_FEATURE_FLOAT32;
    feature.dimension = 2U;

    ktag_feature_provider_registration provider = {0};
    provider.struct_size = sizeof(provider);
    provider.api_version = KTAG_API_VERSION;
    provider.provider_id = "customer.example.plugin";
    provider.provider_version = 1U;
    provider.features = &feature;
    provider.feature_count = 1U;
    provider.callbacks.struct_size = sizeof(provider.callbacks);
    provider.callbacks.api_version = KTAG_API_VERSION;
    provider.callbacks.flow_finish = packet_counts_finish;
    provider.callback_error_policy = KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW;
    return ktag_feature_registry_register(
        registry, &provider, KTAG_FEATURE_REGISTER_DEFAULT);
}

KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor)
{
    if (host_abi_version != KTAG_FEATURE_PLUGIN_ABI_VERSION ||
        descriptor == NULL || descriptor->struct_size < sizeof(*descriptor)) {
        return KTAG_ERROR_NOT_SUPPORTED;
    }
    descriptor->struct_size = sizeof(*descriptor);
    descriptor->api_version = KTAG_API_VERSION;
    descriptor->plugin_abi_version = KTAG_FEATURE_PLUGIN_ABI_VERSION;
    descriptor->plugin_id = "customer.example.plugin";
    descriptor->plugin_version = "1.0.0";
    descriptor->register_providers = register_customer_provider;
    return KTAG_SUCCESS;
}
```

将代码保存为 `custom_plugin.c`，编译为共享库（宿主加载方式见[生命周期约束](#生命周期约束) 与demo配置）。

```bash
cc -std=c11 -fPIC -shared -I/opt/KTAG/include /opt/KTAG/custom_plugin.c \
   -o /opt/KTAG/output/libcustom_plugin.so
```

插件随demo的加载、YAML配置与卸载顺序说明见 `examples/README.md` 第3节。

## Inference API（ktag_inference.h）

Inference层加载Schema-bound模型包，对Feature Batch执行批量推理，输出Prediction Batch。

### API 清单

| 分类 | API | 说明 |
|---|---|---|
| 校验 | ktag_inference_validate_model_package | 部署前校验模型包与Schema的一致性。 |
| 生命周期 | ktag_inference_create | 创建Inference Context。 |
| 生命周期 | ktag_inference_destroy | 销毁Inference Context。 |
| 推理 | ktag_inference_predict | 批量推理。 |
| 查询 | ktag_inference_get_schema_hash | Context的Schema Hash。 |
| 查询 | ktag_inference_get_input_value_count | 输入值数量。 |
| 查询 | ktag_inference_get_model_name | 模型名。 |
| 查询 | ktag_inference_get_last_error | 最近错误详情。 |
| 结果批次 | ktag_prediction_batch_create | 创建Prediction Batch。 |
| 结果批次 | ktag_prediction_batch_destroy | 销毁Prediction Batch。 |
| 结果批次 | ktag_prediction_batch_reserve | 预留容量。 |
| 结果批次 | ktag_prediction_batch_clear | 清空结果。 |
| 结果批次 | ktag_prediction_batch_get_count | 结果数量。 |
| 结果批次 | ktag_prediction_batch_get_record | 按下标取结果。 |

### 模型包结构

模型包是一个目录，至少包含。

```text
model_package_path/
├── manifest.json    # 绑定 Schema Hash、Feature ID/维度/顺序、模型输入、预处理规则
├── model.onnx       # ONNX 模型
└── scaler.json      # 标准化参数（标准差等）
```

Manifest中的Schema Hash必须与运行时Schema、Feature Batch三方一致；`minimum_total_packets` 等预处理规则在推理时生效。

### `ktag_inference_validate_model_package`

#### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

加载manifest.json与scaler元数据，按Feature ID解析全部模型输入，并校验两侧Schema Hash。**不创建ONNX Session**，适合作为部署/升级前的预检。

#### 函数原型

```c
KTAG_API ktag_status ktag_inference_validate_model_package(
    const char *model_package_path, const ktag_feature_schema *feature_schema,
    ktag_model_package_validation *validation);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| model_package_path | 输入 | 模型包目录路径。 |
| feature_schema | 输入 | 运行时Schema。 |
| validation | 输出 | 输出参数。版本化结构体，字段见下。 |

`ktag_model_package_validation` 字段。

| 字段 | 说明 |
|---|---|
| schema_hash | 校验通过的Schema Hash |
| input_feature_count | 模型输入Feature数 |
| input_value_count | 模型输入值数量 |
| minimum_total_packets | Manifest设置的最小总包数阈值（无则0） |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_IO | 模型包目录或必要文件不存在、不可读，或文件路径无效。 |
| KTAG_ERROR_SCHEMA_MISMATCH | Schema Hash、Feature维度或Scaler契约不匹配。 |
| KTAG_ERROR_NOT_FOUND | 必需Feature不存在，且模型包没有允许相应缺失处理。 |
| KTAG_ERROR_ALREADY_EXISTS | 模型输入声明存在重复Feature。 |
| KTAG_ERROR_CAPACITY | 模型输入值数量超过实现容量。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

### `ktag_inference_create`

#### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

加载模型包并创建Inference Context。Schema仅在创建期间借用；其描述符被解析为Context拥有的不可变输入计划。

#### 函数原型

```c
KTAG_API ktag_inference_context *
ktag_inference_create(const ktag_inference_config *config);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| config | 输入 | 非空版本化配置；模型包路径非空、Schema已冻结，线程数不超过 INT_MAX，0 使用ONNX Runtime默认设置。 |

`ktag_inference_config` 字段。

| 字段 | 说明 |
|---|---|
| model_package_path | 模型包目录路径 |
| feature_schema | 与模型包绑定的冻结Schema |
| intra_op_threads | ONNX Runtime intra-op线程数 |
| inter_op_threads | ONNX Runtime inter-op线程数 |

#### 返回值说明

成功返回新Context，失败返回 `NULL`（调用 `ktag_inference_get_last_error()` 获取详情）。

### `ktag_inference_predict`

#### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

对一个Feature Batch执行批量推理。Context在首次调用后绑定到当前线程。

**使用约束**

若Manifest设置 `minimum_total_packets`，低于阈值的记录在预处理阶段被跳过；用结果的 `source_record_index` 回查输入Batch（见[Prediction Batch API](#prediction-batch-api)）。

#### 函数原型

```c
KTAG_API ktag_status ktag_inference_predict(ktag_inference_context *context,
                                            const ktag_feature_batch *features,
                                            ktag_prediction_batch *predictions);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | Inference Context。 |
| features | 输入 | 输入Feature Batch。Schema Hash必须与模型包一致。 |
| predictions | 输出 | 输出Prediction Batch。调用前会被清空。 |

#### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_SCHEMA_MISMATCH | Feature Batch的Hash、值数量、输入宽度或模型输出契约不匹配。 |
| KTAG_ERROR_INVALID_STATE | 线程归属违规。 |
| KTAG_ERROR_NOT_SUPPORTED | 模型输出不是当前实现支持的标签Tensor形式。 |
| KTAG_ERROR_CAPACITY | 批量输入尺寸超出实现范围。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

### Inference Context 查询 API

#### `ktag_inference_get_schema_hash`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

查询Inference Context绑定的Schema Hash。

##### 函数原型

```c
KTAG_API uint64_t
ktag_inference_get_schema_hash(const ktag_inference_context *context);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待查询的对象。 |

##### 返回值说明

返回Schema Hash；Context为 `NULL` 时返回 `0`。

#### `ktag_inference_get_input_value_count`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

查询Inference Context每条模型输入的值数量。

##### 函数原型

```c
KTAG_API uint32_t
ktag_inference_get_input_value_count(const ktag_inference_context *context);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待查询的对象。 |

##### 返回值说明

返回输入值数量；Context为 `NULL` 时返回 `0`。

#### `ktag_inference_get_model_name`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

查询Inference Context模型名称。

##### 函数原型

```c
KTAG_API const char *
ktag_inference_get_model_name(const ktag_inference_context *context);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待查询的对象。 |

##### 返回值说明

返回Context持有的模型名，在Context销毁前有效；Context为 `NULL` 时返回 `NULL`。

#### `ktag_inference_get_last_error`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

查询当前线程最近一次Inference API错误详情。

##### 函数原型

```c
KTAG_API const char *ktag_inference_get_last_error(void);
```

##### 参数说明

无。

##### 返回值说明

返回库持有的错误字符串，无错误时为空字符串；应在下一次相关调用前使用，不得释放或跨线程保存。

### `ktag_inference_destroy`

#### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

销毁Inference Context并释放ONNX Runtime会话等资源。

#### 函数原型

```c
KTAG_API void ktag_inference_destroy(ktag_inference_context *context);
```

#### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| context | 输入 | 待销毁的Context；可为 NULL。 |

#### 返回值说明

无。

### Prediction Batch API

`ktag_prediction_record` 字段。

| 字段 | 说明 |
|---|---|
| metadata | ktag_prediction_metadata：flow_id、flow_key、首/末包序号，来自输入记录 |
| source_record_index | 对应输入Feature Batch的记录下标，用于回查 |
| label | 数字类别标签（int64_t）；业务名称由应用维护映射 |
| confidence | 模型置信度。[0,1] 内为模型概率，0 合法且不得抑制或改写标签；KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE（-1.0F）表示无概率输出但标签仍有效。库不内置unknown阈值策略。 |

#### `ktag_prediction_batch_create`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

创建空Prediction Batch。

##### 函数原型

```c
KTAG_API ktag_prediction_batch *
ktag_prediction_batch_create(uint32_t initial_capacity);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| initial_capacity | 输入 | 预留记录数；0 表示不预留，后续按需扩容。 |

##### 返回值说明

成功返回Batch，分配失败时返回 `NULL`。

#### `ktag_prediction_batch_destroy`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

销毁Prediction Batch及其全部预测记录。

##### 函数原型

```c
KTAG_API void ktag_prediction_batch_destroy(ktag_prediction_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 待释放的对象；可为 NULL。 |

##### 返回值说明

无。

#### `ktag_prediction_batch_reserve`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

预留至少指定数量的预测记录存储。

##### 函数原型

```c
KTAG_API ktag_status ktag_prediction_batch_reserve(ktag_prediction_batch *batch,
                                                   uint32_t capacity);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Prediction Batch。 |
| capacity | 输入 | 预留的记录数量。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |
| KTAG_ERROR_OUT_OF_MEMORY | 内存分配失败。 |
| KTAG_ERROR_INTERNAL | 内部处理失败。 |

#### `ktag_prediction_batch_clear`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

清空全部预测记录，保留已分配的容量。

##### 函数原型

```c
KTAG_API void ktag_prediction_batch_clear(ktag_prediction_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Prediction Batch；可为 NULL。 |

##### 返回值说明

无。

#### `ktag_prediction_batch_get_count`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

查询预测记录数量。

##### 函数原型

```c
KTAG_API uint32_t
ktag_prediction_batch_get_count(const ktag_prediction_batch *batch);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 待查询的对象。 |

##### 返回值说明

返回记录数量；`batch` 为 `NULL` 时返回 `0`。

#### `ktag_prediction_batch_get_record`

##### 功能说明

头文件路径为：`"include/ktag_inference.h"`。

按下标复制一条预测记录。接口填写记录及其元数据版本头，无需预设输出版本头。

##### 函数原型

```c
KTAG_API ktag_status ktag_prediction_batch_get_record(
    const ktag_prediction_batch *batch, uint32_t index,
    ktag_prediction_record *record);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| batch | 输入 | 目标Prediction Batch。 |
| index | 输入 | 记录下标，范围为 [0, count)。 |
| record | 输出 | 非空预测记录存储。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_SUCCESS | 操作成功。 |
| KTAG_ERROR_INVALID_ARGUMENT | 参数为空、取值或版本化结构体不合法。 |

### 最小用例

校验模型包、创建三层上下文、处理PCAP并打印预测结果。完整可编译版本见 `examples/three_layer/three_layer_example.c`，以下为核心流程。

```c
#include <inttypes.h>
#include <stdio.h>
#include "ktag_capture.h"
#include "ktag_feature.h"
#include "ktag_feature_plugin.h"
#include "ktag_inference.h"

static void print_prediction_batch(const ktag_prediction_batch *predictions)
{
    uint32_t index;
    for (index = 0U;
         index < ktag_prediction_batch_get_count(predictions); ++index) {
        ktag_prediction_record record = {0};
        record.struct_size = sizeof(record);
        record.api_version = KTAG_API_VERSION;
        if (ktag_prediction_batch_get_record(
                predictions, index, &record) == KTAG_SUCCESS) {
            printf("flow=%" PRIu64 " label=%" PRId64 " confidence=%.6f\n",
                   record.metadata.flow_id, record.label,
                   (double)record.confidence);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s MODEL_PACKAGE INPUT.pcap\n", argv[0]);
        return 1;
    }

    /* 1. Schema 与 Provider（Legacy V1 契约）。 */
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ktag_feature_registry *registry = ktag_feature_registry_create();
    ktag_loaded_feature_plugin *plugin = registry == NULL ? NULL :
        ktag_feature_plugin_load(
            "/opt/KTAG/output/plugins/libktag_traffic_features.so", registry);
    if (schema == NULL || plugin == NULL) {
        return 1;
    }

    /* 2. 部署预检：不创建 ONNX Session。 */
    ktag_model_package_validation validation = {0};
    validation.struct_size = sizeof(validation);
    validation.api_version = KTAG_API_VERSION;
    if (ktag_inference_validate_model_package(
            argv[1], schema, &validation) != KTAG_SUCCESS) {
        fprintf(stderr, "model package rejected: %s\n",
                ktag_inference_get_last_error());
        return 1;
    }

    /* 3. 三层上下文。 */
    ktag_feature_config fc = {0};
    fc.struct_size = sizeof(fc);
    fc.api_version = KTAG_API_VERSION;
    fc.schema = schema;
    fc.registry = registry;
    fc.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT;
    ktag_feature_context *feature = ktag_feature_create(&fc);

    ktag_inference_config ic = {0};
    ic.struct_size = sizeof(ic);
    ic.api_version = KTAG_API_VERSION;
    ic.model_package_path = argv[1];
    ic.feature_schema = schema;
    ic.intra_op_threads = 1U;
    ic.inter_op_threads = 1U;
    ktag_inference_context *inference = ktag_inference_create(&ic);

    ktag_capture_config cc = {0};
    cc.struct_size = sizeof(cc);
    cc.api_version = KTAG_API_VERSION;
    cc.source_type = KTAG_CAPTURE_SOURCE_PCAP_FILE;
    cc.source = argv[2];
    cc.packet_mode = KTAG_CAPTURE_PACKET_BORROWED;
    ktag_capture_context *capture = ktag_capture_open(&cc);

    ktag_feature_batch *features = ktag_feature_batch_create(schema, 1U);
    ktag_prediction_batch *predictions = ktag_prediction_batch_create(1U);
    if (feature == NULL || inference == NULL || capture == NULL ||
        features == NULL || predictions == NULL) {
        return 1;
    }

    /* 4. 主循环：读取 → 提取 → 推理。 */
    ktag_packet packet;
    ktag_packet_init(&packet);
    for (;;) {
        ktag_status status = ktag_capture_next(capture, &packet);
        if (status != KTAG_SUCCESS) {
            break;
        }
        if (ktag_feature_process_packet(feature, &packet, features) ==
                KTAG_SUCCESS &&
            ktag_feature_batch_get_count(features) > 0U) {
            if (ktag_inference_predict(inference, features, predictions) == KTAG_SUCCESS) {
                print_prediction_batch(predictions);
            }
        }
        ktag_packet_release(&packet);
    }

    /* 5. EOF：flush 剩余流并推理。 */
    if (ktag_feature_flush(feature, features) == KTAG_SUCCESS &&
        ktag_feature_batch_get_count(features) > 0U &&
        ktag_inference_predict(inference, features, predictions) == KTAG_SUCCESS) {
        print_prediction_batch(predictions);
    }

    /* 6. 反向销毁。 */
    ktag_prediction_batch_destroy(predictions);
    ktag_feature_batch_destroy(features);
    ktag_capture_close(capture);
    ktag_inference_destroy(inference);
    ktag_feature_destroy(feature);
    ktag_feature_registry_destroy(registry);
    ktag_feature_schema_destroy(schema);
    ktag_feature_plugin_unload(plugin);
    return 0;
}
```

构建与运行（与 `examples/three_layer` 一致）。

```bash
bash /opt/KTAG/buildtools/build.sh   # 若尚未构建
cc -std=c11 -I/opt/KTAG/include /opt/KTAG/inference_example.c \
   -L/opt/KTAG/output -lktag_capture -lktag_feature -lktag_inference \
   -Wl,-rpath,/opt/KTAG/output -o /opt/KTAG/output/inference_example
/opt/KTAG/output/inference_example model_package_dir sample.pcap
```

正确输出（行数与内容随输入变化）。

```text
flow=1 label=2 confidence=0.941372
flow=2 label=0 confidence=0.873210
```

<a id="端到端调用顺序"></a>

## 端到端调用顺序（跨 API）

```text
Capture open → Registry/插件加载/Schema freeze → Feature Context/Batch
      → 模型包校验 → Inference Context/Prediction Batch
      → next → process_packet →（有 Feature 时）predict
      → EOF → flush →（有 Feature 时）predict
      → 依赖对象的反向销毁
```

处理EOF时，先 `ktag_feature_flush()`，再按依赖反向销毁：Prediction Batch → Inference Context → Capture → Feature Context → Feature Batch → Schema → Registry → 插件卸载。生产系统还应定期调用 `ktag_feature_expire()`、处理 `KTAG_STATUS_TIMEOUT`、检查状态码并落实队列背压与资源记录。

## 冻结特征契约与数据桥接

### 冻结特征提取

特征提取统一使用公共Feature API，
由 `libktag_traffic_features.so` 的Provider执行。`plugins/traffic_features/legacy/`
保存冻结的统计算法、HTTP处理、RawBoW和DFA分词规则。

调用顺序为：创建Registry → 加载Traffic Feature Plugin → 构造Schema → 创建
Feature Context/Batch → `ktag_feature_process_packet` → `ktag_feature_flush`。
需要历史流生命周期时，设置 `KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT`。
RawBoW文本在 `collect_annotations` 开启后，用 `ktag_feature_batch_get_annotation()`
读取 `KTAG_TRAFFIC_RAWBOW_ANNOTATION`（`traffic.rawbow.text`，定义于 `ktag_traffic_features.h`）；不进入模型输入。
冻结Schema继续使用 `ktag_legacy_v1`，每条记录包含354个值。

### 固定向量数据桥接

`ktag_legacy_types.h` 仅保留历史Demo文件格式使用的数据类型，不包含提取算法、
提取器或推理接口。`feature_vector` 仍有355个浮点槽位：Feature Batch的354个值按原顺序
写入前354个槽位，最后一个保留槽位为零。Flow ID位于第0个槽位。
Demo的私有C++ Session负责上述转换，不构成SDK公共接口或独立动态库。

### 统一公共推理 API

模型推理统一使用`ktag_inference_create`、`ktag_inference_predict` 和 `ktag_inference_destroy`，
输入为Schema绑定的Feature Batch，输出为Prediction Batch。
历史ONNX/Scaler可通过 `tools/model_package/create_legacy_package.py` 转换为模型包。
列选择、包数过滤和标准化规则均由模型包定义。

### 产品版本查询

查询产品元数据及库版本，实现位于 `src/core/version.c`。执行 `bash buildtools/build.sh -lib`，或在已有构建上设置 `ENABLE_BUILD_LIB=ON`，生成独立的 `libktag_appidlib.so`；调用方链接 `-lktag_appidlib`，该库目前不随 `KTAGTargets` 导出。

#### `KTAG_GetProductVersion`

##### 功能说明

头文件路径为：`"include/ktag_core.h"`。

查询产品、组件和软件的名称、版本及附加信息。各字段为 `KTAG_VERSION_INFO_LEN`（100）字节的字符数组，由接口写入。

##### 函数原型

```c
KTAGResult KTAG_API KTAG_GetProductVersion(KTAGProVersion *packageInfo);
```

##### 参数说明

| 参数名 | 输入/输出 | 描述 |
| :--- | :--- | :--- |
| packageInfo | 输出 | 非空的 KTAGProVersion 存储，不使用现代API版本头。 |

##### 返回值说明

| 返回值 | 说明 |
| :--- | :--- |
| KTAG_STS_NO_ERR | 查询成功。 |
| KTAG_STS_NULL_PTR_ERR | packageInfo 为 NULL。 |

#### `KTAG_GetLibVersion`

##### 功能说明

头文件路径为：`"include/ktag_core.h"`。

查询构建时确定的库版本信息：`major`、`minor`、`patch` 和 `buildDate`。

##### 函数原型

```c
KTAG_API const KTAGLibraryVersion *KTAG_GetLibVersion(void);
```

##### 参数说明

无。

##### 返回值说明

返回库内静态版本结构体的只读指针，不要求调用方分配内存，不能释放或修改；有效期为动态库加载期间。

### 接入方式

| 能力 | 接口 |
|---|---|
| 创建特征运行上下文 | ktag_feature_create |
| 处理报文、读取结果 | ktag_feature_process_packet 与Feature Batch API |
| 输出剩余流 | ktag_feature_flush |
| 查询特征运行统计 | ktag_feature_get_statistics |
| 模型推理 | ktag_inference_create / ktag_inference_predict / ktag_inference_destroy |
| 产品元数据 / 库版本 | KTAG_GetProductVersion / KTAG_GetLibVersion |

## 协议边界与限制

现代Pipeline支持以下协议范围，超出边界的流量处理行为如下。

- 支持IPv4/IPv6 TCP、UDP；IPv6使用完整128位地址作为Flow Key。
- IPv4/IPv6不跨地址族合流。
- 不做IPv4/IPv6分片重组，不支持IPv6 Jumbogram，不解析ESP加密内容。
- 无法由非首分片构造五元组的报文会被跳过（计入 `rejected_packets`）。
- 加密流量分类基于可观测网络特征和训练数据，应以目标网络的独立数据持续验证和再训练。

## 修订记录

| 文档版本 | 发布日期 | 修改说明 |
| --- | --- | --- |
| 01 | 2026-12-30 | 第一次正式发布。 |
