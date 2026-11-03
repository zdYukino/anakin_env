// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import App from '../src/App';
import { telemetry, TelemetryStore } from '../src/core/telemetry';
import { decodeControlState } from '../src/core/control';
import { setLanguage } from '../src/core/i18n';

vi.mock('../src/components/RobotViewer', () => ({ RobotViewer: () => null }));
vi.mock('../src/components/Chart', () => ({ TelemetryChart: () => null }));
class Socket {
  static OPEN = 1;
  readyState = 1;
  bufferedAmount = 0;
  sent: Record<string, unknown>[] = [];
  send(text: string) {
    this.sent.push(JSON.parse(text));
  }
  close() {
    this.readyState = 3;
  }
}
const status = (overrides: Record<string, unknown> = {}) => ({
  v: 1,
  kind: 'control_state',
  available: true,
  enabled: true,
  owned: false,
  pending: false,
  leased: false,
  environment: 'env-session',
  active_source: 'none',
  mode: 'real',
  state: 'disarmed',
  requested_state: 'disarmed',
  moving: false,
  height: 0.78,
  timeout_ms: 250,
  message: '',
  actions: {
    disarmed: true,
    align: true,
    hold: false,
    asap_loco: false,
    asap_cr7: false,
    clear_fault: false,
    emergency_stop: true,
    motion_toggle: true,
    height_up: true,
    height_down: true,
  },
  ...overrides,
});
let socket: Socket;
beforeEach(() => {
  vi.useFakeTimers();
  vi.stubGlobal('WebSocket', Socket);
  vi.stubGlobal('PointerEvent', MouseEvent);
  vi.stubGlobal(
    'fetch',
    vi.fn().mockResolvedValue({ ok: true, json: async () => ({ joint_names: [] }) }),
  );
  localStorage.clear();
  setLanguage('en');
  telemetry.reset('ros2');
  telemetry.connection = 'CONNECTED';
  socket = new Socket();
  telemetry.socket = socket as unknown as WebSocket;
  telemetry.packet(status());
});
afterEach(() => {
  cleanup();
  telemetry.dispose();
  vi.useRealTimers();
  vi.unstubAllGlobals();
});

it('requires explicit activation and honors authoritative actions instead of the historical robot frame', () => {
  render(<App />);
  expect(screen.getByRole('complementary', { name: 'Real-time joint angles' })).toBeTruthy();
  expect(socket.sent).toHaveLength(0);
  fireEvent.click(screen.getByRole('button', { name: 'Web control' }));
  expect(socket.sent[0].operation).toBe('activate');
  act(() => telemetry.packet(status({ owned: true, leased: true, active_source: 'web' })));
  const commands = screen.getByRole('complementary', { name: 'Web commands' });
  expect(screen.queryByRole('complementary', { name: 'Real-time joint angles' })).toBeNull();
  expect(
    (within(commands).getByRole('button', { name: 'hold' }) as HTMLButtonElement).disabled,
  ).toBe(true);
  fireEvent.click(within(commands).getByRole('button', { name: 'align' }));
  expect(socket.sent.at(-1)?.action).toBe('align');
  const count = socket.sent.filter((packet) => packet.action === 'align').length;
  act(() => vi.advanceTimersByTime(100));
  expect(socket.sent.filter((packet) => packet.action === 'align')).toHaveLength(count);
  fireEvent.click(screen.getByRole('button', { name: 'Disable web control' }));
  expect(socket.sent.at(-1)?.operation).toBe('release');
  expect(screen.getByRole('complementary', { name: 'Real-time joint angles' })).toBeTruthy();
});

