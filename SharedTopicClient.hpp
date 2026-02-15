#pragma once

// clang-format off
/* === MODULE MANIFEST V2 ===
module_description: SharedTopicClient 是一个多 Topic 数据共享与串口转发客户端模块。它用于通过 UART 将多个 Topic 的数据统一打包、发送，实现消息流的串口透明同步转发，适用于分布式系统的多主题数据同步或边缘数据采集。 / SharedTopicClient is a client module for multi-topic data sharing and transparent UART forwarding. It subscribes to multiple Topics, packs their updates, and transmits them via UART, enabling efficient and reliable message synchronization over serial connections—ideal for distributed systems or edge data acquisition.
constructor_args:
  - uart_name: "uart_cdc"
  - task_stack_depth: 2048
  - buffer_size: 256
  - topic_configs:
    - "topic1"
    - ["topic2", "libxr_def_domain"]
template_args: []
required_hardware: uart_name
depends: []
=== END MANIFEST === */
// clang-format on

#include "app_framework.hpp"
#include "uart.hpp"

class SharedTopicClient : public LibXR::Application {
 public:
  typedef struct {
    SharedTopicClient* client;
    uint32_t topic_crc32;
    uint32_t index;
  } CallbackInfo;

  struct TopicConfig {
    const char* name;
    const char* domain = "libxr_def_domain";

    TopicConfig(const char* name) : name(name) {}

    TopicConfig(const char* name, const char* domain)
        : name(name), domain(domain) {}
  };

  SharedTopicClient(LibXR::HardwareContainer& hw,
                    LibXR::ApplicationManager& app, const char* uart_name,
                    uint32_t task_stack_depth, uint32_t buffer_size,
                    std::initializer_list<TopicConfig> topic_configs)
      : uart_(hw.template Find<LibXR::UART>(uart_name)),
        tx_buffer_(new uint8_t[buffer_size], buffer_size),
        tx_queue_(buffer_size) {
    ASSERT(uart_ != nullptr);

    topics_pack_buffer_ = new LibXR::RawData[topic_configs.size()];

    uint32_t i = 0;

    for (auto config : topic_configs) {
      auto domain = LibXR::Topic::Domain(config.domain);
      auto ans = LibXR::Topic::Find(config.name, &domain);
      if (ans == nullptr) {
        XR_LOG_ERROR("Topic not found: %s/%s", config.domain, config.name);
        ASSERT(false);
      }
      topics_pack_buffer_[i] = LibXR::RawData(
          new uint8_t[ans->data_.max_length + LibXR::Topic::PACK_BASE_SIZE],
          ans->data_.max_length + LibXR::Topic::PACK_BASE_SIZE);

      void (*func)(bool, CallbackInfo, LibXR::RawData&) =
          [](bool in_isr, CallbackInfo info, LibXR::RawData& data) {
            LibXR::WriteOperation op;
            LibXR::Topic::PackData(info.topic_crc32,
                                   info.client->topics_pack_buffer_[info.index],
                                   data);
            info.client->tx_queue_.PushBatch(
                static_cast<uint8_t*>(
                    info.client->topics_pack_buffer_[info.index].addr_),
                info.client->topics_pack_buffer_[info.index].size_);
            info.client->tx_sem_.PostFromCallback(in_isr);
          };

      auto msg_cb = LibXR::Topic::Callback::Create(
          func, CallbackInfo{this, ans->data_.crc32, i});

      LibXR::Topic topic(ans);

      topic.RegisterCallback(msg_cb);

      i++;
    }

    tx_thread_.Create(this, TxThreadFun, "SharedTopicClientTxThread",
                      task_stack_depth, LibXR::Thread::Priority::REALTIME);

    app.Register(*this);
  }

  static void TxThreadFun(SharedTopicClient* client) {
    LibXR::Semaphore write_op_sem;
    LibXR::WriteOperation op(write_op_sem);
    LibXR::WriteOperation op_none;
    while (true) {
      client->tx_sem_.Wait();
      auto size =
          LibXR::min(client->tx_queue_.Size(), client->tx_buffer_.size_);
      if (size > 0 && client->tx_queue_.PopBatch(
                          static_cast<uint8_t*>(client->tx_buffer_.addr_),
                          size) == ErrorCode::OK) {
        client->uart_->Write(
            {static_cast<uint8_t*>(client->tx_buffer_.addr_), size}, op_none);
      }
    }
  }
  void OnMonitor() override {}

 private:
  LibXR::UART* uart_;
  LibXR::RawData tx_buffer_;
  LibXR::LockFreeQueue<uint8_t> tx_queue_;
  LibXR::RawData* topics_pack_buffer_;
  LibXR::Semaphore tx_sem_;
  LibXR::Thread tx_thread_;
};
