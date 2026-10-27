#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <anakin_env/transport.hpp>
#include <cstring>

namespace anakin::env {
builtin_interfaces::msg::Time system_stamp() {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  builtin_interfaces::msg::Time stamp;
  stamp.sec = static_cast<int32_t>(ns / 1000000000);
  stamp.nanosec = static_cast<uint32_t>(ns % 1000000000);
  return stamp;
}
void require_loopback(const std::string& interface, int domain) {
  if (interface != "lo" || domain < 1 || domain > 232)
    throw std::runtime_error("Only explicit lo simulation with a nonzero DDS domain is supported");
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error("Cannot inspect loopback interface");
  ifreq request{};
  std::strncpy(request.ifr_name, interface.c_str(), IFNAMSIZ - 1);
  const int result = ioctl(fd, SIOCGIFFLAGS, &request);
  close(fd);
  if (result < 0 || !(request.ifr_flags & IFF_LOOPBACK) || !(request.ifr_flags & IFF_UP))
    throw std::runtime_error("SDK interface is not an active loopback interface");
}
RemoteSample decode_remote(const std::array<uint8_t, 40>& bytes) {
  RemoteSample r;
  if (bytes[0] != 0xfe || bytes[1] != 0xef) return r;
  r.keys = static_cast<uint16_t>(bytes[2]) | static_cast<uint16_t>(bytes[3]) << 8;
  const size_t offsets[] = {4, 20, 8, 12};
  for (size_t i = 0; i < 4; ++i) {
    std::memcpy(&r.axes[i], bytes.data() + offsets[i], sizeof(float));
    if (!std::isfinite(r.axes[i]) || std::abs(r.axes[i]) > 1.001f)
      throw std::runtime_error("Malformed native remote axis");
  }
  r.acquired = Clock::now();
  r.stamp = system_stamp();
  r.valid = true;
  return r;
}
NativeTransport::~NativeTransport() {
  if (state_subscriber_) state_subscriber_->CloseChannel();
  if (remote_subscriber_) remote_subscriber_->CloseChannel();
  if (odometry_subscriber_) odometry_subscriber_->CloseChannel();
  if (publisher_) publisher_->CloseChannel();
}
void NativeTransport::start_loopback(const std::string& interface, int domain,
                                     bool wireless_topic) {
  require_loopback(interface, domain);
  if (started_) throw std::runtime_error("Native transport already started");
  wireless_topic_ = wireless_topic;
  unitree::robot::ChannelFactory::Instance()->Init(domain, interface);
  publisher_ = std::make_unique<unitree::robot::ChannelPublisher<LowCommand>>("rt/lowcmd");
  publisher_->InitChannel();
  state_subscriber_ = std::make_unique<unitree::robot::ChannelSubscriber<LowState>>("rt/lowstate");
  state_subscriber_->InitChannel(
      [this](const void* message) {
        try {
          const auto& low = *static_cast<const LowState*>(message);
          auto frame = decode_state(low);
          if (low.mode_machine() != 5)
            throw std::runtime_error("Expected G1 29-motor machine mode 5");
          const auto acquired = Clock::now();
          const auto stamp = system_stamp();
          frame.header.stamp = stamp;
          frame.joints.header.stamp = stamp;
          frame.imu.header.stamp = stamp;
          std::lock_guard<std::mutex> lock(mutex_);
          // A duplicate SDK tick never renews source-state freshness.
          if (sample_ && low.tick() == sample_->frame.sdk_tick) return;
          if (sample_ && low.tick() < sample_->frame.sdk_tick) ++resets_;
          sample_ = NativeSample{std::move(frame), acquired, low.mode_machine(), ++generation_};
          if (!wireless_topic_) remote_ = decode_remote(low.wireless_remote());
        } catch (const std::exception&) {
          ++bad_;
        }
      },
      1);
  if (wireless_topic_) {
    remote_subscriber_ =
        std::make_unique<unitree::robot::ChannelSubscriber<Wireless>>("rt/wirelesscontroller");
    remote_subscriber_->InitChannel(
        [this](const void* message) {
          const auto& w = *static_cast<const Wireless*>(message);
          RemoteSample remote;
          remote.axes = {w.lx(), w.ly(), w.rx(), w.ry()};
          remote.keys = w.keys();
          for (float value : remote.axes)
            if (!std::isfinite(value) || std::abs(value) > 1.001f) {
              ++bad_;
              return;
            }
          remote.acquired = Clock::now();
          remote.stamp = system_stamp();
          remote.valid = true;
          std::lock_guard<std::mutex> lock(mutex_);
          remote_ = remote;
        },
        1);
  }
  odometry_subscriber_ =
      std::make_unique<unitree::robot::ChannelSubscriber<Sport>>("rt/sportmodestate");
  odometry_subscriber_->InitChannel(
      [this](const void* message) {
        const auto& s = *static_cast<const Sport*>(message);
        OdometrySample o;
        o.position = s.position();
        o.velocity = s.velocity();
        for (size_t i = 0; i < 3; ++i)
          if (!std::isfinite(o.position[i]) || !std::isfinite(o.velocity[i])) {
            ++bad_;
            return;
          }
        o.acquired = Clock::now();
        o.stamp = system_stamp();
        o.valid = true;
        std::lock_guard<std::mutex> lock(mutex_);
        odometry_ = o;
      },
      1);
  started_ = true;
}
std::optional<NativeSample> NativeTransport::sample() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sample_;
}
RemoteSample NativeTransport::remote() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return remote_;
}
OdometrySample NativeTransport::odometry() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return odometry_;
}
bool NativeTransport::write(const LowCommand& command) {
  if (!started_ || !publisher_) return false;
  return publisher_->Write(command, 0);
}
}  // namespace anakin::env