it('opens a floating Joy, uses shoulder combinations, and clears axes/buttons when closed or unfocused', () => {
  telemetry.activateWeb();
  telemetry.packet(status({ owned: true, leased: true, active_source: 'web' }));
  render(<App />);
  fireEvent.click(screen.getByRole('button', { name: 'Open virtual Joy' }));
  const joy = screen.getByRole('dialog', { name: 'Virtual Joy' });
  fireEvent.click(within(joy).getByRole('button', { name: 'RB / R1' }));
  fireEvent.pointerDown(within(joy).getByRole('button', { name: 'X' }), {
    button: 0,
    pointerId: 1,
  });
  expect(telemetry.webInput.shoulders).toBe(2);
  expect(telemetry.webInput.buttons[2]).toBe(true);
  fireEvent.pointerCancel(within(joy).getByRole('button', { name: 'X' }), { pointerId: 1 });
  expect(telemetry.webInput.buttons[2]).toBe(false);
  const stick = within(joy).getByRole('group', { name: 'Left stick' });
  vi.spyOn(stick, 'getBoundingClientRect').mockReturnValue({
    left: 0,
    top: 0,
    width: 112,
    height: 112,
  } as DOMRect);
  fireEvent.pointerDown(stick, { button: 0, pointerId: 2, clientX: 56, clientY: 16 });
  expect(telemetry.webInput.axes[1]).toBe(1);
  fireEvent.pointerUp(stick, { pointerId: 2 });
  expect(telemetry.webInput.axes).toEqual([0, 0, 0, 0]);
  fireEvent.click(within(joy).getByRole('button', { name: 'Close virtual Joy' }));
  expect(telemetry.webInput.shoulders).toBe(0);
  act(() => window.dispatchEvent(new Event('blur')));
  expect(telemetry.webRequested).toBe(false);
  expect(socket.sent.at(-1)?.operation).toBe('release');
});

it('releases on missing control heartbeats, source changes and history without automatically reactivating', () => {
  telemetry.activateWeb();
  telemetry.packet(status({ owned: true, leased: true }));
  vi.advanceTimersByTime(300);
  expect(telemetry.webRequested).toBe(false);
  telemetry.packet(status());
  expect(telemetry.webActive).toBe(false);
  telemetry.activateWeb();
  telemetry.packet(status({ owned: true, leased: true }));
  telemetry.seek(0);
  expect(telemetry.webRequested).toBe(false);
  telemetry.live();
  telemetry.packet(status());
  telemetry.activateWeb();
  telemetry.reset('demo');
  expect(telemetry.webRequested).toBe(false);
  expect(telemetry.control).toBeUndefined();
  expect(telemetry.activateWeb()).toBe(false);
});

it('rejects malformed control state and never allows a second or disconnected client to command', () => {
  expect(decodeControlState(status({ actions: { align: 'yes' } }))).toBeUndefined();
  const other = new TelemetryStore();
  other.source = 'ros2';
  other.connection = 'CONNECTED';
  other.socket = socket as unknown as WebSocket;
  other.packet(status({ leased: true }));
  expect(other.activateWeb()).toBe(false);
  expect(other.command('align')).toBe(false);
  telemetry.activateWeb();
  telemetry.packet(status({ owned: true, leased: true }));
  telemetry.packet(status({ available: false, owned: false, leased: false }));
  expect(telemetry.webRequested).toBe(false);
  expect(telemetry.command('align')).toBe(false);
  other.dispose();
});

it('waits for activation across an older periodic status and bounds the wait', () => {
  expect(telemetry.activateWeb()).toBe(true);
  telemetry.packet(status());
  expect(telemetry.webRequested).toBe(true);
  expect(telemetry.webActive).toBe(false);
  telemetry.packet(status({ pending: true }));
  telemetry.packet(status({ owned: true, leased: true }));
  expect(telemetry.webActive).toBe(true);
  telemetry.packet(status());
  expect(telemetry.webRequested).toBe(false);
  telemetry.activateWeb();
  vi.advanceTimersByTime(300);
  expect(telemetry.webRequested).toBe(false);
  expect(socket.sent.at(-1)?.operation).toBe('release');
});
