import type { Json, Sample, Snapshot } from './types';
import { telemetry } from './telemetry';

export async function startDemo() {
  telemetry.reset('demo');
  const session = telemetry.session;
  telemetry.log('INFO', 'DEMO selected · deterministic 500 Hz synthetic telemetry');
  const response = await fetch('/models/g1/model.json');
  if (!response.ok) throw new Error('G1 model manifest unavailable');
  const { joint_names: names } = (await response.json()) as { joint_names: string[] };
  if (telemetry.source !== 'demo' || telemetry.session !== session) return;
  const epoch = Date.UTC(2026, 10, 3, 1, 41) / 1000;
  const initial = 10;
  const defaults: [string, string][] = [
    ['joints.position[0]', 'L. HIP PITCH'],
    ['joints.position[3]', 'L. KNEE'],
    ['joints.position[6]', 'R. HIP PITCH'],
    ['joints.position[9]', 'R. KNEE'],
    ['imu.angular_velocity.x', 'GYRO X'],
    ['imu.angular_velocity.y', 'GYRO Y'],
  ];
  defaults.forEach(([p, l]) => telemetry.addChannel('/anakin/state', p, l));
  const dataAt = (t: number): Json => {
    const position = names.map((name, i) => {
      const phase = /right/.test(name) ? Math.PI : 0;
      if (/hip_pitch/.test(name)) return -0.12 + Math.sin(t * 2.6 + phase) * 0.16;
      if (/knee/.test(name)) return 0.32 + Math.sin(t * 2.6 + phase - 0.5) * 0.22;
      if (/ankle_pitch/.test(name)) return -0.16 + Math.sin(t * 2.6 + phase + 0.5) * 0.08;
      if (/shoulder_roll/.test(name)) return /left/.test(name) ? 0.16 : -0.16;
      if (/elbow/.test(name)) return 0.25;
      return Math.sin(t * 1.4 + i) * 0.012;
    });
    return {
      joints: {
        name: names,
        position,
        velocity: position.map((_, i) => Math.cos(t * 2.6 + i) * 0.15),
        effort: position.map((_, i) => Math.sin(t * 2.6 + i) * 2.1),
      },
      imu: {
        orientation: { x: 0.001, y: 0.005, z: 0, w: 0.99998 },
        angular_velocity: {
          x: Math.sin(t * 2.4) * 0.036 + Math.sin(t * 17) * 0.004,
          y: Math.cos(t * 2.4) * 0.024,
          z: Math.sin(t) * 0.006,
        },
        linear_acceleration: {
          x: Math.sin(t) * 0.09,
          y: Math.cos(t) * 0.04,
          z: 9.806 + Math.sin(t * 2.6) * 0.12,
        },
      },
      odometry: {
        pose: { pose: { position: { x: t * 0.12, y: 0.003, z: 0.793 } } },
        twist: { twist: { linear: { x: 0.12, y: 0, z: 0 }, angular: { x: 0, y: 0, z: 0.006 } } },
      },
      header: {
        stamp: { sec: Math.floor(epoch + t), nanosec: Math.round(((epoch + t) % 1) * 1e9) },
        frame_id: 'base_link',
      },
      state_seq: Math.round(t * 500),
      schema_version: 3,
      validity: 31,
      execution_mode: 2,
      execution_authorized: false,
      diagnostics: {
        status: [
          { level: 0, name: 'demo/telemetry', message: 'Synthetic data · no robot connected' },
        ],
      },
    };
  };
  function generate(index: number, snapshot: boolean) {
    const elapsed = index / 500,
      t = epoch + elapsed;
    const data = dataAt(elapsed);
    // Same normalized paths as the ROS adapter, while labels always identify DEMO.
    const values: Record<string, number> = {};
    const root = data as {
      joints: { position: number[]; velocity: number[]; effort: number[] };
      imu: {
        angular_velocity: { x: number; y: number; z: number };
        linear_acceleration: { x: number; y: number; z: number };
      };
    };
    root.joints.position.forEach((v, i) => {
      values[`joints.position[${i}]`] = v;
      values[`joints.velocity[${i}]`] = root.joints.velocity[i];
      values[`joints.effort[${i}]`] = root.joints.effort[i];
    });
    for (const axis of ['x', 'y', 'z'] as const) {
      values[`imu.angular_velocity.${axis}`] = root.imu.angular_velocity[axis];
      values[`imu.linear_acceleration.${axis}`] = root.imu.linear_acceleration[axis];
    }
    const sample: Sample = {
      topic: '/anakin/state',
      t,
      values,
      seq: index,
      clock: 'source',
      source_ns: String(BigInt(Math.round(t * 1000)) * 1000000n),
      receive_ns: String(BigInt(Math.round(t * 1000)) * 1000000n + 1200000n),
    };
    telemetry.ingest(sample);
    if (snapshot) telemetry.snapshot({ ...sample, data } as Snapshot);
    telemetry.byteCount += 280;
  }
  for (let i = 0; i < initial * 500; i++) generate(i, i % 17 === 0);
  telemetry.topics = [
    {
      name: '/anakin/state',
      type: 'anakin_sdk/msg/RobotStateFrame',
      hz: 500,
      bytes_per_second: 140000,
      publishers: 1,
      subscribers: 1,
      last_receive_ns: null,
      qos: [{ reliability: 'BEST_EFFORT', durability: 'VOLATILE', depth: 1 }],
    },
    {
      name: '/joint_states',
      type: 'sensor_msgs/msg/JointState',
      hz: 0,
      bytes_per_second: 0,
      publishers: 0,
      subscribers: 0,
      last_receive_ns: null,
      qos: [],
    },
    {
      name: '/imu/data',
      type: 'sensor_msgs/msg/Imu',
      hz: 0,
      bytes_per_second: 0,
      publishers: 0,
      subscribers: 0,
      last_receive_ns: null,
      qos: [],
    },
    {
      name: '/odometry',
      type: 'nav_msgs/msg/Odometry',
      hz: 0,
      bytes_per_second: 0,
      publishers: 0,
      subscribers: 0,
      last_receive_ns: null,
      qos: [],
    },
    {
      name: '/anakin/control_target',
      type: 'anakin_sdk/msg/ControlTarget',
      hz: 0,
      bytes_per_second: 0,
      publishers: 0,
      subscribers: 0,
      last_receive_ns: null,
      qos: [],
    },
    {
      name: '/diagnostics',
      type: 'diagnostic_msgs/msg/DiagnosticArray',
      hz: 0,
      bytes_per_second: 0,
      publishers: 0,
      subscribers: 0,
      last_receive_ns: null,
      qos: [],
    },
  ];
  telemetry.resetRateWindow();
  let index = initial * 500;
  const started = performance.now();
  const timer = window.setInterval(() => {
    const target = initial * 500 + Math.floor((performance.now() - started) / 2);
    const count = Math.min(100, target - index);
    for (let i = 0; i < count; i++) generate(index++, i === count - 1);
  }, 20);
  telemetry.stopSource = () => clearInterval(timer);
  telemetry.cursor = telemetry.end;
  telemetry.emit();
}
