import { convertRecords, previewRecords, readRecords } from './importer';
import type { TimeUnit } from './importer';
let records: ReturnType<typeof readRecords> = [];
self.onmessage = async (
  e: MessageEvent<
    | { kind: 'preview'; file: File }
    | { kind: 'convert'; field: string; unit: TimeUnit; selected: string[] }
  >,
) => {
  try {
    if (e.data.kind === 'preview') {
      if (e.data.file.size > 50_000_000)
        throw new Error('File exceeds 50 MB. Split the recording before importing.');
      records = readRecords(await e.data.file.text(), e.data.file.name);
      self.postMessage({ kind: 'preview', preview: previewRecords(records) });
    } else {
      const result = convertRecords(records, e.data.field, e.data.unit, e.data.selected);
      self.postMessage(
        { kind: 'complete', result },
        { transfer: result.columns.flatMap((c) => [c.times.buffer, c.values.buffer]) },
      );
      records = [];
    }
  } catch (error) {
    self.postMessage({
      kind: 'error',
      message: error instanceof Error ? error.message : String(error),
    });
  }
};
