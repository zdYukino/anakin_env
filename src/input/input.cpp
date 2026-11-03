#include <anakin_env/input.hpp>

#include <algorithm>
#include <cmath>

namespace anakin::env {
namespace {
constexpr std::array<InputSource, 3> priority{InputSource::Remote, InputSource::Gamepad, InputSource::Web};
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
        return binding.action == InputAction::EmergencyStop && match(binding.control, sample);
      });
}
}  // namespace

bool stop_requested(const InputSnapshot& snapshot) { return snapshot.emergency_stop; }
std::optional<Event> input_event(InputAction action) {
  switch (action) {
    case InputAction::Disarmed: return Event::manual(State::Disarmed);
    case InputAction::Damping: return Event::manual(State::Damping);
    case InputAction::Align: return Event::manual(State::Align);
    case InputAction::Hold: return Event::manual(State::Hold);
    case InputAction::AsapLoco: return Event::manual(State::AsapLoco);
    case InputAction::AsapCr7: return Event::manual(State::AsapCr7);
    case InputAction::ClearFault: return Event::system(EventKind::ClearFault);
    case InputAction::EmergencyStop: return Event::system(EventKind::EmergencyStop);
    default: return std::nullopt;
  }
}
InputHub::InputHub(InputConfig config, std::chrono::milliseconds source_timeout)
    : config_(std::move(config)), timeout_(source_timeout), active_(config_.primary), height_(config_.initial_height) {
  if (timeout_.count() <= 0 || (config_.primary && !enabled(*config_.primary)))
    throw std::invalid_argument("InputHub requires validated inputs and a positive source timeout");
  for (const auto source : priority) sources_[index(source)].sample.source = source;
}
bool InputHub::enabled(InputSource source) const {
  return std::find(config_.enabled_sources.begin(), config_.enabled_sources.end(), source) != config_.enabled_sources.end();
}
void InputHub::submit(const InputSample& sample) {
  const auto source_index = index(sample.source);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled(sample.source)) return;
  if (sample.shoulders & ~(shoulder_l1 | shoulder_r1)) throw std::invalid_argument("Invalid normalized shoulder mask");
  for (const auto axis : sample.axes)
    if (!std::isfinite(axis) || std::abs(axis) > 1.001f) throw std::invalid_argument("Invalid normalized input axis");
  if (sample.triggers)
    for (const auto trigger : *sample.triggers)
      if (!std::isfinite(trigger) || trigger < 0.f || trigger > 1.f) throw std::invalid_argument("Invalid normalized trigger");
  if (sample.action && (sample.source != InputSource::Web ||
      std::find(config_.web_actions.begin(), config_.web_actions.end(), *sample.action) == config_.web_actions.end()))
    throw std::invalid_argument("Web action is not configured");
  auto& state = sources_[source_index];
  if (state.initialized && sample.acquired <= state.sample.acquired) return;
  const bool continuing = state.initialized && fresh(state.sample, sample.acquired, timeout_) &&
                          sample.connection == state.sample.connection;
  if (active_ == sample.source && primary_was_online_ && (!sample.connected || !continuing)) pending_source_lost_ = true;
  if (!continuing) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& action) {
      return action.source == sample.source;
    }), pending_.end());
    state.activity = false;
    state.axes_armed = false;
  }
  if (!sample.connected) {
    state.sample = sample;
    state.initialized = true;
    return;
  }
  if (controller_stop(config_, sample) || sample.action == InputAction::EmergencyStop) state.pending_stop = sample.acquired;
  const auto previous = continuing ? state.sample.buttons : sample.buttons;
  const auto armed = continuing ? state.armed : ~sample.buttons;
  for (const auto& binding : config_.controller_bindings) {
    if (binding.action == InputAction::EmergencyStop) continue;
    const auto& button = binding.control;
    const auto bit = static_cast<size_t>(button.button);
    const bool pressed = match(button, sample) && !previous.test(bit);
    const bool held = match(button, sample);
    const bool released = !sample.buttons.test(bit) && previous.test(bit) && state.sample.shoulders == button.shoulders;
    if (armed.test(bit) && ((binding.trigger == Trigger::Pressed && pressed) ||
        (binding.trigger == Trigger::Held && held) || (binding.trigger == Trigger::Released && released))) {
      pending_.push_back({binding.action, sample.acquired, sample.source});
      state.activity = true;
    }
  }
  if (sample.action && *sample.action != InputAction::EmergencyStop) {
    pending_.push_back({*sample.action, sample.acquired, sample.source});
    state.activity = true;
  }
  const bool neutral = std::all_of(sample.axes.begin(), sample.axes.end(), [&](float axis) {
    return std::abs(axis) < config_.deadzone || axis == 0.f;
  });
  state.axes_armed = state.axes_armed || neutral;
  state.activity = state.activity || sample.activate;
  state.armed = armed | ~sample.buttons;
  state.sample = sample;
  state.initialized = true;
}
InputSnapshot InputHub::view(InputTime now) const {
  InputSnapshot result;
  result.active_source = active_;
  result.moving = moving_;
  result.height = height_;
  for (const auto source : priority) {
    const auto i = index(source);
    const auto& state = sources_[i];
    result.sources[i] = state.sample;
    result.online[i] = enabled(source) && fresh(state.sample, now, timeout_);
    bool stop = enabled(source) && state.pending_stop && now >= *state.pending_stop && now - *state.pending_stop <= timeout_;
    if (result.online[i]) stop = stop || controller_stop(config_, state.sample);
    result.emergency_sources[i] = stop;
    result.emergency_stop = result.emergency_stop || stop;
  }
  if (!active_) return result;
  const auto primary = index(*active_);
  result.primary_online = result.online[primary];
  result.primary_acquired = sources_[primary].sample.acquired;
  if (moving_ && result.primary_online && !result.emergency_stop && !pending_source_lost_) {
    for (size_t component = 0; component < result.velocity.size(); ++component) {
      const auto& binding = config_.velocity_axes[component];
      const auto axis = sources_[primary].sample.axes[static_cast<size_t>(binding.axis)];
      if (std::abs(axis) >= config_.deadzone && sources_[primary].axes_armed)
        result.velocity[component] = std::clamp(static_cast<double>(axis), -1., 1.) * binding.scale;
    }
  }
  return result;
}
void InputHub::apply(InputAction action) {
  switch (action) {
    case InputAction::MotionToggle: moving_ = !moving_; break;
    case InputAction::HeightUp: height_ += config_.height_step; break;
    case InputAction::HeightDown: height_ -= config_.height_step; break;
    default: break;
  }
}
InputFrame InputHub::poll(InputTime now) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (now == InputTime{}) now = InputClock::now();
  InputFrame result;
  const auto initial = view(now);
  const bool source_lost = pending_source_lost_ || (primary_was_online_ && !initial.primary_online);
  if (source_lost) {
    result.lost_source = active_;
    result.events.push_back(Event::system(EventKind::SourceLost));
    moving_ = false;
  }
  if (stop_requested(initial)) {
    moving_ = false;
    result.actions.push_back(InputAction::EmergencyStop);
    result.events.insert(result.events.begin(), Event::system(EventKind::EmergencyStop));
  } else if (!source_lost) {
    if (!config_.primary) {
      for (const auto source : priority) {
        const auto& state = sources_[index(source)];
        const bool stick_active = state.axes_armed && std::any_of(state.sample.axes.begin(), state.sample.axes.end(),
            [&](float axis) { return axis != 0.f && std::abs(axis) >= config_.deadzone; });
        if (!initial.online[index(source)] || (!state.activity && !stick_active)) continue;
        if (active_ != source) {
          moving_ = false;
          active_ = source;
        }
        break;
      }
    }
    for (const auto& action : pending_) {
      if (active_ != action.source || !initial.online[index(action.source)] || now < action.acquired || now - action.acquired > timeout_) continue;
      apply(action.action);
      result.actions.push_back(action.action);
      if (const auto event = input_event(action.action)) result.events.push_back(*event);
    }
  }
  result.snapshot = view(now);
  primary_was_online_ = result.snapshot.primary_online;
  if (source_lost && !config_.primary) {
    active_.reset();
    primary_was_online_ = false;
  }
  pending_.clear();
  for (auto& source : sources_) {
    source.pending_stop.reset();
    source.activity = false;
  }
  pending_source_lost_ = false;
  return result;
}
InputSnapshot InputHub::snapshot(InputTime now) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (now == InputTime{}) now = InputClock::now();
  return view(now);
}
}  // namespace anakin::env
