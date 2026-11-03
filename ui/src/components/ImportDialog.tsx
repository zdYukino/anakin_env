import { useI18n } from '../core/i18n';
import { useEffect, useRef, useState } from 'react';
import { FileUp, X, Check, AlertTriangle } from 'lucide-react';
import { telemetry } from '../core/telemetry';
import { RingBuffer } from '../core/buffer';
import { channelId } from '../core/types';
import type { FilePreview, ImportResult, TimeUnit } from '../core/importer';
export function ImportDialog({
  file,
  close,
  loaded,
}: {
  file: File;
  close: () => void;
  loaded: () => void;
}) {
  const { t } = useI18n();
  const worker = useRef<Worker | null>(null),
    [preview, setPreview] = useState<FilePreview>(),
    [error, setError] = useState(''),
    [busy, setBusy] = useState(true);
  const [field, setField] = useState(''),
    [unit, setUnit] = useState<TimeUnit>('s'),
    [selected, setSelected] = useState<string[]>([]);
  useEffect(() => {
    const w = new Worker(new URL('../core/import.worker.ts', import.meta.url), { type: 'module' });
    worker.current = w;
    w.onmessage = (e) => {
      const value = e.data;
      if (value.kind === 'error') {
        setError(value.message);
        setBusy(false);
      }
      if (value.kind === 'preview') {
        const p = value.preview as FilePreview;
        setPreview(p);
        setField(p.guessed);
        setUnit(p.unit);
        setSelected(
          p.numeric.filter((n) => !/stamp|timestamp|elapsed|::time$/.test(n)).slice(0, 6),
        );
        setBusy(false);
      }
      if (value.kind === 'complete') {
        const r = value.result as ImportResult;
        telemetry.reset('file');
        telemetry.fileName = file.name;
        telemetry.start = telemetry.origin = telemetry.cursor = r.start;
        telemetry.end = r.end;
        telemetry.received = r.count;
        telemetry.viewEnd = Math.min(r.end, r.start + telemetry.window);
        r.columns.forEach((c) => {
          const id = channelId(c.topic, c.path);
          const buffer = new RingBuffer(Math.max(1, c.times.length), Infinity);
          for (let i = 0; i < c.times.length; i++) buffer.push(c.times[i], c.values[i]);
          telemetry.buffers.set(id, buffer);
          telemetry.addChannel(c.topic, c.path);
        });
        r.snapshots.forEach((s) => telemetry.snapshot(s));
        const topics = [...new Set(r.snapshots.map((s) => s.topic))];
        telemetry.selectedTopic = topics[0] || '/recording';
        telemetry.topics = topics.map((name) => ({
          name,
          type: 'offline/recording',
          publishers: 0,
          subscribers: 0,
          hz: 0,
          bytes_per_second: 0,
          qos: [],
          last_receive_ns: null,
        }));
        telemetry.log(
          'INFO',
          `Imported ${file.name} · ${r.count} records · ${(r.end - r.start).toFixed(3)} s`,
        );
        r.warnings.forEach((text) => telemetry.log('WARN', text));
        telemetry.emit();
        loaded();
      }
    };
    w.onerror = () => {
      setError('File parser could not complete. Check file size and format.');
      setBusy(false);
    };
    w.postMessage({ kind: 'preview', file });
    return () => w.terminate();
  }, [file, loaded]);
  return (
    <div
      className="modal-backdrop"
      onMouseDown={(e) => {
        if (e.target === e.currentTarget && !busy) close();
      }}
    >
      <section
        className="modal import-modal"
        role="dialog"
        aria-modal="true"
        aria-labelledby="import-title"
      >
        <header>
          <div>
            <span className="eyebrow">{t('LOCAL DATA ANALYSIS')}</span>
            <h2 id="import-title">
              <FileUp size={19} />
              {t('Import recording')}
            </h2>
          </div>
          <button className="icon-button" title={t('Cancel import')} onClick={close}>
            <X size={17} />
          </button>
        </header>
        <div className="modal-body">
          <div className="file-summary">
            <FileUp size={23} />
            <div>
              <strong>{file.name}</strong>
              <span>
                {(file.size / 1e6).toFixed(2)} {t('MB · processed locally in your browser')}
              </span>
            </div>
            {preview && (
              <b>
                {preview.count.toLocaleString()} {t('records')}
              </b>
            )}
          </div>
          {error && (
            <div className="error-message">
              <AlertTriangle size={14} />
              {t(error)}
            </div>
          )}
          {busy && <div className="loading-state">{t('Parsing recording…')}</div>}
          {preview && (
            <>
              <div className="form-row">
                <label>
                  {t('TIME FIELD')}
                  <select value={field} onChange={(e) => setField(e.target.value)}>
                    {preview.timeFields.map((f) => (
                      <option key={f}>{f}</option>
                    ))}
                  </select>
                </label>
                <label>
                  {t('TIME UNIT')}
                  <select value={unit} onChange={(e) => setUnit(e.target.value as TimeUnit)}>
                    <option value="s">{t('Seconds / elapsed')}</option>
                    <option value="ms">{t('Milliseconds')}</option>
                    <option value="ns">{t('Nanoseconds')}</option>
                    <option value="iso">ISO 8601</option>
                    <option value="ros">{t('ROS sec + nanosec')}</option>
                  </select>
                </label>
              </div>
              <div className="section-label">
                {t('ANALYSIS CHANNELS')}
                <span>
                  {selected.length} {t('/ 12 SELECTED')}
                </span>
              </div>
              <div className="import-fields">
                {preview.numeric.map((id) => (
                  <label key={id}>
                    <input
                      type="checkbox"
                      checked={selected.includes(id)}
                      disabled={!selected.includes(id) && selected.length >= 12}
                      onChange={(e) =>
                        setSelected((v) =>
                          e.target.checked ? [...v, id] : v.filter((f) => f !== id),
                        )
                      }
                    />
                    <code>{id.replace('::', ' / ')}</code>
                  </label>
                ))}
              </div>
              <p className="fine-print muted">
                {t(
                  'Source timestamps synchronize all views. Incomplete numeric values are omitted and reported in Diagnostics.',
                )}
              </p>
            </>
          )}
        </div>
        <footer>
          <button className="button" onClick={close}>
            {t('Cancel')}
          </button>
          <button
            className="button primary"
            disabled={busy || !preview || !selected.length}
            onClick={() => {
              setBusy(true);
              setError('');
              worker.current?.postMessage({ kind: 'convert', field, unit, selected });
            }}
          >
            <Check size={13} />
            {t('Load recording')}
          </button>
        </footer>
      </section>
    </div>
  );
}
