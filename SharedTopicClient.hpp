#pragma once

// clang-format off
/* === MODULE MANIFEST V2 ===
module_description: 多 Topic 数据打包与 UART 转发模块 / Module that packs multi-Topic data and forwards it over a UART
depends: []
=== END MANIFEST === */
// clang-format on

#include <cstddef>
#include <cstdint>
#include <memory>

#include "libxr_def.hpp"
#include "message.hpp"
#include "serialized_service.hpp"
#include "slot_pool.hpp"
#include "uart.hpp"

/**
 * @brief 订阅多个 Topic，把每次发布打包后通过 UART 转发。
 *        Subscribes to several Topics, packs every publication and forwards it over a
 *        UART.
 *
 * 待发数据放在一个固定容量的槽池里：Topic 回调认领空槽、把数据打包进槽并发布，
 * 发送端按发布序号取出、交给 UART 的写端口、再释放槽位；发送推进由
 * `LibXR::SerializedService` 串行化，任意上下文都只有一个推进者。
 *
 * Pending data lives in one fixed-capacity slot pool: the Topic callback claims a free
 * slot, packs into it and publishes it; the sender takes the slots in publish order,
 * hands them to the UART write port and releases them. TX progress is serialized by
 * `LibXR::SerializedService`, so exactly one context advances it at a time.
 */
class SharedTopicClient
{
 private:
  struct CallbackInfo
  {
    SharedTopicClient* client;
    LibXR::Topic::TopicHandle topic;
  };

 public:
  /**
   * @brief 需要订阅并转发的 Topic。
   *        A Topic to subscribe to and forward.
   */
  struct TopicConfig
  {
    const char* name;                         ///< Topic 名称 Topic name
    const char* domain = "libxr_def_domain";  ///< Topic 所在的 domain Domain of the Topic

    /**
     * @brief 使用默认 domain `libxr_def_domain` 构造。
     *        Construct with the default domain `libxr_def_domain`.
     *
     * @param name Topic 名称。
     *             Topic name.
     */
    TopicConfig(const char* name) : name(name) {}

    /**
     * @brief 构造并指定 domain。
     *        Construct with an explicit domain.
     *
     * @param name Topic 名称。
     *             Topic name.
     * @param domain Topic 所在的 domain。
     *               Domain of the Topic.
     */
    TopicConfig(const char* name, const char* domain) : name(name), domain(domain) {}
  };

  /**
   * @brief 构造 SharedTopicClient：创建槽池，并为 topic_configs 中的每个 Topic
   *        注册 callback。
   *        Construct SharedTopicClient: create the slot pool, and register a
   *        callback on every Topic of topic_configs.
   *
   * @param uart 发送数据包的 UART，须有可写的 write port。
   *             UART the packets are sent on; it must have a writable write port.
   * @param slot_count 共享的待发槽位数量，须大于 0。
   *                   Number of shared pending slots; must be greater than 0.
   * @param topic_configs 需要订阅并转发的 Topic 列表，至少一项；未找到的 Topic
   *                      输出错误日志并进入致命错误处理。
   *                      Topics to subscribe to and forward, at least one; a Topic that
   *                      is not found is logged as an error and enters the fatal error
   *                      handler.
   */
  SharedTopicClient(LibXR::UART& uart, uint32_t slot_count = 16,
                    std::initializer_list<TopicConfig> topic_configs =
                        {"topic1", {"topic2", "libxr_def_domain"}})
      : uart_(std::addressof(uart)), slots_(slot_count)
  {
    ASSERT(uart_->write_port_ != nullptr);
    ASSERT(uart_->write_port_->Writable());
    ASSERT(topic_configs.size() > 0);
    ASSERT(slot_count > 0);

    size_t max_packet_size = 0;

    for (auto config : topic_configs)
    {
      auto domain = LibXR::Topic::Domain(config.domain);
      auto topic = LibXR::Topic::Find(config.name, &domain);
      if (topic == nullptr)
      {
        XR_LOG_ERROR("Topic not found: %s/%s", config.domain, config.name);
        // 配置错误：所有构建类型都进入致命错误处理
        // Configuration error: every build type enters the fatal error handler
        libxr_fatal_error(__FILE__, __LINE__, false);
      }
      const size_t packet_size = topic->data_.payload_size + LibXR::Topic::PACK_BASE_SIZE;
      max_packet_size = LibXR::max(max_packet_size, packet_size);
    }

    ASSERT(max_packet_size <= uart_->write_port_->Capacity());

    // 每个槽的字节数取订阅 Topic 中最大的打包后长度；数据按槽位下标索引同一块缓冲
    // Each slot holds the largest packed subscribed Topic; slot data is indexed in one
    // buffer by the slot index
    max_packet_size_ = max_packet_size;
    buffers_ = new uint8_t[static_cast<size_t>(slot_count) * max_packet_size];

    tx_callback_ = LibXR::Callback<LibXR::ErrorCode>::CreateGuarded(
        [](bool in_isr, SharedTopicClient* self, LibXR::ErrorCode status)
        { self->OnWriteDone(in_isr, status); }, this);
    tx_op_ = LibXR::WriteOperation(tx_callback_);

    for (auto config : topic_configs)
    {
      auto domain = LibXR::Topic::Domain(config.domain);
      auto topic_handle = LibXR::Topic::Find(config.name, &domain);
      // 上一轮循环已确认每个 Topic 都存在
      // The first loop has confirmed that every Topic exists
      ASSERT(topic_handle != nullptr);
      void (*func)(bool, CallbackInfo, const LibXR::Topic::RawMessageView&) =
          [](bool in_isr, CallbackInfo info, const LibXR::Topic::RawMessageView& message)
      { info.client->OnTopic(in_isr, info, message); };

      auto msg_cb =
          LibXR::Topic::Callback::Create(func, CallbackInfo{this, topic_handle});

      LibXR::Topic topic(topic_handle);

      topic.RegisterCallback(msg_cb);
    }
  }

