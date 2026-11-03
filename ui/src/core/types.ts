export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };
export type Connection = 'DISCONNECTED' | 'CONNECTING' | 'CONNECTED' | 'DEGRADED' | 'ERROR';
export type View =
  | 'Overview'
  | 'Workbench'
  | 'Topics'
  | 'Telemetry'
  | 'Robotics'
  | 'Playback'
  | 'Diagnostics'
  | 'Settings';
export interface Topic {
  name: string;
  type: string;
  publishers: number;
  subscribers: number;
  hz: number;
  bytes_per_second: number;
  last_receive_ns: string | null;
  qos: { reliability: string; durability: string; depth: number; node?: string }[];
}
export interface SchemaField {
  name: string;
  type: string;
  array: boolean;
  children: SchemaField[];
}
export interface Sample {
  topic: string;
  t: number;
  seq?: number;
  source_ns?: string | null;
  receive_ns?: string;
  clock?: string;
  values: Record<string, number>;
}
export interface Snapshot {
  topic: string;
  t: number;
  data: Json;
  source_ns?: string | null;
  receive_ns?: string;
  clock?: string;
}
export interface Channel {
  id: string;
  topic: string;
  path: string;
  label: string;
  unit: string;
  color: string;
  visible: boolean;
}
export const COLORS = [
  '#66B7D8',
  '#C1CAD4',
  '#E4B45A',
  '#56C596',
  '#A59CCA',
  '#E56B6F',
  '#72A8AF',
  '#CABBA4',
  '#89B0DB',
  '#B4C77B',
  '#C997AB',
  '#9CAAB4',
];
export const channelId = (topic: string, path: string) => `${topic}::${path}`;
export const unitFor = (path: string): string =>
  /(?:joints?|joint_state).*position|^position\[|angle|rpy/.test(path)
    ? 'rad'
    : /velocity|angular_velocity|gyroscope/.test(path)
      ? 'rad/s'
      : /effort|tau/.test(path)
        ? 'N·m'
        : /accel/.test(path)
          ? 'm/s²'
          : /temperature/.test(path)
            ? '°C'
            : /voltage|\.vol$/.test(path)
              ? 'V'
              : '';
export function flatten(
  value: Json,
  prefix = '',
  out: Record<string, Json> = {},
): Record<string, Json> {
  if (Array.isArray(value)) value.forEach((v, i) => flatten(v, `${prefix}[${i}]`, out));
  else if (value !== null && typeof value === 'object')
    Object.entries(value).forEach(([k, v]) => flatten(v, prefix ? `${prefix}.${k}` : k, out));
  else if (prefix) out[prefix] = value;
  return out;
}
export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
export function numberAt(value: Json | undefined, path: string): number | undefined {
  if (value === undefined) return;
  let current: Json | undefined = value;
  for (const part of path.replace(/\[(\d+)\]/g, '.$1').split('.')) {
    if (current === null || typeof current !== 'object') return;
    current = (current as Record<string, Json>)[part];
  }
  return typeof current === 'number' && Number.isFinite(current) ? current : undefined;
}
