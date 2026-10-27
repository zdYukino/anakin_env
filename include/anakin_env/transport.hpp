#pragma once
#include <anakin_env/control.hpp>
#include <anakin_env/native_codec.hpp>
#include <atomic>
#include <mutex>
#include <optional>
#include <unitree/idl/go2/SportModeState_.hpp>
#include <unitree/idl/go2/WirelessController_.hpp>
#include <unitree/robot/channel/channel_publisher.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

namespace anakin::env {
using LowState = unitree_hg::msg::dds_::LowState_;
using LowCommand = unitree_hg::msg::dds_::LowCmd_;
using Wireless = unitree_go::msg::dds_::WirelessController_;
using Sport = unitree_go::msg::dds_::SportModeState_;
struct NativeSample {
  Frame frame;
  Clock::time_point acquired{};
  uint8_t mode_machine = 0;
  uint64_t generation = 0;
};
struct OdometrySample {
  std::array<float, 3> position{}, velocity{};
  Clock::time_point acquired{};
  builtin_interfaces::msg::Time stamp;
  bool valid = false;
};
builtin_interfaces::msg::Time system_stamp();
void require_loopback(const std::string& interface, int domain);
RemoteSample decode_remote(const std::array<uint8_t, 40>& bytes);
class NativeTransport {
 public:
  NativeTransport() = default;  // construction opens no channel
  ~NativeTransport();
  void start_loopback(const std::string& interface, int domain, bool wireless_topic);
  std::optional<NativeSample> sample() const;
  RemoteSample remote() const;
  OdometrySample odometry() const;
  bool write(const LowCommand& command);
  uint64_t bad_packets() const { return bad_.load(); }
  uint64_t resets() const { return resets_.load(); }

 private:
  mutable std::mutex mutex_;
  std::optional<NativeSample> sample_;
  RemoteSample remote_;
  OdometrySample odometry_;
  std::atomic<uint64_t> bad_{0}, resets_{0};
  bool started_ = false, wireless_topic_ = false;
  uint64_t generation_ = 0;
  std::unique_ptr<unitree::robot::ChannelPublisher<LowCommand>> publisher_;
  std::unique_ptr<unitree::robot::ChannelSubscriber<LowState>> state_subscriber_;
  std::unique_ptr<unitree::robot::ChannelSubscriber<Wireless>> remote_subscriber_;
  std::unique_ptr<unitree::robot::ChannelSubscriber<Sport>> odometry_subscriber_;
};
}  // namespace anakin::env
