# SharedTopicClient

SharedTopicClient 是一个多 Topic 数据共享与串口转发客户端模块。它订阅多个 Topic，
把每次发布打包后通过 UART 发送，对端用 `SharedTopic` 解析并发布到对端 domain，
适用于分布式系统的多主题数据同步或边缘数据采集。

SharedTopicClient is a client module for multi-topic data sharing and UART
forwarding. It subscribes to several Topics, packs every publication and sends it
over a UART; the peer parses the stream with `SharedTopic` and publishes it in its
own domain. Useful for multi-topic synchronization in distributed systems or edge
data acquisition.

## 运行方式 / Behaviour

- 构造时在给定 domain 中查找每个 Topic（必须已经存在，否则打印 `Topic not found`
  并触发 `ASSERT`），并为其注册 Topic callback。UART 必须有可写的 write port，且最大的
  打包后长度（payload + `Topic::PACK_BASE_SIZE`）不能超过 write port 容量。
- 模块不创建发送线程。所有 Topic 共用 `slot_count` 个固定 packet 槽位，每个槽位的
  字节数取订阅 Topic 中最大的打包后长度。空槽位和待发 packet 各用一个
  `MPMCQueue` 管理；`slot_count = 1` 时队列仍按最小容量 2 构造，但不增加槽位。
- Topic 发布时，callback 申请一个空槽位，用 `Topic::PackRaw()` 打包 payload 和
  envelope timestamp，放入待发队列，然后推进发送。申请不到空槽位时丢弃这条新数据
  （全局背压，不是同 Topic 覆盖）。
- 每次推进只把一个待发 packet 交给 UART `WritePort`，数据拷入写队列后立即归还槽位。
  `WritePort` 忙或写队列满时丢弃该 packet 并归还槽位，不在回调链中重试；之后的写完成
  回调或 Topic callback 会继续推进队列。发送并发与互斥由 LibXR `WritePort` 负责。

- The constructor looks up every Topic in its domain (it must already exist;
  otherwise it logs `Topic not found` and fails an `ASSERT`) and registers a Topic
  callback on it. The UART must have a writable write port, and the largest packed
  size (payload + `Topic::PACK_BASE_SIZE`) must fit into the write port capacity.
- No TX thread is created. All Topics share `slot_count` fixed packet slots, each
  sized for the largest packed subscribed Topic. Free slots and ready packets are
  kept in two `MPMCQueue`s; with `slot_count = 1` the queues are still built with the
  minimum capacity 2, without adding slots.
- On each publication the callback takes a free slot, packs payload and envelope
  timestamp with `Topic::PackRaw()`, pushes it to the ready queue and kicks TX. If no
  slot is free the new packet is dropped (global back-pressure, not per-Topic
  overwrite).
- Each kick hands one ready packet to the UART `WritePort`; the slot is returned as
  soon as the data is copied into the write queue. If the `WritePort` is busy or its
  queue is full, the packet is dropped and its slot returned, without retrying in
  the callback chain; the next write-done callback or Topic callback advances the
  queue. Concurrency and mutual exclusion of writes are left to the LibXR
  `WritePort`.

## 时间戳 / Timestamp

转发时保留 LibXR message envelope timestamp：本地 callback 收到 `(timestamp, payload)`，
`Topic::PackRaw(payload, buffer, timestamp)` 把它写入串口包，对端 `SharedTopic`
解析后用同一个 timestamp 发布。因此同步类 Topic 不需要在 payload 里重复携带时间戳。

The LibXR envelope timestamp is preserved: the local callback receives
`(timestamp, payload)`, `Topic::PackRaw(payload, buffer, timestamp)` writes it into
the packet, and the peer `SharedTopic` publishes with the same timestamp. Payloads
of synchronized Topics therefore do not need their own timestamp field.

## 依赖 / Dependencies

无其他模块依赖，仅使用 LibXR。
No other Modules; LibXR only.

## 构造接口 / Constructor

```cpp
SharedTopicClient(LibXR::UART& uart,
                  uint32_t slot_count = 16,
                  std::initializer_list<TopicConfig> topic_configs = {"topic1", {"topic2", "libxr_def_domain"}});
```

依赖 / Dependencies:

- `uart`：发送数据的 `LibXR::UART`。/ The `LibXR::UART` the packets are sent on.

配置 / Configuration:

- `slot_count`：共享待发槽位数量，> 0，默认 16。/ Number of shared pending slots,
  > 0, default 16.
- `topic_configs`：需要订阅并转发的 Topic 列表，至少一项。每项可以只写 Topic 名
  （使用 `libxr_def_domain`），也可以写 `{topic, domain}`。默认值 `topic1` / `topic2`
  只是占位，应改为实际的 Topic。/ Topics to subscribe and forward, at least one.
  Each item is a Topic name (domain `libxr_def_domain`) or `{topic, domain}`. The
  defaults `topic1` / `topic2` are placeholders; replace them with real Topics.

## 使用 / Use

```sh
xrobot module add xrobot-org/SharedTopicClient
xrobot setup
xrobot instance add xrobot-org/SharedTopicClient
```

`xrobot instance add` 在 `User/xrobot.yaml` 中写入一个实例，依赖项留空，默认值按源码写出；
把 `uart` 填为 BSP 中用 `XR_REGISTER` 注册的 UART 对象名：
`xrobot instance add` writes an instance to `User/xrobot.yaml` with empty
dependencies and the source defaults; set `uart` to the name of a UART object the
BSP registers with `XR_REGISTER`:

```yaml
modules:
  - module: xrobot-org/SharedTopicClient
    id: sharedtopicclient_0
    args:
      - uart: uart_cdc
      - slot_count: '16'
      - topic_configs: '{"topic1", {"topic2", "libxr_def_domain"}}'
```

BSP 侧 / BSP side:

```cpp
XR_REGISTER(uart_cdc, LibXR::UART);
```

被转发的 Topic 必须在本实例构造前创建：创建它们的模块实例应在 `modules:` 中排在前面
（或由 BSP 创建）。
The forwarded Topics must exist before this instance is constructed: list the
instances that create them earlier in `modules:` (or create them in the BSP).

填好后再次运行 `xrobot setup`，生成 `User/xrobot_main.hpp`。
Run `xrobot setup` again to generate `User/xrobot_main.hpp`.

`xrobot module show .`（在本仓库中）或 `xrobot module show Modules/xrobot-org/SharedTopicClient`
（在 BSP 中）打印 manifest 和当前的构造函数。
`xrobot module show .` in this repository, or
`xrobot module show Modules/xrobot-org/SharedTopicClient` in a BSP, prints the
manifest and the current constructor.
