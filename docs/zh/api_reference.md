# API 参考

本文以 `include/` 下发布的头文件为准，按模块分章介绍 KTAG 的公开 C API。每章先给出 API 清单，再逐个说明 API 的功能、签名、参数和返回值；关键流程在章末补充最小用例。新项目应使用 Capture、Feature、Inference 三层 API；旧的固定 `feature_vector` 接口仅保留兼容性（见[第 8 章](#8-历史兼容-api已弃用)）。

应用/代理分类配置和命令行操作见《[用户指南](user_guide.md)》；快速执行 demo 见《[快速上手指南](quick_start.md)》。

## 1 文档范围与公共约定

### 1.1 文档范围

| 头文件 | 职责 | 使用建议 |
|---|---|---|
| `ktag_types.h` | 状态码、时间戳、IP 地址和 Flow Key | 必需 |
| `ktag_capture.h` | PCAP/网口读取、报文生命周期、Flow Key、设备发现 | 推荐 |
| `ktag_feature.h` | 组流、Schema、Provider、Domain Store、Feature Batch | 推荐 |
| `ktag_feature_plugin.h` | 动态 Feature Provider 插件 ABI | 按需 |
| `ktag_inference.h` | 模型包校验和 Schema-aware 批量推理 | 推荐 |
| `ktag_feature_extract.h`、`ktag_legacy_inference.h` | 历史固定向量接口 | 仅兼容 |
| `ktag_core.h`、`ktag_type.h` | 历史产品版本查询及其返回类型 | 仅兼容 |

运行时对应三个共享库：`libktag_capture.so`、`libktag_feature.so`、`libktag_inference.so`。构建产物位于仓库 `output/` 目录，构建方式见《[快速上手](quick_start.md)》。

### 1.2 公共约定

以下约定适用于全部现代 API（`ktag_*.h`），各 API 小节不再重复。

#### 1.2.1 ABI 版本与版本化结构体

`KTAG_API_VERSION` 当前为 `1U`。凡是传入现代 API 或由现代 API 填充的**版本化结构体**（含 `struct_size` 和 `api_version` 两个字段），调用方都必须先清零再设置这两个字段：

```c
ktag_capture_config config = {0};
config.struct_size = sizeof(config);
config.api_version = KTAG_API_VERSION;
```

这包括 `ktag_packet`、各种 `*_config`、Feature 描述符、Provider 注册信息、统计信息、记录和模型包校验结果。没有这两个字段的类型（如 `ktag_flow_packet_sample`）不适用。尺寸不足或版本不匹配会返回 `KTAG_ERROR_INVALID_ARGUMENT`。

#### 1.2.2 状态码

所有返回 `ktag_status` 的 API 使用以下状态码：

| 状态码 | 值 | 含义 |
|---|---:|---|
| `KTAG_SUCCESS` | `0` | 成功 |
| `KTAG_STATUS_TIMEOUT` | `1` | 实时读取超时，无报文，不是错误 |
| `KTAG_STATUS_END_OF_STREAM` | `2` | PCAP 读完或读取被中断 |
| `KTAG_ERROR_INVALID_ARGUMENT` | `-1` | 指针、版本化结构体、枚举或参数不合法 |
| `KTAG_ERROR_OUT_OF_MEMORY` | `-2` | 内存分配失败 |
| `KTAG_ERROR_ALREADY_EXISTS` | `-3` | 对象或注册项已存在 |
| `KTAG_ERROR_NOT_FOUND` | `-4` | 查找对象不存在 |
| `KTAG_ERROR_INVALID_STATE` | `-5` | 生命周期或线程归属不符合要求 |
| `KTAG_ERROR_CAPACITY` | `-6` | 容量、批次大小或资源不足 |
| `KTAG_ERROR_SCHEMA_MISMATCH` | `-7` | Schema、模型包或 Feature Batch 不一致 |
| `KTAG_ERROR_INTERNAL` | `-8` | 内部错误 |
| `KTAG_ERROR_NOT_SUPPORTED` | `-9` | 不支持的操作 |
| `KTAG_ERROR_IO` | `-10` | I/O 错误 |

#### 1.2.3 错误详情

Capture 和 Inference 分别提供线程本地的错误详情接口：`ktag_capture_get_last_error()` 和 `ktag_inference_get_last_error()`。返回字符串由库管理，应在下一次同类调用前记录，不能释放或跨线程保存。

#### 1.2.4 对象所有权

- 文中标注为 opaque 的对象只能通过对应 API 创建、传递和释放，调用者不得访问其内部布局。
- 由 `*_create`/`*_open` 返回的对象，使用对应 `*_destroy`/`*_close` 释放；`NULL` 参数对 destroy 类接口安全。
- 除非接口明确转移所有权，输入对象的所有权仍归调用者。
- 返回 `const char *` 的查询接口，如无特别说明，字符串由被查询对象持有，在该对象销毁前有效。

#### 1.2.5 线程安全

- Feature Context：首次调用 `process`/`expire`/`flush`/统计接口后绑定到单一线程；不同 Context 可并行。
- Inference Context：首次 `predict` 后绑定到单一线程；不同 Context 可并行。
- Domain Store：变更和统计接口线程安全，可被多个 Context 共享。
- Domain Cache、Feature Batch、Prediction Batch：单线程对象。
- `get_last_error` 系列接口：返回线程本地数据。

## 2 基础类型（ktag_types.h）

`ktag_types.h` 只定义类型和常量，没有函数。所有现代 API 都间接依赖它。

| 类型 | 说明 |
|---|---|
| `ktag_status` | `int32_t`，状态码，见 1.2.2 |
| `ktag_struct_header` | 版本化结构体公共头：`struct_size` + `api_version` |
| `ktag_timestamp` | `seconds`（int64）+ `microseconds`（int32） |
| `ktag_ip_address` | `bytes[16]` + `version`（4 或 6）+ `reserved[3]` |
| `ktag_flow_key` | 双向规范化五元组，见下文 |

`ktag_ip_address` 使用 `version` 标记 IPv4/IPv6；IPv4 地址位于 `bytes[0..3]`，其余字节为 0。IPv6 使用全部 16 字节，网络字节序。

`ktag_flow_key` 是**双向规范化**五元组：

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

端点 A 按地址/端口排序后一定位于端点 B 之前，因此同一业务流的正向和反向报文归入同一个 Key。它适合哈希、分流和关联，但**不表示首包的源/目的方向**。需要首包方向五元组时，使用 `ktag_flow_view_get_legacy_tuple()`（见 4.4.3）或 `ktag_feature_record_details`（见 4.6.2）。

## 3 Capture API（ktag_capture.h）

Capture 层负责从 PCAP 文件或网口读取报文，管理报文内存生命周期，并提供轻量 L2/L3/L4 解析、Flow Key 构造、稳定哈希与 worker 分流。本层不创建线程。

### 3.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 打开/关闭 | `ktag_capture_open` | 打开 PCAP 文件或网口。 |
| 打开/关闭 | `ktag_capture_close` | 关闭 Capture 上下文。 |
| 打开/关闭 | `ktag_capture_interrupt` | 唤醒阻塞中的读取。 |
| 打开/关闭 | `ktag_capture_get_link_type` | 查询链路层类型。 |
| 读取 | `ktag_packet_init` | 初始化报文存储对象。 |
| 读取 | `ktag_capture_next` | 读取下一个报文。 |
| 读取 | `ktag_capture_get_last_error` | 查询最近错误详情。 |
| 报文所有权 | `ktag_packet_make_owned` | 借用报文转拥有副本。 |
| 报文所有权 | `ktag_packet_retain` | 增加一个引用。 |
| 报文所有权 | `ktag_packet_release` | 释放一个引用。 |
| 报文所有权 | `ktag_packet_is_owned` | 查询报文是否为拥有型。 |
| Flow Key | `ktag_packet_get_flow_key` | 解析报文并构造规范化 Flow Key。 |
| Flow Key | `ktag_flow_hash` | Flow Key 稳定哈希。 |
| Flow Key | `ktag_flow_select_worker` | 按 Flow Key 选择 worker。 |
| 设备发现 | `ktag_capture_list_devices` | 枚举本机抓包设备。 |
| 设备发现 | `ktag_capture_device_list_destroy` | 释放设备列表。 |
| 设备发现 | `ktag_capture_device_list_get_count` | 设备数量。 |
| 设备发现 | `ktag_capture_device_list_get_name` | 设备名。 |
| 设备发现 | `ktag_capture_device_list_get_description` | 设备描述。 |
| 设备发现 | `ktag_capture_device_list_get_ipv4_count` | 设备 IPv4 地址数。 |
| 设备发现 | `ktag_capture_device_list_get_ipv4` | 设备 IPv4 地址。 |
| 设备发现 | `ktag_capture_device_list_get_ipv6_count` | 设备 IPv6 地址数。 |
| 设备发现 | `ktag_capture_device_list_get_ipv6` | 设备 IPv6 地址。 |

### 3.2 对象与常量

**链路层类型常量**：

| 常量 | 值 | 说明 |
|---|---:|---|
| `KTAG_CAPTURE_LINKTYPE_ETHERNET` | `1` | 以太网 |
| `KTAG_CAPTURE_LINKTYPE_RAW` | `12` | RAW IP |
| `KTAG_CAPTURE_LINKTYPE_LINUX_SLL` | `113` | Linux SLL |
| `KTAG_CAPTURE_LINKTYPE_LINUX_SLL2` | `276` | Linux SLL2 |

**输入源类型 `ktag_capture_source_type`**：

| 枚举值 | 说明 |
|---|---|
| `KTAG_CAPTURE_SOURCE_PCAP_FILE`（`1`） | 离线 PCAP 文件 |
| `KTAG_CAPTURE_SOURCE_LIVE_INTERFACE`（`2`） | 实时网口 |

**报文模式 `ktag_capture_packet_mode`**：

| 枚举值 | 说明 |
|---|---|
| `KTAG_CAPTURE_PACKET_BORROWED`（`0`） | `data` 仅在下一次 `next()` 或 Capture 关闭前有效，低拷贝。 |
| `KTAG_CAPTURE_PACKET_OWNED`（`1`） | `data` 复制到引用计数 PacketBuffer，可跨线程排队。 |

**`ktag_capture_config` 字段**：

| 字段 | 说明 |
|---|---|
| `source_type` | 输入源类型，见上表 |
| `source` | PCAP 文件路径或网口名，不能为空 |
| `packet_mode` | 报文模式，见上表 |
| `snapshot_length` | 抓包长度，`0` 使用库默认值 |
| `promiscuous` | 非 0 时开启混杂模式（仅网口） |
| `read_timeout_ms` | 网口读取超时（毫秒），`0` 使用库默认值 |
| `live_buffer_size` | 网口缓冲区字节数，`0` 使用 libpcap 默认值 |
| `packet_buffer_capacity` | `OWNED` 模式报文池容量，`0` 使用库默认值 |

**`ktag_packet` 字段**（版本化结构体）：

| 字段 | 说明 |
|---|---|
| `data` | 报文数据指针。有效范围由报文模式决定 |
| `captured_length` | 实际捕获的字节数 |
| `original_length` | 线路上的原始字节数（可能大于 `captured_length`） |
| `link_type` | 链路层类型 |
| `sequence` | 本次会话内的报文序号 |
| `timestamp` | 报文时间戳 |
| `owner` | 拥有型报文的 PacketBuffer 引用，借用型为 `NULL` |

### 3.3 `ktag_capture_open`

打开一个离线 PCAP 文件或实时网口，返回 Capture 上下文。本函数不创建任何工作线程。

```c
KTAG_API ktag_capture_context *
ktag_capture_open(const ktag_capture_config *config);
```

| 参数 | 说明 |
|---|---|
| `config` | 配置结构体，必须按 1.2.1 初始化版本头；`source` 不能为空。 |

返回值：

| 返回值 | 说明 |
|---|---|
| 非 `NULL` | 打开成功，调用者用 `ktag_capture_close()` 释放。 |
| `NULL` | 打开失败，调用 `ktag_capture_get_last_error()` 获取详情。 |

### 3.4 `ktag_packet_init`

初始化一个报文存储对象，使其可以首次传入 `ktag_capture_next()`。

```c
KTAG_API void ktag_packet_init(ktag_packet *packet);
```

| 参数 | 说明 |
|---|---|
| `packet` | 待初始化的报文对象。 |

返回值：无。

使用约束：全新的 `ktag_packet` 存储必须先调用本函数才能用于 `ktag_capture_next()`；`ktag_packet_release()` 之后对象自动恢复为可复用状态，无需重新 init。

### 3.5 `ktag_capture_next`

从 Capture 上下文读取下一个报文。

```c
KTAG_API ktag_status ktag_capture_next(ktag_capture_context *context,
                                       ktag_packet *packet);
```

| 参数 | 说明 |
|---|---|
| `context` | 已打开的 Capture 上下文。 |
| `packet` | 输出参数。必须先用 `ktag_packet_init()` 初始化；上一个拥有型报文必须先 `ktag_packet_release()` 释放后才能再次传入同一对象。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 读取成功，`packet` 已填充。 |
| `KTAG_STATUS_TIMEOUT` | 网口读取超时且无报文，不是错误，可继续调用。 |
| `KTAG_STATUS_END_OF_STREAM` | PCAP 读完，或读取被 `ktag_capture_interrupt()` 中断。 |
| 其他负数 | 读取失败，调用 `ktag_capture_get_last_error()` 获取详情。 |

使用约束：

- `BORROWED` 报文的 `data` 仅在下一次 `next()` 或 Capture 关闭前有效，不能跨线程排队；需要跨线程时调用 `ktag_packet_make_owned()`。
- `OWNED` 报文在重新传入同一输出对象前必须先 `ktag_packet_release()`。

### 3.6 `ktag_capture_get_link_type`

查询 Capture 上下文的链路层类型。

```c
KTAG_API uint32_t
ktag_capture_get_link_type(const ktag_capture_context *context);
```

| 参数 | 说明 |
|---|---|
| `context` | 已打开的 Capture 上下文。 |

返回值：链路层类型常量（见 3.2）。

### 3.7 `ktag_capture_interrupt`

从另一线程唤醒阻塞中的 `ktag_capture_next()` 调用，该读取随后返回 `KTAG_STATUS_END_OF_STREAM`。

```c
KTAG_API void ktag_capture_interrupt(ktag_capture_context *context);
```

| 参数 | 说明 |
|---|---|
| `context` | 已打开的 Capture 上下文。 |

返回值：无。

使用约束：停止读取后再关闭 Capture；中断后不要再访问其产生的借用型报文。

### 3.8 `ktag_capture_close`

关闭 Capture 上下文并释放全部关联资源。

```c
KTAG_API void ktag_capture_close(ktag_capture_context *context);
```

| 参数 | 说明 |
|---|---|
| `context` | 待关闭的上下文；可为 `NULL`。 |

返回值：无。

### 3.9 `ktag_capture_get_last_error`

返回当前线程最近一次 Capture API 失败的详情字符串。

```c
KTAG_API const char *ktag_capture_get_last_error(void);
```

| 参数 | 说明 |
|---|---|
| 无 | 该接口无参数。 |

返回值：错误详情字符串；无错误时可能为空字符串。字符串由库管理，在下一次同类调用前有效，不能释放或跨线程保存。

### 3.10 `ktag_packet_make_owned`

把借用型报文转换为独立的拥有型副本，此后可以安全地跨线程排队，并可以继续调用 `ktag_capture_next()`。

```c
KTAG_API ktag_status ktag_packet_make_owned(ktag_packet *packet);
```

| 参数 | 说明 |
|---|---|
| `packet` | 待转换的报文对象。转换成功后原 `data` 指针由 PacketBuffer 支撑。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 转换成功。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | 参数不合法。 |
| `KTAG_ERROR_OUT_OF_MEMORY` | 报文池无法分配新的 PacketBuffer。 |

### 3.11 `ktag_packet_retain`

为拥有型报文增加一个引用。多线程共享同一报文时：先复制 `ktag_packet` 值，再对每个副本调用本函数；每个视图最后各调用一次 `ktag_packet_release()`。

```c
KTAG_API ktag_status ktag_packet_retain(ktag_packet *packet);
```

| 参数 | 说明 |
|---|---|
| `packet` | 拥有型报文视图。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 引用计数已增加。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | 报文不是拥有型；借用型报文不能 retain。 |

### 3.12 `ktag_packet_release`

释放拥有型报文的一个引用，并清空该视图。引用归零时报文内存归还报文池。

```c
KTAG_API void ktag_packet_release(ktag_packet *packet);
```

| 参数 | 说明 |
|---|---|
| `packet` | 待释放的报文视图。释放后对象保持已初始化状态，可再次用于 `ktag_capture_next()`。 |

返回值：无。

### 3.13 `ktag_packet_is_owned`

查询报文当前是否为拥有型。

```c
KTAG_API uint32_t ktag_packet_is_owned(const ktag_packet *packet);
```

| 参数 | 说明 |
|---|---|
| `packet` | 待查询的报文。 |

返回值：非 0 表示拥有型，`0` 表示借用型。

### 3.14 `ktag_packet_get_flow_key`

对报文做轻量只读 L2/L3/L4 解析，输出双向规范化的 Flow Key。

```c
KTAG_API ktag_status ktag_packet_get_flow_key(const ktag_packet *packet,
                                              ktag_flow_key *key);
```

| 参数 | 说明 |
|---|---|
| `packet` | 待解析的报文。 |
| `key` | 输出参数。版本化结构体，必须按 1.2.1 初始化。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 解析成功，`key` 已填充。 |
| 其他负数 | 报文无法解析出五元组（如非首分片、不支持的协议），应跳过该报文。 |

使用约束：只有本函数成功返回的 Key 才能用于 `ktag_flow_hash` / `ktag_flow_select_worker`。

### 3.15 `ktag_flow_hash`

计算 Flow Key 的稳定哈希。只哈希逻辑字段，不受 C 结构体填充影响，同一 Key 在不同进程/平台得到相同结果。

```c
KTAG_API uint64_t ktag_flow_hash(const ktag_flow_key *key, uint64_t seed);
```

| 参数 | 说明 |
|---|---|
| `key` | 有效的 Flow Key（来自 `ktag_packet_get_flow_key()` 成功返回）。 |
| `seed` | 哈希种子。相同 Key 与不同 seed 得到不同结果。 |

返回值：64 位哈希值。

### 3.16 `ktag_flow_select_worker`

按 Flow Key 哈希选择一个 worker 下标，用于多线程分流。同一流的所有报文（正反方向）映射到同一 worker。

```c
KTAG_API uint32_t ktag_flow_select_worker(const ktag_flow_key *key,
                                          uint32_t worker_count, uint64_t seed);
```

| 参数 | 说明 |
|---|---|
| `key` | 有效的 Flow Key。 |
| `worker_count` | worker 数量。 |
| `seed` | 哈希种子，与 `ktag_flow_hash` 语义一致。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `0 .. worker_count-1` | 选中的 worker 下标。 |
| `KTAG_WORKER_INDEX_INVALID`（`UINT32_MAX`） | `worker_count` 为 0 或 Key 不合法。 |

### 3.17 设备发现 API

设备发现接口封装 libpcap 的设备枚举能力，不暴露 libpcap 类型，供 demo 和工具使用。

```c
KTAG_API ktag_capture_device_list *ktag_capture_list_devices(void);
KTAG_API void ktag_capture_device_list_destroy(ktag_capture_device_list *devices);
KTAG_API uint32_t ktag_capture_device_list_get_count(const ktag_capture_device_list *devices);
KTAG_API const char *ktag_capture_device_list_get_name(const ktag_capture_device_list *devices, uint32_t index);
KTAG_API const char *ktag_capture_device_list_get_description(const ktag_capture_device_list *devices, uint32_t index);
KTAG_API uint32_t ktag_capture_device_list_get_ipv4_count(const ktag_capture_device_list *devices, uint32_t index);
KTAG_API const char *ktag_capture_device_list_get_ipv4(const ktag_capture_device_list *devices, uint32_t device_index, uint32_t address_index);
KTAG_API uint32_t ktag_capture_device_list_get_ipv6_count(const ktag_capture_device_list *devices, uint32_t index);
KTAG_API const char *ktag_capture_device_list_get_ipv6(const ktag_capture_device_list *devices, uint32_t device_index, uint32_t address_index);
```

| API | 说明 |
|---|---|
| `ktag_capture_list_devices` | 枚举设备，返回设备列表；失败返回 `NULL`。 |
| `ktag_capture_device_list_destroy` | 释放设备列表；`NULL` 安全。 |
| `..._get_count` | 返回设备数量。 |
| `..._get_name` / `..._get_description` | 返回第 `index` 个设备的名称/描述字符串；索引越界返回 `NULL`。 |
| `..._get_ipv4_count` / `..._get_ipv6_count` | 返回第 `index` 个设备的 IPv4/IPv6 地址数量。 |
| `..._get_ipv4` / `..._get_ipv6` | 返回设备 `device_index` 第 `address_index` 个地址的字符串形式；索引越界返回 `NULL`。 |

使用约束：`get_name`/`get_description`/`get_ipv4`/`get_ipv6` 返回的字符串由设备列表持有，在 `ktag_capture_device_list_destroy()` 前有效。

### 3.18 最小用例

读取一个 PCAP 文件，解析每个报文的 Flow Key 并统计正反方向归一后的流数量。

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

    ktag_capture_context *capture = ktag_capture_open(&config);
    if (capture == NULL) {
        fprintf(stderr, "open failed: %s\n", ktag_capture_get_last_error());
        return 1;
    }

    uint64_t total = 0U, parsed = 0U;
    ktag_packet packet;
    ktag_packet_init(&packet);
    for (;;) {
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
        ktag_packet_release(&packet);
    }

    ktag_capture_close(capture);
    printf("packets=%llu parsed=%llu\n",
           (unsigned long long)total, (unsigned long long)parsed);
    return 0;
}
```

将代码保存为 `capture_example.c`，在仓库根目录执行：

```bash
sh /opt/KTAG/build.sh   # 若尚未构建，产物位于 /opt/KTAG/output/
cc -std=c11 -I/opt/KTAG/include /opt/KTAG/capture_example.c \
   -L/opt/KTAG/output -lktag_capture -Wl,-rpath,/opt/KTAG/output \
   -o /opt/KTAG/output/capture_example
