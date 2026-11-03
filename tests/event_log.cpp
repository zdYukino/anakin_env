#include <anakin_env/event_log.hpp>
#include <iostream>

using namespace anakin::env;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
}
int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("Expected config directory");
    const auto path = std::filesystem::path(argv[1]);
    auto config = load_input_config(path / "inputs.yaml");
    config.primary = InputSource::Gamepad;
    const auto mode = load_mode_config(path / "sim.yaml", RuntimeMode::Sim);
    StateMachine machine(mode);
    SwitchEventLog log;
    InputFrame request;
    request.events = {Event::manual(State::AsapLoco)};
    const auto preparing = machine.dispatch(request.events);
    log.observe(request, config, mode, preparing, "");
    check(log.events().size() == 1 && log.events().back().source == "gamepad" &&
          log.events().back().target == "asap_loco" && log.events().back().result == "pending",
          "Accepted Preparing request is pending, never successful");
    log.observe({}, config, mode, machine.dispatch({}), "");
    check(log.events().size() == 1, "Waiting does not fabricate completion events");
    auto completed = preparing;
    completed.before = preparing.after;
    completed.after.state = completed.after.effective_state = State::AsapLoco;
    completed.cause = EventKind::PolicyReady;
    log.observe({}, config, mode, completed, "");
    check(log.events().back().source == "gamepad" && log.events().back().result == "success",
          "Runtime completion retains the original input source");

    SwitchEventLog failed;
    failed.observe(request, config, mode, preparing, "");
    auto fault = preparing;
    fault.before = preparing.after;
    fault.after.state = State::Fault;
    fault.cause = EventKind::Fault;
    failed.observe({}, config, mode, fault, "MC rejected current transaction");
    check(failed.events().size() == 2 && failed.events().back().source == "gamepad" &&
          failed.events().back().target == "asap_loco" && failed.events().back().result == "failed" &&
          failed.events().back().detail == "MC rejected current transaction", "Failure does not lose request context");

    StateMachine stop_machine(mode);
    SwitchEventLog stops;
    InputFrame stop;
    stop.events = {Event::system(EventKind::EmergencyStop)};
    stop.snapshot.emergency_sources[static_cast<size_t>(InputSource::Remote)] = true;
    stops.observe(stop, config, mode, stop_machine.dispatch(stop.events), "");
    for (int i = 0; i < 1000; ++i) stops.observe(stop, config, mode, stop_machine.dispatch(stop.events), "");
    check(stops.events().size() == 1 && stops.events()[0].source == "remote" &&
          stops.events()[0].target == "damping", "Non-primary stop has its actual source and is not logged at 500Hz");

    SwitchEventLog guarded;
    guarded.observe(request, config, mode, StateMachine(mode).dispatch({}), "");
    check(guarded.events().back().result == "failed", "Guard-filtered request is not silently labelled successful");
    StateMachine bounded_machine(mode);
    SwitchEventLog bounded;
    for (int i = 0; i < 100; ++i) {
      InputFrame item;
      item.events = {Event::manual(i % 2 ? State::Hold : State::Disarmed)};
      bounded.observe(item, config, mode, bounded_machine.dispatch(item.events), "");
    }
    check(bounded.events().size() == 32 && bounded.events().back().sequence == 100,
          "Cumulative runtime log is bounded and sequences stay monotonic");
    std::cout << "Switch event source, result, completion, deduplication and bounds passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
