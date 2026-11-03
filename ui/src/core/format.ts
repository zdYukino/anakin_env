export type Notation = 'fixed' | 'scientific';
export function formatNumber(
  value: number | undefined,
  precision = 3,
  notation: Notation = 'fixed',
): string {
  if (value === undefined || !Number.isFinite(value)) return '—';
  return notation === 'scientific' ||
    Math.abs(value) >= 1e7 ||
    (value !== 0 && Math.abs(value) < 10 ** -Math.max(6, precision))
    ? value.toExponential(precision)
    : value.toFixed(precision);
}
