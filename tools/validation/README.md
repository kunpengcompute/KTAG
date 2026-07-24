# Pipeline 兼容性自动验收

`validate_pipeline_modes.py` 使用相同配置和 PCAP，依次运行旧 `legacy` 路径与
四种新管线模式。验收条件为：

1. 五种模式的保留/过滤样本数完全一致；
2. 控制台存在 accuracy 时，五种模式 accuracy 完全一致；
3. 四种新模式按 `source_index + flow_key + packet sequence + label` 归一化
   后完全一致，不依赖输出顺序和线程本地 `flow_id`。

服务器示例：

```bash
cd /home/s00951452/KTAG-chonggou7-13
python3 tools/validation/validate_pipeline_modes.py \
  --demo output/kdadk_demo \
  --config tools/demo/config.yaml \
  --pcap /data2/ktag_data/bilibili/bilibili_20250721_141827_21600_android_20.pcap \
  --pcap /data2/ktag_data/wenxiaoyan/wenxiaoyan_20250804_205038_144000_android_20.pcap \
  --feature-threads 4 \
  --inference-threads 2
```

结果写入 `phase8_validation/`。任何一个命令失败或结果不一致，脚本都返回非
0，适合接入 CI。
