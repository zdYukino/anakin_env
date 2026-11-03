#pragma once
#include <anakin_env/gate.hpp>

namespace anakin::env {
inline constexpr const char* offline_fixture_frame_id = "offline_fixture_base";
inline constexpr const char* offline_fixture_diagnostic =
    "OFFLINE_FIXTURE: no physics, no native transport, no actuation";
template <class Message>
inline void use_offline_execution_contract(Message& message) {
  message.gain_profile = merged_gain_profile;
  message.execution_profile = sdk_pd_execution_profile;
  message.command_semantics = pd_reference;
}
// This explicit fixture grants inference only. A labelled frame must never claim
// native SDK publication; execution_authorized is not a motor-write permission.
inline bool is_offline_fixture(const Frame& frame) {
  if (frame.header.frame_id != offline_fixture_frame_id ||
      frame.schema_version != schema_version || frame.schema_id != schema_id ||
      frame.execution_mode != Frame::MODE_PREPARING || frame.gain_profile != merged_gain_profile ||
      frame.execution_profile != sdk_pd_execution_profile || frame.command_semantics != pd_reference ||
      frame.sdk_publish_count != 0 || frame.sdk_published_epoch != 0 ||
      frame.sdk_published_target_seq != 0 || frame.sdk_published_phase_mask != 0 ||
      !zero(frame.sdk_published_mc_instance_id) || !zero(frame.sdk_published_request_id))
    return false;
  return std::any_of(frame.diagnostics.status.begin(), frame.diagnostics.status.end(),
                     [](const auto& diagnostic) {
                       return diagnostic.name == "anakin_env/backend" &&
                              diagnostic.message == offline_fixture_diagnostic;
                     });
}
inline bool offline_inference_permitted(const Frame& frame) {
  constexpr auto required = Frame::VALID_JOINTS | Frame::VALID_IMU | Frame::VALID_COMMAND;
  return is_offline_fixture(frame) && frame.execution_authorized &&
         (frame.validity & required) == required;
}
}  // namespace anakin::env
