#pragma once

// clang-format off
/* === MODULE MANIFEST V2 ===
module_description: SharedTopicClient 是一个多 Topic 数据共享与串口转发客户端模块。它用于通过 UART 将多个 Topic 的数据统一打包、发送，实现消息流的串口透明同步转发，适用于分布式系统的多主题数据同步或边缘数据采集。 / SharedTopicClient is a client module for multi-topic data sharing and transparent UART forwarding. It subscribes to multiple Topics, packs their updates, and transmits them via UART, enabling efficient and reliable message synchronization over serial connections—ideal for distributed systems or edge data acquisition.
constructor_args:
  - uart_name: "uart_cdc"
  - slot_count: 16
  - topic_configs:
    - "topic1"
    - ["topic2", "libxr_def_domain"]
template_args: []
required_hardware: uart_name
depends: []
=== END MANIFEST === */
// clang-format on

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "app_framework.hpp"
#include "lockfree_pool.hpp"
#include "lockfree_queue.hpp"
#include "message.hpp"
#include "uart.hpp"

class SharedTopicClient : public LibXR::Application {
 private:
  struct CallbackInfo {
    SharedTopicClient* client;
    uint32_t topic_crc32;
  };

  struct PacketSlot {
    LibXR::RawData buffer;
  };

  struct ReadyPacket {
    uint32_t slot_index = 0;
    size_t packet_size = 0;
  };

 public:
  struct TopicConfig {
    const char* name;
    const char* domain = "libxr_def_domain";

    TopicConfig(const char* name) : name(name) {}

    TopicConfig(const char* name, const char* domain)
        : name(name), domain(domain) {}
  };

  SharedTopicClient(LibXR::HardwareContainer& hw,
                    LibXR::ApplicationManager& app, const char* uart_name,
                    uint32_t slot_count,
                    std::initializer_list<TopicConfig> topic_configs)
      : uart_(hw.template Find<LibXR::UART>(uart_name)) {
    ASSERT(uart_ != nullptr);
    ASSERT(uart_->write_port_ != nullptr);
    ASSERT(uart_->write_port_->queue_data_ != nullptr);
    ASSERT(uart_->write_port_->Writable());
    ASSERT(topic_configs.size() > 0);
    ASSERT(slot_count > 0);

    size_t max_packet_size = 0;

    for (auto config : topic_configs) {
      auto domain = LibXR::Topic::Domain(config.domain);
      auto ans = LibXR::Topic::Find(config.name, &domain);
      if (ans == nullptr) {
        XR_LOG_ERROR("Topic not found: %s/%s", config.domain, config.name);
        ASSERT(false);
      }
      const size_t packet_size =
          ans->data_.max_length + LibXR::Topic::PACK_BASE_SIZE;
      max_packet_size = LibXR::max(max_packet_size, packet_size);
    }

    ASSERT(max_packet_size <= uart_->write_port_->queue_data_->MaxSize());

    packets_ = new PacketSlot[slot_count];
    free_slots_ = new LibXR::LockFreeQueue<uint32_t>(slot_count);
    ready_packets_ = new LibXR::LockFreePool<ReadyPacket>(slot_count);
    for (uint32_t i = 0; i < slot_count; i++) {
      packets_[i].buffer =
          LibXR::RawData(new uint8_t[max_packet_size], max_packet_size);
      auto ans = free_slots_->Push(i);
      UNUSED(ans);
      ASSERT(ans == LibXR::ErrorCode::OK);
    }

    tx_callback_ = LibXR::Callback<LibXR::ErrorCode>::CreateGuarded(
        [](bool in_isr, SharedTopicClient* self, LibXR::ErrorCode status) {
          self->OnWriteDone(in_isr, status);
        },
        this);
    tx_op_ = LibXR::WriteOperation(tx_callback_);

    for (auto config : topic_configs) {
      auto domain = LibXR::Topic::Domain(config.domain);
      auto ans = LibXR::Topic::Find(config.name, &domain);
      ASSERT(ans != nullptr);
      void (*func)(bool, CallbackInfo, LibXR::MicrosecondTimestamp,
                   LibXR::ConstRawData&) =
          [](bool in_isr, CallbackInfo info,
             LibXR::MicrosecondTimestamp timestamp, LibXR::ConstRawData& data) {
            info.client->OnTopic(in_isr, info, timestamp, data);
          };

      auto msg_cb = LibXR::Topic::Callback::Create(
          func, CallbackInfo{this, ans->data_.crc32});

      LibXR::Topic topic(ans);

      topic.RegisterCallback(msg_cb);
    }

    app.Register(*this);
  }

