import { describe, expect, it, vi } from 'vitest';
import { RingBuffer } from '../src/core/buffer';
import { convertRecords, csvRows, previewRecords, readRecords } from '../src/core/importer';
import { findJoints, robotPose } from '../src/core/robot';
import { TelemetryStore } from '../src/core/telemetry';

describe('bounded signal retention', () => {
  it('evicts oldest samples by time and preserves extrema during reduction', () => {
    const b = new RingBuffer(100, 1);
    for (let i = 0; i < 100; i++) b.push(i / 50, i === 80 ? 1000 : Math.sin(i));
    expect(b.at(0)[0]).toBeGreaterThanOrEqual(0.98);
    expect(b.envelope(1, 2, 10).some((p) => p[1] === 1000)).toBe(true);
    expect(b.nearest(1.6)?.[1]).toBe(1000);
    for (let i = 100; i < 300; i++) b.push(i / 50, i);
    expect(b.length).toBeLessThanOrEqual(51);
    expect(b.at(b.length - 1)).toEqual([5.98, 299]);
  });
});
describe('offline import', () => {
  it('handles quotes, newlines, escaped quotes and CRLF without dropping columns', () => {
    expect(csvRows('time,name,value\r\n0,"joint, a",2\r\n1,"say ""ok""\nnext",3')).toEqual([
      ['time', 'name', 'value'],
      ['0', 'joint, a', '2'],
      ['1', 'say "ok"\nnext', '3'],
    ]);
    expect(() => csvRows('t,x\n0,"bad')).toThrow('Unclosed');
  });
  it('maps milliseconds, sorts source time, and reports missing values', () => {
    const rows = readRecords('timestamp,x\n2000,2\n1000,1\n3000,NaN', 'run.csv');
    const result = convertRecords(rows, 'timestamp', 'ms', ['/recording::x']);
    expect([...result.columns[0].times]).toEqual([1, 2]);
    expect([...result.columns[0].values]).toEqual([1, 2]);
    expect(result.warnings).toHaveLength(2);
    expect(result.start).toBe(1);
    expect(result.end).toBe(3);
  });
  it('supports nested ROS headers and named robot state without losing fields', () => {
    const rows = readRecords(
      JSON.stringify([
        {
          topic: '/state',
          data: {
            header: { stamp: { sec: 5, nanosec: 500000000 } },
            joints: { name: ['left_knee_joint'], position: [0.2] },
          },
        },
      ]),
      'run.json',
    );
    const preview = previewRecords(rows);
    expect(preview.guessed).toBe('data.header.stamp.sec');
    expect(preview.unit).toBe('ros');
    const result = convertRecords(rows, preview.guessed, preview.unit, [
      '/state::joints.position[0]',
    ]);
    expect(result.start).toBe(5.5);
    expect(result.columns[0].values[0]).toBe(0.2);
    expect(findJoints(result.snapshots[0].data)?.name).toEqual(['left_knee_joint']);
  });
  it('rejects malformed rows, missing time and unsupported formats', () => {
    expect(() => readRecords('t,x\n0', 'run.csv')).toThrow('row 2');
    expect(() => readRecords('', 'run.mcap')).toThrow('MCAP');
    const records = readRecords('[{"x":1}]', 'run.json');
    expect(() => convertRecords(records, 'timestamp', 's', ['/recording::x'])).toThrow('invalid');
  });
});
describe('shared telemetry state', () => {
  it('continues sampling while paused and historical snapshots follow cursor', () => {
    const store = new TelemetryStore();
    store.source = 'demo';
    store.addChannel('/state', 'x');
    store.ingest({ topic: '/state', t: 100, values: { x: 1 } });
    store.snapshot({ topic: '/state', t: 100, data: { x: 1 } });
    store.togglePause();
    store.ingest({ topic: '/state', t: 101, values: { x: 2 } });
    store.snapshot({ topic: '/state', t: 101, data: { x: 2 } });
    expect(store.cursor).toBe(100);
    expect(store.end).toBe(101);
    expect(store.at('/state')?.data).toEqual({ x: 1 });
    store.live();
    expect(store.at('/state')?.data).toEqual({ x: 2 });
  });
  it('starts a new segment when source clock resets', () => {
    const store = new TelemetryStore();
    store.addChannel('/state', 'x');
    store.ingest({ topic: '/state', t: 100, values: { x: 1 } });
    store.ingest({ topic: '/state', t: 90, values: { x: 2 } });
    expect(store.buffers.get('/state::x')?.length).toBe(1);
    expect(store.logs.at(-1)?.text).toContain('clock reset');
  });
});

