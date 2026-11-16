# KTAG PCAP 推理真实标签与分类报告设计

> 状态：代码已按本设计修改，待服务器手动验证；按用户要求未运行本地编译或测试。
>
> 编写日期：2026-10-31。代码基线：`39ebdee`，分支 `dev_bk26_1230`。
>
> 后续实现先读第 1、4、5、7、8、14 节。第 1 节是需求结论；标为“边界约定”的内容是本设计为消除实现歧义而补齐的规则。
>
> 实施要求：直接修改现有功能，不保留旧模型包回退策略或另一套报告入口。新参数和默认标签均要求模型包提供 `class_labels`。

## 1. 最终需求结论

为 `ktag_demo -r ... -p ...` 增加可选参数 `--pcap-labels`，显式指定每个输入 PCAP 的真实类别，修正分类报告把输入文件序号当成真实类别的问题。

本轮实现必须满足：

1. **不传新参数时保留兼容行为**：第一个 PCAP 的假定真实标签是 0，第二个是 1，以此类推；必须打印提示和文件到标签的映射。
2. **传新参数时必须全部指定**：标签数量与 `-p` 数量相同，按各自列表顺序一一对应。
3. **逐个校验类别合法性**：每个标签都必须属于当前模型支持的类别集合，不能仅比较最大值。
4. **允许重复标签**：例如 `2,2,3`，前两个文件的有效流样本合并到类别 2；报告中类别 2 只出现一行。
5. **报告类别仅取本次 PCAP 真实标签的去重集合，按数值升序输出**。例如 `2,1,3,5` 显示 `1,2,3,5`。
6. **不按模型完整类别名单补齐报告行**。模型完整类别名单只用于校验，不用于扩大本次报告范围。
7. **所有有效样本都参与统计**。预测为未选类别的样本仍计入错误，不得因报告没有该类别行而被删除。
8. 模型预测、特征提取、流生命周期、组批、线程调度和最小包数过滤保持现有语义。

这份文档已经取代讨论过程中“报告固定显示模型所有类别”的早期方案，后续不要再实现那个方案。

## 2. 问题背景与已确认的原因

用户日志中模型实际训练了四个类别：

| 模型标签 | 业务类别 |
|---|---|
| 0 | class-03-ssr-auth-aes128-md5 |
| 1 | class-04-ssr-auth-aes128-sha1 |
| 2 | class-05-vmess-websocket-tls |
| 3 | class-06-vmess-xhttp-h2-tls |

用户执行：

```bash
./output/ktag_demo -r tools/demo/proxy_config.yaml \
  -p /data2/proxy_data/class-05-vmess-websocket-tls/sample-01.pcap \
  --pipeline-mode feature_parallel -n 4 \
  -c ./result/prediction.csv
```

日志显示共输出 14892 条特征流记录，过滤 11815 条，保留 3077 条预测；报告却将全部真实标签设为 0，最终显示准确率 0%。结合报告生成逻辑及报告中类别 0、2 的数值，可推断这 3077 条有效样本全部预测为 2。

原因是当前 standalone PCAP 路径：

- 在文件循环中将 `index` 作为 `source_index`；
- 打印 `Label=index`，容易被当作预测结果；
- 生成报告时把 `prediction.source_index` 放进 `y_true`；
- 真正的预测结果则来自 `prediction.record.label`。

只有当 PCAP 恰好按模型类别 0、1、2……各提供一份时，这种默认真值约定才恰好适用。输入单个类别 2 的文件时，它不会自动知道该文件属于类别 2。

## 3. 术语与数据边界

