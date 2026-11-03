import { isRecord } from './types';

export const INPUT_TOPIC = '/anakin/input_state';
export type ControllerSource = 'gamepad' | 'remote';
export interface InputBinding {
  control: string;
  shoulders: string[] | 'any';
  trigger: string;
  action: string;
}
export interface RuntimeInputConfig {
  node: string;
  path: string;
  modePath: string;
  timeoutMs: number;
  primary: string;
  enabledSources: string[];
  deviceIndex: number;
  gamepadMappings: string[];
  remoteChannel: string;
  deadzone: number;
  velocityAxes: { component: string; axis: string; scale: number }[];
  controllerBindings: InputBinding[];
  webActions: string[];
}
export interface ControllerInput {
  enabled: boolean;
  primary: boolean;
  connected: boolean;
  online: boolean;
  ageMs?: number;
  axes: Record<string, number>;
  buttons: Record<string, boolean>;
  triggers: Record<string, number>;
  device?: { name: string; guid: string; mapping: string };
}
const finite = (value: unknown): value is number =>
  typeof value === 'number' && Number.isFinite(value);
const strings = (value: unknown): value is string[] =>
  Array.isArray(value) && value.every((item) => typeof item === 'string');

function bindings(value: unknown): InputBinding[] | undefined {
  if (!Array.isArray(value)) return;
  const result: InputBinding[] = [];
  for (const item of value) {
    if (!isRecord(item)) return;
    const control = item.button;
    if (
      typeof control !== 'string' ||
      typeof item.action !== 'string' ||
      !['pressed', 'held', 'released'].includes(String(item.trigger)) ||
      (item.shoulders !== 'any' && !strings(item.shoulders))
    )
      return;
    result.push({
      control,
      action: item.action,
      trigger: String(item.trigger),
      shoulders: item.shoulders as string[] | 'any',
    });
  }
  return result;
}

export function decodeInputConfig(packet: Record<string, unknown>): RuntimeInputConfig | undefined {
  const config = packet.config;
  if (packet.available !== true || !isRecord(config) || config.schema_version !== 2) return;
  const { gamepad, remote, motion, web } = config;
  if (
    typeof packet.node !== 'string' ||
    typeof packet.path !== 'string' ||
    typeof packet.mode_path !== 'string' ||
    !finite(packet.input_timeout_ms) ||
    packet.input_timeout_ms <= 0 ||
    typeof config.primary !== 'string' ||
    !strings(config.enabled_sources) ||
    (config.primary !== 'auto' && !config.enabled_sources.includes(config.primary)) ||
    !isRecord(web) ||
    !strings(web.actions) ||
    !isRecord(gamepad) ||
    !finite(gamepad.device_index) ||
    (gamepad.mappings !== undefined && !strings(gamepad.mappings)) ||
    !isRecord(remote) ||
    typeof remote.channel !== 'string' ||
    !isRecord(motion) ||
    !finite(motion.deadzone) ||
    !isRecord(motion.velocity_axes)
  )
    return;
  const controllerBindings = bindings(config.controller_bindings);
  if (!controllerBindings) return;
  const velocityAxes: RuntimeInputConfig['velocityAxes'] = [];
  for (const [component, value] of Object.entries(motion.velocity_axes)) {
    if (!isRecord(value) || typeof value.axis !== 'string' || !finite(value.scale)) return;
    velocityAxes.push({ component, axis: value.axis, scale: value.scale });
  }
  return {
    node: packet.node,
    path: packet.path,
    modePath: packet.mode_path,
    timeoutMs: packet.input_timeout_ms,
    primary: config.primary,
    enabledSources: config.enabled_sources,
    deviceIndex: gamepad.device_index,
    gamepadMappings: (gamepad.mappings as string[] | undefined) || [],
    remoteChannel: remote.channel,
    deadzone: motion.deadzone,
    velocityAxes,
    controllerBindings,
    webActions: web.actions,
  };
}

export function controllerInput(
  data: unknown,
  source: ControllerSource,
): ControllerInput | undefined {
  if (!isRecord(data) || !Array.isArray(data.status)) return;
  const status = data.status.find(
    (value) => isRecord(value) && value.name === `anakin_env/input/${source}`,
  );
  if (!isRecord(status) || !Array.isArray(status.values)) return;
  const values = new Map<string, string>();
  for (const entry of status.values)
    if (isRecord(entry) && typeof entry.key === 'string' && typeof entry.value === 'string')
      values.set(entry.key, entry.value);
  if (values.get('schema_version') !== '1') return;
  const flag = (key: string) => values.get(key) === 'true';
  if (
    ['enabled', 'primary', 'connected', 'online'].some(
      (key) => !['true', 'false'].includes(values.get(key) || ''),
    )
  )
    return;
  const axes: Record<string, number> = {},
    buttons: Record<string, boolean> = {},
    triggers: Record<string, number> = {};
  for (const [key, value] of values) {
    if (
      key.startsWith('axis.') &&
      value.trim() &&
      Number.isFinite(Number(value)) &&
      Math.abs(Number(value)) <= 1.001
    )
      axes[key.slice(5)] = Number(value);
    if (key.startsWith('button.') && ['true', 'false'].includes(value))
      buttons[key.slice(7)] = value === 'true';
    if (
      key.startsWith('trigger.') &&
      value.trim() &&
      Number.isFinite(Number(value)) &&
      Number(value) >= 0 &&
      Number(value) <= 1
    )
      triggers[key.slice(8)] = Number(value);
  }
  const age = values.get('age_ms');
  return {
    enabled: flag('enabled'),
    primary: flag('primary'),
    connected: flag('connected'),
    online: flag('online'),
    ageMs:
      age?.trim() && Number.isFinite(Number(age)) && Number(age) >= 0 ? Number(age) : undefined,
    axes,
    buttons,
    triggers,
    device: values.has('device_name')
      ? {
          name: values.get('device_name')!,
          guid: values.get('device_guid') || '',
          mapping: values.get('sdl_mapping') || '',
        }
      : undefined,
  };
}
