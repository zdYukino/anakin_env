import { mkdirSync, writeFileSync } from 'node:fs';
import { expect, it } from 'vitest';
import { TelemetryStore } from '../src/core/telemetry';
it('retains 60s of twelve 500Hz channels in bounded storage', () => {
  const store = new TelemetryStore();
  const fields = Array.from({ length: 12 }, (_, i) => `position[${i}]`);
  fields.forEach((p) => store.addChannel('/state', p));
  const start = performance.now();
  for (let i = 0; i < 35000; i++)
    store.ingest({
      topic: '/state',
      t: 1000 + i / 500,
      values: Object.fromEntries(fields.map((p, j) => [p, Math.sin(i * 0.01 + j)])),
    });
  const ingestMs = performance.now() - start;
  const drawing = performance.now();
  let points = 0;
  store.buffers.forEach((b) => {
    expect(b.length).toBeLessThanOrEqual(30001);
    expect(b.at(0)[0]).toBeGreaterThanOrEqual(1009.998);
    points += b.envelope(1060, 1070, 800).length;
  });
  const reduceMs = performance.now() - drawing;
  const bytes = [...store.buffers.values()].reduce(
    (n, b) => n + b.times.byteLength + b.values.byteLength,
    0,
  );
  const result = {
    inputHz: 500,
    channels: 12,
    simulatedSeconds: 70,
    ingestMs,
    reduce12ChannelsMs: reduceMs,
    bytes,
    envelopePoints: points,
  };
  mkdirSync('artifacts', { recursive: true });
  writeFileSync('artifacts/buffer-performance.json', JSON.stringify(result, null, 2));
  expect(bytes).toBe(6144000);
  expect(points).toBeLessThanOrEqual(12 * 802 * 2);
  expect(ingestMs).toBeLessThan(5000);
});