| 名称 | 定义 | 本方案中的约束 |
|---|---|---|
| `source_index` | 本次命令中输入文件的序号，从 0 开始 | 始终保持文件序号，不能改成真实标签 |
| PCAP 真实标签 | 用户声明该文件所代表的类别；未指定时按序号假定 | 同一个文件的所有有效流共享该真实标签 |
| 预测标签 / `record.label` | 模型返回的类别 | 不能被真实标签覆盖、纠正或重新编号 |
| 模型类别集合 | 当前训练模型实际支持的完整类别名单 | 校验用，可能不连续 |
| 报告类别集合 | 本次所有 PCAP 真实标签的去重、升序集合 | 决定报告行和 macro average 的范围 |
| 有效样本 | 已成功产生预测、并具有本次 PCAP 真实标签的流记录 | 统计单位是流，不是报文或 PCAP 文件 |

一个 PCAP 只允许声明一个真实类别。PCAP 内若混合多个业务类别，用单一标签评估本来就不准确；本参数不提供逐流标注功能，也不自动解析目录名。

## 4. 命令行接口

### 4.1 参数格式

唯一新增参数：

```text
--pcap-labels LABEL0,LABEL1,...
```

示例：

```bash
# 一个输入文件，真实类别为 2。
./output/ktag_demo -r tools/demo/proxy_config.yaml \
  -p /data2/proxy_data/class-05-vmess-websocket-tls/sample-01.pcap \
  --pcap-labels 2 \
  --pipeline-mode feature_parallel -n 4 \
  -c ./result/prediction.csv

# 三个文件；前两个文件都属于类别 2。
./output/ktag_demo -r /absolute/path/config.yaml \
  -p /data/a.pcap -p /data/b.pcap -p /data/c.pcap \
  --pcap-labels 2,2,3 \
  -c /absolute/path/result.csv
```

参数名固定为复数 `--pcap-labels`。不增加 `--pcap-label`、`--pcap-lable` 等别名。

标签与 `-p` 的先后顺序对应，而不是按文件名、标签大小或目录顺序对应。参数可以放在全部 `-p` 之前或之后，完成命令行解析后再统一检查数量。

### 4.2 解析规则

边界约定：

- 参数一次命令最多出现一次；重复指定直接报错，不静默覆盖或拼接。
- 每个标签为十进制非负整数，第一版范围 `0..INT_MAX`，与现有 C++ 分类报告的 `int` 类型一致。
- 允许 0、前导零及 token 两端空白；前导零按十进制解析，例如 `02` 等于 2。
- 允许 shell 引号包围的列表，例如 `--pcap-labels '2, 1,3'`。
- 拒绝空列表、空 token、首尾逗号、连续逗号、负数、正号、小数、十六进制、尾随字符和整数溢出。
- 使用完整 token 校验；不能用会把 `2abc` 误读为 2 的宽松解析。
- 保留原始标签顺序用于文件映射；只有构造报告类别集合时才排序、去重。

### 4.3 可使用的模式

本轮支持 standalone 的 `-r + -p`：现代四种 Pipeline 拓扑和已有 legacy 调度均应采用相同标签及报告规则，不能只修复日志中使用的 `feature_parallel`。

`--pcap-labels` 在以下情况下直接报参数错误：

- 非 `-r`，例如训练 `-t`、评估 `-e`、特征提取 `-f`；
- 没有 `-p`；
- 同时存在 `-i`，避免把 PCAP 标签错误应用到优先运行的网口输入；
- UB 分布式 Frontend 或 Backend。

UB 已有 `pipeline.distributed.ub_ring.ground_truth_labels` 及独立传输语义。本轮不修改 UB 报文格式，不把新 CLI 参数与 UB 标签配置混用。

本轮也不改变网口 `-i` 的报告行为。该路径当前仍存在以占位标签生成报告的历史逻辑，应作为后续独立问题处理；不得把它当作本方案已经提供真实标签的评估。

### 4.4 未指定参数时的兼容行为

N 个 PCAP 自动形成：

```text
source_labels = [0, 1, ..., N-1]
```

在开始处理前打印：

```text
WARNING: 未指定 --pcap-labels，按 PCAP 输入顺序假定真实标签；
         请确认该顺序与模型类别一致。
PCAP[0]: /data/a.pcap -> ground_truth_label=0 (source=pcap_order)
PCAP[1]: /data/b.pcap -> ground_truth_label=1 (source=pcap_order)
报告类别: [0,1]
```

