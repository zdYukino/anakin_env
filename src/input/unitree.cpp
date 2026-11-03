#include <anakin_env/input.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace anakin::env {
InputSample UnitreeRemoteAdapter::wireless(uint16_t keys, const std::array<float, 4>& axes,
                                           InputTime acquired, bool valid) {
  InputSample sample;
  sample.source = InputSource::Remote;
  sample.acquired = acquired;
  sample.connected = valid;
  if (!valid) return sample;
  for (const auto axis : axes)
    if (!std::isfinite(axis) || std::abs(axis) > 1.001f)
      throw std::invalid_argument("Malformed Unitree remote axis");
  sample.axes = axes;
  // HG R1 is bit0 and L1 bit1: normalized masks deliberately use logical names.
  if (keys & (1u << 0)) sample.shoulders |= shoulder_r1;
  if (keys & (1u << 1)) sample.shoulders |= shoulder_l1;
  const std::array<std::pair<Button, unsigned>, input_button_count> buttons{{
      {Button::A, 8}, {Button::B, 9}, {Button::X, 10}, {Button::Y, 11},
      {Button::Up, 12}, {Button::Down, 14}, {Button::Left, 15}, {Button::Right, 13},
      {Button::Start, 2}, {Button::Select, 3}}};
  for (const auto& button : buttons)
    sample.buttons.set(static_cast<size_t>(button.first), keys & (1u << button.second));
  return sample;
}

InputSample UnitreeRemoteAdapter::lowstate(const std::array<uint8_t, 40>& bytes, InputTime acquired) {
  if (bytes[0] != 0xfe || bytes[1] != 0xef) return wireless(0, {}, acquired, false);
  const auto keys = static_cast<uint16_t>(bytes[2]) | static_cast<uint16_t>(bytes[3]) << 8;
  std::array<float, 4> axes{};
  const size_t offsets[]{4, 20, 8, 12};
  for (size_t i = 0; i < axes.size(); ++i)
    std::memcpy(&axes[i], bytes.data() + offsets[i], sizeof(float));
  return wireless(keys, axes, acquired);
}
}  // namespace anakin::env