  void OnMonitor() override {}

 private:
  void OnTopic(bool in_isr, CallbackInfo info,
               LibXR::MicrosecondTimestamp timestamp,
               LibXR::ConstRawData& data) {
    const size_t packet_size = data.size_ + LibXR::Topic::PACK_BASE_SIZE;
    uint32_t slot_index = 0;

    if (free_slots_->Pop(slot_index) != LibXR::ErrorCode::OK) {
      dropped_packets_.fetch_add(1U, std::memory_order_relaxed);
      return;
    }

    auto& slot = packets_[slot_index];
    ASSERT(packet_size <= slot.buffer.size_);
    LibXR::Topic::PackData(info.topic_crc32, slot.buffer, timestamp, data);

    auto ans = ready_packets_->Put(ReadyPacket{slot_index, packet_size});
    UNUSED(ans);
    ASSERT(ans == LibXR::ErrorCode::OK);
    TxService(in_isr);
  }

  void TxService(bool in_isr) {
    tx_pend_.store(1U, std::memory_order_release);

    uint32_t expected = 0U;
    if (!tx_lock_.compare_exchange_strong(expected, 1U,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
      return;
    }

    while (true) {
      tx_pend_.store(0U, std::memory_order_release);
      bool blocked = false;

      while (true) {
        ReadyPacket packet = retry_packet_;
        if (!retry_pending_) {
          if (ready_packets_->Get(packet) != LibXR::ErrorCode::OK) {
            break;
          }
        }

        auto& slot = packets_[packet.slot_index];
        auto ans = uart_->Write(
            LibXR::ConstRawData{slot.buffer.addr_, packet.packet_size}, tx_op_,
            in_isr);
        if (static_cast<int8_t>(ans) < 0) {
          retry_packet_ = packet;
          retry_pending_ = true;
          blocked = true;
          break;
        }

        retry_pending_ = false;
        ReturnFreeSlot(packet.slot_index);
      }

      tx_lock_.store(0U, std::memory_order_release);
      if (blocked || tx_pend_.load(std::memory_order_acquire) == 0U) {
        return;
      }

      expected = 0U;
      if (!tx_lock_.compare_exchange_strong(expected, 1U,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
        return;
      }
    }
  }

  void OnWriteDone(bool in_isr, LibXR::ErrorCode status) {
    UNUSED(status);
    TxService(in_isr);
  }

  void ReturnFreeSlot(uint32_t slot_index) {
    auto ans = free_slots_->Push(slot_index);
    UNUSED(ans);
    ASSERT(ans == LibXR::ErrorCode::OK);
  }

  LibXR::UART* uart_;
  PacketSlot* packets_ = nullptr;
  LibXR::LockFreeQueue<uint32_t>* free_slots_ = nullptr;
  LibXR::LockFreePool<ReadyPacket>* ready_packets_ = nullptr;
  ReadyPacket retry_packet_;
  bool retry_pending_ = false;
  std::atomic<uint32_t> tx_lock_{0};
  std::atomic<uint32_t> tx_pend_{0};
  std::atomic<uint32_t> dropped_packets_{0};
  LibXR::Callback<LibXR::ErrorCode> tx_callback_;
  LibXR::WriteOperation tx_op_;
};