显式指定时打印同样的映射，但来源写为 `source=explicit`。运行中的文件提示应区分 `source_index` 和 `ground_truth_label`，不再只打印容易误解的 `(Label=0)`。

自动分配的标签在模型类别元数据可用时，也必须通过同一套合法性校验。

## 5. 模型类别名单与标签校验

### 5.1 为什么需要模型类别名单

模型类别可能是 `{0,2,5}`，并非一定为 `0..最大值`。例如输入标签 1，虽然小于最大值 5，仍应拒绝。

也不能用下列内容替代模型实际类别：

- 当前命令传了几个 PCAP；
- 当前 YAML 的训练分组数；
- 目录名中的 class 编号；
- 某次推理恰好输出过哪些标签；
- ONNX 概率输出的列数。

YAML 可能在训练后被修改，单次预测也未必覆盖所有模型类别。

### 5.2 模型包扩展

当前 `manifest.json` 没有完整类别字段。建议新增可选字段：

```json
"class_labels": [0, 1, 2, 3, 4, 5]
```

导出要求：

1. 正常 `-t` 训练流程从刚训练完成的 `model.classes_` 获取实际类别。
2. 转换为普通整数，检查范围、非空和唯一性，按数值升序保存。
3. 不从 `range(len(training_data_paths))` 猜测，避免过滤后某类没有实际训练样本等情况造成声明与模型不一致。
4. 模型、scaler、manifest 必须来自对应的同一次训练/导出。

读取要求：字段存在时必须是非空整数数组，禁止 bool、浮点、字符串、负数、溢出和重复值；可以接受乱序数组，但读取后规范化为升序集合。字段非法应明确报模型包元数据错误，不能降级成“字段不存在”。

本轮可在 Demo 层读取该字段，使用与实际推理相同的模型包路径；不必新增公共 Inference API，也不必为了报表元数据修改 ONNX 执行流程。现有核心模型包读取器忽略未知字段，因此增加此可选字段不会改变原有预测接口。

该字段是随模型包发布的契约声明，可信来源是对应训练模型的导出。本轮不实现任意 ONNX 图类别提取，也不声称能识别人为伪造的 manifest 与 ONNX 不一致情况。

### 5.3 统一模型包要求

根据实施前的最新指示，不保留旧包回退策略。standalone PCAP 推理无论是否传入 `--pcap-labels`，模型包都必须有合法的 `class_labels`。

缺少字段、空数组、非法类型、重复类别等情况均在读包前报错。服务器使用新版本执行一次 `-t`，即可正常导出这项信息；不能根据 YAML 分组数自行猜测并填入。

`create_legacy_package.py` 当前只转换 ONNX 和 scaler，不作为这次正常训练路径的替代导出入口。未提供类别信息的历史包将不能直接用于本功能。

### 5.4 校验时机与错误

在读取 PCAP、执行推理和创建输出结果文件前完成：

1. 参数模式及列表语法检查；
2. 标签数量与 PCAP 数量检查；
3. 解析实际模型包位置及类别元数据；
4. 按文件顺序逐个检查 `label in model_class_labels`。

错误示例：

```text
错误: --pcap-labels 有 2 个标签，但本次指定了 3 个 PCAP，必须一一对应。
错误: PCAP[2] 的真实标签 6 不属于当前模型；模型支持 [0,1,2,3,4,5]。
错误: 当前模型包缺少 class_labels，无法校验 --pcap-labels；请从对应训练模型重新导出类别元数据。
```

失败返回非零退出码，不自动忽略无效标签、不截断列表、不修改预测值。

## 6. 数据流与最小改动方式

### 6.1 推荐内部数据结构

在 Demo 层维护一个轻量的标签计划，示意：

