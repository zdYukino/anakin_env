// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import App from '../src/App';
import { telemetry, TelemetryStore } from '../src/core/telemetry';
import { controllerInput, decodeInputConfig, INPUT_TOPIC } from '../src/core/inputs';
import { loadLayout } from '../src/core/layout';
import { switchEvents } from '../src/core/events';

vi.mock('../src/components/RobotViewer', () => ({ RobotViewer: () => null }));
vi.mock('../src/components/Chart', () => ({ TelemetryChart: () => null }));
const configuration = (button = 'Y') => ({
  v: 1,
  kind: 'input_config',
  available: true,
  node: '/custom_env',
  path: '/running/custom-inputs.yaml',
  mode_path: '/running/sim.yaml',
  input_timeout_ms: 250,
  config: {
    schema_version: 2,
    primary: 'remote',
    enabled_sources: ['gamepad', 'remote'],
    gamepad: { device_index: 3 },
    remote: { channel: 'wireless_topic' },
    motion: { deadzone: 0.12, velocity_axes: { forward: { axis: 'right_y', scale: 0.8 } } },
    controller_bindings: [{ button, shoulders: ['R1'], trigger: 'released', action: 'hold' }],
    web: { actions: ['hold', 'emergency_stop'] },
  },
});
const report = (gamepadOnline = true) => ({
  status: ['gamepad', 'remote'].map((source) => ({
    name: `anakin_env/input/${source}`,
    values: Object.entries({
      schema_version: '1',
      enabled: 'true',
      primary: String(source === 'remote'),
      connected: 'true',
      online: String(source === 'remote' || gamepadOnline),
      age_ms: source === 'remote' ? '4' : '12',
      'axis.left_x': source === 'remote' ? '-0.5' : '0.25',
      'axis.left_y': '0.75',
      'axis.right_x': '0.125',
      'axis.right_y': source === 'gamepad' ? '-0.5' : '0.1',
      'button.A': String(source === 'gamepad'),
      'button.L1': String(source === 'gamepad'),
      'button.R1': String(source === 'remote'),
      ...(source === 'gamepad' ? { 'trigger.LT': '0.25', 'trigger.RT': '0.75' } : {}),
    }).map(([key, value]) => ({ key, value })),
  })),
});

beforeEach(() => {
  vi.spyOn(performance, "now").mockReturnValue(0);
  localStorage.clear();
  telemetry.reset('ros2');
  telemetry.connection = 'CONNECTED';
  vi.stubGlobal(
    'fetch',
    vi.fn().mockResolvedValue({ ok: true, json: async () => ({ joint_names: [] }) }),
  );
});
afterEach(() => {
  cleanup();
  telemetry.dispose();
  vi.unstubAllGlobals();
  vi.useRealTimers();
  vi.restoreAllMocks();
});

it('shows independent Joy and remote readings and only the running YAML bindings', () => {
  telemetry.packet(configuration());
  telemetry.ingest({ topic: INPUT_TOPIC, t: 10, values: {} });
  telemetry.snapshot({ topic: INPUT_TOPIC, t: 10, data: report() });
  render(<App />);
  expect(screen.queryByRole('button', { name: 'Robotics' })).toBeNull();
  fireEvent.click(screen.getByRole('button', { name: 'Controllers' }));
  const joy = screen.getByRole('region', { name: 'Joy gamepad' });
  const remote = screen.getByRole('region', { name: 'Unitree remote' });
  expect(within(joy).getByText('0.250')).toBeTruthy();
  expect(within(remote).getByText('-0.500')).toBeTruthy();
  expect(within(joy).getByLabelText('A: PRESSED')).toBeTruthy();
  expect(within(joy).getByLabelText('LB: PRESSED')).toBeTruthy();
  expect(within(joy).queryByText('L1')).toBeNull();
  expect(within(joy).getByRole('meter', { name: 'LT' }).getAttribute('aria-valuenow')).toBe('0.25');
  expect(within(joy).getByRole('meter', { name: 'RT' }).getAttribute('aria-valuenow')).toBe('0.75');
  expect(within(remote).queryByRole('meter')).toBeNull();
  expect(
    within(joy)
      .getByRole('img', { name: 'RIGHT STICK' })
      .querySelector('.stick-position')
      ?.getAttribute('cy'),
  ).toBe('33');
  expect(within(remote).getByLabelText('A: RELEASED')).toBeTruthy();
  expect(within(remote).getByText('PRIMARY INPUT')).toBeTruthy();
  const guide = screen.getByRole('complementary', { name: 'State switching guide' });
  expect(within(guide).getByText('/running/custom-inputs.yaml')).toBeTruthy();
  expect(within(guide).getByText('released')).toBeTruthy();
  expect(within(guide).getByText('RB / R1')).toBeTruthy();
  expect(within(guide).getByText('Y')).toBeTruthy();
  expect(within(guide).queryByText('X')).toBeNull();
  act(() => telemetry.packet(configuration('Start')));
  expect(within(guide).getByText('Start')).toBeTruthy();
  expect(within(guide).queryByText('Y')).toBeNull();
  act(() => {
    telemetry.snapshotArrival.set(INPUT_TOPIC, performance.now() - 3000);
    telemetry.emit();
  });
  expect(within(joy).getByText('LAST RECEIVED FRAME')).toBeTruthy();
  expect(within(joy).getByLabelText('A: UNAVAILABLE')).toBeTruthy();
  act(() => {
    telemetry.seek(10);
  });
  expect(within(joy).getByText('HISTORICAL FRAME')).toBeTruthy();
  expect(within(joy).getByText('0.250')).toBeTruthy();
  act(() => {
    telemetry.reset('none');
  });
  expect(within(joy).getByText('NO INPUT DATA')).toBeTruthy();
  expect(within(guide).queryByText('Start')).toBeNull();
});