/opt/KTAG/output/capture_example sample.pcap
```

正确输出（数字随输入文件变化）：

```text
packets=128 parsed=126
```

## 4 Feature API（ktag_feature.h）

Feature 层把报文组成流、调度已注册的 Feature Provider 计算特征，并按 Schema 契约输出 Feature Batch。本章按对象族分为 8 节：Schema（4.2）、Registry 与 Provider（4.3）、Provider 视图与输出（4.4）、Feature Context（4.5）、Feature Batch（4.6）、Domain Store（4.7）、Domain Cache（4.8）。

### 4.1 对象关系总览

```text
ktag_feature_registry ──register──▶ Provider（内置/插件）
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

典型创建顺序：创建 Registry → 注册 Provider → 创建/组合 Schema → Freeze → 创建 Feature Context（+可选共享 Domain Store）→ 创建 Feature Batch。

### 4.2 Schema API

Schema 固化模型输入的 Feature ID、类型、维度和顺序；创建后可修改，Freeze 后不可变。Schema Hash 是 Feature Batch、模型包与 Context 之间的一致性凭证。

#### 4.2.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | `ktag_feature_schema_create` | 创建 Schema。 |
| 生命周期 | `ktag_feature_schema_destroy` | 销毁 Schema。 |
| 组合 | `ktag_feature_schema_add` | 添加 Feature 描述符。 |
| 组合 | `ktag_feature_schema_add_registered` | 从 Registry 添加已注册 Feature。 |
| 组合 | `ktag_feature_schema_remove` | 移除 Feature。 |
| 组合 | `ktag_feature_schema_freeze` | 冻结 Schema。 |
| 组合 | `ktag_feature_schema_create_legacy_v1` | 旧 354 值契约工厂。 |
| 查询 | `ktag_feature_schema_is_frozen` | 是否已冻结。 |
| 查询 | `ktag_feature_schema_get_feature_count` | Feature 数量。 |
| 查询 | `ktag_feature_schema_get_value_count` | 值总数量。 |
| 查询 | `ktag_feature_schema_get_hash` | Schema Hash。 |
| 查询 | `ktag_feature_schema_get_name` | Schema 名称。 |
| 查询 | `ktag_feature_schema_get_version` | Schema 版本。 |
| 查询 | `ktag_feature_schema_get_descriptor` | 按下标取描述符。 |
| 查询 | `ktag_feature_schema_find` | 按 Feature ID 查找。 |

