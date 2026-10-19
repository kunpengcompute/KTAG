# 库级基础能力

本目录放置不属于具体 Capture、Feature 或 Inference 业务层的基础实现。

`version.c` 提供产品与组件版本查询，继续编译到现有的
`libktag_appidlib.so`；不新增独立动态库，也不改变公共接口名称。

`packet_decode_internal.cpp` 和 `packet_decode_internal.h` 提供 Capture 与 Feature
共用的有界 L2/L3/L4 解码实现。头文件仅供库内部使用；目录迁移不改变
现有库的组织方式或公共接口。