```cpp
struct ktag_demo_pcap_label_plan {
    bool explicit_labels = false;
    std::vector<int> source_labels;       // 每个 PCAP 的标签，保留输入顺序。
    std::vector<int> report_labels;       // source_labels 去重后数值升序。
    std::vector<int> model_labels;        // 只用于校验。
    std::string description;             // 文件映射和统计口径。
};
```

上面是建议结构名，不要求逐字照抄；三组列表的语义必须保留，不能共用一个列表反复排序。

CLI 的 `CommandLineArgs` 当前使用 `memset` 初始化，属于 POD 风格结构。只在其中保存参数原始字符串指针及“是否已指定”标记，**不要直接向这个结构塞入 `std::vector`**。在参数收集结束后的 C++ 层构造标签计划。

声明集中在 `pcap_labels.h`，实现在既有 `utils.cpp`，所有调度入口使用同一套解析、类别检查和计划生成逻辑，不增加构建依赖。

### 6.2 modern Pipeline

- 文件分发和 `source_index` 继续使用 0、1、2……。
- Demo 配置中仅保存 CLI 标签字符串及是否指定的标记；Pipeline 开始时构造本次只读标签计划，不新增同名 YAML 配置。
- 最终生成报告时使用 `source_labels[prediction.source_index]` 作为 `y_true`。
- `y_pred` 始终取 `prediction.record.label`。
- 报告显式传入 `report_labels`，不再使用观测到的真值和预测值的并集决定行数。
- 访问映射时检查下标，内部记录引用不存在的文件应报内部错误，不能越界访问。

这一实现不需要把真实标签写进 Feature Batch，不需要修改模型输入，也不需要给公共 `ktag_prediction_record` 增加字段。

### 6.3 legacy 调度

`online_inference_mode` 目前把文件循环中的 `i` 传给 `run_source`，最终加入 `output.labels`。改为从同一标签计划取真实标签，并把报告类别集合传到报告生成处。

直接修改现有 `online_inference_mode` 的参数和调用方，传入标签字符串；函数内部复用同一标签计划构造器和同一报告函数。不保留旧签名包装，不增加第二套标签策略。

新标签参数本身不能强制启用新 Pipeline。不能将它加入 `overrides.force_enable`，从而悄悄改变用户原有调度方式。

### 6.4 输出文件兼容性

本轮保持预测 CSV/JSON 的既有字段和意义：

- `source_index` 仍是文件序号；
- `label` 仍是模型预测值；
- `confidence` 不变。

不为这次功能强制新增逐流输出列。文件到真实标签的映射在控制台明确打印；分类报告保存时也应附带本次标签来源、映射和报告类别范围，便于离开控制台后核对。

## 7. Classification Report 统计规则

### 7.1 类别集合

```text
L = sort_numeric(unique(source_labels))
```

| 传入的真实标签列表 | 报告行顺序 |
|---|---|
| `2` | `2` |
| `2,1,3,5` | `1,2,3,5` |
| `2,2,3` | `2,3` |
| `10,2` | `2,10`，不能按字符串排成 `10,2` |

报告行集合不能取模型完整类别，不能取预测类别集合，也不能取两者与真实标签的并集。

### 7.2 样本集合

保留全部成功产生预测且具有本次真实标签的流：

```text
y_true[j] = source_labels[prediction[j].source_index]
y_pred[j] = prediction[j].record.label
```

仍然受原有最小包数过滤约束；被过滤、未产生预测的流不补造预测，也不进入本次 classification metrics 的分母。

同一标签对应多个文件时，先合并它们的流样本再统计，不能先算每个文件的指标再简单平均。

### 7.3 每类指标

对每一个 `c in L`，扫描完整有效样本集合：

```text
TP(c) = count(y_true == c and y_pred == c)
FP(c) = count(y_true != c and y_pred == c)
FN(c) = count(y_true == c and y_pred != c)
support(c) = count(y_true == c)

precision(c) = TP / (TP + FP)
recall(c) = TP / (TP + FN)
F1(c) = 2 * precision * recall / (precision + recall)
```