#### 4.2.2 `ktag_feature_descriptor`

Feature 描述符是 Schema 的组成单元（版本化结构体）：

| 字段 | 说明 |
|---|---|
| `feature_id` | Feature 唯一标识，如 `"customer.packet_counts"`，最长 255 字节 |
| `display_name` | 展示名，最长 255 字节 |
| `provider_id` | 提供方标识，最长 255 字节 |
| `provider_version` | 提供方版本 |
| `value_type` | 值类型，当前仅有 `KTAG_FEATURE_FLOAT32`（`1`） |
| `dimension` | 该 Feature 输出的 float 值个数 |

#### 4.2.3 `ktag_feature_schema_create`

创建一个可修改的空 Schema。

```c
KTAG_API ktag_feature_schema *
ktag_feature_schema_create(const char *schema_name, uint32_t schema_version);
```

| 参数 | 说明 |
|---|---|
| `schema_name` | Schema 名称，不能为空。 |
| `schema_version` | Schema 版本号。 |

返回值：成功返回新 Schema，失败返回 `NULL`。

使用约束：Feature ID、维度或顺序发生变更时，必须使用新的 Schema 名称/版本并重新训练模型，不能复用旧 Hash。

#### 4.2.4 `ktag_feature_schema_add`

向 Schema 添加一个 Feature 描述符。仅可在 Freeze 前调用。

```c
KTAG_API ktag_status ktag_feature_schema_add(
    ktag_feature_schema *schema, const ktag_feature_descriptor *descriptor);
```

| 参数 | 说明 |
|---|---|
| `schema` | 目标 Schema。 |
| `descriptor` | 版本化描述符，按 1.2.1 初始化。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 添加成功。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | Schema/描述符不合法。 |
| `KTAG_ERROR_INVALID_STATE` | Schema 已冻结。 |
| `KTAG_ERROR_ALREADY_EXISTS` | `feature_id` 已存在。 |
| `KTAG_ERROR_CAPACITY` | 值总数将超出 `uint32_t` 范围。 |
| `KTAG_ERROR_OUT_OF_MEMORY` | 分配失败。 |

#### 4.2.5 `ktag_feature_schema_add_registered`

按 Feature ID 把 Registry 中已注册 Provider 的 Feature 加入 Schema，保证描述符与 Provider 声明一致。

```c
KTAG_API ktag_status ktag_feature_schema_add_registered(
    ktag_feature_schema *schema, const ktag_feature_registry *registry,
    const char *feature_id);
```

| 参数 | 说明 |
|---|---|
| `schema` | 目标 Schema。 |
| `registry` | 已注册目标 Feature 的 Registry。 |
| `feature_id` | 要添加的 Feature ID。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 添加成功。 |
| `KTAG_ERROR_NOT_FOUND` | Registry 中不存在该 Feature。 |
| 其他负数 | Schema 已冻结或参数不合法。 |

#### 4.2.6 `ktag_feature_schema_remove`

按 Feature ID 从 Schema 移除一个 Feature。仅可在 Freeze 前调用。

```c
KTAG_API ktag_status ktag_feature_schema_remove(ktag_feature_schema *schema,
                                                const char *feature_id);
```

| 参数 | 说明 |
|---|---|
| `schema` | 目标 Schema。 |
| `feature_id` | 要移除的 Feature ID。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 移除成功。 |
| `KTAG_ERROR_NOT_FOUND` | Feature 不存在。 |
| `KTAG_ERROR_INVALID_STATE` | Schema 已冻结。 |

#### 4.2.7 `ktag_feature_schema_freeze`

冻结 Schema。冻结后不可再 add/remove，Schema Hash 最终确定。

```c
KTAG_API ktag_status ktag_feature_schema_freeze(ktag_feature_schema *schema);
```

| 参数 | 说明 |
|---|---|
| `schema` | 目标 Schema。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 冻结成功；对已冻结的 Schema 重复调用直接成功。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | `schema` 为 `NULL`。 |
| `KTAG_ERROR_INVALID_STATE` | Schema 为空（未添加任何 Feature）。 |
| `KTAG_ERROR_OUT_OF_MEMORY` | 分配失败。 |

#### 4.2.8 `ktag_feature_schema_destroy`

销毁 Schema 并释放全部关联内存。

```c
KTAG_API void ktag_feature_schema_destroy(ktag_feature_schema *schema);
```

| 参数 | 说明 |
|---|---|
| `schema` | 待销毁的 Schema；可为 `NULL`。 |

返回值：无。

#### 4.2.9 Schema 查询 API

以下只读查询接口均不对 Schema 做修改，可在任意时刻调用：

```c
KTAG_API uint32_t ktag_feature_schema_is_frozen(const ktag_feature_schema *schema);
KTAG_API uint32_t ktag_feature_schema_get_feature_count(const ktag_feature_schema *schema);
KTAG_API uint32_t ktag_feature_schema_get_value_count(const ktag_feature_schema *schema);
KTAG_API uint64_t ktag_feature_schema_get_hash(const ktag_feature_schema *schema);
KTAG_API const char *ktag_feature_schema_get_name(const ktag_feature_schema *schema);
KTAG_API uint32_t ktag_feature_schema_get_version(const ktag_feature_schema *schema);
```

| API | 返回值 |
|---|---|
| `ktag_feature_schema_is_frozen` | 非 0 表示已冻结。 |
| `ktag_feature_schema_get_feature_count` | Schema 中 Feature 描述符个数。 |
| `ktag_feature_schema_get_value_count` | 全部 Feature 的 `dimension` 之和，即每条记录的 float 值个数。 |
| `ktag_feature_schema_get_hash` | Schema Hash。仅 Freeze 后有效；未冻结时返回 `0`。 |
| `ktag_feature_schema_get_name` | Schema 名称字符串，由 Schema 持有。 |
| `ktag_feature_schema_get_version` | Schema 版本号。 |

#### 4.2.10 `ktag_feature_schema_get_descriptor`

按下标取出 Feature 描述符及其在值数组中的偏移。

```c
KTAG_API ktag_status ktag_feature_schema_get_descriptor(
    const ktag_feature_schema *schema, uint32_t index,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset);
```

| 参数 | 说明 |
|---|---|
| `schema` | 目标 Schema，必须已冻结。 |
| `index` | Feature 下标，`0 .. feature_count-1`。 |
| `descriptor` | 输出参数。版本化结构体。其中的字符串由 Schema 持有，在 Schema 销毁前有效。 |
| `value_offset` | 输出参数。该 Feature 首个值在记录值数组中的偏移。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 取得描述符。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | `index` 越界、Schema 未冻结或参数为空。 |

#### 4.2.11 `ktag_feature_schema_find`

按 Feature ID 查找描述符和值偏移。

```c
KTAG_API ktag_status ktag_feature_schema_find(
    const ktag_feature_schema *schema, const char *feature_id,
    ktag_feature_descriptor *descriptor, uint32_t *value_offset);
```

| 参数 | 说明 |
|---|---|
| `schema` | 目标 Schema，必须已冻结。 |
| `feature_id` | Feature ID。 |
| `descriptor` | 输出参数。版本化结构体。 |
| `value_offset` | 输出参数。该 Feature 首个值在记录值数组中的偏移。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 找到并填充输出。 |
| `KTAG_ERROR_NOT_FOUND` | Feature 不存在。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | Schema 未冻结或参数不合法。 |

#### 4.2.12 `ktag_feature_schema_create_legacy_v1`

重构前固定 354 值 CSV/模型契约的 Schema 工厂，返回**已冻结**的 `ktag_legacy_v1` v1 Schema（53 个 Feature、354 个值）。与之配套的 Provider 通过 `ktag_feature_registry_register_legacy_builtins()` 注册。

```c
KTAG_API ktag_feature_schema *ktag_feature_schema_create_legacy_v1(void);
```

| 参数 | 说明 |
|---|---|
| 无 | 该接口无参数。 |

返回值：成功返回冻结 Schema，失败返回 `NULL`。

### 4.3 Registry 与 Provider API

Registry 管理 Provider 注册项；Feature Context 创建时把 Registry 和 Schema 复制进自己的 Provider Pipeline。

