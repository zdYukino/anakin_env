/** Typed, bounded sample storage. Draws preserve pixel-bucket extrema. */
export class RingBuffer {
  readonly times: Float64Array;
  readonly values: Float64Array;
  head = 0;
  length = 0;
  constructor(
    readonly capacity = 32000,
    readonly retention = 60,
  ) {
    this.times = new Float64Array(capacity);
    this.values = new Float64Array(capacity);
  }
  at(index: number): [number, number] {
    const i = (this.head - this.length + index + this.capacity) % this.capacity;
    return [this.times[i], this.values[i]];
  }
  push(t: number, value: number) {
    if (!Number.isFinite(t) || !Number.isFinite(value)) return;
    this.times[this.head] = t;
    this.values[this.head] = value;
    this.head = (this.head + 1) % this.capacity;
    this.length = Math.min(this.length + 1, this.capacity);
    while (this.length > 1 && t - this.at(0)[0] > this.retention) this.length--;
  }
  lowerBound(t: number) {
    let lo = 0,
      hi = this.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (this.at(mid)[0] < t) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }
  nearest(t: number): [number, number] | undefined {
    if (!this.length) return;
    const i = Math.min(this.length - 1, this.lowerBound(t));
    if (i > 0 && t - this.at(i - 1)[0] < this.at(i)[0] - t) return this.at(i - 1);
    return this.at(i);
  }
  envelope(start: number, end: number, pixels: number): [number, number][] {
    const output: [number, number][] = [];
    const lo = this.lowerBound(start),
      hi = Math.min(this.length, this.lowerBound(end) + 1);
    let bucket = -1,
      min: [number, number] = [0, Infinity],
      max: [number, number] = [0, -Infinity];
    const flush = () => {
      if (min[1] !== Infinity) {
        const pair = min[0] <= max[0] ? [min, max] : [max, min];
        output.push(...pair);
      }
    };
    for (let i = lo; i < hi; i++) {
      const point = this.at(i),
        b = Math.floor(((point[0] - start) / Math.max(1e-9, end - start)) * pixels);
      if (b !== bucket) {
        flush();
        bucket = b;
        min = [point[0], Infinity];
        max = [point[0], -Infinity];
      }
      if (point[1] < min[1]) min = point;
      if (point[1] > max[1]) max = point;
    }
    flush();
    return output;
  }
}
