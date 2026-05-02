# SharedTopicClient

SharedTopicClient 是一个多 Topic 数据共享与串口转发客户端模块。它用于通过 UART 将多个 Topic 的数据统一打包、发送，实现消息流的串口透明同步转发，适用于分布式系统的多主题数据同步或边缘数据采集。

SharedTopicClient is a client module for multi-topic data sharing and transparent UART forwarding. It subscribes to multiple Topics, packs their updates, and transmits them via UART, enabling efficient and reliable message synchronization over serial connections—ideal for distributed systems or edge data acquisition.

---

## 硬件需求 / Required Hardware

- uart_name

## 构造参数 / Constructor Arguments

- uart_name: 串口设备名 / UART device name (e.g., "uart_cdc")
- task_stack_depth: 任务堆栈大小 / Task stack depth (e.g., 512)
- buffer_size: 发送缓冲区字节数 / TX buffer size (e.g., 256)
- topic_configs: 需要订阅并转发的 Topic 配置列表。每项可以只写 topic 名，也可以写
  `[topic, domain]`。/ Topic configs to subscribe and forward. Each item may be a
  topic name or `[topic, domain]`.

## Timestamp

`SharedTopicClient` 转发 Topic 时会保留 libxr message envelope timestamp：

1. 本地 Topic callback 收到 `(timestamp, payload)`。
2. `Topic::PackData(topic_crc, buffer, timestamp, payload)` 写入串口包。
3. 对端 `SharedTopic` 解析后用同一个 timestamp 发布到对端 domain。

因此同步类 topic 不需要在 payload 里重复携带时间戳；payload 只保留业务字段即可。

## 依赖 / Depends

- 无（No dependencies）
