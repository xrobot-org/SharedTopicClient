# SharedTopicClient

SharedTopicClient 是一个多 Topic 数据共享与串口转发客户端模块。它用于通过 UART 将多个 Topic 的数据统一打包、发送，实现消息流的串口透明同步转发，适用于分布式系统的多主题数据同步或边缘数据采集。

SharedTopicClient is a client module for multi-topic data sharing and transparent UART forwarding. It subscribes to multiple Topics, packs their updates, and transmits them via UART, enabling efficient and reliable message synchronization over serial connections—ideal for distributed systems or edge data acquisition.

---

## 硬件需求 / Required Hardware

- uart_name

## 构造参数 / Constructor Arguments

- uart_name: 串口设备名 / UART device name (e.g., "uart_cdc")
- buffer_size: 单个 Topic 串口包最大字节数 / Maximum bytes of one forwarded Topic packet (e.g., 256)
- topic_configs: 需要订阅并转发的 Topic 配置列表。每项可以只写 topic 名，也可以写
  `[topic, domain]`。/ Topic configs to subscribe and forward. Each item may be a
  topic name or `[topic, domain]`.

## 运行方式

`SharedTopicClient` 不创建发送线程。模块注册 Topic callback；每次 Topic 发布时，
callback 内直接完成打包并把当前包写入 UART `write_port`。UART 写入仍走 libxr
非阻塞写队列，包内容在 `Write()` 返回前已经复制到 UART 队列。

## Timestamp

`SharedTopicClient` 转发 Topic 时会保留 libxr message envelope timestamp：

1. 本地 Topic callback 收到 `(timestamp, payload)`。
2. `Topic::PackData(topic_crc, buffer, timestamp, payload)` 写入串口包。
3. 对端 `SharedTopic` 解析后用同一个 timestamp 发布到对端 domain。

因此同步类 topic 不需要在 payload 里重复携带时间戳；payload 只保留业务字段即可。

## 依赖 / Depends

- 无（No dependencies）
