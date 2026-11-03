#pragma once
#include <anakin_env/gate.hpp>
#include <unitree/idl/hg/LowCmd_.hpp>
#include <unitree/idl/hg/LowState_.hpp>
namespace anakin::env {
uint32_t crc_words(const uint32_t* words, size_t count);
uint32_t command_crc(const unitree_hg::msg::dds_::LowCmd_& command);
bool valid_state_crc(const unitree_hg::msg::dds_::LowState_& state);
Frame decode_state(const unitree_hg::msg::dds_::LowState_& state);
struct PdReference {
  Positions q{}, dq{}, kp{}, kd{}, tau{};
};
unitree_hg::msg::dds_::LowCmd_ encode_pd(const PdReference&, uint8_t mode_machine, bool enabled = true);
unitree_hg::msg::dds_::LowCmd_ encode_policy_pd(const Positions&, uint8_t mode_machine,
    const std::string& profile = merged_gain_profile, const std::string& model = "asap_loco");
unitree_hg::msg::dds_::LowCmd_ encode_release(uint8_t mode_machine);
// Must run on the final command object passed to the writer, after all copies.
void finalize_command(unitree_hg::msg::dds_::LowCmd_& command);
unitree_hg::msg::dds_::LowCmd_ encode_position(const Positions& q, uint8_t mode_machine,
                                               const std::string& profile = legacy_gain_profile,
                                               const std::string& model = "asap_loco");
unitree_hg::msg::dds_::LowCmd_ encode_damping(uint8_t mode_machine);
}  // namespace anakin::env
