#include <anakin_env/input.hpp>

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace anakin::env {
namespace {
std::optional<Key> letter(unsigned char value) {
  switch (value) {
    case '1': return Key::Digit1;
    case '2': return Key::Digit2;
    case '3': return Key::Digit3;
    case '4': return Key::Digit4;
    case '5': return Key::Digit5;
    case '6': return Key::Digit6;
    case ' ': return Key::Space;
    case 'c': case 'C': return Key::C;
    case 'w': case 'W': return Key::W;
    case 'a': case 'A': return Key::A;
    case 's': case 'S': return Key::S;
    case 'd': case 'D': return Key::D;
    case 'q': case 'Q': return Key::Q;
    case 'e': case 'E': return Key::E;
    case 'g': case 'G': return Key::G;
    default: return std::nullopt;
  }
}
std::optional<Key> arrow(unsigned char value) {
  switch (value) {
    case 'A': return Key::Up;
    case 'B': return Key::Down;
    case 'C': return Key::Right;
    case 'D': return Key::Left;
    default: return std::nullopt;
  }
}
}  // namespace

struct TerminalKeyboard::Impl {
  int fd = -1;
  termios original{};
  bool active = false, connected = true;
  std::string escape;
  InputTime escape_started{};
  void restore() noexcept {
    if (!active) return;
    while (tcsetattr(fd, TCSANOW, &original) < 0 && errno == EINTR) {}
    active = false;
  }
  ~Impl() {
    restore();
    if (fd >= 0) close(fd);
  }
  void parse(unsigned char value, InputTime now, std::vector<Key>& keys) {
    if (!escape.empty()) {
      if (escape.size() == 1 && (value == '[' || value == 'O')) {
        escape.push_back(static_cast<char>(value));
        return;
      }
      if (escape.size() == 2)
        if (const auto key = arrow(value)) keys.push_back(*key);
      escape.clear();
    } else if (value == 27) {
      escape = "\x1b";
      escape_started = now;
    } else if (const auto key = letter(value)) {
      keys.push_back(*key);
    }
  }
};

TerminalKeyboard::TerminalKeyboard(int fd) : impl_(std::make_unique<Impl>()) {
  impl_->fd = dup(fd);
  if (impl_->fd < 0 || tcgetattr(impl_->fd, &impl_->original) < 0)
    throw std::runtime_error("Keyboard requires an accessible terminal: " + std::string(std::strerror(errno)));
  auto settings = impl_->original;
  settings.c_lflag &= ~(ICANON | ECHO);
  settings.c_iflag &= ~(ICRNL | IXON);
  settings.c_cc[VMIN] = 0;
  settings.c_cc[VTIME] = 0;
  // Keep ISIG so the runtime's normal signal shutdown can unwind this RAII owner.
  // Discard queued pre-start bytes rather than fabricate deliberate new controls.
  if (tcsetattr(impl_->fd, TCSAFLUSH, &settings) < 0)
    throw std::runtime_error("Cannot configure keyboard terminal: " + std::string(std::strerror(errno)));
  impl_->active = true;
}
TerminalKeyboard::~TerminalKeyboard() = default;
void TerminalKeyboard::restore() noexcept { impl_->restore(); }

InputSample TerminalKeyboard::poll(InputTime now) {
  InputSample sample;
  sample.source = InputSource::Keyboard;
  sample.acquired = now;
  sample.connected = impl_->active && impl_->connected;
  if (!sample.connected) return sample;
  if (!impl_->escape.empty() && now - impl_->escape_started > std::chrono::milliseconds(100))
    impl_->escape.clear();
  // VMIN=VTIME=0 and poll(0) keep idle SSH/TTY input nonblocking and bounded.
  for (int batch = 0; batch < 32; ++batch) {
    pollfd descriptor{impl_->fd, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, 0);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) throw std::runtime_error("Keyboard poll failed");
    if (ready == 0) break;
    if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) {
      impl_->connected = sample.connected = false;
      break;
    }
    if (!(descriptor.revents & POLLIN)) break;
    unsigned char bytes[128];
    const auto count = read(impl_->fd, bytes, sizeof(bytes));
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (count < 0) throw std::runtime_error("Keyboard read failed");
    if (count == 0) break;
    for (ssize_t i = 0; i < count; ++i) impl_->parse(bytes[i], now, sample.keys);
  }
  return sample;
}
}  // namespace anakin::env