it('does not label an expired device as live while the other source remains online', () => {
  telemetry.packet(configuration());
  telemetry.ingest({ topic: INPUT_TOPIC, t: 10, values: {} });
  telemetry.snapshot({ topic: INPUT_TOPIC, t: 10, data: report(false) });
  render(<App />);
  fireEvent.click(screen.getByRole('button', { name: 'Controllers' }));
  expect(
    within(screen.getByRole('region', { name: 'Joy gamepad' })).getByText('STALE'),
  ).toBeTruthy();
  expect(
    within(screen.getByRole('region', { name: 'Unitree remote' })).getByText('LIVE'),
  ).toBeTruthy();
});

it('rejects malformed configuration and missing or invalid input values', () => {
  expect(decodeInputConfig(configuration())?.controllerBindings[0].control).toBe('Y');
  expect(decodeInputConfig({ ...configuration(), config: null })).toBeUndefined();
  const bad = configuration();
  bad.config.controller_bindings[0].shoulders = [null as unknown as string];
  expect(decodeInputConfig(bad)).toBeUndefined();
  expect(controllerInput(null, 'remote')).toBeUndefined();
  const input = report();
  input.status[0].values.push(
    { key: 'axis.right_x', value: '' },
    { key: 'axis.right_y', value: 'NaN' },
  );
  expect(controllerInput(input, 'gamepad')?.axes.right_x).toBeUndefined();
  expect(controllerInput(input, 'gamepad')?.axes.right_y).toBeUndefined();
});

const switchStatus = (sequence: number, result: string, session = 'env-events') => ({
  name: 'anakin_env/switch_event',
  hardware_id: session,
  values: Object.entries({
    schema_version: '1',
    sequence: String(sequence),
    time_ms: '1793700000000',
    source: 'gamepad',
    from: 'hold',
    target: 'asap_loco',
    current: result === 'success' ? 'asap_loco' : 'preparing',
    result,
    detail: 'Runtime confirmation',
  }).map(([key, value]) => ({ key, value })),
});

it('mirrors confirmed input switching events into the footer and event log without duplicates', () => {
  const pending = switchStatus(1, 'pending');
  const data = { status: [pending] };
  telemetry.ingest({ topic: INPUT_TOPIC, t: 10, values: {} });
  telemetry.snapshot({ topic: INPUT_TOPIC, t: 10, data });
  render(<App />);
  const indicator = screen.getByRole('button', { name: 'Open event log' });
  expect(within(indicator).getByRole('status').textContent).toBe('gamepad → asap_loco · pending');
  act(() =>
    telemetry.packet({
      v: 1,
      kind: 'batch',
      samples: [],
      snapshots: [
        { topic: INPUT_TOPIC, t: 10, data: { status: [pending, switchStatus(2, 'success')] } },
      ],
    }),
  );
  expect(within(indicator).getByRole('status').textContent).toBe('gamepad → asap_loco · success');
  act(() =>
    telemetry.packet({
      v: 1,
      kind: 'batch',
      samples: [],
      snapshots: [
        { topic: INPUT_TOPIC, t: 10, data: { status: [pending, switchStatus(2, 'success')] } },
      ],
    }),
  );
  expect(telemetry.logs.filter((log) => log.controlEvent)).toHaveLength(2);
  fireEvent.click(indicator);
  expect(screen.getByRole('heading', { name: 'Diagnostics' })).toBeTruthy();
  expect(screen.getByRole('table').textContent).toContain('gamepad → asap_loco · success');
  act(() =>
    telemetry.packet({
      v: 1,
      kind: 'batch',
      samples: [],
      snapshots: [{ topic: INPUT_TOPIC, t: 10, data: { status: [switchStatus(3, 'failed')] } }],
    }),
  );
  expect(telemetry.latestEvent?.level).toBe('ERROR');
  expect(within(indicator).getByRole('status').textContent).toContain('failed');
  expect(switchEvents({ status: [switchStatus(4, 'unconfirmed')] })).toEqual([]);
  expect(switchEvents({ status: [switchStatus(1, 'success', 'another-env')] })[0].id).toBe(
    'another-env:1',
  );
});

it('migrates saved layouts without restoring the removed robot panel', () => {
  localStorage.setItem(
    'aerospace.layout.v1',
    JSON.stringify({
      v: 1,
      order: ['robot', 'topics', 'trend', 'status', 'imu'],
      hidden: ['robot', 'imu'],
      collapsed: ['robot', 'topics'],
    }),
  );
  const layout = loadLayout();
  expect(layout.order).toEqual(['topics', 'trend', 'status', 'imu']);
  expect(layout.hidden).toEqual(['imu']);
  expect(layout.collapsed).toEqual(['topics']);
});

it('subscribes input telemetry when discovered late and retains it while inspecting other topics', () => {
  vi.useFakeTimers();
  vi.stubGlobal('WebSocket', { OPEN: 1 });
  const store = new TelemetryStore();
  const send = vi.fn();
  store.socket = { readyState: 1, send, close() {} } as unknown as WebSocket;
  const topic = (name: string) => ({ name, type: 'diagnostic_msgs/msg/DiagnosticArray', qos: [] });
  store.packet({ v: 1, kind: 'topics', topics: [topic('/other')] });
  vi.advanceTimersByTime(50);
  store.packet({ v: 1, kind: 'topics', topics: [topic('/other'), topic(INPUT_TOPIC)] });
  vi.advanceTimersByTime(50);
  const packet = JSON.parse(send.mock.calls.at(-1)![0]);
  expect(packet.subscriptions.map((s: { topic: string }) => s.topic)).toEqual(
    expect.arrayContaining(['/other', INPUT_TOPIC]),
  );
  store.dispose();
});
