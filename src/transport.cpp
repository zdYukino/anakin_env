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
void validate_transport_endpoint(RuntimeMode mode, const std::string& interface, int domain) {
  if (interface.empty() || interface.size() >= IFNAMSIZ ||
      interface.find_first_of(" \t\r\n/") != std::string::npos || domain < 0 || domain > 232)
    throw std::runtime_error("Expected explicit valid NIC and DDS domain 0..232");
  if (mode == RuntimeMode::Sim && (interface != "lo" || domain == 0))
    throw std::runtime_error("Simulation requires explicit lo and a nonzero DDS domain");
  if (mode == RuntimeMode::Real && interface == "lo")
    throw std::runtime_error("Real HG requires an explicit robot-facing non-loopback NIC");
}
namespace {
void require_interface(RuntimeMode mode, const std::string& interface, int domain) {
  validate_transport_endpoint(mode, interface, domain);
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error("Cannot inspect configured SDK interface");
  ifreq request{};
  std::strncpy(request.ifr_name, interface.c_str(), IFNAMSIZ - 1);
  const int result = ioctl(fd, SIOCGIFFLAGS, &request);
  close(fd);
  const bool loopback = request.ifr_flags & IFF_LOOPBACK;
  if (result < 0 || !(request.ifr_flags & IFF_UP) || loopback != (mode == RuntimeMode::Sim))
    throw std::runtime_error("Configured SDK interface is unavailable/down or has the wrong loopback type: " + interface);
}
}  // namespace
void require_loopback(const std::string& interface, int domain) { require_interface(RuntimeMode::Sim, interface, domain); }
Frame decode_g1_feedback(const LowState& state) {
  if (state.mode_machine() != 5) throw std::runtime_error("Only G1 canonical29 HG machine mode 5 is supported");
  return decode_state(state);
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
  ModeConfig config;
  config.mode = RuntimeMode::Sim;
  config.transport.interface = interface;
  config.transport.sdk_domain = domain;
  config.transport.odometry_topic = "rt/sportmodestate";
  start(config, wireless_topic);
}
void NativeTransport::start(const ModeConfig& config, bool wireless_topic) {
  const auto& interface = config.transport.interface;
  const auto domain = config.transport.sdk_domain;
  require_interface(config.mode, interface, domain);
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
          auto frame = decode_g1_feedback(low);
          const auto acquired = Clock::now();
          const auto stamp = system_stamp();
          frame.header.stamp = stamp;
          frame.joints.header.stamp = stamp;
          frame.imu.header.stamp = stamp;
          std::lock_guard<std::mutex> lock(mutex_);
          feedback_.observe(std::move(frame), low.mode_machine(), acquired);
          resets_ = feedback_.rollbacks();  // Diagnostic counter; runtime selects the guard.
          if (!wireless_topic_) {
            remote_ = decode_remote(low.wireless_remote());
            if (remote_sink_) remote_sink_(remote_);
          }
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
          if (remote_sink_) remote_sink_(remote_);
        },
        1);
  }
  odometry_subscriber_ =
      std::make_unique<unitree::robot::ChannelSubscriber<Sport>>(config.transport.odometry_topic);
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
  return feedback_.sample();
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
  LowCommand packet = command;
  finalize_command(packet);
  return publisher_->Write(packet, 0);
}
}  // namespace anakin::env