#### 4.3.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | `ktag_feature_registry_create` | 创建 Registry。 |
| 生命周期 | `ktag_feature_registry_destroy` | 销毁 Registry。 |
| 注册 | `ktag_feature_registry_register` | 注册 Provider。 |
| 注册 | `ktag_feature_registry_unregister` | 注销 Provider。 |
| 注册 | `ktag_feature_registry_register_legacy_builtins` | 注册旧契约内置 Provider。 |
| 查询 | `ktag_feature_registry_get_provider_count` | 已注册 Provider 数量。 |

注册标志：

| 常量 | 值 | 说明 |
|---|---:|---|
| `KTAG_FEATURE_REGISTER_DEFAULT` | `0` | 常规注册；Provider ID 冲突时返回 `KTAG_ERROR_ALREADY_EXISTS`。 |
| `KTAG_FEATURE_REGISTER_REPLACE` | `1 << 0` | 替换同 ID 的旧 Provider。替换前必须确保旧 Provider 未被运行中的 Context 使用。 |

#### 4.3.2 `ktag_feature_registry_create` / `ktag_feature_registry_destroy`

```c
KTAG_API ktag_feature_registry *ktag_feature_registry_create(void);
KTAG_API void ktag_feature_registry_destroy(ktag_feature_registry *registry);
```

| API | 说明 |
|---|---|
| `ktag_feature_registry_create` | 创建空 Registry；失败返回 `NULL`。 |
| `ktag_feature_registry_destroy` | 销毁 Registry；`NULL` 安全。 |

使用约束：注册数据和描述符在注册时被**深拷贝**，但 `user_data` 指针被**借用**，必须保持有效直到由该 Registry 创建的全部 Pipeline（即全部 Feature Context）销毁。

#### 4.3.3 `ktag_feature_registry_register`

注册一个 Feature Provider。

```c
KTAG_API ktag_status ktag_feature_registry_register(
    ktag_feature_registry *registry,
    const ktag_feature_provider_registration *registration, uint32_t flags);
```

| 参数 | 说明 |
|---|---|
| `registry` | 目标 Registry。 |
| `registration` | 版本化注册结构体，字段见下。 |
| `flags` | `KTAG_FEATURE_REGISTER_DEFAULT` 或 `KTAG_FEATURE_REGISTER_REPLACE`。 |

`ktag_feature_provider_registration` 字段：

| 字段 | 说明 |
|---|---|
| `provider_id` | Provider 标识，最长 255 字节 |
| `provider_version` | Provider 版本 |
| `features` | Feature 描述符数组 |
| `feature_count` | 描述符数量 |
| `callbacks` | 回调集合，见 4.4.1 |
| `callback_error_policy` | 回调失败策略，见下 |
| `user_data` | 原样传给各回调的用户数据，借用语义 |

回调失败策略：

| 策略 | 说明 |
|---|---|
| `KTAG_FEATURE_CALLBACK_ERROR_FAIL_FLOW`（`0`） | 单流回调失败时丢弃该流。 |
| `KTAG_FEATURE_CALLBACK_ERROR_FILL_ZERO`（`1`） | 可恢复的失败以 0 填充该 Provider 的输出。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 注册成功。 |
| `KTAG_ERROR_ALREADY_EXISTS` | Provider ID 已存在且未指定 REPLACE。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | 注册信息、回调或描述符不合法（`flow_finish` 回调必需）。 |
| `KTAG_ERROR_OUT_OF_MEMORY` | 深拷贝分配失败。 |

#### 4.3.4 `ktag_feature_registry_unregister`

按 Provider ID 注销注册项。正在运行的 Context 不受影响（它们持有自己的 Pipeline 副本），但新 Context 不再包含该 Provider。

```c
KTAG_API ktag_status ktag_feature_registry_unregister(
    ktag_feature_registry *registry, const char *provider_id);
```

| 参数 | 说明 |
|---|---|
| `registry` | 目标 Registry。 |
| `provider_id` | Provider 标识。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 注销成功。 |
| `KTAG_ERROR_NOT_FOUND` | Provider 不存在。 |

#### 4.3.5 `ktag_feature_registry_get_provider_count`

```c
KTAG_API uint32_t
ktag_feature_registry_get_provider_count(const ktag_feature_registry *registry);
```

| 参数 | 说明 |
|---|---|
| `registry` | 目标 Registry。 |

返回值：已注册 Provider 数量。

#### 4.3.6 `ktag_feature_registry_register_legacy_builtins`

注册 `ktag_feature_schema_create_legacy_v1()` 所需的全部内置 Provider。使用 Legacy V1 Schema 前必须调用。

```c
KTAG_API ktag_status
ktag_feature_registry_register_legacy_builtins(ktag_feature_registry *registry);
```

| 参数 | 说明 |
|---|---|
| `registry` | 目标 Registry。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 注册成功。 |
| 其他负数 | 注册失败。 |

### 4.4 Provider 回调、视图与输出 API

Provider 通过回调参与组流；回调内通过 Flow View、Packet View 读取流与报文信息，通过 Feature Output 写出特征值。

#### 4.4.1 回调结构 `ktag_feature_callbacks`

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
| `provider_create` | 可选 | Feature Context 创建时初始化 Provider 状态。 |
| `flow_create` | 可选 | 新流建立时初始化流状态。 |
| `on_packet` | 可选 | 当前流收到每个可解析报文时。 |
| `flow_finish` | **必需** | 流结束时输出特征；在此调用 `ktag_feature_output_set()`。 |
| `flow_destroy` | 可选 | 流状态销毁时释放流级资源。 |
| `provider_destroy` | 可选 | Provider 状态销毁时释放 Provider 级资源。 |

`ktag_feature_provider_config`（传给 `provider_create`）字段：`provider_id`、`provider_version`、`schema`（借用）、`user_data`。

使用约束：

- Provider 不得把 `ktag_flow_view`、`ktag_packet_view` 或其返回指针保留到回调返回之后。
- `provider_create`/`flow_create` 失败时与回调失败同样受 `callback_error_policy` 支配（可恢复场景下按策略处理）。

#### 4.4.2 `ktag_feature_output_set`

在 `flow_finish` 回调中写入一个 Feature 的输出值。

```c
KTAG_API ktag_status ktag_feature_output_set(ktag_feature_output *output,
                                             const char *feature_id,
                                             const float *values,
                                             uint32_t value_count);
```

| 参数 | 说明 |
|---|---|
| `output` | `flow_finish` 回调传入的输出对象。 |
| `feature_id` | Provider 注册时声明的 Feature ID。 |
| `values` | 值数组，调用期间被复制。 |
| `value_count` | 值个数，必须等于该 Feature 声明的 `dimension`。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 写入成功。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | 参数为空。 |
| `KTAG_ERROR_NOT_FOUND` | `feature_id` 不在 Schema 中。 |
| `KTAG_ERROR_SCHEMA_MISMATCH` | Feature 属于其他 Provider，或 `value_count` 与声明的 `dimension` 不符。 |
| `KTAG_ERROR_INVALID_STATE` | 同一 Feature 已在本流写入过。 |

#### 4.4.3 Flow View 查询 API

Flow View 是 `flow_create`/`on_packet`/`flow_finish` 回调中的只读流视图：

```c
KTAG_API uint64_t ktag_flow_view_get_flow_id(const ktag_flow_view *flow);
KTAG_API ktag_status ktag_flow_view_get_flow_key(const ktag_flow_view *flow, ktag_flow_key *key);
KTAG_API ktag_status ktag_flow_view_get_legacy_tuple(const ktag_flow_view *flow,
    uint32_t *src_ip, uint16_t *src_port, uint32_t *dst_ip, uint16_t *dst_port,
    uint8_t *protocol);
KTAG_API uint32_t ktag_flow_view_get_packet_count(const ktag_flow_view *flow, ktag_direction direction);
KTAG_API uint64_t ktag_flow_view_get_total_bytes(const ktag_flow_view *flow, ktag_direction direction);
KTAG_API ktag_status ktag_flow_view_get_packet_sample(const ktag_flow_view *flow,
    ktag_direction direction, uint32_t index, ktag_flow_packet_sample *sample);
KTAG_API const char *ktag_flow_view_get_domain(const ktag_flow_view *flow, ktag_direction direction);
KTAG_API uint32_t ktag_flow_view_get_rawbow_count(const ktag_flow_view *flow);
KTAG_API const char *ktag_flow_view_get_rawbow_token(const ktag_flow_view *flow, uint32_t index);
```

| API | 说明 |
|---|---|
| `ktag_flow_view_get_flow_id` | 返回流 ID（`uint64_t`）。 |
| `ktag_flow_view_get_flow_key` | 输出规范化 Flow Key；`key` 需初始化版本头。 |
| `ktag_flow_view_get_legacy_tuple` | 输出**首包方向**的原始 IPv4 五元组（网络字节序 `src_ip`/`dst_ip`），供旧逻辑迁移。 |
| `ktag_flow_view_get_packet_count` | 返回指定方向（`KTAG_DIRECTION_FORWARD`/`REVERSE`）的累计包数。 |
| `ktag_flow_view_get_total_bytes` | 返回指定方向的累计字节数。 |
| `ktag_flow_view_get_packet_sample` | 取第 `index` 个包样本到 `sample`（**无版本头**结构体，字段：`header_length`、`payload_length`、`timestamp`）；索引越界返回 `KTAG_ERROR_NOT_FOUND`。 |
| `ktag_flow_view_get_domain` | 返回指定方向端点的域名（来自 Domain Cache/Store），未命中返回 `NULL`。 |
| `ktag_flow_view_get_rawbow_count` | 返回 RawBoW token 数量。 |
| `ktag_flow_view_get_rawbow_token` | 返回第 `index` 个 RawBoW token；越界返回 `NULL`。 |

方向枚举：`KTAG_DIRECTION_FORWARD`（`0`，首包方向）/ `KTAG_DIRECTION_REVERSE`（`1`）。

#### 4.4.4 Packet View 查询 API

Packet View 是 `on_packet` 回调中的只读报文视图：

```c
KTAG_API const uint8_t *ktag_packet_view_get_data(const ktag_packet_view *packet, uint32_t *length);
KTAG_API ktag_status ktag_packet_view_get_timestamp(const ktag_packet_view *packet, ktag_timestamp *timestamp);
KTAG_API ktag_status ktag_packet_view_get_transport(const ktag_packet_view *packet, ktag_packet_transport_view *transport);
KTAG_API const uint8_t *ktag_packet_view_get_payload(const ktag_packet_view *packet, uint32_t *length);
```

| API | 说明 |
|---|---|
| `ktag_packet_view_get_data` | 返回完整报文数据指针，`length` 输出字节数（`length` 可为 `NULL`）。 |
| `ktag_packet_view_get_timestamp` | 输出报文时间戳。 |
| `ktag_packet_view_get_transport` | 输出传输层视图，见下表；`transport` 必须先初始化版本头。 |
| `ktag_packet_view_get_payload` | 返回 L4 载荷指针，`length` 输出字节数；无载荷时长度为 0。 |

`ktag_packet_transport_view` 字段：

