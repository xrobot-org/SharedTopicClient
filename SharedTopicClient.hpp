#pragma once

// clang-format off
/* === MODULE MANIFEST ===
module_name: SharedTopicClient
module_description: No description provided
constructor_args:
  - uart_name: "uart_cdc"
  - task_stack_depth: 512
  - buffer_size: 256
  - topic_names:
    - "topic1"
    - "topic2"
required_hardware: uart_name
=== END MANIFEST === */
// clang-format on

#include "app_framework.hpp"
#include "uart.hpp"

class SharedTopicClient : public LibXR::Application {
 public:
  typedef struct {
    SharedTopicClient *client;
    uint32_t topic_crc32;
    uint32_t index;
  } CallbackInfo;

  SharedTopicClient(LibXR::HardwareContainer &hw,
                    LibXR::ApplicationManager &app, const char *uart_name,
                    uint32_t task_stack_depth, uint32_t buffer_size,
                    std::initializer_list<const char *> topic_names)
      : uart_(hw.template Find<LibXR::UART>(uart_name)),
        tx_buffer_(new uint8_t[buffer_size], buffer_size),
        tx_queue_(buffer_size) {
    ASSERT(uart_ != nullptr);

    topics_pack_buffer_ = new LibXR::RawData[topic_names.size()];

    uint32_t i = 0;

    for (auto name : topic_names) {
      auto ans = LibXR::Topic::Find(name);
      if (ans == nullptr) {
        XR_LOG_ERROR("Topic not found: %s", name);
        ASSERT(false);
      }
      topics_pack_buffer_[i] = LibXR::RawData(
          new uint8_t[ans->data_.max_length + LibXR::Topic::PACK_BASE_SIZE],
          ans->data_.max_length + LibXR::Topic::PACK_BASE_SIZE);

      void (*func)(bool, CallbackInfo, LibXR::RawData &) =
          [](bool in_isr, CallbackInfo info, LibXR::RawData &data) {
            LibXR::WriteOperation op;
            LibXR::Topic::PackData(info.topic_crc32,
                                   info.client->topics_pack_buffer_[info.index],
                                   data);
            info.client->tx_queue_.PushBatch(
                static_cast<uint8_t *>(
                    info.client->topics_pack_buffer_[info.index].addr_),
                info.client->topics_pack_buffer_[info.index].size_);
            info.client->tx_sem_.PostFromCallback(in_isr);
          };

      auto msg_cb = LibXR::Callback<LibXR::RawData &>::Create(
          func, CallbackInfo{this, ans->data_.crc32, i});

      LibXR::Topic topic(ans);

      topic.RegisterCallback(msg_cb);

      i++;
    }

    tx_thread_.Create(this, TxThreadFun, "SharedTopicClientTxThread",
                      task_stack_depth, LibXR::Thread::Priority::REALTIME);

    app.Register(*this);
  }

  static void TxThreadFun(SharedTopicClient *client) {
    LibXR::Semaphore write_op_sem;
    LibXR::WriteOperation op(write_op_sem);
    LibXR::WriteOperation op_none;
    while (true) {
      client->tx_sem_.Wait();
      auto size =
          LibXR::min(client->tx_queue_.Size(), client->tx_buffer_.size_);
      if (size > 0 && client->tx_queue_.PopBatch(
                          static_cast<uint8_t *>(client->tx_buffer_.addr_),
                          size) == ErrorCode::OK) {
        client->uart_->write_port_(
            {static_cast<uint8_t *>(client->tx_buffer_.addr_), size}, op_none);
      }
    }
  }
  void OnMonitor() override {}

 private:
  LibXR::UART *uart_;
  LibXR::RawData tx_buffer_;
  LibXR::LockFreeQueue<uint8_t> tx_queue_;
  LibXR::RawData *topics_pack_buffer_;
  LibXR::Semaphore tx_sem_;
  LibXR::Thread tx_thread_;
};
