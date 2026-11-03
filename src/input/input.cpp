#include <anakin_env/input.hpp>

#include <algorithm>
#include <cmath>

namespace anakin::env {
namespace {
size_t index(InputSource source) {
  const auto result = static_cast<size_t>(source);
  if (result >= 3) throw std::invalid_argument("Unknown input source");
  return result;
}
bool fresh(const InputSample& sample, InputTime now, std::chrono::milliseconds timeout) {
  return sample.connected && now >= sample.acquired && now - sample.acquired <= timeout;
}
bool match(const ButtonBinding& binding, const InputSample& sample) {
  return sample.buttons.test(static_cast<size_t>(binding.button)) &&
         (binding.any_shoulders || sample.shoulders == binding.shoulders);
}
bool controller_stop(const InputConfig& config, const InputSample& sample) {
  return std::any_of(config.controller_bindings.begin(), config.controller_bindings.end(),
      [&](const InputBinding& binding) {
        return binding.action == InputAction::EmergencyStop &&
               match(std::get<ButtonBinding>(binding.control), sample);
      });
}
std::optional<Event> mode_event(InputAction action) {
  switch (action) {
    case InputAction::Disarmed: return Event::manual(State::Disarmed);
    case InputAction::Damping: return Event::manual(State::Damping);
    case InputAction::Align: return Event::manual(State::Align);
    case InputAction::Hold: return Event::manual(State::Hold);
    case InputAction::AsapLoco: return Event::manual(State::AsapLoco);
    case InputAction::AsapCr7: return Event::manual(State::AsapCr7);
    case InputAction::ClearFault: return Event::system(EventKind::ClearFault);
    default: return std::nullopt;
  }
}
}  // namespace

bool stop_requested(const InputSnapshot& snapshot) { return snapshot.emergency_stop; }

InputHub::InputHub(InputConfig config, std::chrono::milliseconds source_timeout)
    : config_(std::move(config)), timeout_(source_timeout), height_(config_.initial_height) {
  if (timeout_.count() <= 0 || !enabled(config_.primary))
    throw std::invalid_argument("InputHub requires validated inputs and a positive source timeout");
  for (const auto source : {InputSource::Keyboard, InputSource::Gamepad, InputSource::Remote})
    sources_[index(source)].sample.source = source;
}
bool InputHub::enabled(InputSource source) const {
  return std::find(config_.enabled_sources.begin(), config_.enabled_sources.end(), source) !=
         config_.enabled_sources.end();
}

void InputHub::submit(const InputSample& sample) {
  const auto source_index = index(sample.source);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled(sample.source)) return;
  if (sample.shoulders & ~(shoulder_l1 | shoulder_r1))
    throw std::invalid_argument("Invalid normalized shoulder mask");
  for (const auto axis : sample.axes)
    if (!std::isfinite(axis) || std::abs(axis) > 1.001f)
      throw std::invalid_argument("Invalid normalized input axis");
  auto& state = sources_[source_index];
  if (state.initialized && sample.acquired < state.sample.acquired) return;
  const bool continuing = state.initialized && fresh(state.sample, sample.acquired, timeout_) &&
                          sample.connection == state.sample.connection;
  if (sample.source == config_.primary && primary_was_online_ &&
      (!sample.connected || !continuing))
    pending_source_lost_ = true;
  if (!continuing && sample.source == config_.primary) pending_.clear();
  if (!sample.connected) {
    state.sample = sample;
    // Keep the acquisition high-water mark even while disconnected. An older
    // cached packet must not resurrect the device after a newer loss sample.
    state.initialized = true;
    if (sample.source == config_.primary) pending_.clear();
    return;
  }
  if (sample.source == InputSource::Keyboard) {
    for (const auto key : sample.keys) {
      for (const auto& binding : config_.keyboard_bindings) {
        if (std::get<KeyboardBinding>(binding.control).key != key) continue;
        if (binding.action == InputAction::EmergencyStop) {
          state.pending_stop = sample.acquired;
          state.keyboard_stop_until = sample.acquired + config_.keyboard_command_timeout;
        } else if (sample.source == config_.primary) {
          pending_.push_back({binding.action, sample.acquired});
        }
      }
    }
  } else {
    if (controller_stop(config_, sample)) state.pending_stop = sample.acquired;
    const auto previous = continuing ? state.sample.buttons : sample.buttons;
    const auto armed = continuing ? state.armed : ~sample.buttons;
    if (sample.source == config_.primary) {
      for (const auto& binding : config_.controller_bindings) {
        if (binding.action == InputAction::EmergencyStop) continue;
        const auto& button = std::get<ButtonBinding>(binding.control);
        const auto bit = static_cast<size_t>(button.button);
        const bool pressed = match(button, sample) && !previous.test(bit);
        const bool held = match(button, sample);
        const bool released = !sample.buttons.test(bit) && previous.test(bit) &&
                              state.sample.shoulders == button.shoulders;
        if (armed.test(bit) && ((binding.trigger == Trigger::Pressed && pressed) ||
            (binding.trigger == Trigger::Held && held) ||
            (binding.trigger == Trigger::Released && released)))
          pending_.push_back({binding.action, sample.acquired});
      }
    }
    state.armed = armed | ~sample.buttons;
  }
  state.sample = sample;
  state.initialized = true;
}