| 字段 | 说明 |
|---|---|
| `direction` | 本报文方向（`KTAG_DIRECTION_*`） |
| `network_version` | 4 或 6 |
| `transport_protocol` | 传输层协议号 |
| `tcp_flags` | TCP 标志位 |
| `fragment_state` | `KTAG_IP_FRAGMENT_NONE`/`ATOMIC`/`FIRST`/`NON_INITIAL` |
| `network_header_length` / `transport_header_length` | 头部长度 |
| `ipv6_extension_header_count` / `ipv6_extension_header_bytes` | IPv6 扩展头计数/字节数 |
| `tcp_sequence` / `tcp_acknowledgment` | TCP 序号/确认号 |

### 4.5 Feature Context API

Feature Context 持有组流状态和 Provider Pipeline，是特征提取的执行主体。

#### 4.5.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | `ktag_feature_create` | 创建 Feature Context。 |
| 生命周期 | `ktag_feature_destroy` | 销毁 Feature Context。 |
| 处理 | `ktag_feature_process_packet` | 处理一个报文。 |
| 处理 | `ktag_feature_expire` | 按空闲超时结束流。 |
| 处理 | `ktag_feature_flush` | 输出全部剩余流。 |
| 统计 | `ktag_feature_get_statistics` | 查询运行统计。 |

#### 4.5.2 `ktag_feature_config`

| 字段 | 说明 |
|---|---|
| `schema` | 已冻结 Schema，必填 |
| `registry` | 已注册所需 Provider 的 Registry，必填 |
| `shared_domain_store` | 可选的跨 Context 共享域名权威存储 |
| `flow_capacity` | 活跃流上限；`0` 使用默认值 65536 |
| `tcp_idle_timeout_us` | TCP 空闲超时（微秒）；`0` 使用默认值 300 秒 |
| `udp_idle_timeout_us` | UDP 空闲超时（微秒）；`0` 使用默认值 30 秒 |
| `udp_packet_limit` | UDP 包数上限；`0` 使用默认值 100 |
| `domain_authority_lookup_on_miss` | Cache 未命中时是否回查 Domain Store |
| `lifecycle_mode` | 生命周期模式，见下 |

生命周期模式：

| 模式 | 说明 |
|---|---|
| `KTAG_FEATURE_LIFECYCLE_MODERN`（`0`） | UDP 达到 `udp_packet_limit` 后结束流；`expire()` 按空闲超时结束 TCP/UDP 流。 |
| `KTAG_FEATURE_LIFECYCLE_LEGACY_COMPAT`（`1`） | 保持历史固定向量行为：UDP 只保留前 N 个包，通常由 `flush()` 一次性输出；`expire()` 仅执行 DomainStore 维护，不因超时结束流。 |

TCP 协议关闭处理两种模式一致。训练与部署必须使用相同的 Schema 与生命周期模式。

#### 4.5.3 `ktag_feature_create`

创建 Feature Context。Schema 与 Registry 被复制进 Context 的 Provider Pipeline，创建后修改 Registry 不影响已创建的 Context。

```c
KTAG_API ktag_feature_context *
ktag_feature_create(const ktag_feature_config *config);
```

| 参数 | 说明 |
|---|---|
| `config` | 版本化配置结构体，见 4.5.2。 |

返回值：成功返回新 Context，失败返回 `NULL`。

#### 4.5.4 `ktag_feature_process_packet`

处理一个报文；若因此有流结束，完成的流被写入输出 Batch。

```c
KTAG_API ktag_status ktag_feature_process_packet(ktag_feature_context *context,
                                                 const ktag_packet *packet,
                                                 ktag_feature_batch *output);
```

| 参数 | 说明 |
|---|---|
| `context` | Feature Context。 |
| `packet` | 来自 `ktag_capture_next()` 的报文。 |
| `output` | 输出 Batch。调用前会被清空；Schema Hash 必须与 Context 一致。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 处理成功。输出 Batch 可能新增 0 条或多条记录。 |
| `KTAG_ERROR_SCHEMA_MISMATCH` | 输出 Batch 与 Context 的 Schema Hash 不一致。 |
| `KTAG_ERROR_INVALID_STATE` | 线程归属违规。 |

#### 4.5.5 `ktag_feature_expire`

以当前时间驱动空闲流检查，结束超时流并写入输出 Batch。生产系统应周期性调用。

```c
KTAG_API ktag_status ktag_feature_expire(ktag_feature_context *context,
                                         uint64_t current_time_us,
                                         ktag_feature_batch *output);
```

| 参数 | 说明 |
|---|---|
| `context` | Feature Context。 |
| `current_time_us` | 当前微秒时间戳。 |
| `output` | 输出 Batch，语义同 4.5.4。 |

返回值：同 4.5.4。

#### 4.5.6 `ktag_feature_flush`

结束并输出全部剩余流。PCAP EOF 或停止处理时必须调用，否则尚未结束的流不会输出。

```c
KTAG_API ktag_status ktag_feature_flush(ktag_feature_context *context,
                                        ktag_feature_batch *output);
```

| 参数 | 说明 |
|---|---|
| `context` | Feature Context。 |
| `output` | 输出 Batch，语义同 4.5.4。 |

返回值：同 4.5.4。

#### 4.5.7 `ktag_feature_get_statistics`

查询运行统计。该调用会将 Context 绑定到当前线程（与其他处理接口一致）。

```c
KTAG_API ktag_status ktag_feature_get_statistics(
    const ktag_feature_context *context, ktag_feature_statistics *statistics);
```

| 参数 | 说明 |
|---|---|
| `context` | Feature Context。 |
| `statistics` | 输出参数。版本化结构体。 |

`ktag_feature_statistics` 字段：

| 字段 | 说明 |
|---|---|
| `received_packets` / `received_bytes` | 收到的报文数/字节数 |
| `decoded_packets` / `rejected_packets` | 成功解析/被拒绝的报文数 |
| `created_flows` / `completed_flows` / `emitted_flows` | 建立/完成/输出的流数 |
| `expired_tcp_flows` / `expired_udp_flows` | 因超时结束的 TCP/UDP 流数 |
| `flushed_flows` / `dropped_flows` | Flush 输出/丢弃的流数 |
| `capacity_rejections` | 因容量满被拒绝的报文数 |
| `callback_failures` | Provider 回调失败次数 |
| `domain_expired_entries` | DomainStore 过期条目数 |
| `thread_violations` | 线程归属违规次数 |
| `active_flows` / `peak_active_flows` | 当前/峰值活跃流数 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 查询成功。 |
| 其他负数 | 参数不合法。 |

#### 4.5.8 `ktag_feature_destroy`

销毁 Feature Context。活跃流的回调状态被直接销毁，**不会补发结果**；需要输出时先调用 `ktag_feature_flush()`。

```c
KTAG_API void ktag_feature_destroy(ktag_feature_context *context);
```

| 参数 | 说明 |
|---|---|
| `context` | 待销毁的 Context；可为 `NULL`。 |

返回值：无。

### 4.6 Feature Batch API

Feature Batch 是动态记录数组，承载流完成后的特征记录。记录的值在 `append` 时被复制。

#### 4.6.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | `ktag_feature_batch_create` | 创建 Batch。 |
| 生命周期 | `ktag_feature_batch_destroy` | 销毁 Batch。 |
| 容量 | `ktag_feature_batch_reserve` | 预留容量。 |
| 写入 | `ktag_feature_batch_append` | 追加一条记录。 |
| 写入 | `ktag_feature_batch_clear` | 清空全部记录。 |
| 查询 | `ktag_feature_batch_get_count` | 记录数量。 |
| 查询 | `ktag_feature_batch_get_value_count` | 每条记录的值数量。 |
| 查询 | `ktag_feature_batch_get_schema_hash` | Schema Hash。 |
| 查询 | `ktag_feature_batch_get_record` | 按下标取记录。 |
| 查询 | `ktag_feature_batch_get_record_details` | 按下标取审计详情。 |

#### 4.6.2 记录结构

`ktag_feature_record_metadata`（核心元数据，进入模型输入管线）：

| 字段 | 说明 |
|---|---|
| `flow_id` | 流 ID |
| `flow_key` | 规范化 Flow Key |
| `first_packet_sequence` / `last_packet_sequence` | 首/末包序号 |

`ktag_feature_record`（`get_record` 输出，值指针为**借用**）：

| 字段 | 说明 |
|---|---|
| `schema_hash` | Schema Hash |
| `metadata` | 核心元数据，见上 |
| `values` | 连续 float 值指针，在 Batch 再次写入/清空/销毁前有效 |
| `value_count` | 值数量，等于 Schema value count |

`ktag_feature_record_details`（`get_record_details` 输出，仅供审计/CSV，**不进入模型输入**）：在 `metadata` 基础上增加首包方向的 `source_address`/`destination_address`/`source_port`/`destination_port`、`ipv6_extension_packet_count`、`ip_fragment_first_packet_count`。

#### 4.6.3 `ktag_feature_batch_create` / `ktag_feature_batch_destroy`

```c
KTAG_API ktag_feature_batch *
ktag_feature_batch_create(const ktag_feature_schema *schema,
                          uint32_t initial_capacity);
KTAG_API void ktag_feature_batch_destroy(ktag_feature_batch *batch);
```

| 参数 | 说明 |
|---|---|
| `schema` | 绑定的 Schema，决定每条记录的值数量和 Schema Hash。 |
| `initial_capacity` | 初始记录容量，`0` 使用库默认值。 |

返回值：成功返回新 Batch，失败返回 `NULL`。`destroy` 的 `batch` 可为 `NULL`。

#### 4.6.4 `ktag_feature_batch_reserve`

预留至少 `capacity` 条记录的存储，避免追加时反复扩容。

```c
KTAG_API ktag_status ktag_feature_batch_reserve(ktag_feature_batch *batch,
                                                uint32_t capacity);
```

| 参数 | 说明 |
|---|---|
| `batch` | 目标 Batch。 |
| `capacity` | 需要容纳的记录数。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 预留成功。 |
| `KTAG_ERROR_OUT_OF_MEMORY` | 分配失败。 |

#### 4.6.5 `ktag_feature_batch_append`

追加一条记录，值和元数据被复制进 Batch。

```c
KTAG_API ktag_status ktag_feature_batch_append(
    ktag_feature_batch *batch, const ktag_feature_record_metadata *metadata,
    const float *values, uint32_t value_count);
```

| 参数 | 说明 |
|---|---|
| `batch` | 目标 Batch。 |
| `metadata` | 记录元数据。 |
| `values` | 值数组。 |
| `value_count` | 必须等于 Batch 的 Schema value count。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 追加成功。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | 参数或元数据版本头不合法。 |
| `KTAG_ERROR_SCHEMA_MISMATCH` | `value_count` 与 Batch 的 Schema value count 不符。 |
| `KTAG_ERROR_OUT_OF_MEMORY` | 分配失败。 |

#### 4.6.6 `ktag_feature_batch_clear`

清空全部记录，不改变已分配容量。

```c
KTAG_API void ktag_feature_batch_clear(ktag_feature_batch *batch);
```

| 参数 | 说明 |
|---|---|
| `batch` | 目标 Batch。 |

返回值：无。

#### 4.6.7 Batch 查询 API

