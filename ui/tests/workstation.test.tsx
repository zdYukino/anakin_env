// @vitest-environment jsdom
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { act, cleanup, fireEvent, render, screen, within } from '@testing-library/react';
import App from '../src/App';
import { telemetry } from '../src/core/telemetry';
vi.mock('../src/components/RobotViewer', () => ({
  RobotViewer: () => <div data-testid="model-slot">Model renderer</div>,
}));
vi.mock('../src/components/Chart', () => ({
  TelemetryChart: ({ title }: { title: string }) => <div>{title}</div>,
}));
class SocketStub {
  static OPEN = 1;
  readyState = 0;
  onerror?: () => void;
  onclose?: () => void;
  constructor(public url: string) {
    SocketStub.current = this;
  }
  static current: SocketStub;
  close() {}
  send() {}
}
beforeEach(() => {
  localStorage.clear();
  telemetry.reset('none');
  vi.stubGlobal('WebSocket', SocketStub);
  vi.stubGlobal(
    'fetch',
    vi.fn().mockResolvedValue({
      ok: true,
      json: async () => ({ joint_names: ['left_hip_pitch_joint', 'right_shoulder_pitch_joint'] }),
    }),
  );
});
afterEach(() => {
  cleanup();
  telemetry.dispose();
  vi.unstubAllGlobals();
});
describe('workstation interactions (DOM; not browser visual verification)', () => {
  it('starts disconnected, changes views, and reports errors without automatic demo', () => {
    render(<App />);
    expect(screen.getAllByText('DISCONNECTED').length).toBeGreaterThan(0);
    fireEvent.click(screen.getByRole('button', { name: 'Connect source' }));
    const dialog = screen.getByRole('dialog');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Connect gateway' }));
    expect(telemetry.source).toBe('ros2');
    SocketStub.current.onerror?.();
    expect(telemetry.connection).toBe('ERROR');
    expect(telemetry.source).toBe('ros2');
    fireEvent.click(screen.getByRole('button', { name: 'Topics' }));
    expect(screen.getByRole('heading', { name: 'Topics' })).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(screen.getByText('Numeric precision')).toBeTruthy();
  });
  it('removes, adds, collapses and restores panels; saves layout and changes precision', () => {
    render(<App />);
    fireEvent.click(screen.getByRole('button', { name: 'Workbench' }));
    fireEvent.click(screen.getByRole('button', { name: 'Remove ROBOT STATE' }));
    expect(screen.queryByTestId('model-slot')).toBeNull();
    fireEvent.change(screen.getByRole('combobox', { name: 'Add data view' }), {
      target: { value: 'robot' },
    });
    expect(screen.getByTestId('model-slot')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Collapse ROBOT STATE' }));
    expect(screen.queryByTestId('model-slot')).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'Expand ROBOT STATE' }));
    fireEvent.click(screen.getByRole('button', { name: 'Save layout' }));
    expect(JSON.parse(localStorage.getItem('aerospace.layout.v1')!).hidden).toEqual([]);
    fireEvent.click(screen.getByRole('button', { name: 'Collapse inspector' }));
    expect(screen.queryByRole('button', { name: 'Collapse inspector' })).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'Open inspector' }));
    expect(screen.getByRole('button', { name: 'Collapse inspector' })).toBeTruthy();
  });
  it('lets a numeric field create a plotted channel, then hide and remove it', () => {
    telemetry.source = 'demo';
    telemetry.selectedTopic = '/recording';
    telemetry.cursor = 1;
    telemetry.snapshot({ topic: '/recording', t: 1, data: { value: 2.5 } });
    render(<App />);
    fireEvent.click(screen.getByRole('button', { name: 'Workbench' }));
    fireEvent.click(screen.getByRole('button', { name: 'Topics' }));
    fireEvent.click(screen.getAllByRole('button', { name: 'Plot value' })[0]);
    expect(telemetry.channels[0].path).toBe('value');
    fireEvent.click(screen.getByRole('button', { name: 'Telemetry' }));
    fireEvent.click(screen.getByRole('checkbox'));
    expect(telemetry.channels[0].visible).toBe(false);
    fireEvent.click(screen.getByRole('button', { name: 'Remove channel value' }));
    expect(telemetry.channels).toHaveLength(0);
  });
  it('shows named joint angles, filters groups and identifies a disconnected last frame', async () => {
    telemetry.source = 'ros2';
    telemetry.connection = 'CONNECTED';
    telemetry.ingest({ topic: '/anakin/state', t: 10, values: {} });
    telemetry.snapshot({
      topic: '/anakin/state',
      t: 10,
      data: {
        validity: 19,
        joints: {
          name: ['left_hip_pitch_joint', 'right_shoulder_pitch_joint'],
          position: [Math.PI / 2, null],
        },
        imu: { angular_velocity: { x: 0, y: 0, z: 0 } },
        odometry: { pose: { pose: { position: { z: 0.8 } } } },
      },
    });
    render(<App />);
    const joints = screen.getByRole('complementary', { name: 'Real-time joint angles' });
    expect(await within(joints).findByText('L / HIP PITCH')).toBeTruthy();
    expect(within(joints).getByText('90.0')).toBeTruthy();
    expect(
      (within(joints).getByRole('button', { name: /R \/ SHOULDER PITCH/ }) as HTMLButtonElement)
        .disabled,
    ).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: 'LOWER' }));
    expect(within(joints).queryByText('R / SHOULDER PITCH')).toBeNull();
    fireEvent.click(within(joints).getByRole('button', { name: /L \/ HIP PITCH/ }));
    expect(telemetry.channels[0].path).toBe('joints.position[0]');
    act(() => {
      telemetry.snapshotArrival.set('/anakin/state', performance.now() - 3000);
      telemetry.emit();
    });
    expect(screen.getAllByText(/LAST RECEIVED FRAME/).length).toBeGreaterThan(0);
    act(() => {
      telemetry.connection = 'DISCONNECTED';
      telemetry.mode = 'HISTORY';
      telemetry.emit();
    });
    expect(screen.getAllByText(/LAST RECEIVED FRAME/).length).toBeGreaterThan(0);
    expect(screen.getByRole('button', { name: 'Return to live' })).toBeTruthy();
  });
  it('can hide and restore the overview instruments', () => {
    render(<App />);
    expect(screen.getByText('ANGULAR SPEED')).toBeTruthy();
    fireEvent.click(screen.getByRole('button', { name: 'Hide instruments' }));
    expect(screen.queryByText('ANGULAR SPEED')).toBeNull();
    expect(localStorage.getItem('aerospace.overview.gauges')).toBe('hidden');
    fireEvent.click(screen.getByRole('button', { name: 'Show instruments' }));
    expect(screen.getByText('ANGULAR SPEED')).toBeTruthy();
  });
});
