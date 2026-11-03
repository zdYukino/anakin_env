// Compiled ELF used exclusively by launchers.py. No ROS/SDK dependencies.
#include <cstdlib>
#include <fstream>
#include <unistd.h>
int main(int argc, char** argv) {
  const char* destination = std::getenv("ANAKIN_CAPTURE");
  if (!destination) return 91;
  std::ofstream out(destination, std::ios::binary);
  if (!out) return 92;
  const auto field = [&](const char* value) { out << (value ? value : "") << '\0'; };
  for (int i = 0; i < argc; ++i) field(argv[i]);
  field("--ENV--");
  for (const auto* name : {"ANAKIN_PREFIX", "ANAKIN_SDK_PREFIX", "ROS_DOMAIN_ID", "ROS_LOCALHOST_ONLY",
                           "RMW_IMPLEMENTATION", "AMENT_PREFIX_PATH", "LD_LIBRARY_PATH"}) {
    field(name); field(std::getenv(name));
  }
  field("--PID--");
  out << getpid() << '\0';
  return out ? 0 : 93;
}
