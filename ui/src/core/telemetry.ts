import { RingBuffer } from './buffer';
import { channelId, COLORS, unitFor, isRecord } from './types';
import type { Channel, Connection, Json, Sample, Snapshot, Topic, SchemaField } from './types';
import { decodeInputConfig, INPUT_TOPIC } from './inputs';
import type { RuntimeInputConfig } from './inputs';
import { switchEvents } from './events';
import type { SwitchEvent } from './events';
import { decodeControlState, neutralInput } from './control';
import type { ControlState, WebInput } from './control';

export interface LogEntry {
  time: number;
  level: 'INFO' | 'WARN' | 'ERROR';
  text: string;
  controlEvent?: SwitchEvent;
}
export class TelemetryStore {
  source: 'none' | 'ros2' | 'demo' | 'file' = 'none';
  connection: Connection = 'DISCONNECTED';
  topics: Topic[] = [];
  schemas = new Map<string, { fields: SchemaField[]; type: string }>();
  channels: Channel[] = [];
  buffers = new Map<string, RingBuffer>();
  snapshots = new Map<string, Snapshot[]>();
  snapshotArrival = new Map<string, number>();
  logs: LogEntry[] = [];
  latestEvent?: LogEntry;
  private controlEventKeys = new Set<string>();
  listeners = new Set<() => void>();
  version = 0;
  mode: 'LIVE' | 'PAUSED' | 'HISTORY' | 'PLAYBACK' = 'LIVE';
  cursor = 0;
  start = 0;
  end = 0;
  origin = 0;
  viewEnd: number | undefined;
  window = 10;
  speed = 1;
  playing = false;
  received = 0;
  dropped = 0;
  byteCount = 0;
  hz = 0;
  bandwidth = 0;
  frameMs = 0;
  error = '';
  fileName = '';
  clock = '';
  qos = 'auto';
  notation: 'fixed' | 'scientific' = 'fixed';
  selectedTopic = '/anakin/state';
  selectedChannel = '';
  inputConfig?: RuntimeInputConfig;
  inputConfigMessage = '';
  control?: ControlState;
  controlMessage = '';
  webRequested = false;
  webInput = neutralInput();
  private controlArrival = 0;
  private activationPendingUntil = 0;
  private controlTimer?: ReturnType<typeof setInterval>;
  socket?: WebSocket;
  stopSource?: () => void;
  private generation = 0;
  private graphInitialized = false;
  private subscriptionTimer: ReturnType<typeof setTimeout> | undefined;
  private sampleCounter = 0;
  private tickTime = performance.now();
  private byteTick = 0;
  private timer = 0;
  private lastEmit = 0;
  private topicTimes = new Map<string, number>();
  constructor() {
    this.log('INFO', 'Workspace initialized');
  }
  get session() {
    return this.generation;
  }
  subscribe = (listener: () => void) => {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  };
  getVersion = () => this.version;
  emit() {
    this.version++;
    this.listeners.forEach((fn) => fn());
  }
  log(level: LogEntry['level'], text: string, controlEvent?: SwitchEvent) {
    const entry = { time: controlEvent?.timeMs ?? Date.now(), level, text, controlEvent };
    this.logs.push(entry);
    this.latestEvent = entry;
    if (this.logs.length > 200) this.logs.shift();
  }
  resetRateWindow() {
    this.sampleCounter = 0;
    this.tickTime = performance.now();
    this.byteTick = this.byteCount;
  }
  startClock() {
    if (this.timer) return;
    let last = performance.now();
    this.timer = window.setInterval(() => {
      const now = performance.now(),
        delta = (now - last) / 1000;
      last = now;
      if (this.mode === 'LIVE') this.cursor = this.end;
      if (this.playing) {
        this.cursor = Math.min(this.end, this.cursor + delta * this.speed);
        if (this.cursor >= this.end) this.playing = false;
        if (this.cursor > (this.viewEnd ?? this.start)) this.viewEnd = this.cursor;
      }
      if (now - this.tickTime >= 1000) {
        this.hz = this.sampleCounter / ((now - this.tickTime) / 1000);
        this.bandwidth = (this.byteCount - this.byteTick) / ((now - this.tickTime) / 1000);
        this.sampleCounter = 0;
        this.tickTime = now;
        this.byteTick = this.byteCount;
      }
      if (now - this.lastEmit >= 100) {
        this.emit();
        this.lastEmit = now;
      }
    }, 33);
  }
  dispose() {
    this.disconnect();
    clearInterval(this.timer);
    this.timer = 0;
  }
  disconnect() {
    this.releaseWeb();
    this.control = undefined;
    this.controlMessage = '';
    clearTimeout(this.subscriptionTimer);
    this.generation++;
    this.stopSource?.();
    this.stopSource = undefined;
    this.socket?.close();
    this.socket = undefined;
    this.inputConfig = undefined;
    this.inputConfigMessage = '';
    if (this.source === 'ros2') this.connection = 'DISCONNECTED';
    this.emit();
  }
  reset(source: TelemetryStore['source']) {
    this.disconnect();
    this.source = source;
    this.latestEvent = undefined;
    this.graphInitialized = false;
    this.buffers.clear();
    this.snapshots.clear();
    this.snapshotArrival.clear();
    this.topicTimes.clear();
    this.topics = [];
    this.channels = [];
    this.schemas.clear();
    this.received =
      this.dropped =
      this.byteCount =
      this.sampleCounter =
      this.hz =
      this.bandwidth =
        0;
    this.byteTick = 0;
    this.tickTime = performance.now();
    this.origin = this.start = this.end = this.cursor = 0;
    this.viewEnd = undefined;
    this.clock = '';
    this.error = '';
    this.fileName = '';
    this.playing = false;
    this.window = 10;
    this.mode = source === 'file' ? 'PLAYBACK' : 'LIVE';
    this.connection = source === 'demo' ? 'CONNECTED' : 'DISCONNECTED';
  }
  addChannel(topic: string, path: string, label?: string) {
    const id = channelId(topic, path);
    if (this.source === 'file' && !this.buffers.has(id)) {
      this.error =
        'This recording field was not loaded. Reopen the recording to select it at full sample rate.';
      this.emit();
      return;
    }
    if (this.channels.some((c) => c.id === id)) return;
    if (this.channels.length >= 12) {
      this.error = 'Channel limit reached · remove a channel to add another (12 maximum).';
      this.emit();
      return;
    }
    this.channels.push({
      id,
      topic,
      path,
      label: label || path,
      unit: unitFor(path),
      color: COLORS[this.channels.length],
      visible: true,
    });
    if (!this.buffers.has(id)) this.buffers.set(id, new RingBuffer());
    this.syncSubscriptions();
    this.emit();
  }
  removeChannel(id: string) {
    this.channels = this.channels.filter((c) => c.id !== id);
    if (this.source !== 'file') this.buffers.delete(id);
    this.syncSubscriptions();
    this.emit();
  }
  syncSubscriptions() {
    if (this.socket?.readyState !== WebSocket.OPEN) return;
    clearTimeout(this.subscriptionTimer);
    const session = this.generation;
    this.subscriptionTimer = setTimeout(() => {
      if (session !== this.generation || this.socket?.readyState !== WebSocket.OPEN) return;
      const requests = new Map<string, string[]>();
      // Keep overview and both controllers active while inspecting other topics.
      for (const topic of ['/anakin/state', INPUT_TOPIC])
        if (this.topics.some((item) => item.name === topic)) requests.set(topic, []);
      requests.set(this.selectedTopic, []);
      this.channels.forEach((c) =>
        requests.set(c.topic, [...(requests.get(c.topic) || []), c.path]),
      );
      this.socket.send(
        JSON.stringify({
          v: 1,
          kind: 'subscribe',
          subscriptions: [...requests].map(([topic, fields]) => ({ topic, fields, qos: this.qos })),
        }),
      );
    }, 40);
  }
  inspectTopic(topic: string) {
    this.selectedTopic = topic;
    this.socket?.readyState === WebSocket.OPEN &&
      this.socket.send(JSON.stringify({ v: 1, kind: 'schema', topic }));
    this.syncSubscriptions();
    this.emit();
  }
  ingest(sample: Sample) {
    if (!Number.isFinite(sample.t)) return;
    const previous = this.topicTimes.get(sample.topic);
    // A known Topic can reset its clock. A newly added foreign domain cannot.
    if (previous === undefined && this.end && Math.abs(sample.t - this.end) > 3600) {
      this.error = 'Clock domain mismatch · this sample cannot share the current timeline.';
      return;
    }
    if (previous !== undefined && (sample.t < previous - 2 || sample.t > previous + 3600)) {
      this.buffers.forEach((_, id) => this.buffers.set(id, new RingBuffer()));
      this.snapshots.clear();
      this.snapshotArrival.clear();
      this.topicTimes.clear();
      this.origin = this.start = this.end = 0;
      this.cursor = sample.t;
      this.viewEnd = sample.t;
      this.playing = false;
      this.error = 'Source clock reset · started a new timeline segment';
      this.log('WARN', this.error);
    } else if (previous !== undefined && sample.t < previous) {
      this.error = 'Out-of-order source sample omitted to preserve the time axis';
      return;
    }
    this.topicTimes.set(sample.topic, sample.t);
    if (!this.origin) this.origin = sample.t;
    this.end = Math.max(this.end, sample.t);
    this.start = Math.max(this.origin, this.end - 60);
    if (this.mode === 'LIVE') this.cursor = this.end;
    for (const [path, value] of Object.entries(sample.values))
      this.buffers.get(channelId(sample.topic, path))?.push(sample.t, value);
    this.clock = sample.clock || 'source';
    this.received++;
    this.sampleCounter++;
  }
  snapshot(snapshot: Snapshot) {
    if (
      !Number.isFinite(snapshot.t) ||
      (this.source !== 'file' && this.end && Math.abs(snapshot.t - this.end) > 3600)
    )
      return;
    const rows = this.snapshots.get(snapshot.topic) || [];
    if (rows.length && snapshot.t < rows[rows.length - 1].t) return;
    rows.push(snapshot);
    this.snapshotArrival.set(snapshot.topic, performance.now());
    if (this.source !== 'file') {
      while (rows.length > 1800 || (rows.length > 1 && rows[rows.length - 1].t - rows[0].t > 60))
        rows.shift();
    }
    this.snapshots.set(snapshot.topic, rows);
    if (snapshot.topic === INPUT_TOPIC && this.source === 'ros2') {
      for (const event of switchEvents(snapshot.data)) {
        if (this.controlEventKeys.has(event.id)) continue;
        this.controlEventKeys.add(event.id);
        if (this.controlEventKeys.size > 512)
          this.controlEventKeys.delete(this.controlEventKeys.values().next().value!);
        this.log(
          event.result === 'failed' ? 'ERROR' : event.result === 'cancelled' ? 'WARN' : 'INFO',
          `${event.source} -> ${event.target}: ${event.result}`,
          event,
        );
      }
    }
  }
  at(topic = this.selectedTopic, time = this.cursor): Snapshot | undefined {
    const rows = this.snapshots.get(topic);
    if (!rows?.length) return;
    let lo = 0,
      hi = rows.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (rows[mid].t <= time + 1e-6) lo = mid + 1;
      else hi = mid;
    }
    return lo > 0 ? rows[lo - 1] : undefined;
  }
  zoom(inside: boolean) {
    const base = [0.25, 0.5, 1, 2, 5, 10, 20, 30, 60];
    const choices =
      this.source === 'file'
        ? [...new Set([...base, 120, 300, 600, 1800, 3600, this.end - this.start])]
            .filter((n) => n > 0 && n <= Math.max(60, this.end - this.start))
            .sort((a, b) => a - b)
        : base;
    this.window = inside
      ? ([...choices].reverse().find((n) => n < this.window) ?? 0.25)
      : (choices.find((n) => n > this.window) ?? choices.at(-1)!);
    this.emit();
  }
  range(): [number, number] {
    const right = this.mode === 'LIVE' ? this.end : (this.viewEnd ?? this.cursor);
    return [right - this.window, right];
  }
  seek(t: number) {
    this.releaseWeb();
    if (this.mode === 'LIVE') this.viewEnd = this.end;
    this.cursor = Math.max(this.start, Math.min(this.end, t));
    this.mode = this.source === 'file' ? 'PLAYBACK' : 'HISTORY';
    this.playing = false;
    this.emit();
  }
  togglePause() {
    if (this.source === 'file') {
      if (this.cursor >= this.end) this.cursor = this.start;
      this.playing = !this.playing;
    } else if (this.mode === 'LIVE') {
      this.releaseWeb();
      this.mode = 'PAUSED';
      this.viewEnd = this.end;
      this.playing = false;
    } else this.live();
    this.emit();
  }
  live() {
    this.mode = this.source === 'file' ? 'PLAYBACK' : 'LIVE';
    this.cursor = this.end;
    this.viewEnd = undefined;
    this.playing = false;
    this.emit();
  }
  connect(url: string) {
    this.reset('ros2');
    const token = this.generation;
    this.connection = 'CONNECTING';
    this.log('INFO', `Connecting to ${url}`);
    this.emit();
    let ws: WebSocket;
    try {
      ws = new WebSocket(url);
    } catch (error) {
      this.connection = 'ERROR';
      this.error = String(error);
      this.emit();
      return;
    }
    this.socket = ws;
    ws.onmessage = (event) => {
      if (token !== this.generation) return;
      this.byteCount += typeof event.data === 'string' ? event.data.length : 0;
      try {
        this.packet(JSON.parse(event.data));
      } catch (error) {
        this.error = `Invalid gateway payload: ${String(error)}`;
        this.log('ERROR', this.error);
      }
    };
    ws.onerror = () => {
      if (token !== this.generation) return;
      this.releaseWeb();
      this.control = undefined;
      this.inputConfig = undefined;
      this.inputConfigMessage = '';
      this.connection = 'ERROR';
      this.error = 'Gateway connection failed. Check URL, ROS environment and gateway process.';
      this.log('ERROR', this.error);
      this.emit();
    };
    ws.onclose = () => {
      if (token !== this.generation) return;
      this.releaseWeb();
      this.control = undefined;
      this.inputConfig = undefined;
      this.inputConfigMessage = '';
      if (this.connection !== 'ERROR') this.connection = 'DISCONNECTED';
      this.log('WARN', 'Gateway socket closed');
      this.emit();
    };
  }
  packet(value: unknown) {
    if (!isRecord(value) || value.v !== 1 || typeof value.kind !== 'string')
      throw new Error('Expected gateway protocol v1');
    switch (value.kind) {
      case 'hello':
        this.connection = 'CONNECTED';
        this.log('INFO', `ROS2 connected · domain ${value.domain_id}`);
        break;
      case 'topics': {
        if (!Array.isArray(value.topics)) throw new Error('topics must be an array');
        const previousSources = ['/anakin/state', INPUT_TOPIC].map((name) =>
          this.topics.some((topic) => topic.name === name),
        );
        this.topics = value.topics.filter(
          (t): t is Topic =>
            isRecord(t) &&
            typeof t.name === 'string' &&
            typeof t.type === 'string' &&
            Array.isArray(t.qos),
        );
        if (this.topics.length && !this.graphInitialized) {
          this.graphInitialized = true;
          const primary =
            this.topics.find((t) => t.name === '/anakin/state') ||
            this.topics.find((t) => /JointState/.test(t.type)) ||
            this.topics[0];
          this.selectedTopic = primary.name;
          if (/RobotStateFrame/.test(primary.type)) {
            ['joints.position[0]', 'joints.position[3]', 'imu.angular_velocity.x'].forEach((p) =>
              this.addChannel(primary.name, p),
            );
          }
          this.inspectTopic(primary.name);
        } else if (
          previousSources.some(
            (had, i) =>
              had !== this.topics.some((topic) => topic.name === ['/anakin/state', INPUT_TOPIC][i]),
          )
        ) {
          this.syncSubscriptions();
        }
        break;
      }
      case 'schema':
        if (
          typeof value.topic === 'string' &&
          typeof value.type === 'string' &&
          Array.isArray(value.fields)
        )
          this.schemas.set(value.topic, {
            fields: value.fields as SchemaField[],
            type: value.type,
          });
        break;
      case 'control_state': {
        if (this.source !== 'ros2') break;
        const control = decodeControlState(value);
        if (
          !control ||
          (this.control?.environment && control.environment !== this.control.environment)
        )
          this.releaseWeb();
        this.control = control;
        this.controlArrival = performance.now();
        const awaitingActivation = this.activationPendingUntil > performance.now();
        if (control?.owned || control?.pending) this.activationPendingUntil = 0;
        if (
          this.webRequested &&
          (!control?.available || (!control.owned && !control.pending && !awaitingActivation))
        ) {
          this.controlMessage = control?.message || 'Web control released';
          this.releaseWeb();
        }
        break;
      }
      case 'input_config':
        this.inputConfig = decodeInputConfig(value);
        this.inputConfigMessage = this.inputConfig
          ? ''
          : String(value.message || 'Runtime input configuration unavailable');
        break;
      case 'status':
        if (['CONNECTED', 'DEGRADED', 'ERROR'].includes(String(value.state)))
          this.connection = value.state as Connection;
        break;
      case 'error':
        if (value.code === 'CONTROL_REJECTED') {
          this.controlMessage = String(value.message || 'Control request rejected');
          this.log('WARN', this.controlMessage);
          if (!this.control?.owned) this.releaseWeb();
          break;
        }
        this.error = String(value.message || 'Gateway error');
        this.log('ERROR', this.error);
        if (value.code === 'ROS_UNAVAILABLE') this.connection = 'ERROR';
        break;
      case 'batch':
        if (!Array.isArray(value.samples) || !Array.isArray(value.snapshots))
          throw new Error('Invalid batch');
        value.samples.forEach((s: unknown) => {
          if (
            isRecord(s) &&
            typeof s.topic === 'string' &&
            typeof s.t === 'number' &&
            isRecord(s.values)
          ) {
            const values = Object.fromEntries(
              Object.entries(s.values).filter(
                ([, v]) => typeof v === 'number' && Number.isFinite(v),
              ),
            ) as Record<string, number>;
            this.ingest({ ...(s as unknown as Sample), values });
          }
        });
        value.snapshots.forEach((s: unknown) => {
          if (isRecord(s) && typeof s.topic === 'string' && typeof s.t === 'number' && 'data' in s)
            this.snapshot(s as unknown as Snapshot);
        });
        this.dropped = typeof value.dropped === 'number' ? value.dropped : 0;
        break;
    }
    this.emit();
  }
  get canWebControl() {
    return (
      this.source === 'ros2' &&
      ['CONNECTED', 'DEGRADED'].includes(this.connection) &&
      this.mode === 'LIVE' &&
      Boolean(this.control?.available && this.control.enabled) &&
      performance.now() - this.controlArrival <= (this.control?.timeoutMs ?? 250)
    );
  }
  get webActive() {
    return this.webRequested && this.canWebControl && Boolean(this.control?.owned);
  }
  private sendControl(request: Record<string, unknown>) {
    if (this.socket?.readyState !== WebSocket.OPEN || this.socket.bufferedAmount > 65536)
      return false;
    this.socket.send(JSON.stringify({ v: 1, kind: 'control', ...request }));
    return true;
  }
  activateWeb() {
    if (!this.canWebControl || this.control?.leased || this.webRequested) return false;
    this.webInput = neutralInput();
    this.controlMessage = '';
    if (!this.sendControl({ operation: 'activate' })) return false;
    this.webRequested = true;
    this.activationPendingUntil = performance.now() + (this.control?.timeoutMs ?? 250);
    clearInterval(this.controlTimer);
    this.controlTimer = setInterval(() => {
      if (!this.webRequested) return;
      if (
        !this.canWebControl ||
        (this.activationPendingUntil > 0 && performance.now() >= this.activationPendingUntil)
      ) {
        this.releaseWeb();
        this.emit();
        return;
      }
      this.sendControl({ operation: 'input', ...this.webInput });
    }, 50);
    this.emit();
    return true;
  }
  releaseWeb() {
    clearInterval(this.controlTimer);
    this.controlTimer = undefined;
    if (this.webRequested) this.sendControl({ operation: 'release' });
    this.webRequested = false;
    this.activationPendingUntil = 0;
    this.webInput = neutralInput();
  }
  setWebInput(input: WebInput) {
    if (!this.webActive) return;
    this.webInput = {
      axes: [...input.axes],
      buttons: [...input.buttons],
      shoulders: input.shoulders,
    };
    this.sendControl({ operation: 'input', ...this.webInput });
  }
  neutralWeb() {
    this.webInput = neutralInput();
    if (this.webRequested) this.sendControl({ operation: 'input', ...this.webInput });
  }
  command(action: string) {
    if (!this.webActive || !this.control?.actions[action]) return false;
    this.controlMessage = '';
    return this.sendControl({ operation: 'input', ...this.webInput, action });
  }
}
export const telemetry = new TelemetryStore();
export function jsonObject(value: Json | undefined): { [key: string]: Json } | undefined {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : undefined;
}