分母为 0 时，相应 precision、recall 或 F1 输出 0，与当前报告的零除规则一致。

### 7.4 整体与平均指标

```text
N = 所有有效样本数
accuracy = count(y_true == y_pred) / N
macro_metric = sum(metric(c), c in L) / |L|
weighted_metric = sum(metric(c) * support(c), c in L) / N
```

由于所有真实标签都在 L 中，正常情况下 `sum(support(c)) == N`。实施时应保留这个一致性检查。

accuracy 是全部有效样本上的整体准确率，不是所显示类别 precision 的平均值，也不是“只保留预测标签属于 L 的样本”后的准确率。

### 7.5 预测落入未选类别

假设模型支持 `[0,1,2,3,4,5]`，本次选择 `[1,2,3,5]`：

```text
y_true = [2,1,3,5]
y_pred = [2,0,3,5]
```

要求：

- 报告仍只显示 `1,2,3,5`，不添加类别 0 的行。
- 第二条样本保留，并计入类别 1 的 FN。
- `accuracy = 3/4 = 75%`。
- 各类别 support 均为 1；类别 1 的 precision/recall/F1 为 0，其余三类均为 1。
- macro F1 与 weighted F1 均为 0.75。
- 可以补充一行“预测落入报告范围外的有效样本数: 1”，但不能据此丢弃样本。

边界约定：如果预测标签不属于模型元数据声明的任何类别，这是模型包契约不一致，应该报告错误；它与“属于模型但不在本次报告类别中”不是同一种情况。

## 8. 空样本、过滤与覆盖范围

### 8.1 某个已选类别没有有效样本

边界约定：报告行集合取自**本次选定的 PCAP 标签列表**，不取过滤后的 `unique(y_true)`。因此某个已选类别的文件全部被过滤时：

- 仍显示该类别一次；
- support、precision、recall、F1 为 0；
- 明确提示该类别本次没有有效样本；
- 它仍计入本次选定类别上的 macro average。

这样严格遵守“输入 `2,1,3,5` 就显示 `1,2,3,5`”的约定。它消除了**未选模型类别**的补零影响，但不能保证**已选且被全部过滤的类别**不影响 macro average。

这是必须在日志/报告中解释的边界；未来若要改成只对实际有样本的类别做 macro average，应作为另一项口径变更讨论，不能悄悄修改。

### 8.2 全部文件都没有有效样本

保留原本合法的空预测处理，不计算伪造的 0% 或 100% 准确率：

```text
无有效推理样本，未生成 Classification Report；accuracy=N/A。
选定报告类别: [1,2,3,5]
```

不将“全部被合法预处理规则过滤”本身视为模型执行错误。I/O、模型加载等真正失败仍按原有错误路径返回非零状态。

### 8.3 截断抓包的部分结果

沿用既有部分 PCAP 处理策略和警告，指标仅覆盖实际成功预测的有效流。新参数不把部分结果冒充完整数据，也不改变原有截断文件容错规则。

## 9. 分类报告函数的改动建议

原来的 `calculate_classification_report(y_true, y_pred, count)` 自动取真值和预测类别并集。直接修改现有函数签名和调用方，不保留原算法或新旧入口：

```cpp
ClassificationReport *calculate_classification_report(
    const int *y_true, const int *y_pred, int count,
    const std::vector<int> &report_labels, const std::string &context);
```

统一函数：

1. 验证类别集合非空、唯一、数值升序，样本数组有效；
2. 验证所有真实标签都属于报告类别集合；
3. 复用 `calculate_class_metrics`，但传入明确的报告类别集合；
4. 复用全量样本的 `calculate_accuracy`；
5. 复用平均指标逻辑，类别数采用本次报告类别数。

移除自动取真值/预测并集的旧函数。PCAP 调用方传入选定类别；其他报告调用方也明确传入相应真实标签集合；不会为预测到的额外类别自动补行。

