import { flatten, isRecord } from './types';
import type { Json, Snapshot } from './types';
export type TimeUnit = 's' | 'ms' | 'ns' | 'iso' | 'ros';
export interface FilePreview {
  count: number;
  numeric: string[];
  timeFields: string[];
  guessed: string;
  unit: TimeUnit;
  topics: string[];
}
export interface ImportColumn {
  topic: string;
  path: string;
  times: Float64Array;
  values: Float64Array;
}
export interface ImportResult {
  columns: ImportColumn[];
  snapshots: Snapshot[];
  start: number;
  end: number;
  count: number;
  warnings: string[];
}
interface Row {
  topic: string;
  data: Json;
  time: Record<string, Json>;
}

export function csvRows(text: string): string[][] {
  const rows: string[][] = [];
  let row: string[] = [],
    field = '',
    quoted = false;
  const delimiter =
    text.split(/\r?\n/, 1)[0].includes(';') && !text.split(/\r?\n/, 1)[0].includes(',') ? ';' : ',';
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (c === '"') {
      if (quoted && text[i + 1] === '"') {
        field += '"';
        i++;
      } else if (quoted || !field) quoted = !quoted;
      else throw new Error(`Malformed CSV quote near character ${i}`);
    } else if (c === delimiter && !quoted) {
      row.push(field);
      field = '';
    } else if ((c === '\n' || c === '\r') && !quoted) {
      if (c === '\r' && text[i + 1] === '\n') i++;
      row.push(field);
      if (row.some((v) => v.trim())) rows.push(row);
      row = [];
      field = '';
    } else field += c;
  }
  if (quoted) throw new Error('Unclosed quoted CSV field');
  row.push(field);
  if (row.some((v) => v.trim())) rows.push(row);
  return rows;
}
export function readRecords(text: string, name: string): Row[] {
  text = text.replace(/^\uFEFF/, '');
  if (/\.mcap$/i.test(name))
    throw new Error('MCAP support is planned for the next phase. Import CSV or JSON.');
  if (/\.csv$/i.test(name)) {
    const [headers, ...rows] = csvRows(text);
    if (
      !headers?.length ||
      new Set(headers).size !== headers.length ||
      headers.some((h) => !h.trim())
    )
      throw new Error('CSV needs unique, nonempty column headers');
    return rows.map((cells, i) => {
      if (cells.length !== headers.length)
        throw new Error(
          `CSV row ${i + 2}: expected ${headers.length} columns, received ${cells.length}`,
        );
      const data: Record<string, Json> = {};
      headers.forEach((h, n) => {
        const raw = cells[n].trim(),
          num = Number(raw);
        data[h.trim()] = raw !== '' && Number.isFinite(num) ? num : raw;
      });
      return { topic: '/recording', data, time: data };
    });
  }
  if (!/\.json$/i.test(name)) throw new Error('Supported file types: .csv and .json');
  const parsed: unknown = JSON.parse(text);
  const items = Array.isArray(parsed)
    ? parsed
    : isRecord(parsed) && Array.isArray(parsed.messages)
      ? parsed.messages
      : null;
  if (!items)
    throw new Error(
      'JSON must be an array of timestamped records, or an object with a messages array',
    );
  return items.map((item, i) => {
    if (!isRecord(item)) throw new Error(`JSON record ${i + 1} must be an object`);
    const data = ('data' in item ? item.data : item) as Json;
    if (data === null || typeof data !== 'object')
      throw new Error(`JSON record ${i + 1}: data must be an object`);
    return {
      topic: typeof item.topic === 'string' ? item.topic : '/recording',
      data,
      time: flatten(item as Json),
    };
  });
}
export function previewRecords(records: Row[]): FilePreview {
  if (!records.length) throw new Error('The file contains no data records');
  if (records.length > 1_000_000)
    throw new Error('Recording exceeds the 1,000,000 record limit. Split the recording.');
  const numeric = new Set<string>(),
    time = new Set<string>(),
    topics = new Set<string>();
  records.slice(0, 500).forEach((row) => {
    topics.add(row.topic);
    Object.entries(flatten(row.data)).forEach(([path, value]) => {
      if (typeof value === 'number' && Number.isFinite(value)) numeric.add(`${row.topic}::${path}`);
    });
    Object.keys(row.time).forEach((key) => {
      if (/time|stamp|elapsed|^t$|\.sec$/.test(key.toLowerCase())) time.add(key);
    });
  });
  const timeFields = [...time],
    guessed =
      [
        'timestamp',
        'time',
        't',
        'elapsed',
        'source_ns',
        'header.stamp.sec',
        'data.header.stamp.sec',
      ].find((v) => time.has(v)) ||
      timeFields[0] ||
      Object.keys(records[0].time)[0];
  if (!numeric.size) throw new Error('No finite numeric fields were found');
  const value = records[0].time[guessed];
  const unit: TimeUnit = guessed.endsWith('.sec')
    ? 'ros'
    : /ns/.test(guessed)
      ? 'ns'
      : /ms/.test(guessed)
        ? 'ms'
        : typeof value === 'string' && /T.*:/.test(value)
          ? 'iso'
          : 's';
  return {
    count: records.length,
    numeric: [...numeric].slice(0, 512),
    timeFields: [...new Set([...timeFields, ...Object.keys(records[0].time)])],
    guessed,
    unit,
    topics: [...topics],
  };
}
function seconds(row: Row, field: string, unit: TimeUnit): number {
  const raw = row.time[field];
  if (raw === undefined || raw === null || raw === '') return NaN;
  if (unit === 'iso') return typeof raw === 'string' ? Date.parse(raw) / 1000 : NaN;
  if (unit === 'ros')
    return Number(raw) + Number(row.time[field.replace(/\.sec$/, '.nanosec')] || 0) / 1e9;
  return Number(raw) / (unit === 'ms' ? 1000 : unit === 'ns' ? 1e9 : 1);
}
export function convertRecords(
  records: Row[],
  timeField: string,
  unit: TimeUnit,
  selected: string[],
): ImportResult {
  if (!selected.length || selected.length > 12)
    throw new Error('Choose between 1 and 12 numeric channels');
  const warnings: string[] = [];
  const sorted = records.map((row, index) => ({ row, t: seconds(row, timeField, unit), index }));
  const invalid = sorted.find((x) => !Number.isFinite(x.t));
  if (invalid)
    throw new Error(`Record ${invalid.index + 1}: invalid ${unit} timestamp in ${timeField}`);
  if (sorted.some((v, i) => i && v.t < sorted[i - 1].t)) {
    warnings.push('Non-monotonic timestamps sorted into source-time order.');
    sorted.sort((a, b) => a.t - b.t);
  }
  let duplicates = 0;
  sorted.forEach((v, i) => {
    if (i && v.t === sorted[i - 1].t) duplicates++;
  });
  if (duplicates) warnings.push(`${duplicates} duplicate timestamps retained in original order.`);
  // A positive anchor keeps elapsed=0 distinct from an uninitialized live clock.
  const offset = sorted[0].t < 1 ? 1 - sorted[0].t : 0;
  const specs = selected.map((id) => {
    const split = id.indexOf('::');
    return {
      topic: id.slice(0, split),
      path: id.slice(split + 2),
      ts: [] as number[],
      vs: [] as number[],
    };
  });
  const snapshots: Snapshot[] = [],
    last = new Map<string, number>();
  let skipped = 0;
  for (const { row, t: rawT } of sorted) {
    const t = rawT + offset,
      data = flatten(row.data);
    specs.forEach((spec) => {
      if (row.topic !== spec.topic) return;
      const value = data[spec.path];
      if (typeof value === 'number' && Number.isFinite(value)) {
        spec.ts.push(t);
        spec.vs.push(value);
      } else skipped++;
    });
    if (t - (last.get(row.topic) ?? -Infinity) >= 1 / 30) {
      snapshots.push({ topic: row.topic, t, data: row.data, clock: 'source' });
      last.set(row.topic, t);
    }
  }
  if (skipped) warnings.push(`${skipped} missing or nonnumeric channel values omitted.`);
  const columns = specs.map((s) => ({
    topic: s.topic,
    path: s.path,
    times: new Float64Array(s.ts),
    values: new Float64Array(s.vs),
  }));
  if (!columns.some((c) => c.values.length))
    throw new Error('Selected channels contain no usable samples');
  return {
    columns,
    snapshots,
    start: sorted[0].t + offset,
    end: sorted[sorted.length - 1].t + offset,
    count: records.length,
    warnings,
  };
}
