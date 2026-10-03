# SharedTopicClient

多 Topic 数据打包与 UART 转发模块 / Module that packs multi-Topic data and forwards it over a UART

## 1. 模块作用 / Purpose

SharedTopicClient 订阅多个 Topic，把每次发布打包后通过 UART 发送，对端用 `SharedTopic` 解析并发布到对端的 domain。

- 构造时，SharedTopicClient 在给定 domain 中查找每个 Topic 并注册 Topic callback。Topic 须已存在，未找到的 Topic 输出错误日志 `Topic not found` 并被跳过，其余 Topic 照常订阅。UART 须有可写的 write port，且最大的打包后长度（payload + `Topic::PACK_BASE_SIZE`）不超过 write port 的容量。
- 发送由 Topic callback 和写完成回调驱动。所有 Topic 共用 `slot_count` 个固定 packet 槽位，每个槽位的字节数取订阅 Topic 中最大的打包后长度。空槽位和待发 packet 各用一个 `MPMCQueue` 管理；`slot_count = 1` 时队列按最小容量 2 构造，槽位数仍为 1。
- Topic 发布时，callback 申请一个空槽位，用 `Topic::PackRaw()` 打包 payload 和 envelope timestamp，放入待发队列，然后推进发送。申请不到空槽位时丢弃这条新数据，该背压由所有 Topic 共用。
- 每次推进把一个待发 packet 交给 UART `WritePort`，数据拷入写队列后立即归还槽位。`WritePort` 忙或写队列满时丢弃该 packet 并归还槽位；之后的写完成回调或 Topic callback 继续推进队列。发送的并发与互斥由 LibXR `WritePort` 负责。

SharedTopicClient subscribes to several Topics, packs every publication and sends it over a UART; the peer parses the stream with `SharedTopic` and publishes it in its own domain.

- Upon construction, SharedTopicClient looks up every Topic in its domain and registers a Topic callback on it. The Topics must already exist; a Topic that is not found is logged as the error `Topic not found` and skipped, and the remaining Topics are subscribed as usual. The UART must have a writable write port, and the largest packed size (payload + `Topic::PACK_BASE_SIZE`) must fit into the write port capacity.
- Transmission is driven by the Topic callbacks and the write-done callback. All Topics share `slot_count` fixed packet slots, each sized for the largest packed subscribed Topic. Free slots and ready packets are kept in two `MPMCQueue`s; with `slot_count = 1` the queues are built with the minimum capacity 2 and the slot count remains 1.
- On each publication the callback takes a free slot, packs the payload and the envelope timestamp with `Topic::PackRaw()`, pushes the packet to the ready queue and kicks TX. If no slot is free the new packet is dropped; this back-pressure is shared by all Topics.
- Each kick hands one ready packet to the UART `WritePort`, and the slot is returned as soon as the data is copied into the write queue. If the `WritePort` is busy or its queue is full, the packet is dropped and its slot returned; the next write-done callback or Topic callback continues advancing the queue. Concurrency and mutual exclusion of writes are handled by the LibXR `WritePort`.

## 2. 时间戳 / Timestamp

转发时保留 LibXR message envelope timestamp：本地 callback 收到 `(timestamp, payload)`，`Topic::PackRaw(payload, buffer, timestamp)` 把它写入串口包，对端 `SharedTopic` 解析后用同一个 timestamp 发布。同步类 Topic 的 payload 因此不需要自带时间戳字段。

The LibXR envelope timestamp is preserved: the local callback receives `(timestamp, payload)`, `Topic::PackRaw(payload, buffer, timestamp)` writes it into the packet, and the peer `SharedTopic` publishes with the same timestamp. Payloads of synchronized Topics therefore carry no timestamp field of their own.

## 3. 构造接口 / Constructor

```cpp
SharedTopicClient(LibXR::UART& uart,
                  uint32_t slot_count = 16,
                  std::initializer_list<TopicConfig> topic_configs = {"topic1", {"topic2", "libxr_def_domain"}});
```

依赖：

- `uart`：发送数据包的 `LibXR::UART`。

配置参数：

- `slot_count`：共享的待发槽位数量，大于 0，默认 16。
- `topic_configs`：需要订阅并转发的 Topic 列表，至少一项。每项是 Topic 名（domain 为 `libxr_def_domain`），或 `{topic, domain}`。默认值 `topic1` 与 `topic2` 是占位名称，按实际 Topic 填写。被转发的 Topic 须在本实例构造前创建，创建它们的模块实例在 `modules:` 中排在本实例之前，或由 BSP 创建。

Dependencies:

- `uart`: the `LibXR::UART` the packets are sent on.

Configuration parameters:

- `slot_count`: number of shared pending slots, greater than 0, default 16.
- `topic_configs`: list of Topics to subscribe to and forward, at least one item. Each item is a Topic name (domain `libxr_def_domain`) or `{topic, domain}`. The defaults `topic1` and `topic2` are placeholder names to be replaced with the actual Topics. The forwarded Topics must be created before this instance is constructed: the instances that create them are listed before this one in `modules:`, or they are created by the BSP.

## 4. Topic

| Topic（默认名称） | 方向 | 类型 | 说明 |
| --- | --- | --- | --- |
| `topic_configs` 中的每个 Topic（默认 `topic1`、`topic2`） | 订阅 | 该 Topic 创建时的类型 | 每次发布被打包并通过 UART 发送 |

| Topic (default name) | Direction | Type | Meaning |
| --- | --- | --- | --- |
| Each Topic of `topic_configs` (default `topic1`, `topic2`) | Subscribe | the type the Topic was created with | Every publication is packed and sent over the UART |

## 5. 配置示例 / Configuration Example

`xrobot instance add xrobot-org/SharedTopicClient` 写入的实例，`uart` 填写为 BSP 通过 `XR_REGISTER`（硬件注册）注册的 UART 名称，`topic_configs` 填写为需要转发的 Topic：

An instance written by `xrobot instance add xrobot-org/SharedTopicClient`, with `uart` set to a UART name registered by the BSP's `XR_REGISTER` (Registration) and `topic_configs` set to the Topics to forward:

```yaml
modules:
  - module: xrobot-org/SharedTopicClient
    id: shared_topic_client
    args:
      - uart: usb_otg_hs_cdc
      - slot_count: 16
      - topic_configs: '{"ahrs_quaternion"}'
```

## 6. 依赖与硬件 / Dependencies and Hardware

依赖：LibXR。

硬件：一个具有可写 write port 的 UART，与接收端相连。

Dependencies: LibXR.

Hardware: one UART with a writable write port, connected to the receiver.
