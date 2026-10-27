// No ROS context, SDK channel factory, native transport, threads, or simulator.
// This records calls to the same guarded_send orchestration used by NativeNode.
#include <anakin_env/execution.hpp>
#include <anakin_env/reference.hpp>
#include <anakin_env/send.hpp>
#include <functional>
#include <iostream>
#include <vector>
using namespace anakin;
using namespace anakin::env;
using namespace std::chrono_literals;
using Command = unitree_hg::msg::dds_::LowCmd_;
void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
struct Harness {
  Clock::time_point now = Clock::time_point{} + 1s;
  SendSnapshot snapshot;
  uint64_t observations = 0, encodings = 0, attempts = 0, successes = 0;
  uint64_t position_writes = 0, damping_writes = 0;
  std::vector<Clock::time_point> sent_at;
  bool transport_accepts = true, transport_throws = false, bind_feedback = false;
  std::function<void()> during_encoding, after_first_observation, on_transport;
  Harness() {
    snapshot.position_authorized = true;
    snapshot.state_available = true;
    snapshot.remote_valid = true;
    snapshot.feedback_integrity_ok = true;
    snapshot.target_authorized = true;
    snapshot.authorization_generation = 7;
    snapshot.feedback_generation = 10;
    snapshot.state_acquired = now;
    snapshot.remote_acquired = now;
    snapshot.target_deadline = now + 40ms;
  }
  SendRequest request(bool damping = false, bool target = true) const {
    return {damping, target, snapshot.authorization_generation, Clock::time_point::max()};
  }
  SendResult send(const SendRequest& request) {
    return guarded_send<Command>(
        request, [&] { return now; },
        [&] {
          const auto copy = snapshot;
          ++observations;
          if (observations == 1 && after_first_observation) after_first_observation();
          return copy;
        },
        [&](const SendSnapshot& before) {
          ++encodings;
          PreparedSend<Command> prepared{
              request.damping ? encode_damping(5) : encode_position(Positions{}, 5), std::nullopt};
          if (bind_feedback)
            prepared.feedback = EncodedFeedback{before.feedback_generation, before.state_acquired};
          if (during_encoding) during_encoding();
          return prepared;
        },
        [&](const Command& command) {
          ++attempts;
          check(command.crc() == command_crc(command), "Recorded command CRC");
          if (on_transport) on_transport();
          if (transport_throws) throw std::runtime_error("recorded transport failure");
          if (!transport_accepts) return false;
          ++successes;
          if (command.motor_cmd()[0].kp() == 0)
            ++damping_writes;
          else
            ++position_writes;
          sent_at.push_back(now);
          return true;
        });
  }
};
int main() {
  try {
    size_t cases = 0;
    {
      Harness h;
      auto r = h.send(h.request());
      check(r.written() && h.encodings == 1 && h.attempts == 1 && h.successes == 1 &&
                h.position_writes == 1 && h.damping_writes == 0,
            "Healthy call writes once");
      ++cases;
    }
    {
      Harness h;
      h.during_encoding = [&] { h.now += 40ms; };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::TargetExpired && r.phase == SendPhase::AfterEncoding &&
                h.encodings == 1 && h.attempts == 0,
            "Target expires after encoding; zero transport calls");
      ++cases;
    }
    {
      Harness h;
      h.snapshot.target_deadline = h.now + 1s;
      h.during_encoding = [&] { h.now += 41ms; };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::StateStale && h.attempts == 0,
            "State expires after encoding even when target appears live");
      ++cases;
    }
    {
      Harness h;
      h.snapshot.target_deadline = h.now + 1s;
      h.during_encoding = [&] {
        h.now += 251ms;
        h.snapshot.state_acquired = h.now;
      };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::RemoteStale && h.attempts == 0,
            "Remote expires after encoding");
      ++cases;
    }
    {
      Harness h;
      h.bind_feedback = true;
      h.during_encoding = [&] {
        h.now += 1ms;
        ++h.snapshot.feedback_generation;
        h.snapshot.state_acquired = h.now;
      };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::FeedbackChanged && h.attempts == 0,
            "Fresh changed feedback discards a state-dependent encoding");
      h.during_encoding = {};
      const auto fresh = h.send(h.request());
      check(fresh.written() && h.successes == 1,
            "No same-call retry burst; next explicit attempt re-encodes fresh feedback");
      ++cases;
    }
    {
      Harness h;
      h.bind_feedback = true;
      h.snapshot.target_deadline = h.now + 1s;
      h.during_encoding = [&] {
        h.now += 41ms;
        h.snapshot.state_acquired = h.now;
      };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::StateStale && h.attempts == 0,
            "Fresh observer cannot disguise expired feedback used for encoding");
      ++cases;
    }
    {
      Harness h;
      h.during_encoding = [&] { h.snapshot.feedback_integrity_ok = false; };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::FeedbackInvalid && h.attempts == 0,
            "Feedback CRC/reset integrity change prevents a send");
      ++cases;
    }
    {
      Harness h;
      h.during_encoding = [&] {
        require_native_reference_budget(equivalent_execution_profile, pd_reference,
                                        frozen_reference_budget());
      };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::EncodingFailure &&
                r.detail.find("per_joint_velocity_limit") != std::string::npos && h.attempts == 0,
            "Actual missing reference budget rejects before transport");
      ++cases;
    }
    {
      Harness h;
      h.during_encoding = [&] {
        h.snapshot.position_authorized = false;
        ++h.snapshot.authorization_generation;
      };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::Revoked && h.attempts == 0,
            "Local stop during encoding revokes position writes");
      ++cases;
    }
    {
      Harness h;
      h.during_encoding = [&] { h.snapshot.stop_requested = true; };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::RemoteStop && h.attempts == 0,
            "A/B remote stop observed after encoding prevents a position call");
      ++cases;
    }
    {
      Harness h;
      h.during_encoding = [&] {
        h.now += 251ms;
        h.snapshot.state_acquired = h.now;
        h.snapshot.stop_requested = true;
        h.snapshot.target_deadline = h.now + 40ms;
      };
      const auto r = h.send(h.request());
      check(r.status == SendStatus::RemoteStale && h.attempts == 0,
            "Expired RC remains a freshness fault even if a stop bit is present");
      ++cases;
    }
    {
      Harness h;
      const auto old = h.request();
      check(h.send(old).written(), "Initial receipt");
      h.snapshot.position_authorized = false;
      ++h.snapshot.authorization_generation;
      check(h.send(old).status == SendStatus::Revoked && h.successes == 1 && h.attempts == 1,
            "Stopped old request cannot increase send count");
      h.snapshot.position_authorized = true;
      ++h.snapshot.authorization_generation;
      check(h.send(old).status == SendStatus::Revoked && h.successes == 1,
            "New authorization cannot revive an old send generation");
      check(h.send(h.request()).written() && h.successes == 2,
            "Only a fresh send generation can write again");
      ++cases;
    }
    {
      Harness h;
      h.snapshot.position_authorized = false;
      auto r = h.send(h.request(false, false));
      check(r.status == SendStatus::Revoked && h.encodings == 0 && h.attempts == 0,
            "Alignment/hold admission checks authorization too");
      ++cases;
    }
    {
      Harness h;
      h.snapshot.position_authorized = false;
      h.snapshot.damping_allowed = true;
      h.snapshot.damping_deadline = h.now + 100ms;
      h.during_encoding = [&] { h.now += 100ms; };
      const auto r = h.send(h.request(true, false));
      check(r.status == SendStatus::DampingExpired && h.attempts == 0,
            "Actual damping deadline rechecked after encoding");
      ++cases;
    }
    {
      Harness h;
      h.snapshot.position_authorized = false;
      h.snapshot.damping_allowed = true;
      h.snapshot.damping_deadline = h.now + 100ms;
      const auto end = h.now + 100ms;
      auto request = h.request(true, false);
      request.deadline = end;
      h.during_encoding = [&] { h.now += 101ms; };
      const auto r = h.send(request);
      check(r.status == SendStatus::DeadlineExpired && h.attempts == 0,
            "Shutdown absolute deadline is not a scheduled timestamp");
      ++cases;
    }
    {
      Harness h;
      h.snapshot.position_authorized = false;
      h.snapshot.damping_allowed = true;
      h.snapshot.damping_deadline = h.now + 100ms;
      h.after_first_observation = [&] { h.now += 100ms; };
      const auto r = h.send(h.request(true, false));
      check(r.status == SendStatus::DampingExpired && h.encodings == 0 && h.attempts == 0,
            "A copied true damping flag cannot extend its absolute deadline");
      ++cases;
    }
    {
      Harness h;
      h.transport_accepts = false;
      const auto r = h.send(h.request());
      check(r.status == SendStatus::TransportRejected && h.attempts == 1 && h.successes == 0,
            "Writer rejection is not a successful SDK receipt");
      ++cases;
    }
    {
      Harness h;
      h.transport_throws = true;
      const auto r = h.send(h.request());
      check(r.status == SendStatus::TransportFailure && h.attempts == 1 && h.successes == 0,
            "Writer exception is contained and not a receipt");
      ++cases;
    }
    {
      Harness h;
      const auto old = h.request();
      h.on_transport = [&] {
        ++h.snapshot.authorization_generation;
        h.snapshot.position_authorized = false;
      };
      check(h.send(old).written() && h.attempts == 1,
            "Already-invoked transport cannot be retroactively cancelled");
      check(h.send(old).status == SendStatus::Revoked && h.attempts == 1,
            "Stop during Write forbids the next call");
      ++cases;
    }
    {
      Harness h;
      PeriodicSendSchedule schedule(h.now);
      const auto start = h.now;
      const auto request = h.request();
      check(schedule.consume(h.now).due && h.send(request).written(), "First periodic slot");
      h.now = start + 9ms;
      const auto delayed = schedule.consume(h.now);
      check(delayed.due && delayed.skipped == 3 && delayed.lateness == 7ms,
            "Worker skips missed 4/6/8 ms slots");
      check(h.send(request).written() && !schedule.consume(h.now).due && h.successes == 2,
            "No periodic catch-up writes at one timestamp");
      ++cases;
    }
    {
      Harness h;
      h.snapshot.position_authorized = false;
      h.snapshot.damping_allowed = true;
      const auto start = h.now;
      h.snapshot.damping_deadline = start + 100ms;
      DeadlineSchedule schedule(start, 100ms);
      auto request = h.request(true, false);
      request.deadline = schedule.end();
      for (auto elapsed : {0ms, 2ms, 86ms, 86ms, 100ms, 105ms}) {
        h.now = start + elapsed;
        if (schedule.due(h.now)) h.send(request);
      }
      check(h.successes == 3 && h.damping_writes == 3 && h.position_writes == 0 &&
                h.sent_at[0] == start && h.sent_at[1] == start + 2ms &&
                h.sent_at[2] == start + 86ms,
            "Delayed damping skips missed slots and never backfills past deadline");
      ++cases;
    }
    std::cout << "{\"pass\":true,\"cases\":" << cases
              << ",\"sdk_channels\":false,\"real_transport_constructed\":false,"
              << "\"simulator_started\":false,\"production_guarded_send_used\":true,\"production_"
                 "periodic_schedule_used\":true}\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL send decisions: " << error.what() << '\n';
    return 1;
  }
}
