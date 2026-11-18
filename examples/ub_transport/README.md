# 只使用 UB 传输任意字节：最小 1:1 示例

本示例让两个节点通过现有的 `rte_ub_ring` 传递二进制数据，不进行收包、组流、特征提取或模型推理。发送端读一个文件，接收端把收到的字节复制到本地缓冲区，再写入文件。文件只是方便验收的输入输出；客户可以替换成自己的数据来源和处理函数。

配置固定为一个 Provider（发送端）、一个 User（接收端）、一组双向队列和一个往返复用的 UB 缓冲区。它展示接口用法和缓冲区所有权，不用于测吞吐。

## 1. 先明确传输模型

这里复用的是 KFlowLens 底层的 C++ 封装 `ktag_demo_ub_ring_transport`，没有复用特征专用的 Sender 或特征帧协议。类名中的 `demo` 是内部历史命名。

```text
Provider / 发送端                       User / 接收端
                                      allocate_buffer()
                       <----- CREDIT：授予一个可写 UB 缓冲区
读取客户字节，填入该 UB 缓冲区
send_burst(DATA)        ----->         receive_copy()：复制到本地缓冲区
                                      on_payload()：处理本地副本
                       <----- CREDIT：允许复用刚才的 UB 缓冲区
发送后续 DATA ……
send_burst(END)         ----->         校验总帧数/总字节数，flush 输出
                       <----- ACK
校验 ACK，return_buffer()
```

“发送端直接把任意指针交给队列、接收端读取任意远端地址”不符合这套接口。发送端必须写入 UB 分配并授权使用的缓冲区；队列元素携带 Provider 标识和缓冲区偏移，封装将其解析为当前进程可访问的 UB 缓冲区。User 的 `receive_copy()` 随后将内容复制到调用者持有的本地内存。具体跨节点内存访问由部署的 UB SDK 完成，应用层不另行发起通用的远端地址读取。

## 2. 编译

需要 Linux、C++17 编译器、CMake、pkg-config，以及现场配套的 DPDK 24.11 或更新版本和 UB 扩展。普通上游 DPDK 不包含 `rte_ub_ring.h`；必须使用已经验证能运行 KFlowLens UB 的 SDK、库和系统服务环境。

**推荐独立构建**，这样不需要 ONNX Runtime、yaml-cpp、模型、Feature 插件或三个 KTAG 业务库。在仓库根目录执行：

```bash
# 若 SDK 不在默认搜索路径，先设置其实际安装目录：
# export PKG_CONFIG_PATH=/path/to/dpdk/lib64/pkgconfig:$PKG_CONFIG_PATH
# export LD_LIBRARY_PATH=/path/to/dpdk/lib64:$LD_LIBRARY_PATH
pkg-config --modversion libdpdk
cmake -S examples/ub_transport -B build/ub-bytes -DCMAKE_BUILD_TYPE=Release
cmake --build build/ub-bytes --parallel 4
./build/ub-bytes/ktag_ub_bytes_example --help
```

独立构建只编译 `main.cpp` 与 `tools/KFlowLens/pipeline/ub_ring_transport.cpp`，需要保留该封装的 `.h` 和同目录的 `ub_node_validation.h`。这是源码级复用；目前没有独立安装的 UB 公共库，也没有 `KTAG::ub` 导出目标。

如果本来就要构建整个 KTAG 工程，也可以在项目正常依赖满足时开启：

```bash
cmake -S . -B build -DKTAG_ENABLE_DPDK_UB=ON -DKTAG_BUILD_EXAMPLES=ON
cmake --build build --target ktag_ub_bytes_example --parallel 4
# 整仓构建的产物：output/ktag_ub_bytes_example
```

默认不启用 UB 时，不构建本示例。

## 3. 在两个节点运行

下面以 Provider 的 `hostname` 为 `computer02`，User 为 `controller` 举例。请替换为两台机器的真实 hostname，并在两端使用完全相同的 `--provider`、`--user` 和 `--shm-prefix`。

当前 SDK 配套封装要求 **Provider hostname 的字典序小于 User hostname**；不满足会在初始化 SDK 前报错。这里 `computer02 < controller` 满足要求。不能仅在命令行虚构一个名称绕过实际 hostname 注册。

先在 Provider 节点启动发送端：

```bash
# /tmp/ub-input.bin 可以换成任意非空或空的二进制文件。
dd if=/dev/urandom of=/tmp/ub-input.bin bs=1M count=2
./build/ub-bytes/ktag_ub_bytes_example \
  --role sender --provider computer02 --user controller \
  --input /tmp/ub-input.bin --shm-prefix ktag_ub_bytes \
  --eal-args 'ub_bytes --no-pci --no-huge --iova-mode=va --lcores=0@(0-3)'
```

随后在 User 节点启动接收端：

```bash
./build/ub-bytes/ktag_ub_bytes_example \
  --role receiver --provider computer02 --user controller \
  --output /tmp/ub-output.bin --shm-prefix ktag_ub_bytes \
  --eal-args 'ub_bytes --no-pci --no-huge --iova-mode=va --lcores=0@(0-3)'
```