`ClassificationReport` 保留主要数值字段，新增报告上下文和范围外预测计数，不引入新的报表框架。

## 10. 旧模型与训练导出的实施约束

推荐训练导出调用形式示意：

```python
create_pipeline_package(
    config, config_path, scaler, prepared,
    class_labels=model.classes_.tolist(),
)
```

参数名可按现有风格调整。关键约束是实际 `-t` 主流程始终传入所训练模型的类别；使用该函数的单测和辅助调用也要同步考虑。

模型包新增字段不会改变：

- Schema Hash、特征顺序和输入维度；
- scaler 数值；
- 模型输出标签的原始编号；
- ONNX 图结构及推理数值。

不要为了让标签连续而重新编号模型输出。例如模型类别是 `[0,2,5]`，本方案应保留这个集合，而不是改成 `[0,1,2]`。

## 11. 建议代码改动位置

以下位置基于 `39ebdee`，后续行号可能变化，应按函数名定位。

| 文件 | 当前职责/问题 | 计划改动 |
|---|---|---|
| [main.cpp](main.cpp) | CLI 解析、模式选择和调用入口 | 参数、帮助、适用模式及数量校验；构造/传递标签计划 |
| [pipeline/pipeline_config.h](pipeline/pipeline_config.h) | Demo 运行配置 | 承载现代路径的本次只读标签计划，不新增 YAML 配置项 |
| [pipeline/pipeline_runner.cpp](pipeline/pipeline_runner.cpp) | `source_index` 被用于 y_true | 按计划映射真实标签、调整文件提示、使用选定类别报告函数 |
| [pipeline/pipeline_runner.h](pipeline/pipeline_runner.h) | Demo 预测视图/运行入口 | 仅按传参需要调整，保留 `source_index` 含义 |
| [online_inference.cpp](online_inference.cpp)、[online_inference.h](online_inference.h) | legacy 的文件 i 被当作真值 | 传递同一标签计划，覆盖各 legacy 推理模式 |
| [utils.cpp](utils.cpp)、[utils.h](utils.h) | 报告类别由 y_true/y_pred 并集决定 | 直接修改现有报告函数，统一使用显式类别集合 |
| [python/training.py](python/training.py) | 模型包未导出类别名单 | 从模型实际 classes_ 导出 `class_labels` |
| [pcap_labels.h](pcap_labels.h) 和既有 utils.cpp | 标签定义及共享实现 | 集中解析列表、读取模型类别和建立映射 |
| [../../test/common/functest/func_demo_pipeline.cpp](../../test/common/functest/func_demo_pipeline.cpp) 或独立相关测试 | Pipeline/报告测试 | 标签计划、类别集合、样本统计和推理结果不变性验证 |
| [../../test/python/test_schema_training.py](../../test/python/test_schema_training.py) | Schema 训练/导出测试 | 实际类别导出、稀疏编号、字段有效性和兼容性 |
| [../../test/build/functest/Makefile.am](../../test/build/functest/Makefile.am)、[CMakeLists.txt](CMakeLists.txt) | Demo 辅助源码构建 | 仅当新增 .cpp 时将其加入现有构建，不扩大链接依赖 |
| 用户说明/命令帮助 | 当前无新参数说明 | 示例、默认假定真值提示、选定类别 macro 口径 |

## 12. 处理流程伪代码

```text
解析全部 CLI 参数
    若未指定 --pcap-labels 且不属于 standalone -r/-p：保持既有其他模式
    若指定了参数但模式不允许：参数错误退出

对 standalone -r/-p：
    labels = 显式解析的列表，或 [0..pcap_count-1]
    校验列表长度、整数范围
    从与推理相同的模型包读取 class_labels
    字段缺失或非法直接报错
    逐个验证 labels 的成员资格
    report_labels = 数值升序去重(labels)
    打印完整文件映射及标签来源

    正常执行特征提取、预处理和模型推理
    保持每条预测的 source_index 和模型预测 label
    y_true = 用 source_index 查 labels
    y_pred = 原始模型预测 label
    确认预测 label 未超出模型类别集合

    若没有有效样本：报告 N/A，不生成伪造准确率
    否则：
        对 report_labels 中各类，在全量有效样本上计算 TP/FP/FN/support
        计算全量 accuracy、选定类别 macro 和按 support 加权的指标
        保存标签映射、统计口径及报告
```