```c
KTAG_API uint32_t ktag_feature_batch_get_count(const ktag_feature_batch *batch);
KTAG_API uint32_t ktag_feature_batch_get_value_count(const ktag_feature_batch *batch);
KTAG_API uint64_t ktag_feature_batch_get_schema_hash(const ktag_feature_batch *batch);
```

| API | 返回值 |
|---|---|
| `ktag_feature_batch_get_count` | 当前记录数量。 |
| `ktag_feature_batch_get_value_count` | 每条记录的值数量。 |
| `ktag_feature_batch_get_schema_hash` | 绑定 Schema 的 Hash。 |

#### 4.6.8 `ktag_feature_batch_get_record`

按下标取出只读记录视图。

```c
KTAG_API ktag_status
ktag_feature_batch_get_record(const ktag_feature_batch *batch, uint32_t index,
                              ktag_feature_record *record);
```

| 参数 | 说明 |
|---|---|
| `batch` | 目标 Batch。 |
| `index` | 记录下标，`0 .. count-1`。 |
| `record` | 输出参数。版本化结构体；`values` 为借用指针。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 取得记录。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | `index` 越界或参数为空。 |

#### 4.6.9 `ktag_feature_batch_get_record_details`

按下标取出审计详情（含首包方向五元组等扩展信息）。

```c
KTAG_API ktag_status
ktag_feature_batch_get_record_details(const ktag_feature_batch *batch,
                                      uint32_t index,
                                      ktag_feature_record_details *details);
```

| 参数 | 说明 |
|---|---|
| `batch` | 目标 Batch。 |
| `index` | 记录下标。 |
| `details` | 输出参数。版本化结构体，见 4.6.2。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 取得详情。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | `index` 越界或参数为空。 |

### 4.7 Domain Store API

Domain Store 维护 IP→域名的权威映射，供多个 Feature Context 共享。Store 的变更和统计接口线程安全；Store 引用可在其 Cache 之前释放，对象存活到最后一个已注册 Cache 销毁。

#### 4.7.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | `ktag_domain_store_create` | 创建 Store。 |
| 生命周期 | `ktag_domain_store_destroy` | 释放 Store 引用。 |
| 变更 | `ktag_domain_store_put` | 写入/更新映射。 |
| 变更 | `ktag_domain_store_delete` | 删除映射。 |
| 变更 | `ktag_domain_store_expire` | 发布到期映射的删除事件。 |
| 查询 | `ktag_domain_store_get_version` | 当前发布版本号。 |
| 查询 | `ktag_domain_store_get_stats` | 查询统计。 |

#### 4.7.2 `ktag_domain_store_create` / `ktag_domain_store_destroy`

```c
KTAG_API ktag_domain_store *
ktag_domain_store_create(const ktag_domain_store_config *config);
KTAG_API void ktag_domain_store_destroy(ktag_domain_store *store);
```

`ktag_domain_store_config` 字段：

| 字段 | 说明 |
|---|---|
| `max_entries` | 最大条目数；`0` 使用默认值 10000 |
| `journal_capacity` | 增量同步日志容量；`0` 使用默认值 4096 |

返回值：`create` 成功返回新 Store，失败返回 `NULL`；`destroy` 的 `store` 可为 `NULL`。

#### 4.7.3 `ktag_domain_store_put`

写入或更新一个 IP→域名映射，并发布新版本。

```c
KTAG_API ktag_status ktag_domain_store_put(ktag_domain_store *store,
                                           const ktag_ip_address *address,
                                           const char *domain,
                                           uint64_t expire_at_us,
                                           uint64_t *published_version);
```

| 参数 | 说明 |
|---|---|
| `store` | 目标 Store。 |
| `address` | IP 地址。 |
| `domain` | 域名字符串，最长 255 字节。 |
| `expire_at_us` | 到期时间（微秒）；`0` 表示不自动过期。 |
| `published_version` | 可选输出。本次变更发布后的版本号。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 写入成功。 |
| `KTAG_ERROR_CAPACITY` | 达到 `max_entries` 上限或域名超过 255 字节。 |
| 其他负数 | 参数不合法。 |

#### 4.7.4 `ktag_domain_store_delete`

删除一个映射并发布新版本。

```c
KTAG_API ktag_status ktag_domain_store_delete(ktag_domain_store *store,
                                              const ktag_ip_address *address,
                                              uint64_t *published_version);
```

| 参数 | 说明 |
|---|---|
| `store` | 目标 Store。 |
| `address` | 要删除的 IP 地址。 |
| `published_version` | 可选输出。本次变更发布后的版本号。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 删除成功（不存在时也成功）。 |
| 其他负数 | 参数不合法。 |

#### 4.7.5 `ktag_domain_store_expire`

为 TTL 到达（`expire_at_us` 不晚于 `now_us`）的条目发布 DELETE 变更。

```c
KTAG_API ktag_status ktag_domain_store_expire(ktag_domain_store *store,
                                              uint64_t now_us,
                                              uint32_t *deleted_count);
```

| 参数 | 说明 |
|---|---|
| `store` | 目标 Store。 |
| `now_us` | 当前微秒时间戳。 |
| `deleted_count` | 可选输出。本次删除的条目数。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 检查完成。 |
| 其他负数 | 参数不合法。 |

#### 4.7.6 `ktag_domain_store_get_version`

```c
KTAG_API uint64_t ktag_domain_store_get_version(const ktag_domain_store *store);
```

| 参数 | 说明 |
|---|---|
| `store` | 目标 Store。 |

返回值：当前发布版本号。Cache 通过比较版本号决定是否增量同步。

#### 4.7.7 `ktag_domain_store_get_stats`

```c
KTAG_API ktag_status ktag_domain_store_get_stats(
    const ktag_domain_store *store, ktag_domain_store_stats *stats);
```

`ktag_domain_store_stats` 字段：

| 字段 | 说明 |
|---|---|
| `version` | 当前发布版本号 |
| `entry_count` | 存活条目数 |
| `journal_count` | 增量日志中的变更数 |
| `journal_oldest_version` | 日志中最旧的版本号 |
| `registered_cache_count` | 已注册的 Cache 数量 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 查询成功。 |
| 其他负数 | 参数不合法。 |

### 4.8 Domain Cache API

Domain Cache 是每个 Feature Context 的本地缓存。单线程拥有；查找热路径只做一次 acquire 版本加载和本地 map 访问，无锁。

#### 4.8.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 生命周期 | `ktag_domain_cache_create` | 创建 Cache 并注册到 Store。 |
| 生命周期 | `ktag_domain_cache_destroy` | 销毁 Cache。 |
| 同步 | `ktag_domain_cache_sync` | 从 Store 增量/快照同步。 |
| 查询 | `ktag_domain_cache_lookup` | 查询 IP 对应域名。 |
| 查询 | `ktag_domain_cache_get_applied_version` | 已应用的版本号。 |
| 查询 | `ktag_domain_cache_get_stats` | 查询统计。 |

#### 4.8.2 `ktag_domain_cache_create` / `ktag_domain_cache_destroy`

```c
KTAG_API ktag_domain_cache *
ktag_domain_cache_create(ktag_domain_store *store,
                         const ktag_domain_cache_config *config);
KTAG_API void ktag_domain_cache_destroy(ktag_domain_cache *cache);
```

`ktag_domain_cache_config` 字段：

| 字段 | 说明 |
|---|---|
| `authority_lookup_on_miss` | 非 0 时本地未命中回查权威 Store。 |

返回值：`create` 成功返回新 Cache（已注册到 `store`），失败返回 `NULL`；`destroy` 的 `cache` 可为 `NULL`。

#### 4.8.3 `ktag_domain_cache_sync`

把 Store 自上次同步以来的增量变更应用到本地；增量日志不足时自动执行快照恢复。

```c
KTAG_API ktag_status ktag_domain_cache_sync(ktag_domain_cache *cache);
```

| 参数 | 说明 |
|---|---|
| `cache` | 目标 Cache。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 同步完成。 |
| 其他负数 | 同步失败。 |

#### 4.8.4 `ktag_domain_cache_lookup`

查询 IP 对应的域名。

```c
KTAG_API ktag_status ktag_domain_cache_lookup(ktag_domain_cache *cache,
                                              const ktag_ip_address *address,
                                              uint64_t now_us, char *domain,
                                              uint32_t domain_capacity,
                                              uint32_t *required_capacity);
```

| 参数 | 说明 |
|---|---|
| `cache` | 目标 Cache。 |
| `address` | 要查询的 IP 地址。 |
| `now_us` | 当前微秒时间戳，用于 TTL 判断。 |
| `domain` | 输出缓冲区。 |
| `domain_capacity` | 缓冲区容量（字节）。 |
| `required_capacity` | 可选输出。缓冲区不足时返回**含末尾 NUL** 的所需大小。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 命中，`domain` 已写入。 |
| `KTAG_ERROR_NOT_FOUND` | 未命中（含回查 Store 后仍未命中）。 |
| `KTAG_ERROR_CAPACITY` | 缓冲区不足，按 `required_capacity` 扩容后重试。 |

#### 4.8.5 `ktag_domain_cache_get_applied_version`

```c
KTAG_API uint64_t
ktag_domain_cache_get_applied_version(const ktag_domain_cache *cache);
```

| 参数 | 说明 |
|---|---|
| `cache` | 目标 Cache。 |

返回值：已应用的 Store 版本号。

#### 4.8.6 `ktag_domain_cache_get_stats`

```c
KTAG_API ktag_status ktag_domain_cache_get_stats(
    const ktag_domain_cache *cache, ktag_domain_cache_stats *stats);
```

`ktag_domain_cache_stats` 字段：

| 字段 | 说明 |
|---|---|
| `applied_version` | 已应用版本号 |
| `local_hits` / `local_misses` | 本地命中/未命中次数 |
| `sync_count` | 同步次数 |
| `incremental_changes` / `snapshot_recoveries` | 增量变更/快照恢复次数 |
| `authority_lookups` / `authority_hits` | 回查 Store 次数/命中次数 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 查询成功。 |
| 其他负数 | 参数不合法。 |

### 4.9 最小用例

使用 Legacy V1 契约处理 PCAP：创建 Schema/Registry/Context，逐包处理，EOF 后 flush，并打印每条输出记录的 Flow ID 与值数量。

```c
#include <inttypes.h>
#include <stdio.h>
#include "ktag_capture.h"
#include "ktag_feature.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s INPUT.pcap\n", argv[0]);
        return 1;
    }

    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ktag_feature_registry *registry = ktag_feature_registry_create();
    if (schema == NULL || registry == NULL ||
        ktag_feature_registry_register_legacy_builtins(registry) !=
            KTAG_SUCCESS) {
        fprintf(stderr, "Could not create the Legacy V1 feature contract\n");
        return 1;
    }

    ktag_feature_config config = {0};
    config.struct_size = sizeof(config);
    config.api_version = KTAG_API_VERSION;
    config.schema = schema;
    config.registry = registry;
    config.lifecycle_mode = KTAG_FEATURE_LIFECYCLE_MODERN;
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
            break;
        }
        ktag_packet_release(&packet);
    }
    ktag_feature_flush(feature, batch);

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

    ktag_capture_close(capture);
    ktag_feature_batch_destroy(batch);
    ktag_feature_destroy(feature);
    ktag_feature_registry_destroy(registry);
    ktag_feature_schema_destroy(schema);
    return 0;
}
```

