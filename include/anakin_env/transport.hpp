#pragma once
#include <anakin_env/control.hpp>
#include <anakin_env/native_codec.hpp>
#include <anakin_env/feedback.hpp>
#include <anakin_env/config.hpp>
#include <atomic>
#include <mutex>
#include <optional>
#include <functional>
#include <unitree/idl/go2/SportModeState_.hpp>
#include <unitree/idl/go2/WirelessController_.hpp>
#include <unitree/robot/channel/channel_publisher.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

namespace anakin::env {
using LowState = unitree_hg::msg::dds_::LowState_;
using LowCommand = unitree_hg::msg::dds_::LowCmd_;
using Wireless = unitree_go::msg::dds_::WirelessController_;
using Sport = unitree_go::msg::dds_::SportModeState_;
struct OdometrySample {
  std::array<float, 3> position{}, velocity{};
  Clock::time_point acquired{};
  builtin_interfaces::msg::Time stamp;
  bool valid = false;
};
builtin_interfaces::msg::Time system_stamp();
void require_loopback(const std::string& interface, int domain);
void validate_transport_endpoint(RuntimeMode mode, const std::string& interface, int domain);
Frame decode_g1_feedback(const LowState& state);
RemoteSample decode_remote(const std::array<uint8_t, 40>& bytes);
class NativeTransport {
 public:
  NativeTransport() = default;  // construction opens no channel
  ~NativeTransport();
  void start_loopback(const std::string& interface, int domain, bool wireless_topic);
  void start(const ModeConfig& config, bool wireless_topic);
  void set_remote_sink(std::function<void(const RemoteSample&)> sink) {
    if (started_) throw std::logic_error("Set remote sink before starting transport");
    remote_sink_ = std::move(sink);
  }
  std::optional<NativeSample> sample() const;
  RemoteSample remote() const;
  OdometrySample odometry() const;
  bool write(const LowCommand& command);
  uint64_t bad_packets() const { return bad_.load(); }
  uint64_t resets() const { return resets_.load(); }

 private:
  mutable std::mutex mutex_;
  FeedbackTracker feedback_;
  std::function<void(const RemoteSample&)> remote_sink_;
  RemoteSample remote_;
  OdometrySample odometry_;
  std::atomic<uint64_t> bad_{0}, resets_{0};
  bool started_ = false, wireless_topic_ = false;
  std::unique_ptr<unitree::robot::ChannelPublisher<LowCommand>> publisher_;
  std::unique_ptr<unitree::robot::ChannelSubscriber<LowState>> state_subscriber_;
  std::unique_ptr<unitree::robot::ChannelSubscriber<Wireless>> remote_subscriber_;
  std::unique_ptr<unitree::robot::ChannelSubscriber<Sport>> odometry_subscriber_;
};
}  // namespace anakin::env
