// @vitest-environment jsdom
import { afterEach, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { ImportDialog } from '../src/components/ImportDialog';
import { convertRecords, previewRecords, readRecords } from '../src/core/importer';
import { telemetry } from '../src/core/telemetry';

class ParserWorker {
  onmessage?: (event: { data: unknown }) => void;
  onerror?: () => void;
  rows = readRecords('elapsed,position\n0,0.25\n0.5,0.75\n1,1.25', 'test.csv');
  postMessage(packet: { kind: string; field: string; unit: 's'; selected: string[] }) {
    const data =
      packet.kind === 'preview'
        ? { kind: 'preview', preview: previewRecords(this.rows) }
        : {
            kind: 'complete',
            result: convertRecords(this.rows, packet.field, packet.unit, packet.selected),
          };
    queueMicrotask(() => this.onmessage?.({ data }));
  }
  terminate() {}
}
afterEach(() => {
  cleanup();
  telemetry.reset('none');
  vi.unstubAllGlobals();
});
it('confirms file mapping, loads buffered channels and drives historical snapshots', async () => {
  vi.stubGlobal('Worker', ParserWorker);
  const loaded = vi.fn();
  render(<ImportDialog file={new File(['test'], 'test.csv')} close={vi.fn()} loaded={loaded} />);
  await screen.findByText('3 records');
  expect(
    screen.getAllByRole('checkbox').filter((node) => (node as HTMLInputElement).checked),
  ).toHaveLength(1);
  fireEvent.click(screen.getByRole('button', { name: 'Load recording' }));
  await waitFor(() => expect(loaded).toHaveBeenCalledOnce());
  expect(telemetry.source).toBe('file');
  expect(telemetry.mode).toBe('PLAYBACK');
  expect(telemetry.channels[0].path).toBe('position');
  expect(telemetry.buffers.get('/recording::position')?.length).toBe(3);
  telemetry.seek(1.5);
  expect(telemetry.at()?.data).toEqual({ elapsed: 0.5, position: 0.75 });
  telemetry.togglePause();
  expect(telemetry.playing).toBe(true);
});