`0-3` 必须替换为各自机器允许使用的 CPU 范围；两端可以不同。`--eal-args` 是由示例拆分的参数字符串，**必须包含第一个程序名** `ub_bytes`。它不需要绑定抓包网卡，也不需要 `--dpdk`、PCI 收包端口或 UB ethdev；底层 UB 服务和驱动仍须正常运行。

本示例约定 Provider 先启动，随后启动 User，不扩展任意启动顺序、断线重连或跨次运行恢复。两端的应用层轮询默认等待 60 秒，可用 `--timeout-ms 120000` 调整；这个参数不控制 SDK 初始化函数内部的阻塞时间。

`--output` 会覆盖已有文件；失败时可能留下部分文件，不能将其视为传输成功。若 SDK 报共享内存已存在，先确认上一轮两端进程均已退出，再按现场 UB 操作规范清理；本示例不会自动删除共享内存。也可以为新的一轮在两端指定同一个全新 prefix，避免与业务使用的 `ktag_feature` 混用。

## 4. 验收

正常结束时，两端都应显示 `completed`，且 `messages`、`bytes` 一致；发送端还会显示 `final ACK verified`。ACK 只在接收端已处理所有字节、核对总数并成功 flush 输出后发出，flush 不等于磁盘 `fsync`。

分别在发送端和接收端执行，并人工比对哈希值：

```bash
# Provider
sha256sum /tmp/ub-input.bin
# User
sha256sum /tmp/ub-output.bin
```

建议至少验证空文件、含 `0x00` 的二进制文件，以及大于 4064 字节的文件。单帧 UB payload 容量为 4096 字节，其中示例头占 32 字节，客户数据最多 4064 字节；大文件会被拆成多帧并按序重组。协议还校验帧标识、版本、长度、payload CRC32、序号和最终总数。它是本示例协议，与 KFlowLens 的特征帧协议不互通。

开发时还可在仓库根目录运行不需要 SDK 的应用层回归：

```bash
python3 test/common/ub_bytes_example_check.py --cxx c++
```

它用内存模拟传输封装，执行真实示例代码，检查空文件、分片边界、2 MiB 二进制内容、所有权、CRC/序号/总数错误、丢失 ACK、超时和中断。它不能代替现场 SDK 的两节点验收。

## 5. 客户如何接入自己的数据

从 `main.cpp` 的以下位置开始：

| 位置 | 当前做法 | 客户替换点 |
| --- | --- | --- |
| `run_sender()` | 从文件读最多 4064 字节 | 从自己的对象、消息或缓存取得序列化字节 |
| `channel::credit()` → `channel::send()` | 获得 CREDIT，编码到 UB 缓冲区后发送 | 保留缓冲区流控，修改业务 payload 和分片规则 |
| `channel::receive()` | `receive_copy()` 复制完整帧到本地 `local` | 继续使用自己持有的本地缓存，按约定解析长度 |
| `on_payload()` | 把本地有效字节写入文件 | 放入客户自己的缓存，或调用客户的处理函数 |

发送 C++ 结构体时不能原样复制包含指针、`std::string` 或虚函数的对象；应先序列化，约定长度、字段和字节序。示例对 payload 内容不作解释，但文件分片不保留客户原始消息边界；需要消息边界时应自行在 payload 中编码。

缓冲区使用规则：

- `allocate_buffer()` 在 User 侧执行，分配的是对应 Provider 数据区中的 UB 缓冲区，并非接收端的普通业务缓存。
- Provider 收到 CREDIT 后才能写该 UB 缓冲区；`send_burst()` 成功后所有权交给对端，不能继续读写或释放，直到下一次收到 CREDIT。
- User 收到 DATA 后先 `receive_copy()`，业务仅处理本地副本；`receipt.ub_buffer` 只用于资源管理和发回 CREDIT/ACK。
- 示例为便于理解，在 `on_payload()` 完成后才返回 CREDIT。返回后 Provider 可以覆盖 UB 缓冲区，但已复制的本地字节不受影响。
- `local` 在下一次接收时会被覆盖。如果客户异步消费，应另行复制到客户缓存或转交独占的本地缓冲区，不能长期保留当前 `data` 指针。
- 最后 ACK 的缓冲区由 Provider `return_buffer()` 交回 SDK；异常路径根据当前持有的所有权发送 ERROR 或归还缓冲区。没有超时重传、重连或业务事务保证。

底层接口的位置：

| 文件 | 内容 |
| --- | --- |
| `examples/ub_transport/main.cpp` | 参数、字节帧、CREDIT/DATA/END/ACK、文件输入和本地消费 |
| `examples/ub_transport/CMakeLists.txt` | 独立构建和整仓构建入口 |
| `tools/KFlowLens/pipeline/ub_ring_transport.h` | 配置、队列映射、分配、收发、复制和回收接口 |
| `tools/KFlowLens/pipeline/ub_ring_transport.cpp` | EAL/UB 初始化和释放、队列元素解析、拷贝及边界保护 |
| `tools/KFlowLens/pipeline/ub_node_validation.h` | 节点 hostname 字典序检查 |

为了让同一传输封装可供字节示例复用，传输层仅要求 payload 容量为正；特征帧所需容量继续由业务流水线按其帧格式校验。