it('preserves invalid feedback as unavailable, and honors source validity flags', () => {
  expect(findJoints({ validity: 0, joints: { name: ['a'], position: [1] } })).toBeUndefined();
  const joints = findJoints({ joints: { name: ['a', 'b'], position: [null, 0.2] } });
  expect(Number.isNaN(joints?.position[0])).toBe(true);
  expect(joints?.position[1]).toBe(0.2);
});

it('holds the last valid live pose across missing sensor groups and accepts recovery', () => {
  const valid = {
    validity: 19,
    joints: { name: ['a'], position: [0.4] },
    imu: { orientation: { x: 0, y: 0, z: 0, w: 1 } },
    odometry: { pose: { pose: { position: { z: 0.5 } } } },
  };
  const first = robotPose(valid);
  const dropped = robotPose({ ...valid, validity: 0 }, first);
  expect(dropped.height).toBe(0.5);
  expect(dropped.orientation).toEqual([0, 0, 0, 1]);
  expect(dropped.angles.get('a')).toBe(0.4);
  expect(
    robotPose({ ...valid, odometry: { pose: { pose: { position: { z: 0.7 } } } } }, dropped).height,
  ).toBe(0.7);
  expect(robotPose({ validity: 0 }).height).toBeUndefined();
});

it('does not recreate channels the operator removed when graph discovery refreshes', () => {
  const store = new TelemetryStore();
  const packet = {
    v: 1,
    kind: 'topics',
    topics: [
      {
        name: '/anakin/state',
        type: 'anakin_sdk/msg/RobotStateFrame',
        qos: [],
        publishers: 1,
        subscribers: 0,
      },
    ],
  };
  store.packet(packet);
  expect(store.channels).toHaveLength(3);
  [...store.channels].forEach((c) => store.removeChannel(c.id));
  store.packet(packet);
  expect(store.channels).toHaveLength(0);
});

it('keeps robot state subscribed when inspecting another topic', () => {
  vi.useFakeTimers();
  vi.stubGlobal('WebSocket', { OPEN: 1 });
  try {
    const store = new TelemetryStore();
    const send = vi.fn();
    store.socket = { readyState: 1, send } as unknown as WebSocket;
    store.topics = [
      {
        name: '/anakin/state',
        type: 'RobotStateFrame',
        qos: [],
        publishers: 1,
        subscribers: 0,
        hz: 0,
        bytes_per_second: 0,
        last_receive_ns: null,
      },
      {
        name: '/other',
        type: 'Other',
        qos: [],
        publishers: 1,
        subscribers: 0,
        hz: 0,
        bytes_per_second: 0,
        last_receive_ns: null,
      },
    ];
    store.selectedTopic = '/other';
    store.addChannel('/other', 'value');
    vi.advanceTimersByTime(41);
    const packet = JSON.parse(send.mock.calls[0][0] as string);
    expect(packet.subscriptions).toEqual([
      { topic: '/anakin/state', fields: [], qos: 'auto' },
      { topic: '/other', fields: ['value'], qos: 'auto' },
    ]);
  } finally {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  }
});

it('isolates foreign clock domains and resets large jumps on a known Topic', () => {
  const store = new TelemetryStore();
  store.addChannel('/a', 'x');
  store.ingest({ topic: '/a', t: 10000, values: { x: 1 } });
  store.ingest({ topic: '/b', t: 5, values: { x: 2 } });
  store.snapshot({ topic: '/b', t: 5, data: { x: 2 } });
  expect(store.at('/b')).toBeUndefined();
  expect(store.end).toBe(10000);
  store.ingest({ topic: '/a', t: 5, values: { x: 3 } });
  expect(store.end).toBe(5);
  expect(store.buffers.get('/a::x')?.at(0)).toEqual([5, 3]);
});

it('does not fabricate full-rate offline channels from decimated message snapshots', () => {
  const store = new TelemetryStore();
  store.source = 'file';
  store.snapshot({ topic: '/recording', t: 1, data: { x: 2 } });
  store.addChannel('/recording', 'x');
  expect(store.channels).toHaveLength(0);
  expect(store.error).toContain('Reopen the recording');
});
