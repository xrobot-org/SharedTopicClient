# SharedTopicClient

SharedTopicClient 是一个多 Topic 数据共享与串口转发客户端模块。它用于通过 UART 将多个 Topic 的数据统一打包、发送，实现消息流的串口透明同步转发，适用于分布式系统的多主题数据同步或边缘数据采集。

SharedTopicClient is a client module for multi-topic data sharing and transparent UART forwarding. It subscribes to multiple Topics, packs their updates, and transmits them via UART, enabling efficient and reliable message synchronization over serial connections—ideal for distributed systems or edge data acquisition.

---

## 硬件需求 / Required Hardware

- uart_name

## 构造参数 / Constructor Arguments

- uart_name: 串口设备名 / UART device name (e.g., "uart_cdc")
- slot_count: 共享待发槽位数量。每个槽位的字节数由订阅 Topic 中最大的打包后长度自动计算。
  / Number of shared pending slots. Each slot size is derived from the largest packed subscribed Topic.
- topic_configs: 需要订阅并转发的 Topic 配置列表。每项可以只写 topic 名，也可以写
  `[topic, domain]`。/ Topic configs to subscribe and forward. Each item may be a
  topic name or `[topic, domain]`.

## 运行方式

`SharedTopicClient` 不创建发送线程。模块注册 Topic callback；每次 Topic 发布时，
callback 内先完成打包并写入全局共享槽位池，然后尝试抢占当前 UART 发送服务：

- 所有 Topic 共用一组固定槽位，packet 按进入槽位池的顺序发送。
- 发送空闲：`TxService()` 从共享槽位池取最早 packet 写入 UART。
- 发送忙碌：新 packet 继续进入共享槽位池，不按 Topic 覆盖旧 packet。
- 槽位池满：丢弃当前新 packet，并记录丢包计数；这是全局背压，不是同 Topic 覆盖。

写完成回调会继续调用 `TxService()`，直到 UART 写队列再次满或共享槽位池为空。
这样不会让多个 Topic callback 同时推进 UART 服务，也不会为转发链路额外引入发送线程。

## Timestamp

`SharedTopicClient` 转发 Topic 时会保留 libxr message envelope timestamp：

1. 本地 Topic callback 收到 `(timestamp, payload)`。
2. `Topic::PackData(topic_crc, buffer, timestamp, payload)` 写入串口包。
3. 对端 `SharedTopic` 解析后用同一个 timestamp 发布到对端 domain。

因此同步类 topic 不需要在 payload 里重复携带时间戳；payload 只保留业务字段即可。

## 依赖 / Depends

- 无（No dependencies）
