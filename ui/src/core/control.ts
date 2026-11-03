import { isRecord } from './types';

export interface ControlState {
  available: boolean;
  enabled: boolean;
  owned: boolean;
  pending: boolean;
  leased: boolean;
  environment: string;
  activeSource: string;
  mode: string;
  state: string;
  requestedState: string;
  moving: boolean;
  height: number;
  timeoutMs: number;
  actions: Record<string, boolean>;
  message: string;
}
export interface WebInput {
  axes: [number, number, number, number];
  buttons: boolean[];
  shoulders: number;
}
export const neutralInput = (): WebInput => ({
  axes: [0, 0, 0, 0],
  buttons: Array<boolean>(10).fill(false),
  shoulders: 0,
});

export function decodeControlState(packet: Record<string, unknown>): ControlState | undefined {
  for (const field of ['available', 'enabled', 'owned', 'pending', 'leased', 'moving'])
    if (typeof packet[field] !== 'boolean') return;
  for (const field of [
    'environment',
    'active_source',
    'mode',
    'state',
    'requested_state',
    'message',
  ])
    if (typeof packet[field] !== 'string') return;
  if (
    typeof packet.height !== 'number' ||
    !Number.isFinite(packet.height) ||
    typeof packet.timeout_ms !== 'number' ||
    !Number.isFinite(packet.timeout_ms) ||
    packet.timeout_ms <= 0 ||
    !isRecord(packet.actions) ||
    Object.keys(packet.actions).length > 32
  )
    return;
  const actions: Record<string, boolean> = {};
  for (const [action, allowed] of Object.entries(packet.actions)) {
    if (!/^[a-z][a-z0-9_]{0,31}$/.test(action) || typeof allowed !== 'boolean') return;
    actions[action] = allowed;
  }
  return {
    available: packet.available as boolean,
    enabled: packet.enabled as boolean,
    owned: packet.owned as boolean,
    pending: packet.pending as boolean,
    leased: packet.leased as boolean,
    environment: String(packet.environment),
    activeSource: String(packet.active_source),
    mode: String(packet.mode),
    state: String(packet.state),
    requestedState: String(packet.requested_state),
    moving: packet.moving as boolean,
    height: packet.height,
    timeoutMs: packet.timeout_ms,
    actions,
    message: String(packet.message),
  };
}
