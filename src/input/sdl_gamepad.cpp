#include <anakin_env/input.hpp>

#include <SDL.h>

namespace anakin::env {
struct SdlGamepad::Impl {
  int device_index;
  SDL_GameController* controller = nullptr;
  std::shared_ptr<const GamepadDevice> device;
  explicit Impl(int index) : device_index(index) {
    if (index < 0) throw std::invalid_argument("Gamepad device index must be nonnegative");
    // No video subsystem/window. State is polled explicitly, even without changes.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
      throw std::runtime_error("Cannot initialize SDL gamepad: " + std::string(SDL_GetError()));
  }
  ~Impl() {
    if (controller) SDL_GameControllerClose(controller);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
  }
};

SdlGamepad::SdlGamepad(int device_index, const std::vector<std::string>& mappings)
    : impl_(std::make_unique<Impl>(device_index)) {
  for (const auto& mapping : mappings)
    if (SDL_GameControllerAddMapping(mapping.c_str()) < 0)
      throw std::runtime_error("Cannot load configured SDL mapping: " + std::string(SDL_GetError()));
}
SdlGamepad::~SdlGamepad() = default;

InputSample SdlGamepad::poll(InputTime now) {
  SDL_PumpEvents();
  SDL_GameControllerUpdate();
  // This adapter consumes current state, not the change queue. Leave unrelated
  // SDL events alone and avoid an unbounded backlog of joystick change events.
  SDL_FlushEvents(SDL_JOYAXISMOTION, SDL_JOYDEVICEREMOVED);
  SDL_FlushEvents(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLERDEVICEREMAPPED);
  InputSample sample;
  sample.source = InputSource::Gamepad;
  sample.acquired = now;
  if (impl_->controller && !SDL_GameControllerGetAttached(impl_->controller)) {
    SDL_GameControllerClose(impl_->controller);
    impl_->controller = nullptr;
    impl_->device.reset();
    // Report an offline sample once before selecting a replacement device.
    return sample;
  }
  if (!impl_->controller && impl_->device_index < SDL_NumJoysticks() &&
      SDL_IsGameController(impl_->device_index)) {
    impl_->controller = SDL_GameControllerOpen(impl_->device_index);
    if (!impl_->controller)
      throw std::runtime_error("Cannot open SDL gamepad: " + std::string(SDL_GetError()));
    auto info = std::make_shared<GamepadDevice>();
    if (const auto* name = SDL_GameControllerName(impl_->controller)) info->name = name;
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(impl_->device_index), guid, sizeof(guid));
    info->guid = guid;
    if (auto* mapping = SDL_GameControllerMapping(impl_->controller)) {
      info->mapping = mapping;
      SDL_free(mapping);
    }
    impl_->device = std::move(info);
  }
  if (!impl_->controller) return sample;
  sample.connected = SDL_GameControllerGetAttached(impl_->controller);
  sample.device = impl_->device;
  sample.connection = static_cast<uint64_t>(SDL_JoystickInstanceID(
      SDL_GameControllerGetJoystick(impl_->controller))) + 1;
  const std::array<std::pair<Button, SDL_GameControllerButton>, input_button_count> buttons{{
      {Button::A, SDL_CONTROLLER_BUTTON_A}, {Button::B, SDL_CONTROLLER_BUTTON_B},
      {Button::X, SDL_CONTROLLER_BUTTON_X}, {Button::Y, SDL_CONTROLLER_BUTTON_Y},
      {Button::Up, SDL_CONTROLLER_BUTTON_DPAD_UP}, {Button::Down, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
      {Button::Left, SDL_CONTROLLER_BUTTON_DPAD_LEFT}, {Button::Right, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
      {Button::Start, SDL_CONTROLLER_BUTTON_START}, {Button::Select, SDL_CONTROLLER_BUTTON_BACK}}};
  for (const auto& button : buttons)
    sample.buttons.set(static_cast<size_t>(button.first),
                       SDL_GameControllerGetButton(impl_->controller, button.second));
  if (SDL_GameControllerGetButton(impl_->controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
    sample.shoulders |= shoulder_l1;
  if (SDL_GameControllerGetButton(impl_->controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
    sample.shoulders |= shoulder_r1;
  const std::array<SDL_GameControllerAxis, 4> axes{{SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
      SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY}};
  for (size_t i = 0; i < axes.size(); ++i) {
    const auto value = SDL_GameControllerGetAxis(impl_->controller, axes[i]);
    sample.axes[i] = static_cast<float>(value) / (value < 0 ? 32768.f : 32767.f);
  }
  sample.axes[1] = -sample.axes[1];  // SDL up is negative; logical forward is positive.
  if (SDL_GameControllerHasAxis(impl_->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) &&
      SDL_GameControllerHasAxis(impl_->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
    sample.triggers = std::array<float, 2>{
        static_cast<float>(SDL_GameControllerGetAxis(impl_->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) / 32767.f,
        static_cast<float>(SDL_GameControllerGetAxis(impl_->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) / 32767.f};
  }
  return sample;
}
}  // namespace anakin::env
