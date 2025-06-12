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
- topic_names: 需要订阅并转发的 Topic 名称列表 / List of topic names to subscribe and forward (e.g., ["topic1", "topic2"])

## 依赖 / Depends

- 无（No dependencies）