InputSnapshot InputHub::view(InputTime now) const {
  InputSnapshot result;
  result.moving = moving_;
  result.height = height_;
  for (const auto source : {InputSource::Keyboard, InputSource::Gamepad, InputSource::Remote}) {
    const auto i = index(source);
    const auto& state = sources_[i];
    result.sources[i] = state.sample;
    result.online[i] = enabled(source) && fresh(state.sample, now, timeout_);
    if (enabled(source) && state.pending_stop && now >= *state.pending_stop &&
        now - *state.pending_stop <= timeout_)
      result.emergency_stop = true;
    if (result.online[i]) {
      if (source == InputSource::Keyboard)
        result.emergency_stop = result.emergency_stop || now < state.keyboard_stop_until;
      else
        result.emergency_stop = result.emergency_stop || controller_stop(config_, state.sample);
    }
  }
  const auto primary = index(config_.primary);
  result.primary_online = result.online[primary];
  result.primary_acquired = sources_[primary].sample.acquired;
  if (moving_ && result.primary_online && !result.emergency_stop && !pending_source_lost_) {
    for (size_t component = 0; component < result.velocity.size(); ++component) {
      if (config_.primary == InputSource::Keyboard) {
        if (now < keyboard_expiry_[component]) result.velocity[component] = keyboard_velocity_[component];
      } else {
        const auto& binding = config_.velocity_axes[component];
        const auto axis = sources_[primary].sample.axes[static_cast<size_t>(binding.axis)];
        if (std::abs(axis) >= config_.deadzone)
          result.velocity[component] = std::clamp(static_cast<double>(axis), -1., 1.) * binding.scale;
      }
    }
  }
  return result;
}

void InputHub::apply(InputAction action, InputTime acquired, InputTime now) {
  switch (action) {
    case InputAction::MotionToggle: moving_ = !moving_; break;
    case InputAction::HeightUp: height_ += config_.height_step; break;
    case InputAction::HeightDown: height_ -= config_.height_step; break;
    case InputAction::Forward:
    case InputAction::Backward:
    case InputAction::Left:
    case InputAction::Right:
    case InputAction::TurnLeft:
    case InputAction::TurnRight: {
      const size_t component = action == InputAction::Forward || action == InputAction::Backward ? 0 :
          (action == InputAction::Left || action == InputAction::Right ? 1 : 2);
      const bool positive = action == InputAction::Forward || action == InputAction::Left ||
                            action == InputAction::TurnLeft;
      keyboard_expiry_[component] = acquired + config_.keyboard_command_timeout;
      keyboard_velocity_[component] = now < keyboard_expiry_[component]
          ? (positive ? 1. : -1.) * std::abs(config_.velocity_axes[component].scale) : 0.;
      break;
    }
    default: break;
  }
}

InputFrame InputHub::poll(InputTime now) {
  std::lock_guard<std::mutex> lock(mutex_);
  InputFrame result;
  const auto initial = view(now);
  const bool source_lost = pending_source_lost_ || (primary_was_online_ && !initial.primary_online);
  if (source_lost) {
    result.events.push_back(Event::system(EventKind::SourceLost));
    moving_ = false;
    keyboard_velocity_ = {};
  }
  primary_was_online_ = initial.primary_online;
  if (stop_requested(initial)) {
    moving_ = false;
    keyboard_velocity_ = {};
    result.actions.push_back(InputAction::EmergencyStop);
    result.events.insert(result.events.begin(), Event::system(EventKind::EmergencyStop));
  } else if (initial.primary_online && !source_lost) {
    for (const auto& action : pending_) {
      if (now < action.acquired || now - action.acquired > timeout_) continue;
      apply(action.action, action.acquired, now);
      result.actions.push_back(action.action);
      if (const auto event = mode_event(action.action)) result.events.push_back(*event);
    }
  }
  result.snapshot = view(now);
  pending_.clear();
  for (auto& source : sources_) source.pending_stop.reset();
  pending_source_lost_ = false;
  return result;
}

InputSnapshot InputHub::snapshot(InputTime now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return view(now);
}

}  // namespace anakin::env
