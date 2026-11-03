import { isRecord } from './types';

export interface SwitchEvent {
  id: string;
  timeMs: number;
  source: string;
  from: string;
  target: string;
  current: string;
  result: 'pending' | 'success' | 'failed' | 'cancelled';
  detail: string;
}

export function switchEvents(data: unknown): SwitchEvent[] {
  if (!isRecord(data) || !Array.isArray(data.status)) return [];
  const events: SwitchEvent[] = [];
  for (const status of data.status) {
    if (
      !isRecord(status) ||
      status.name !== 'anakin_env/switch_event' ||
      typeof status.hardware_id !== 'string' ||
      !status.hardware_id ||
      !Array.isArray(status.values)
    )
      continue;
    const fields: Record<string, string> = {};
    for (const item of status.values)
      if (isRecord(item) && typeof item.key === 'string' && typeof item.value === 'string')
        fields[item.key] = item.value;
    const sequence = Number(fields.sequence),
      timeMs = Number(fields.time_ms);
    if (
      fields.schema_version !== '1' ||
      !Number.isSafeInteger(sequence) ||
      sequence <= 0 ||
      !Number.isSafeInteger(timeMs) ||
      timeMs <= 0 ||
      !fields.source ||
      !fields.from ||
      !fields.target ||
      !fields.current ||
      !['pending', 'success', 'failed', 'cancelled'].includes(fields.result)
    )
      continue;
    events.push({
      id: `${status.hardware_id}:${sequence}`,
      timeMs,
      source: fields.source,
      from: fields.from,
      target: fields.target,
      current: fields.current,
      result: fields.result as SwitchEvent['result'],
      detail: fields.detail || '',
    });
  }
  return events;
}

export function switchEventRoute(event: SwitchEvent, t: (message: string) => string) {
  const source = event.source
    .split('+')
    .map((item) => t(item))
    .join(' + ');
  return `${source} → ${t(event.target)}`;
}
export function switchEventText(event: SwitchEvent, t: (message: string) => string) {
  return `${switchEventRoute(event, t)} · ${t(event.result)}`;
}
