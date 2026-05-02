#pragma once

// clang-format off
/* === MODULE MANIFEST V2 ===
module_description: SharedTopicClient 是一个多 Topic 数据共享与串口转发客户端模块。它用于通过 UART 将多个 Topic 的数据统一打包、发送，实现消息流的串口透明同步转发，适用于分布式系统的多主题数据同步或边缘数据采集。 / SharedTopicClient is a client module for multi-topic data sharing and transparent UART forwarding. It subscribes to multiple Topics, packs their updates, and transmits them via UART, enabling efficient and reliable message synchronization over serial connections—ideal for distributed systems or edge data acquisition.
constructor_args:
  - uart_name: "uart_cdc"
  - buffer_size: 256
  - topic_configs:
    - "topic1"
    - ["topic2", "libxr_def_domain"]
template_args: []
required_hardware: uart_name
depends: []
=== END MANIFEST === */
// clang-format on

#include <cstddef>
#include <cstdint>

#include "app_framework.hpp"
#include "message.hpp"
#include "uart.hpp"

class SharedTopicClient : public LibXR::Application {
 public:
  struct CallbackInfo {
    SharedTopicClient* client;
    uint32_t topic_crc32;
    uint32_t index;
  };

  struct TopicConfig {
    const char* name;
    const char* domain = "libxr_def_domain";

    TopicConfig(const char* name) : name(name) {}

    TopicConfig(const char* name, const char* domain)
        : name(name), domain(domain) {}
  };

  SharedTopicClient(LibXR::HardwareContainer& hw,
                    LibXR::ApplicationManager& app, const char* uart_name,
                    uint32_t buffer_size,
                    std::initializer_list<TopicConfig> topic_configs)
      : uart_(hw.template Find<LibXR::UART>(uart_name)) {
    ASSERT(uart_ != nullptr);
    ASSERT(uart_->write_port_ != nullptr);
    ASSERT(uart_->write_port_->queue_data_ != nullptr);

    topics_pack_buffer_ = new LibXR::RawData[topic_configs.size()];
    uint32_t i = 0;
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
      ASSERT(packet_size <= buffer_size);
      max_packet_size = LibXR::max(max_packet_size, packet_size);
      topics_pack_buffer_[i] = LibXR::RawData(
          new uint8_t[packet_size], packet_size);

      void (*func)(bool, CallbackInfo, LibXR::MicrosecondTimestamp,
                   LibXR::ConstRawData&) =
          [](bool in_isr, CallbackInfo info,
             LibXR::MicrosecondTimestamp timestamp, LibXR::ConstRawData& data) {
            auto& buffer = info.client->topics_pack_buffer_[info.index];
            ASSERT(data.size_ + LibXR::Topic::PACK_BASE_SIZE <= buffer.size_);
            LibXR::Topic::PackData(info.topic_crc32, buffer, timestamp, data);

            LibXR::WriteOperation op;
            auto ans = info.client->uart_->Write(
                {buffer.addr_, data.size_ + LibXR::Topic::PACK_BASE_SIZE}, op,
                in_isr);
            UNUSED(ans);
          };

      auto msg_cb = LibXR::Topic::Callback::Create(
          func, CallbackInfo{this, ans->data_.crc32, i});

      LibXR::Topic topic(ans);

      topic.RegisterCallback(msg_cb);

      i++;
    }

    ASSERT(max_packet_size <= uart_->write_port_->queue_data_->MaxSize());

    app.Register(*this);
  }

  void OnMonitor() override {}

 private:
  LibXR::UART* uart_;
  LibXR::RawData* topics_pack_buffer_;
};