 private:
  void OnTopic(bool in_isr, CallbackInfo info,
               const LibXR::Topic::RawMessageView& message)
  {
    const size_t packet_size = message.payload.size_ + LibXR::Topic::PACK_BASE_SIZE;
    size_t slot_index = 0;

    if (slots_.Acquire(slot_index) != LibXR::ErrorCode::OK)
    {
      // 没有空槽位：丢弃这条新数据，背压由所有 Topic 共用
      // No free slot: drop this publication; the back-pressure is shared by all Topics
      return;
    }

    uint8_t* buffer = buffers_ + slot_index * max_packet_size_;
    ASSERT(packet_size <= max_packet_size_);
    if (LibXR::Topic(info.topic)
            .PackRaw(message.payload, LibXR::RawData(buffer, max_packet_size_),
                     message.timestamp) != LibXR::ErrorCode::OK)
    {
      slots_.Release(slot_index);
      return;
    }

    slots_.At(slot_index) = packet_size;
    slots_.Publish(slot_index);
    KickTx(in_isr);
  }

  /**
   * @brief 请求推进发送 / Request TX progress.
   *
   * 并发或重入调用只发布事件，由当前的消费者在自己的上下文里继续推进；取得执行权的
   * 调用以自己的上下文执行 `ServiceTx`，因此 `Write` 的 `in_isr` 与之相同。
   *
   * Reentrant or concurrent calls only publish an event and the current consumer keeps
   * advancing in its own context; the claimer runs `ServiceTx` with its own context, so
   * the `in_isr` of `Write` matches it.
   *
   * @param in_isr 当前调用上下文 / Context of this call
   */
  void KickTx(bool in_isr)
  {
    tx_service_.Invoke(1U, in_isr,
                       [this](uint32_t, bool owner_in_isr) { ServiceTx(owner_in_isr); });
  }

  /**
   * @brief 在唯一的消费者中取出一个待发数据包并发送 / Take one ready packet and send
   *        it in the sole consumer.
   *
   * @param in_isr 取得执行权的上下文 / Context of the claimer
   */
  void ServiceTx(bool in_isr)
  {
    size_t slot_index = 0;
    if (slots_.Take(slot_index) != LibXR::ErrorCode::OK)
    {
      return;
    }

    const size_t packet_size = slots_.At(slot_index);
    uart_->Write(
        LibXR::ConstRawData{buffers_ + slot_index * max_packet_size_, packet_size},
        tx_op_, in_isr);
    // 数据已拷入 WritePort 的字节队列，槽位立即归还；Write 失败时这一包同样被丢弃
    // The data is already copied into the write port queue, so the slot is returned
    // right away; a failed Write drops this packet as well
    slots_.Release(slot_index);
  }

  void OnWriteDone(bool in_isr, LibXR::ErrorCode) { KickTx(in_isr); }

  LibXR::UART* uart_;
  LibXR::SlotPool<size_t> slots_;
  uint8_t* buffers_ = nullptr;
  size_t max_packet_size_ = 0;
  LibXR::Callback<LibXR::ErrorCode> tx_callback_;
  LibXR::WriteOperation tx_op_;
  LibXR::SerializedService tx_service_;
};