将代码保存为 `feature_example.c`，在仓库根目录执行：

```bash
sh /opt/KTAG/build.sh   # 若尚未构建
cc -std=c11 -I/opt/KTAG/include /opt/KTAG/feature_example.c \
   -L/opt/KTAG/output -lktag_capture -lktag_feature \
   -Wl,-rpath,/opt/KTAG/output -o /opt/KTAG/output/feature_example
/opt/KTAG/output/feature_example sample.pcap
```

正确输出（行数与内容随输入文件变化，Legacy V1 每条记录 354 个值）：

```text
flow=1 values=354
flow=2 values=354
```

自定义 Feature Provider 与动态插件的完整示例见 `examples/feature_provider/custom_feature_provider.c` 和 `examples/feature_plugin/custom_feature_plugin.c`，说明见 `examples/README.md`。

## 5 动态 Feature 插件 ABI（ktag_feature_plugin.h）

Feature Provider 可以编译为独立共享库，由宿主（如 demo 加载器）在运行期通过 `dlopen` 加载。本章定义插件必须满足的 C ABI 契约；宿主负责动态库生命周期、ABI 版本协商和卸载顺序，插件不创建流水线线程。

### 5.1 常量与入口符号

| 常量 | 值 | 说明 |
|---|---:|---|
| `KTAG_FEATURE_PLUGIN_ABI_VERSION` | `1U` | 插件 ABI 版本。宿主与插件必须一致。 |
| `KTAG_FEATURE_PLUGIN_ENTRYPOINT` | `"ktag_feature_plugin_query"` | 插件必须导出的入口符号名。 |
| `KTAG_FEATURE_PLUGIN_EXPORT` | 平台相关 | 导出宏：MSVC 为 `__declspec(dllexport)`，GCC 4+ 为 `__attribute__((visibility("default")))`。 |

插件必须在共享库中导出精确的 C 符号 `ktag_feature_plugin_query`，不能发生 C++ 名称改编（C++ 源码需包裹 `extern "C"`）。

### 5.2 `ktag_feature_plugin_descriptor`

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
| `struct_size` / `api_version` | 宿主调用入口前预先设置；插件按 1.2.1 语义处理。 |
| `plugin_abi_version` | 插件填写的 ABI 版本，必须等于 `KTAG_FEATURE_PLUGIN_ABI_VERSION`。 |
| `reserved` | ABI v1 必须置 0。 |
| `plugin_id` / `plugin_version` | 插件标识与版本字符串，不能为空。 |
| `plugin_create` | 可选。初始化插件状态。 |
| `register_providers` | **必需**。以事务方式向 Registry 注册 Provider；返回失败前应自行撤销本次已注册的 Provider。 |
| `plugin_destroy` | 可选；但提供 `plugin_create` 时**必需**。释放插件状态。 |

### 5.3 `ktag_feature_plugin_query`

每个插件导出的唯一入口。宿主先初始化 `descriptor.struct_size` 和 `api_version`，再调用本函数协商 ABI 并取得描述符。

```c
typedef ktag_status (*ktag_feature_plugin_query_fn)(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor);

KTAG_FEATURE_PLUGIN_EXPORT ktag_status ktag_feature_plugin_query(
    uint32_t host_abi_version,
    ktag_feature_plugin_descriptor *descriptor);
```

| 参数 | 说明 |
|---|---|
| `host_abi_version` | 宿主的 ABI 版本。 |
| `descriptor` | 输出参数。宿主已初始化 `struct_size`/`api_version`；插件校验后填写全部字段。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 协商成功，描述符已填写。 |
| `KTAG_ERROR_NOT_SUPPORTED` | `host_abi_version` 与插件 ABI 不一致。 |
| `KTAG_ERROR_INVALID_ARGUMENT` | `descriptor` 为空或 `struct_size` 不足。 |

### 5.4 生命周期约束

- 插件状态由插件管理到 `plugin_destroy`；Provider 注册数据由 Registry 深拷贝（见 4.3.2）。
- 在 Registry、Schema、Feature Context 或任何回调仍引用插件代码/数据时，**不得卸载**动态库；卸载必须发生在所有 Context 销毁之后。
- 官方 `libktag_proxy_features.so` 是 `ktag_proxy_v1` Schema 的必需插件，该 Schema 需要完整的 68 个 `proxy.*` Feature；加载与配置方式见《[用户指南](user_guide.md)》。

### 5.5 最小用例

以下插件导出一个输出正/反向包数的二维 Feature（节选自 `examples/feature_plugin/custom_feature_plugin.c`，可直接编译为共享库）：

```c
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

将代码保存为 `custom_plugin.c`，编译为共享库（宿主加载方式见 5.4 与 demo 配置）：

```bash
cc -std=c11 -fPIC -shared -I/opt/KTAG/include /opt/KTAG/custom_plugin.c \
   -o /opt/KTAG/output/libcustom_plugin.so
```

插件随 demo 的加载、YAML 配置与卸载顺序说明见 `examples/README.md` 第 3 节。

## 6 Inference API（ktag_inference.h）

Inference 层加载 Schema-bound 模型包，对 Feature Batch 执行批量推理，输出 Prediction Batch。

### 6.1 API 清单

| 分类 | API | 说明 |
|---|---|---|
| 校验 | `ktag_inference_validate_model_package` | 部署前校验模型包与 Schema 的一致性。 |
| 生命周期 | `ktag_inference_create` | 创建 Inference Context。 |
| 生命周期 | `ktag_inference_destroy` | 销毁 Inference Context。 |
| 推理 | `ktag_inference_predict` | 批量推理。 |
| 查询 | `ktag_inference_get_schema_hash` | Context 的 Schema Hash。 |
| 查询 | `ktag_inference_get_input_value_count` | 输入值数量。 |
| 查询 | `ktag_inference_get_model_name` | 模型名。 |
| 查询 | `ktag_inference_get_last_error` | 最近错误详情。 |
| 结果批次 | `ktag_prediction_batch_create` | 创建 Prediction Batch。 |
| 结果批次 | `ktag_prediction_batch_destroy` | 销毁 Prediction Batch。 |
| 结果批次 | `ktag_prediction_batch_reserve` | 预留容量。 |
| 结果批次 | `ktag_prediction_batch_clear` | 清空结果。 |
| 结果批次 | `ktag_prediction_batch_get_count` | 结果数量。 |
| 结果批次 | `ktag_prediction_batch_get_record` | 按下标取结果。 |

### 6.2 模型包结构

模型包是一个目录，至少包含：

```text
model_package_path/
├── manifest.json    # 绑定 Schema Hash、Feature ID/维度/顺序、模型输入、预处理规则
├── model.onnx       # ONNX 模型
└── scaler.json      # 标准化参数（标准差等）
```

Manifest 中的 Schema Hash 必须与运行时 Schema、Feature Batch 三方一致；`minimum_total_packets` 等预处理规则在推理时生效。

### 6.3 `ktag_inference_validate_model_package`

加载 manifest.json 与 scaler 元数据，按 Feature ID 解析全部模型输入，并校验两侧 Schema Hash。**不创建 ONNX Session**，适合作为部署/升级前的预检。

```c
KTAG_API ktag_status ktag_inference_validate_model_package(
    const char *model_package_path, const ktag_feature_schema *feature_schema,
    ktag_model_package_validation *validation);
```

| 参数 | 说明 |
|---|---|
| `model_package_path` | 模型包目录路径。 |
| `feature_schema` | 运行时 Schema。 |
| `validation` | 输出参数。版本化结构体，字段见下。 |

`ktag_model_package_validation` 字段：

| 字段 | 说明 |
|---|---|
| `schema_hash` | 校验通过的 Schema Hash |
| `input_feature_count` | 模型输入 Feature 数 |
| `input_value_count` | 模型输入值数量 |
| `minimum_total_packets` | Manifest 设置的最小总包数阈值（无则 0） |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 校验通过，模型包与 Schema 可安全组合。 |
| `KTAG_ERROR_SCHEMA_MISMATCH` | Schema Hash 或 Feature 解析不一致，不能部署。 |
| 其他负数 | 路径/I/O 错误或参数不合法，调用 `ktag_inference_get_last_error()` 获取详情。 |

### 6.4 `ktag_inference_create`

加载模型包并创建 Inference Context。Schema 仅在创建期间借用；其描述符被解析为 Context 拥有的不可变输入计划。

```c
KTAG_API ktag_inference_context *
ktag_inference_create(const ktag_inference_config *config);
```

`ktag_inference_config` 字段：

| 字段 | 说明 |
|---|---|
| `model_package_path` | 模型包目录路径 |
| `feature_schema` | 与模型包绑定的冻结 Schema |
| `intra_op_threads` | ONNX Runtime intra-op 线程数 |
| `inter_op_threads` | ONNX Runtime inter-op 线程数 |

返回值：成功返回新 Context，失败返回 `NULL`（调用 `ktag_inference_get_last_error()` 获取详情）。

### 6.5 `ktag_inference_predict`

对一个 Feature Batch 执行批量推理。Context 在首次调用后绑定到当前线程。

```c
KTAG_API ktag_status ktag_inference_predict(ktag_inference_context *context,
                                            const ktag_feature_batch *features,
                                            ktag_prediction_batch *predictions);
```

| 参数 | 说明 |
|---|---|
| `context` | Inference Context。 |
| `features` | 输入 Feature Batch。Schema Hash 必须与模型包一致。 |
| `predictions` | 输出 Prediction Batch。调用前会被清空。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_SUCCESS` | 推理完成。被 Manifest 预处理拒绝的记录不产生预测，因此结果数量可能小于输入记录数量。 |
| `KTAG_ERROR_SCHEMA_MISMATCH` | 输入 Batch 的 Schema Hash 与模型包不一致。 |
| `KTAG_ERROR_INVALID_STATE` | 线程归属违规。 |
| 其他负数 | 推理失败，调用 `ktag_inference_get_last_error()` 获取详情。 |

使用约束：若 Manifest 设置 `minimum_total_packets`，低于阈值的记录在预处理阶段被跳过；用结果的 `source_record_index` 回查输入 Batch（见 6.8）。

### 6.6 Inference Context 查询 API

```c
KTAG_API uint64_t ktag_inference_get_schema_hash(const ktag_inference_context *context);
KTAG_API uint32_t ktag_inference_get_input_value_count(const ktag_inference_context *context);
KTAG_API const char *ktag_inference_get_model_name(const ktag_inference_context *context);
KTAG_API const char *ktag_inference_get_last_error(void);
```

| API | 说明 |
|---|---|
| `ktag_inference_get_schema_hash` | 返回 Context 绑定的 Schema Hash。 |
| `ktag_inference_get_input_value_count` | 返回每个模型输入记录的值数量。 |
| `ktag_inference_get_model_name` | 返回模型名，借用至 Context 销毁。 |
| `ktag_inference_get_last_error` | 返回当前线程最近一次 Inference 失败详情；语义同 `ktag_capture_get_last_error()`（见 3.9）。 |