实现前后，相同模型、配置、PCAP 下的预测 `label/confidence` 应一致；区别只应在真实标签映射、提示和报告统计范围。

## 13. 命令与报告示例

### 13.1 复现用户日志中的单文件情况

```bash
./output/ktag_demo -r tools/demo/proxy_config.yaml \
  -p /data2/proxy_data/class-05-vmess-websocket-tls/sample-01.pcap \
  --pcap-labels 2 \
  --pipeline-mode feature_parallel -n 4 \
  -c ./result/prediction.csv
```

前提：模型包已包含对应模型真实导出的 `class_labels`。

若仍产生 3077 条有效预测且全部预测为 2，则预期：

```text
source_index=0, ground_truth_label=2
报告类别: [2]

label  precision  recall  f1-score  support
2      1.0000     1.0000  1.0000    3077

accuracy:     1.0000
macro avg:    1.0000
weighted avg: 1.0000
```

该结果只覆盖 3077 条有效流，不代表全部 14892 条特征记录或所有报文都已完成分类。

### 13.2 乱序真实标签

```text
-p a.pcap -p b.pcap -p c.pcap -p d.pcap
--pcap-labels 2,1,3,5

source_index  文件    ground_truth_label
0             a.pcap  2
1             b.pcap  1
2             c.pcap  3
3             d.pcap  5

报告类别行：1、2、3、5。
模型即使还支持 0、4，也不为它们补行。
```

## 14. 验收测试清单

| 编号 | 输入/条件 | 必须满足的结果 |
|---|---|---|
| C01 | 1 个 PCAP，显式标签 2，模型含 2 | 正常运行，真实标签为 2，source_index 仍为 0 |
| C02 | 4 个 PCAP，`2,1,3,5` | 文件映射保留输入顺序，报告行严格为 `1,2,3,5` |
| C03 | 3 个 PCAP，`2,2,3` | 允许重复，类别 2 合并流样本，报告只含 `2,3` |
| C04 | `10,2`，模型包含两者 | 报告按数值顺序 `2,10` |
| C05 | 3 个 PCAP，仅 2 个标签或提供 4 个 | 读取输入前报数量错误，非零退出，不创建结果文件 |
| C06 | 模型 `{0,2,5}`，标签 1 | 拒绝；证明不是仅检查最大值 |
| C07 | 模型 `0..5`，标签 `2,5,6` | 指出第三个 PCAP 的 6 无效 |
| C08 | 未传参数，N 个 PCAP，模型支持 `0..N-1` | 兼容生成默认标签，打印警告、映射和报告集合 |
| C09 | 默认标签包含模型不支持的数字 | 有类别元数据时仍报错，不特殊放行默认值 |
| C10 | `''`、`,2`、`2,`、`2,,3` | 拒绝空 token |
| C11 | `-1`、`+2`、`2.0`、`0x2`、`2abc`、超过 INT_MAX | 拒绝，不能部分解析 |
| C12 | `'02, 1'` | 解析成 `[2,1]`，报告顺序 `[1,2]` |
| C13 | 重复传两次 `--pcap-labels` | 拒绝，不覆盖或拼接 |
| C14 | `-t/-e/-f/-i/UB` 与新参数组合 | 明确报告不支持该组合 |
| M01 | 新训练模型实际 classes_ 为 `[0,2,5]` | manifest 保存 `[0,2,5]`，不推断连续编号 |
| M02 | class_labels 缺失，显式指定标签 | 报需补齐元数据，不静默跳过合法性检查 |
| M03 | class_labels 缺失，未传新参数 | 同样报错，要求正常导出类别元数据，不静默跳过校验 |
| M04 | 字段空、重复、字符串、bool、负数等 | 报模型包类别元数据错误 |
| R01 | `y_true=[2,1,3,5]`、`y_pred=[2,0,3,5]` | 只显示 `1,2,3,5`，accuracy=0.75，错误样本不被删除 |
| R02 | 不同真实类别被误预测为某个已选类别 | 误预测计入该类别 FP，precision 相应降低 |
| R03 | 多个同标签文件的样本数不同 | 合并原始流计数计算，不能平均各文件指标 |
| R04 | 模型有 6 类，只选 2 类，均预测正确 | 报告只含选定两类，macro/weighted/accuracy 均为 1 |
| R05 | 已选一类全部被最小包数过滤，其他类有预测 | 已选空类保留 support=0、指标 0，并提示 macro 包含该类 |
| R06 | 全部输入没有有效预测 | 不除以 0，不报伪造 0%/100%，打印 N/A |
| R07 | 预测标签属于模型，但不在报告集合中 | 不增加报告行，保留错误对 accuracy/FN 的影响 |
| R08 | 预测标签超出已知模型类别集合 | 报模型包/预测契约不一致，不能忽略 |
| I01 | 同一输入在 single_thread/feature_parallel/full_parallel 等路径运行 | 标签映射和统计规则一致，不受线程完成顺序影响 |
| I02 | legacy 的 `-m` 模式及显式 legacy 调度 | 与 modern 采用同样的标签列表和报告类别集合 |
| I03 | 启用/不启用新参数，对同一批输入比较原始预测 | `label/confidence` 不被真实标签功能改写 |
| I04 | 查看预测 CSV/JSON | 既有字段和含义不变，source_index 不变成真实标签 |
| I05 | 其他既有报告调用方 | 同样传入明确真实类别集合，使用统一报告函数 |

