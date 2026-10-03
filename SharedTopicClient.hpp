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
#include "queue.hpp"
#include "uart.hpp"

/**
 * @brief 订阅多个 Topic，把每次发布打包后通过 UART 转发。
 *        Subscribes to several Topics, packs every publication and forwards it over a
 *        UART.
 */
class SharedTopicClient
{
 private:
  struct CallbackInfo
  {
    SharedTopicClient* client;
    LibXR::Topic::TopicHandle topic;
  };

  struct PacketSlot
  {
    LibXR::RawData buffer;
  };

  struct ReadyPacket
  {
    uint32_t slot_index = 0;
    size_t packet_size = 0;
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
   * @brief 构造 SharedTopicClient：创建槽位与队列，并为 topic_configs 中的每个 Topic
   *        注册 callback。
   *        Construct SharedTopicClient: create the slots and queues, and register a
   *        callback on every Topic of topic_configs.
   *
   * @param uart 发送数据包的 UART，须有可写的 write port。
   *             UART the packets are sent on; it must have a writable write port.
   * @param slot_count 共享的待发槽位数量，须大于 0。
   *                   Number of shared pending slots; must be greater than 0.
   * @param topic_configs 需要订阅并转发的 Topic 列表，至少一项，Topic 须已存在。
   *                      Topics to subscribe to and forward, at least one; they must
   *                      already exist.
   */
  SharedTopicClient(
      LibXR::UART& uart,
      uint32_t slot_count = 16,
      std::initializer_list<TopicConfig> topic_configs = {"topic1", {"topic2", "libxr_def_domain"}})
      : uart_(std::addressof(uart))
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
        ASSERT(false);
      }
      const size_t packet_size = topic->data_.payload_size + LibXR::Topic::PACK_BASE_SIZE;
      max_packet_size = LibXR::max(max_packet_size, packet_size);
    }

    ASSERT(max_packet_size <= uart_->write_port_->Capacity());

    const size_t queue_capacity = LibXR::max(static_cast<size_t>(slot_count), size_t{2});
    packets_ = new PacketSlot[slot_count];
    free_slots_ = new LibXR::MPMCQueue<uint32_t>(queue_capacity);
    ready_packets_ = new LibXR::MPMCQueue<ReadyPacket>(queue_capacity);
    for (uint32_t i = 0; i < slot_count; i++)
    {
      packets_[i].buffer = LibXR::RawData(new uint8_t[max_packet_size], max_packet_size);
      ASSERT(free_slots_->Push(i) == LibXR::ErrorCode::OK);
    }

    tx_callback_ = LibXR::Callback<LibXR::ErrorCode>::CreateGuarded(
        [](bool in_isr, SharedTopicClient* self, LibXR::ErrorCode status)
        { self->OnWriteDone(in_isr, status); }, this);
    tx_op_ = LibXR::WriteOperation(tx_callback_);

    for (auto config : topic_configs)
    {
      auto domain = LibXR::Topic::Domain(config.domain);
      auto topic_handle = LibXR::Topic::Find(config.name, &domain);
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
    uint32_t slot_index = 0;

    if (free_slots_->Pop(slot_index) != LibXR::ErrorCode::OK)
    {
      return;
    }

    auto& slot = packets_[slot_index];
    ASSERT(packet_size <= slot.buffer.size_);
    if (LibXR::Topic(info.topic)
            .PackRaw(message.payload, slot.buffer, message.timestamp) !=
        LibXR::ErrorCode::OK)
    {
      ReturnFreeSlot(slot_index);
      return;
    }

    if (ready_packets_->Push(ReadyPacket{slot_index, packet_size}) !=
        LibXR::ErrorCode::OK)
    {
      ReturnFreeSlot(slot_index);
      return;
    }
    KickTx(in_isr);
  }

  void KickTx(bool in_isr) { tx_callback_.Run(in_isr, LibXR::ErrorCode::OK); }

  void TxService(bool in_isr)
  {
    ReadyPacket packet;
    if (ready_packets_->Pop(packet) != LibXR::ErrorCode::OK)
    {
      return;
    }

    auto& slot = packets_[packet.slot_index];
    auto write_status = uart_->Write(
        LibXR::ConstRawData{slot.buffer.addr_, packet.packet_size}, tx_op_, in_isr);
    if (static_cast<int8_t>(write_status) < 0)
    {
      ReturnFreeSlot(packet.slot_index);
      return;
    }

    ReturnFreeSlot(packet.slot_index);
  }

  void OnWriteDone(bool in_isr, LibXR::ErrorCode status)
  {
    if (static_cast<int8_t>(status) < 0)
    {
      return;
    }
    TxService(in_isr);
  }

  void ReturnFreeSlot(uint32_t slot_index)
  {
    ASSERT(free_slots_->Push(slot_index) == LibXR::ErrorCode::OK);
  }

  LibXR::UART* uart_;
  PacketSlot* packets_ = nullptr;
  LibXR::MPMCQueue<uint32_t>* free_slots_ = nullptr;
  LibXR::MPMCQueue<ReadyPacket>* ready_packets_ = nullptr;
  LibXR::Callback<LibXR::ErrorCode> tx_callback_;
  LibXR::WriteOperation tx_op_;
};