### 6.7 `ktag_inference_destroy`

销毁 Inference Context 并释放 ONNX Runtime 会话等资源。

```c
KTAG_API void ktag_inference_destroy(ktag_inference_context *context);
```

| 参数 | 说明 |
|---|---|
| `context` | 待销毁的 Context；可为 `NULL`。 |

返回值：无。

### 6.8 Prediction Batch API

Prediction Batch 承载推理结果，单线程对象，接口与 Feature Batch 对称：

```c
KTAG_API ktag_prediction_batch *ktag_prediction_batch_create(uint32_t initial_capacity);
KTAG_API void ktag_prediction_batch_destroy(ktag_prediction_batch *batch);
KTAG_API ktag_status ktag_prediction_batch_reserve(ktag_prediction_batch *batch, uint32_t capacity);
KTAG_API void ktag_prediction_batch_clear(ktag_prediction_batch *batch);
KTAG_API uint32_t ktag_prediction_batch_get_count(const ktag_prediction_batch *batch);
KTAG_API ktag_status ktag_prediction_batch_get_record(
    const ktag_prediction_batch *batch, uint32_t index,
    ktag_prediction_record *record);
```

| API | 说明 |
|---|---|
| `ktag_prediction_batch_create` | 创建 Batch；`initial_capacity` 为 `0` 时使用库默认值；失败返回 `NULL`。 |
| `ktag_prediction_batch_destroy` | 销毁 Batch；`NULL` 安全。 |
| `ktag_prediction_batch_reserve` | 预留容量，成功返回 `KTAG_SUCCESS`。 |
| `ktag_prediction_batch_clear` | 清空全部结果。 |
| `ktag_prediction_batch_get_count` | 返回结果数量。 |
| `ktag_prediction_batch_get_record` | 按下标取结果到 `record`（版本化结构体）；索引越界返回 `KTAG_ERROR_INVALID_ARGUMENT`。 |

`ktag_prediction_record` 字段：

| 字段 | 说明 |
|---|---|
| `metadata` | `ktag_prediction_metadata`：`flow_id`、`flow_key`、首/末包序号，来自输入记录 |
| `source_record_index` | 对应输入 Feature Batch 的记录下标，用于回查 |
| `label` | 数字类别标签（`int64_t`）；业务名称由应用维护映射 |
| `confidence` | 模型置信度。`[0,1]` 内为模型概率，`0` 合法且不得抑制或改写标签；`KTAG_INFERENCE_CONFIDENCE_UNAVAILABLE`（`-1.0F`）表示无概率输出但标签仍有效。库不内置 unknown 阈值策略。 |

### 6.9 最小用例

校验模型包、创建三层上下文、处理 PCAP 并打印预测结果。完整可编译版本见 `examples/three_layer/three_layer_example.c`，以下为核心流程：

```c
#include <inttypes.h>
#include <stdio.h>
#include "ktag_capture.h"
#include "ktag_feature.h"
#include "ktag_inference.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s MODEL_PACKAGE INPUT.pcap\n", argv[0]);
        return 1;
    }

    /* 1. Schema 与 Provider（Legacy V1 契约）。 */
    ktag_feature_schema *schema = ktag_feature_schema_create_legacy_v1();
    ktag_feature_registry *registry = ktag_feature_registry_create();
    if (schema == NULL || registry == NULL ||
        ktag_feature_registry_register_legacy_builtins(registry) !=
            KTAG_SUCCESS) {
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
            ktag_inference_predict(inference, features, predictions);
        }
        ktag_packet_release(&packet);
    }

    /* 5. EOF：flush 剩余流并推理。 */
    ktag_feature_flush(feature, features);
    if (ktag_feature_batch_get_count(features) > 0U) {
        ktag_inference_predict(inference, features, predictions);
    }

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

    /* 6. 反向销毁。 */
    ktag_prediction_batch_destroy(predictions);
    ktag_feature_batch_destroy(features);
    ktag_capture_close(capture);
    ktag_inference_destroy(inference);
    ktag_feature_destroy(feature);
    ktag_feature_registry_destroy(registry);
    ktag_feature_schema_destroy(schema);
    return 0;
}
```

构建与运行（与 `examples/three_layer` 一致）：

```bash
sh /opt/KTAG/build.sh   # 若尚未构建
cc -std=c11 -I/opt/KTAG/include /opt/KTAG/inference_example.c \
   -L/opt/KTAG/output -lktag_capture -lktag_feature -lktag_inference \
   -Wl,-rpath,/opt/KTAG/output -o /opt/KTAG/output/inference_example
/opt/KTAG/output/inference_example model_package_dir sample.pcap
```

正确输出（行数与内容随输入变化）：

```text
flow=1 label=2 confidence=0.941372
flow=2 label=0 confidence=0.873210
```

## 7 端到端调用顺序

```text
Capture open → Registry/Schema freeze → Feature Context/Batch
      → 模型包校验 → Inference Context/Prediction Batch
      → next → process_packet →（有 Feature 时）predict
      → EOF → flush →（有 Feature 时）predict
      → 依赖对象的反向销毁
```

处理 EOF 时，先 `ktag_feature_flush()`，再按依赖反向销毁：Prediction Batch → Inference Context → Capture → Feature Context → Feature Batch → Schema → Registry。生产系统还应定期调用 `ktag_feature_expire()`、处理 `KTAG_STATUS_TIMEOUT`、检查状态码并落实队列背压与资源监控。

## 8 历史兼容 API（已弃用）

### 8.1 兼容性开关

`ktag_feature_extract.h`、`ktag_legacy_inference.h` 保留历史固定向量接口的数据布局与处理行为，编译开关 `KTAG_LEGACY_FEATURE_COMPAT_AVAILABLE` 与 `KTAG_LEGACY_INFERENCE_COMPAT_AVAILABLE` 当前均为 `1`。命名统一后不再提供早期名称的头文件、库名、类型和版本查询符号；调用方需要同步更新名称、构建参数和安装路径，并重新编译。全部接口标记 `KTAG_DEPRECATED`，新代码必须使用现代接口。

### 8.2 旧特征提取 API（ktag_feature_extract.h）

返回值约定：`EXTRACTOR_SUCCESS`（`0`）/ `EXTRACTOR_ERROR`（`-1`）。

```c
feature_extractor *extractor_init(int link_type);
feature_extractor *extractor_init_with_domain_store(
    int link_type, ktag_domain_store *domain_store);
void extractor_destroy(feature_extractor *extractor);
int process_packet(feature_extractor *extractor, const unsigned char *packet,
                   uint32_t packet_len, const struct timeval *ts,
                   feature_vector *features, int *has_feature);
int process_packet_with_rawbow(feature_extractor *extractor,
                   const unsigned char *packet, uint32_t packet_len,
                   const struct timeval *ts, feature_vector *features,
                   rawbow *rawbows, int *has_feature);
int extractor_finalize(feature_extractor *extractor,
                       feature_vector_list *features);
int extractor_finalize_with_rawbow(feature_extractor *extractor,
                       feature_vector_list *features, rawbow_list *rawbows);
int extractor_get_statistics(feature_extractor *extractor, statistics *stats);
void extractor_reset_statistics(feature_extractor *extractor);
```

| API | 说明 |
|---|---|
| `extractor_init` | 按链路层类型创建旧提取器。 |
| `extractor_init_with_domain_store` | 创建共享 Domain Store 的旧提取器。 |
| `extractor_destroy` | 销毁提取器。 |
| `process_packet` | 处理一个原始报文；仅当流结束时通过 `has_feature` 输出单条向量。 |
| `process_packet_with_rawbow` | 同上，并额外输出 RawBoW。 |
| `extractor_finalize` / `_with_rawbow` | 输出全部剩余流。 |
| `extractor_get_statistics` | 查询旧统计（`total_packets`/`total_bytes`/`decoded_packets`/`extracted_flows`）。 |
| `extractor_reset_statistics` | 重置统计。 |

数据布局约定：`feature_vector.features[0]` 是 Flow ID，`features[1..354]` 是特征值，`FEATURES_NUM` 为 355。新 Schema 模型包不应继续依赖这一布局。

### 8.3 旧推理 API（ktag_legacy_inference.h）

返回值约定：`INFERENCE_SUCCESS`（`0`）/ `INFERENCE_ERROR`（`-1`）。

```c
inference_engine *inference_init(const inference_config *config);
void inference_destroy(inference_engine *engine);
int inference_predict(inference_engine *engine,
                      const feature_vector_list *features,
                      inference_result *result);
```

| API | 说明 |
|---|---|
| `inference_init` | 按 `config.config_file`（YAML 路径）创建旧推理引擎。 |
| `inference_destroy` | 销毁引擎。 |
| `inference_predict` | 对旧向量列表批量推理。`inference_result.predictions` 数组由调用方预先分配，容量必须覆盖结果数量；`count` 返回实际数量。 |

### 8.4 `KTAG_GetProductVersion`（ktag_core.h）

查询历史产品元数据。

```c
KTAGResult KTAG_GetProductVersion(KTAGProVersion *packageInfo);
```

| 参数 | 说明 |
|---|---|
| `packageInfo` | 输出参数。调用方提供的 `KTAGProVersion` 存储，包含产品、组件与软件的名称、版本和附加信息字段，均为固定长度 `KTAG_VERSION_INFO_LEN` 字符数组。 |

返回值：

| 返回值 | 说明 |
|---|---|
| `KTAG_STS_NO_ERR` | 查询成功。 |
| `KTAG_STS_NULL_PTR_ERR` | 指针为空。 |

### 8.5 迁移对照表

| 历史接口 | 现代替代 |
|---|---|
| `extractor_init` / `extractor_init_with_domain_store` | `ktag_feature_create`（含 `shared_domain_store` 配置） |
| `process_packet` | `ktag_feature_process_packet` |
| `process_packet_with_rawbow` | Provider 回调 + `ktag_flow_view_get_rawbow_*` |
| `extractor_finalize` / `_with_rawbow` | `ktag_feature_flush` |
| `extractor_get_statistics` | `ktag_feature_get_statistics` |
| `extractor_reset_statistics` | 重建 Feature Context |
| `inference_init` / `inference_predict` / `inference_destroy` | `ktag_inference_create` / `ktag_inference_predict` / `ktag_inference_destroy` + 模型包 |
| `KTAG_GetProductVersion` | 无对应现代运行时接口 |

## 9 协议边界与限制

现代 Pipeline 支持以下协议范围，超出边界的流量处理行为如下：

- 支持 IPv4/IPv6 TCP、UDP；IPv6 使用完整 128 位地址作为 Flow Key。
- IPv4/IPv6 不跨地址族合流。
- 不做 IPv4/IPv6 分片重组，不支持 IPv6 Jumbogram，不解析 ESP 加密内容。
- 无法由非首分片构造五元组的报文会被跳过（计入 `rejected_packets`）。
- 加密流量分类基于可观测网络特征和训练数据，应以目标网络的独立数据持续验证和再训练。
