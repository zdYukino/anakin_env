#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
extern char** environ;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int signal) { interrupted = signal; }
struct Child {
  std::string name;
  pid_t pid = -1;
  bool alive = true;
  int code = -1;
};
class Children {
 public:
  ~Children() { cleanup(); }
  size_t spawn(const std::string& name, const std::vector<std::string>& arguments,
               const fs::path& log, const std::map<std::string, std::string>& overrides,
               int inherited_fd = -1) {
    std::vector<char*> argv;
    for (const auto& value : arguments) argv.push_back(const_cast<char*>(value.c_str()));
    argv.push_back(nullptr);
    std::map<std::string, std::string> values;
    for (char** entry = environ; *entry; ++entry) {
      const std::string value = *entry;
      const auto equals = value.find('=');
      if (equals != std::string::npos) values[value.substr(0, equals)] = value.substr(equals + 1);
    }
    for (const auto& item : overrides) values[item.first] = item.second;
    std::vector<std::string> env_strings;
    for (const auto& item : values) env_strings.push_back(item.first + "=" + item.second);
    std::vector<char*> env;
    for (auto& item : env_strings) env.push_back(item.data());
    env.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                     O_CREAT | O_WRONLY | O_EXCL, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (inherited_fd >= 0) posix_spawn_file_actions_adddup2(&actions, inherited_fd, inherited_fd);
    pid_t pid = -1;
    const int result =
        posix_spawn(&pid, arguments[0].c_str(), &actions, nullptr, argv.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    if (result) throw std::runtime_error("Cannot spawn " + name + ": " + std::to_string(result));
    entries.push_back(Child{name, pid, true, -1});
    std::cout << "OWNED_CHILD " << name << " pid=" << pid << std::endl;
    return entries.size() - 1;
  }
  void poll() {
    for (auto& child : entries) {
      if (!child.alive) continue;
      int status = 0;
      const auto result = waitpid(child.pid, &status, WNOHANG);
      if (result == child.pid) {
        child.alive = false;
        child.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        std::cout << "OWNED_EXIT " << child.name << " code=" << child.code << std::endl;
      } else if (result < 0)
        throw std::runtime_error("Lost owned child wait status");
    }
  }
  void stop(size_t index) {
    for (int signal : {SIGINT, SIGTERM, SIGKILL}) {
      poll();
      if (!entries[index].alive) return;
      kill(entries[index].pid, signal);
      const auto end = Clock::now() + 1s;
      while (Clock::now() < end) {
        poll();
        if (!entries[index].alive) return;
        std::this_thread::sleep_for(20ms);
      }
    }
    throw std::runtime_error("Owned child exceeded cleanup deadline");
  }
  void cleanup() noexcept {
    // Creation order is Xvfb, simulator, MC, env, probe. Stop env before its RC producer.
    try {
      for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].name == "env" && entries[i].alive) stop(i);
      for (size_t i = entries.size(); i > 0; --i)
        if (entries[i - 1].alive) stop(i - 1);
    } catch (const std::exception& error) {
      std::cerr << "cleanup: " << error.what() << '\n';
    }
  }
  std::vector<Child> entries;
};
std::string read(const fs::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Missing input: " + path.string());
  return std::string(std::istreambuf_iterator<char>(input), {});
}
void write(const fs::path& path, const std::string& text) {
  std::ofstream output(path);
  if (!output) throw std::runtime_error("Cannot write evidence: " + path.string());
  output << text;
}
fs::path suspended_scene(const fs::path& source, const fs::path& run) {
  std::string robot = read(source / "g1_29dof.xml");
  if (robot.find("type=\"free\"") == std::string::npos &&
      robot.find("<freejoint") == std::string::npos)
    throw std::runtime_error("Official model must retain its floating joint");
  const std::string mesh = "meshdir=\"" + (source / "meshes").string() + "\"";
  robot = std::regex_replace(robot, std::regex("meshdir=\"meshes\""), mesh);
  std::smatch pelvis;
  const std::regex pelvis_pattern("(<body name=\"pelvis\" pos=\")[^\"]*(\")");
  if (!std::regex_search(robot, pelvis, pelvis_pattern))
    throw std::runtime_error("Official pelvis definition unavailable");
  robot.replace(pelvis.position(), pelvis.length(), pelvis[1].str() + "0 0 1.2" + pelvis[2].str());
  const auto close = robot.rfind("</mujoco>");
  if (close == std::string::npos || robot.find("<equality") != std::string::npos)
    throw std::runtime_error("Unexpected official equality layout");
  robot.insert(close,
               "<equality><weld name=\"anakin_suspended_test\" body1=\"pelvis\"/></equality>\n");
  const auto robot_path = run / "g1_29dof_suspended.xml";
  write(robot_path, robot);
  auto scene = read(source / "scene_29dof.xml");
  scene = std::regex_replace(scene, std::regex("file=\"g1_29dof.xml\""),
                             "file=\"" + robot_path.string() + "\"");
  const auto scene_path = run / "scene_29dof_suspended.xml";
  write(scene_path, scene);
  return scene_path;
}
}  // namespace
int main(int argc, char** argv) {
  fs::path run;
  try {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    fs::path ws;
    bool suspended = false, demo = false, rc_loss = false, trace_inference = false;
    int sdk_domain = 31, ros_domain = 188;
    double duration = 20., alignment_seconds = 2.;
    std::string gain_profile = "g1_asap_merged_sim_v1";
    std::string execution_profile = "g1_position_strict_sim_v1";
    for (int i = 1; i < argc; ++i) {
      const std::string argument = argv[i];
      if (argument == "--workspace" && i + 1 < argc)
        ws = fs::canonical(argv[++i]);
      else if (argument == "--suspended")
        suspended = true;
      else if (argument == "--demo-switch")
        demo = true;
      else if (argument == "--rc-loss")
        rc_loss = true;
      else if (argument == "--trace-inference")
        trace_inference = true;
      else if (argument == "--sdk-domain" && i + 1 < argc)
        sdk_domain = std::stoi(argv[++i]);
      else if (argument == "--ros-domain" && i + 1 < argc)
        ros_domain = std::stoi(argv[++i]);
      else if (argument == "--duration" && i + 1 < argc)
        duration = std::stod(argv[++i]);
      else if (argument == "--alignment-seconds" && i + 1 < argc)
        alignment_seconds = std::stod(argv[++i]);
      else if (argument == "--execution-profile" && i + 1 < argc)
        execution_profile = argv[++i];
      else if (argument == "--gain-profile" && i + 1 < argc)
        gain_profile = argv[++i];
      else
        throw std::runtime_error(
            "Usage: --workspace PATH [--suspended] [--demo-switch|--rc-loss] [--duration 20] "
            "[--sdk-domain 31] [--ros-domain 188] [--execution-profile PROFILE]");
    }
    if (ws.empty() || !std::isfinite(duration) || !std::isfinite(alignment_seconds) ||
        duration < 10 || duration > 60 || alignment_seconds < .25 || alignment_seconds > 5. ||
        sdk_domain < 1 || sdk_domain > 232 || ros_domain < 1 || ros_domain > 232 ||
        (demo && rc_loss))
      throw std::runtime_error("Invalid explicit simulation options");
    const auto base = ws.parent_path();
    const auto simulator = base / "unitree_mujoco/simulate/build/unitree_mujoco";
    const auto original_scene = base / "unitree_mujoco/unitree_robots/g1/scene_29dof.xml";
    const auto xvfb_installed = fs::path("/usr/bin/Xvfb");
    const auto xvfb = fs::exists(xvfb_installed)
                          ? xvfb_installed
                          : base / "Anakin/build/unitree_mujoco-deps/xvfb/usr/bin/Xvfb";
    for (const auto& path : {simulator, original_scene, xvfb,
                             ws / "install/anakin_env/lib/anakin_env/anakin_sim_probe"})
      if (!fs::exists(path))
        throw std::runtime_error("Simulation prerequisite missing: " + path.string());
    const auto config = read(base / "unitree_mujoco/simulate/config.yaml");
    if (!std::regex_search(config, std::regex("enable_elastic_band:[ \\t]+0")))
      throw std::runtime_error(
          "Official simulator elastic band must be disabled; reference config is read-only");
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    run =
        ws / "evidence" / ("native-sim-" + std::to_string(stamp) + "-" + std::to_string(getpid()));
    fs::create_directory(run);
    const auto scene =
        suspended ? suspended_scene(original_scene.parent_path(), run) : original_scene;
    std::cout << "SIM_EVIDENCE " << run << " supported_base=" << std::boolalpha << suspended
              << std::endl;
    Children children;
    int pipe_fd[2];
    if (pipe(pipe_fd) < 0) throw std::runtime_error("Cannot create Xvfb announcement pipe");
    fcntl(pipe_fd[0], F_SETFD, FD_CLOEXEC);
    const auto xserver =
        children.spawn("xvfb",
                       {xvfb.string(), "-displayfd", std::to_string(pipe_fd[1]), "-screen", "0",
                        "1280x800x24", "-nolisten", "tcp", "-noreset"},
                       run / "xvfb.log", {}, pipe_fd[1]);
    close(pipe_fd[1]);
    pollfd descriptor{pipe_fd[0], POLLIN, 0};
    if (poll(&descriptor, 1, 5000) != 1) {
      close(pipe_fd[0]);
      throw std::runtime_error("Xvfb startup deadline");
    }
    char display_number[32]{};
    const auto length = ::read(pipe_fd[0], display_number, sizeof(display_number) - 1);
    close(pipe_fd[0]);
    if (length <= 0) throw std::runtime_error("Xvfb did not announce a private display");
    std::string number(display_number, static_cast<size_t>(length));
    while (!number.empty() && (number.back() == '\n' || number.back() == '\r')) number.pop_back();
    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
      throw std::runtime_error("Invalid private display number");
    const auto sdk = ws / "install/anakin_sdk";
    const auto prefix = ws / "install/anakin_env";
    std::map<std::string, std::string> env{
        {"DISPLAY", ":" + number},
        {"ANAKIN_WS", ws.string()},
        {"ROS_LOCALHOST_ONLY", "1"},
        {"ROS_DOMAIN_ID", std::to_string(ros_domain)},
        {"RMW_IMPLEMENTATION", "rmw_fastrtps_cpp"},
        {"ANAKIN_SDK_PREFIX", sdk.string()},
        {"AMENT_PREFIX_PATH", sdk.string() + ":" + prefix.string() + ":/opt/ros/humble"},
        {"LD_LIBRARY_PATH",
         (sdk / "lib").string() + ":" + (prefix / "lib").string() + ":/opt/ros/humble/lib"}};
    const auto sim =
        children.spawn("simulator",
                       {simulator.string(), "-r", "g1", "-i", std::to_string(sdk_domain), "-n",
                        "lo", "-t", "1", "-s", scene.string()},
                       run / "simulator.log", env);
    auto mc_env = env;
    mc_env["ANAKIN_PREFIX"] = (ws / "install/anakin_mc").string();
    std::vector<std::string> mc_arguments{(base / "anakin_mc/en_run.sh").string()};
    if (trace_inference) {
      mc_arguments.push_back("-p");
      mc_arguments.push_back("trace_jsonl:=" + (run / "inference.jsonl").string());
    }
    const auto mc = children.spawn("mc", mc_arguments, run / "mc.log", mc_env);
    auto env_env = env;
    env_env["ANAKIN_PREFIX"] = prefix.string();
    const auto node = children.spawn("env",
                                     {(base / "anakin_env/en_run.sh").string(),
                                      "--loopback-sim",
                                      "-p",
                                      "sdk_domain:=" + std::to_string(sdk_domain),
                                      "-p",
                                      "remote_source:=wireless_topic",
                                      "-p",
                                      "duration:=" + std::to_string(duration),
                                      "-p",
                                      "alignment_seconds:=" + std::to_string(alignment_seconds),
                                      "-p",
                                      "gain_profile:=" + gain_profile,
                                      "-p",
                                      "execution_profile:=" + execution_profile,
                                      "-p",
                                      "min_active_seconds:=5.0",
                                      "-p",
                                      std::string("expected_modes:=") + (demo ? "3" : "1"),
                                      "-p",
                                      "result_json:=" + (run / "native-result.json").string(),
                                      "-p",
                                      "telemetry_csv:=" + (run / "native.csv").string()},
                                     run / "env.log", env_env);
    const auto probe =
        children.spawn("probe",
                       {(prefix / "lib/anakin_env/anakin_sim_probe").string(), "--ros-args", "-p",
                        "sdk_domain:=" + std::to_string(sdk_domain), "-p",
                        "simulator_pid:=" + std::to_string(children.entries[sim].pid), "-p",
                        "alignment_seconds:=" + std::to_string(alignment_seconds), "-p",
                        std::string("suspended:=") + (suspended ? "true" : "false"), "-p",
                        std::string("demo_switch:=") + (demo ? "true" : "false"), "-p",
                        std::string("rc_loss:=") + (rc_loss ? "true" : "false"), "-p",
                        "result_json:=" + (run / "probe-result.json").string()},
                       run / "probe.log", env);
    std::ostringstream manifest;
    manifest << std::boolalpha << "{\n  \"official_binary\": " << std::quoted(simulator.string())
             << ",\n  \"scene\": " << std::quoted(scene.string())
             << ",\n  \"supported_base\": " << suspended
             << ",\n  \"elastic_band\": false,\n  \"sdk_interface\": \"lo\",\n  \"sdk_domain\": "
             << sdk_domain << ",\n  \"ros_domain\": " << ros_domain
             << ",\n  \"duration\": " << duration << ",\n  \"demo_switch\": " << demo
             << ",\n  \"alignment_seconds\": " << alignment_seconds
             << ",\n  \"gain_profile\": " << std::quoted(gain_profile)
             << ",\n  \"execution_profile\": " << std::quoted(execution_profile)
             << ",\n  \"remote_loss_fault\": " << rc_loss << "\n}\n";
    write(run / "manifest.json", manifest.str());
    const auto deadline = Clock::now() + std::chrono::duration<double>(duration + 15);
    std::string failure;
    while (Clock::now() < deadline && !interrupted) {
      children.poll();
      if (!children.entries[node].alive) break;
      if (!children.entries[xserver].alive || !children.entries[sim].alive ||
          !children.entries[mc].alive) {
        failure = "required owned child exited early";
        break;
      }
      if (!children.entries[probe].alive && children.entries[probe].code != 0) {
        failure = "simulation probe failed";
        break;
      }
      std::this_thread::sleep_for(20ms);
    }
    children.poll();
    if (children.entries[node].alive && failure.empty())
      failure = interrupted ? "interrupted" : "simulation deadline";
    children.cleanup();
    const int expected = rc_loss ? 1 : 0;
    const bool passed = failure.empty() && children.entries[node].code == expected &&
                        children.entries[probe].code == 0;
    std::ostringstream summary;
    summary << std::boolalpha << "{\n  \"success\": " << passed
            << ",\n  \"expected_env_exit\": " << expected
            << ",\n  \"env_exit\": " << children.entries[node].code
            << ",\n  \"probe_exit\": " << children.entries[probe].code
            << ",\n  \"failure\": " << std::quoted(failure)
            << ",\n  \"owned_children_reaped\": true\n}\n";
    write(run / "runner-result.json", summary.str());
    std::cout << "SIM_RESULT success=" << passed << " evidence=" << run
              << " env_exit=" << children.entries[node].code
              << " probe_exit=" << children.entries[probe].code << " failure=" << failure
              << std::endl;
    return passed ? 0 : 1;
  } catch (const std::exception& error) {
    if (!run.empty()) write(run / "runner-error.txt", std::string(error.what()) + "\n");
    std::cerr << "simulation runner: " << error.what() << '\n';
    return 1;
  }
}