测试优先使用可精确手算的小样本覆盖报告数学规则，再使用现有小型 PCAP/ONNX 夹具验证链路。不要以大规模性能基准替代这些功能断言。

## 15. 实施顺序与提交建议

1. 先实现标签计划及解析/校验单测，冻结默认兼容行为和错误策略。
2. 给正常训练导出流程增加 `class_labels`，补导出测试。
3. 直接修改现有报告入口，用小数组验证统计公式及隐藏预测类别的处理。
4. 接入 standalone modern 和 legacy 的 PCAP 路径，保持 source_index 和预测结果不变。
5. 完成日志、报告元信息和命令帮助，跑对应测试及现有相关回归。

最终可作为一项完整功能提交，或按“模型类别元数据 / 标签与报告功能”拆分；不要求本设计阶段提前提交。不要把用户本地数据路径配置修改混入实现提交。

## 16. 后续实现必须记住的限制

- 当前工作区已修改；服务器需重新构建二进制并用新训练脚本导出模型包。
- 最新需求是**报告只显示本次真实标签集合**，不是全部模型类别，也不是实际预测类别并集。
- 默认按序号假定真值是保留兼容行为，不代表程序已经验证文件真实业务归属。
- 同文件单标签是前提，提供标签不会让模型“强制预测成该类”。
- 模型类别元数据用来校验，报告类别计划用来显示和平均，两者不能混为一谈。
- 校验每个类别的成员资格，不能只检查最大值或类别数量。
- 不论显式或默认标签，缺少类别元数据都直接报错，不保留旧包回退分支。
- 没有显示的预测类别，其错误仍必须保留在全量统计中。
- 报告以成功预测的流为样本单位，被预处理过滤的数据不补造预测。
- 已选类别过滤后无样本仍保留一行，macro 包含它；全部无样本则 N/A。
- 本轮不改 UB 传输格式、不重写推理 API、不改特征契约、不调整识别模型和训练超参数。
