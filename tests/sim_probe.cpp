
#include <dlfcn.h>

#include <anakin_env/actuation_limits.hpp>
#include <anakin_env/transport.hpp>
#include <anakin_sdk/msg/control_target.hpp>
#include <anakin_sdk/msg/inference_status.hpp>
#include <anakin_sdk/simulation_profile.hpp>
#include <atomic>
#include <csignal>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <set>
#include <thread>
// Xlib defines Status/None macros; keep this block after the ROS headers.
// clang-format off
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#undef Status
#undef None
// clang-format on
using namespace std::chrono_literals;
namespace {
volatile std::sig_atomic_t stopping = 0;
void on_signal(int) { stopping = 1; }
using namespace anakin;
using namespace anakin::env;
using Status = anakin_sdk::msg::InferenceStatus;
class Probe final : public rclcpp::Node {
 public:
  Probe() : rclcpp::Node("anakin_sim_probe") {
    domain_ = declare_parameter<int>("sdk_domain", 31);
    alignment_seconds_ = declare_parameter<double>("alignment_seconds", 2.);
    simulator_pid_ = declare_parameter<int>("simulator_pid", 0);
    suspended_ = declare_parameter<bool>("suspended", false);
    demo_ = declare_parameter<bool>("demo_switch", false);
    rc_loss_ = declare_parameter<bool>("rc_loss", false);
    result_ = declare_parameter<std::string>("result_json", "");
    require_loopback("lo", domain_);
    unitree::robot::ChannelFactory::Instance()->Init(domain_, "lo");
    remote_ = std::make_unique<unitree::robot::ChannelPublisher<Wireless>>("rt/wirelesscontroller");
    remote_->InitChannel();
    lowcmd_ = std::make_unique<unitree::robot::ChannelSubscriber<LowCommand>>("rt/lowcmd");
    lowcmd_->InitChannel(
        [this](const void* data) {
          const auto& command = *static_cast<const LowCommand*>(data);
          std::lock_guard<std::mutex> lock(mutex_);
          ++observed_commands_;
          if (command.crc() != command_crc(command) || command.mode_machine() != 5 ||
              command.mode_pr() != 0)
            ++bad_commands_;
          bool damping = true;
          for (size_t i = 0; i < 29; ++i) {
            const auto& motor = command.motor_cmd()[i];
            if (!std::isfinite(motor.q()) || !std::isfinite(motor.kp()) ||
                !std::isfinite(motor.kd()))
              ++bad_commands_;
            damping = damping && motor.kp() == 0 && motor.kd() > 0 && motor.tau() == 0;
          }
          if (damping)
            ++observed_damping_;
          else {
            ++observed_position_;
            if (fault_seen_ != Clock::time_point{} && Clock::now() - fault_seen_ > 20ms)
              ++late_position_after_fault_;
            for (auto it = targets_.rbegin(); it != targets_.rend(); ++it) {
              bool matches = known_execution_contract(it->execution_profile, it->command_semantics,
                                                      it->gain_profile);
              if (!matches) continue;
              const auto mode = execution_encoding(it->execution_profile, it->command_semantics);
              const auto& gains = simulation_gains(it->gain_profile, it->model_id);
              const auto lower = reference_lower_limits(), upper = reference_upper_limits();
              for (size_t i = 0; i < 29; ++i) {
                const double mux = it->target.points[0].positions[i];
                float expected_q = static_cast<float>(mux), expected_tau = 0;
                if (mode != ExecutionEncoding::StrictPosition) {
                  expected_q = static_cast<float>(std::clamp(mux, lower[i], upper[i]));
                  if (expected_q < lower[i])
                    expected_q = std::nextafter(expected_q, std::numeric_limits<float>::infinity());
                  if (expected_q > upper[i])
                    expected_q =
                        std::nextafter(expected_q, -std::numeric_limits<float>::infinity());
                  if (mode == ExecutionEncoding::EquivalentReference)
                    expected_tau = static_cast<float>(
                        static_cast<double>(static_cast<float>(gains.kp[i])) *
                        (static_cast<double>(static_cast<float>(mux)) - expected_q));
                }
                const auto& motor = command.motor_cmd()[i];
                matches = matches && std::abs(motor.q() - expected_q) < 1e-6f &&
                          std::abs(motor.tau() - expected_tau) < 1e-6f &&
                          motor.kp() == static_cast<float>(gains.kp[i]) &&
                          motor.kd() == static_cast<float>(gains.kd[i]);
              }
              if (matches) {
                ++matched_targets_;
                break;
              }
            }
          }
        },
        1);
    frame_sub_ = create_subscription<Frame>(
        "anakin/state", rclcpp::QoS(1).best_effort().durability_volatile(),
        [this](Frame::ConstSharedPtr frame) {
          frame_ = frame;
          ++frames_;
          if (frame->sdk_published_epoch == frame->epoch && (frame->sdk_published_phase_mask & 4))
            sdk_active_epochs_.insert(frame->epoch);
        });
    target_sub_ = create_subscription<Target>(
        "anakin/control_target", rclcpp::QoS(1).best_effort().durability_volatile(),
        [this](Target::ConstSharedPtr target) {
          if (target->target.points.size() != 1 || target->target.points[0].positions.size() != 29)
            return;
          std::lock_guard<std::mutex> lock(mutex_);
          targets_.push_back(*target);
          if (targets_.size() > 64) targets_.pop_front();
        });
    status_sub_ = create_subscription<Status>("anakin/inference_status",
                                              rclcpp::QoS(128).reliable().durability_volatile(),
                                              [this](Status::ConstSharedPtr status) {
                                                if (status->env_sdk_published_phase_mask & 4)
                                                  mc_ack_epochs_.insert(status->epoch);
                                              });
  }
  ~Probe() override {
    if (lowcmd_) lowcmd_->CloseChannel();
    if (remote_) remote_->CloseChannel();
    if (display_) XCloseDisplay(display_);
    if (xtest_) dlclose(xtest_);
  }
  int execute() {
    const auto started = Clock::now();
    auto pulse_until = started, next = started;
    uint16_t pulse_keys = 0;
    enum class Stage { Baseline, Reset, Align, Hold, Run };
    Stage stage = Stage::Baseline;
    bool reset_sent = false, loss_sent = false, expected_fault = false;
    uint32_t before_reset = 0;
    Clock::time_point first_active{}, stage_started = started;
    std::string failure;
    while (!stopping && rclcpp::ok()) {
      rclcpp::spin_some(shared_from_this());
      const auto now = Clock::now();
      const double elapsed = std::chrono::duration<double>(now - started).count();
      if (elapsed > 35) {
        failure = "probe deadline";
        break;
      }
      if (frame_ && frame_->execution_mode == Frame::MODE_FAULT) {
        if (rc_loss_ && loss_sent && !frame_->diagnostics.status.empty() &&
            frame_->diagnostics.status[0].message.find("Remote age exceeded 250 ms") !=
                std::string::npos) {
          expected_fault = true;
          std::lock_guard<std::mutex> lock(mutex_);
          fault_seen_ = Clock::now();
          loss_to_fault_ms_ =
              std::chrono::duration<double, std::milli>(fault_seen_ - loss_started_).count();
        } else
          failure = "native execution fault";
        break;
      }
      if (stage == Stage::Baseline && frame_ && frames_ >= 20 && elapsed > .3) {
        {
          std::lock_guard<std::mutex> lock(mutex_);
          if (observed_commands_ || frame_->sdk_publish_count) {
            failure = "command before explicit RC authorization";
            break;
          }
        }
        zero_before_authorization_ = true;
        {
          if (frame_->sdk_tick < 300) {
            next += 20ms;
            std::this_thread::sleep_until(next);
            continue;
          }
          if (!reset_sent) {
            before_reset = frame_->sdk_tick;
            reset_own_simulator();
            reset_sent = true;
            stage = Stage::Reset;
            stage_started = now;
          }
        }
      } else if (stage == Stage::Reset && frame_) {
        const auto& q = frame_->imu.orientation;
        if (frame_->sdk_tick < before_reset && frame_->sdk_tick < 150 &&
            q.x * q.x + q.y * q.y < .02) {
          reset_confirmed_ = true;
          pulse_keys = 3u | (1u << 10);
          pulse_until = now + 100ms;
          stage = Stage::Align;
          stage_started = now;
        } else if (now - stage_started > 1s) {
          failure = "initial simulator reset not confirmed";
          break;
        }
      } else if (stage == Stage::Align && frame_ && frame_->execution_mode == Frame::MODE_HOLD) {
        stage = Stage::Hold;
        stage_started = now;
      } else if (stage == Stage::Hold && now - stage_started > 100ms && !loss_sent) {
        if (rc_loss_) {
          loss_sent = true;
          loss_started_ = now;
        } else {
          pulse_keys = 3u | (1u << 10);
          pulse_until = now + 100ms;
          stage = Stage::Run;
          stage_started = now;
        }
      }
      if (frame_ && frame_->execution_mode == Frame::MODE_ACTIVE &&
          (frame_->sdk_published_phase_mask & 4)) {
        if (first_active == Clock::time_point{}) {
          first_active = now;
          std::cout << "PROBE actual SDK target publication observed; epoch=" << frame_->epoch
                    << std::endl;
        }
        if (demo_ && !mimic_requested_ && now - first_active > 1s) {
          pulse_keys = 1u | (1u << 12);
          pulse_until = now + 100ms;
          mimic_requested_ = true;
        }
        if (rc_loss_ && !loss_sent && now - first_active > 1s) {
          loss_sent = true;
          loss_started_ = now;
        }
      }
      if (!loss_sent) {
        Wireless remote;
        remote.keys() = now < pulse_until ? pulse_keys : 0;
        if (!remote_->Write(remote, 0)) {
          failure = "RC simulation publication rejected";
          break;
        }
      }
      if (stage == Stage::Align &&
          std::chrono::duration<double>(now - stage_started).count() > alignment_seconds_ + 2.) {
        failure = "alignment did not reach hold";
        break;
      }
      if (stage == Stage::Run && first_active == Clock::time_point{} && now - stage_started > 6s) {
        failure = "no actual active SDK publication";
        break;
      }
      if (loss_sent && now - loss_started_ > 1s) {
        failure = "RC loss did not stop within bound";
        break;
      }
      next += 20ms;
      if (next < now) next = now + 20ms;
      std::this_thread::sleep_until(next);
    }
    if (expected_fault) {
      const auto end = Clock::now() + 150ms;
      while (Clock::now() < end) {
        rclcpp::spin_some(shared_from_this());
        std::this_thread::sleep_for(5ms);
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool policy_pass = matched_targets_ > 20 &&
                             sdk_active_epochs_.size() >= (demo_ ? 3u : 1u) &&
                             mc_ack_epochs_.size() >= (demo_ ? 3u : 1u);
    const bool pass = failure.empty() && zero_before_authorization_ && observed_commands_ > 100 &&
                      bad_commands_ == 0 &&
                      (rc_loss_ ? (expected_fault && observed_damping_ > 0 &&
                                   late_position_after_fault_ == 0 && loss_to_fault_ms_ < 350.)
                                : policy_pass);
    std::ofstream out(result_);
    out << std::boolalpha << "{\n  \"success\": " << pass << ",\n  \"suspended\": " << suspended_
        << ",\n  \"zero_commands_before_authorization\": " << zero_before_authorization_
        << ",\n  \"reset_confirmed\": " << reset_confirmed_
        << ",\n  \"observed_native_commands\": " << observed_commands_
        << ",\n  \"observed_position_commands\": " << observed_position_
        << ",\n  \"observed_damping_commands\": " << observed_damping_
        << ",\n  \"matched_canonical_targets\": " << matched_targets_
        << ",\n  \"bad_native_commands\": " << bad_commands_
        << ",\n  \"sdk_active_epochs\": " << sdk_active_epochs_.size()
        << ",\n  \"mc_sdk_ack_epochs\": " << mc_ack_epochs_.size()
        << ",\n  \"expected_remote_loss_fault\": " << expected_fault
        << ",\n  \"remote_loss_to_fault_ms\": " << loss_to_fault_ms_
        << ",\n  \"position_commands_after_fault_bound\": " << late_position_after_fault_
        << ",\n  \"failure\": " << std::quoted(failure) << "\n}\n";
    std::cout << "PROBE_RESULT success=" << pass << " observed=" << observed_commands_
              << " matched=" << matched_targets_ << " sdk_epochs=" << sdk_active_epochs_.size()
              << " ack_epochs=" << mc_ack_epochs_.size() << " failure=" << failure << std::endl;
    return pass ? 0 : 1;
  }

 private:
  void reset_own_simulator() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) throw std::runtime_error("Cannot connect to private simulation display");
    Window root = DefaultRootWindow(display_), parent, *children = nullptr, window = 0;
    unsigned count = 0;
    XQueryTree(display_, root, &root, &parent, &children, &count);
    const Atom pid_atom = XInternAtom(display_, "_NET_WM_PID", False);
    for (unsigned i = 0; i < count; ++i) {
      Atom type;
      int format;
      unsigned long size, after;
      unsigned char* data = nullptr;
      if (XGetWindowProperty(display_, children[i], pid_atom, 0, 1, False, XA_CARDINAL, &type,
                             &format, &size, &after, &data) == Success &&
          data) {
        if (size == 1 && format == 32 &&
            *reinterpret_cast<unsigned long*>(data) == static_cast<unsigned long>(simulator_pid_))
          window = children[i];
        XFree(data);
      }
    }
    if (children) XFree(children);
    if (!window) throw std::runtime_error("Owned official simulator window was not found");
    xtest_ = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!xtest_) throw std::runtime_error("XTest unavailable");
    using FakeKey = Bool (*)(Display*, unsigned int, Bool, unsigned long);
    auto key = reinterpret_cast<FakeKey>(dlsym(xtest_, "XTestFakeKeyEvent"));
    if (!key) throw std::runtime_error("XTest key injection unavailable");
    XSetInputFocus(display_, window, RevertToParent, CurrentTime);
    const auto code = XKeysymToKeycode(display_, XK_BackSpace);
    key(display_, code, True, 0);
    key(display_, code, False, 0);
    XSync(display_, False);
  }
  int domain_ = 31, simulator_pid_ = 0;
  double alignment_seconds_ = 2.;
  bool suspended_ = false, demo_ = false, rc_loss_ = false, zero_before_authorization_ = false,
       reset_confirmed_ = false, mimic_requested_ = false;
  uint64_t frames_ = 0, observed_commands_ = 0, observed_position_ = 0, observed_damping_ = 0,
           matched_targets_ = 0, bad_commands_ = 0;
  std::mutex mutex_;
  Clock::time_point loss_started_{}, fault_seen_{};
  uint64_t late_position_after_fault_ = 0;
  double loss_to_fault_ms_ = 0.;
  std::string result_;
  Frame::ConstSharedPtr frame_;
  std::deque<Target> targets_;
  std::set<uint64_t> sdk_active_epochs_, mc_ack_epochs_;
  Display* display_ = nullptr;
  void* xtest_ = nullptr;
  std::unique_ptr<unitree::robot::ChannelPublisher<Wireless>> remote_;
  std::unique_ptr<unitree::robot::ChannelSubscriber<LowCommand>> lowcmd_;
  rclcpp::Subscription<Frame>::SharedPtr frame_sub_;
  rclcpp::Subscription<Target>::SharedPtr target_sub_;
  rclcpp::Subscription<Status>::SharedPtr status_sub_;
};
}  // namespace
int main(int argc, char** argv) {
  try {
    rclcpp::init(argc, argv, rclcpp::InitOptions{}, rclcpp::SignalHandlerOptions::None);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    auto probe = std::make_shared<Probe>();
    const int result = probe->execute();
    probe.reset();
    rclcpp::shutdown();
    return result;
  } catch (const std::exception& error) {
    std::cerr << "sim probe: " << error.what() << '\n';
    return 1;
  }
}
