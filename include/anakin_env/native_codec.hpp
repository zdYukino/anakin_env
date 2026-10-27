#pragma once
#include <anakin_env/gate.hpp>
#include <unitree/idl/hg/LowCmd_.hpp>
#include <unitree/idl/hg/LowState_.hpp>
namespace anakin::env {
uint32_t crc_words(const uint32_t* words, size_t count);
uint32_t command_crc(const unitree_hg::msg::dds_::LowCmd_& command);
bool valid_state_crc(const unitree_hg::msg::dds_::LowState_& state);
Frame decode_state(const unitree_hg::msg::dds_::LowState_& state);
unitree_hg::msg::dds_::LowCmd_ encode_position(const Positions& q, uint8_t mode_machine,
                                               const std::string& profile = legacy_gain_profile,
                                               const std::string& model = "asap_loco");
unitree_hg::msg::dds_::LowCmd_ encode_damping(uint8_t mode_machine);
}  // namespace anakin::env
