import { useI18n } from '../core/i18n';
import { useEffect, useState } from 'react';
import {
  ArrowRightToLine,
  Clock3,
  Pause,
  Play,
  Radio,
  SkipBack,
  SkipForward,
  ZoomIn,
  ZoomOut,
} from 'lucide-react';
import { telemetry } from '../core/telemetry';
export function Timeline() {
  const { t } = useI18n();
  const [editing, setEditing] = useState(false),
    [jump, setJump] = useState('0.000');
  useEffect(() => {
    if (!editing) setJump(Math.max(0, telemetry.cursor - telemetry.origin).toFixed(3));
  }, [editing, telemetry.cursor, telemetry.origin]);
  const offline = telemetry.source === 'file',
    active = telemetry.mode === 'LIVE' || telemetry.playing;
  const duration = Math.max(0.001, telemetry.end - telemetry.start);
  return (
    <div className="timeline">
      <div className="timeline-main">
        <div className="timeline-mode">
          <Radio size={12} />
          <span className={telemetry.mode === 'LIVE' ? 'status-good' : 'status-warn'}>
            {t(telemetry.mode)}
          </span>
        </div>
        <div className="transport-buttons">
          <button
            title={t('Jump to buffer start')}
            onClick={() => telemetry.seek(telemetry.start)}
            disabled={!telemetry.received}
          >
            <SkipBack size={13} />
          </button>
          <button
            className="play-button"
            title={t(active ? 'Pause display' : offline ? 'Play recording' : 'Return to live')}
            onClick={() => telemetry.togglePause()}
            disabled={!telemetry.received}
          >
            {active ? <Pause size={14} /> : <Play size={14} />}
          </button>
          <button
            title={t('Jump to end')}
            onClick={() => telemetry.seek(telemetry.end)}
            disabled={!telemetry.received}
          >
            <SkipForward size={13} />
          </button>
        </div>
        <select
          title={t('Playback speed')}
          aria-label={t('Playback speed')}
          disabled={!offline}
          value={telemetry.speed}
          onChange={(e) => {
            telemetry.speed = Number(e.target.value);
            telemetry.emit();
          }}
        >
          {[0.25, 0.5, 1, 2, 4].map((v) => (
            <option key={v} value={v}>
              {v}×
            </option>
          ))}
        </select>
        <div className="timeline-scrub">
          <div className="timeline-ticks">
            <span>T+{Math.max(0, telemetry.start - telemetry.origin).toFixed(1)}</span>
            <span>{Math.max(0, telemetry.end - telemetry.origin).toFixed(1)} s</span>
          </div>
          <input
            aria-label={t('Timeline position')}
            type="range"
            min="0"
            max="10000"
            value={Math.min(
              10000,
              Math.max(0, ((telemetry.cursor - telemetry.start) / duration) * 10000),
            )}
            onChange={(e) => {
              telemetry.seek(telemetry.start + (Number(e.target.value) / 10000) * duration);
              if (offline)
                telemetry.viewEnd = Math.min(
                  telemetry.end,
                  Math.max(telemetry.cursor, telemetry.start + telemetry.window),
                );
            }}
            disabled={!telemetry.received}
          />
        </div>
        <label className="time-jump">
          <Clock3 size={12} />
          <input
            aria-label={t('Jump to elapsed seconds')}
            value={jump}
            onChange={(e) => setJump(e.target.value)}
            onFocus={() => setEditing(true)}
            onBlur={() => setEditing(false)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') {
                const n = Number(e.currentTarget.value);
                if (Number.isFinite(n)) telemetry.seek(telemetry.origin + n);
                setEditing(false);
              }
            }}
          />
          <span>s</span>
        </label>
        <div className="timeline-zoom">
          <button
            title={t('Wider time range')}
            onClick={() => {
              telemetry.zoom(false);
            }}
          >
            <ZoomOut size={13} />
          </button>
          <select
            aria-label={t('Chart time range')}
            value={telemetry.window}
            onChange={(e) => {
              telemetry.window = Number(e.target.value);
              telemetry.emit();
            }}
          >
            {[...new Set([0.25, 0.5, 1, 2, 5, 10, 20, 30, 60, telemetry.window])]
              .sort((a, b) => a - b)
              .map((v) => (
                <option key={v} value={v}>
                  {v} s
                </option>
              ))}
          </select>
          <button
            title={t('Narrower time range')}
            onClick={() => {
              telemetry.zoom(true);
            }}
          >
            <ZoomIn size={13} />
          </button>
        </div>
        <button className="live-button" onClick={() => telemetry.live()} disabled={offline}>
          <ArrowRightToLine size={12} />
          {t('LIVE')}
        </button>
      </div>
      <div className="timeline-status">
        <span>
          <i />{' '}
          {offline
            ? t('LOCAL RECORDING')
            : telemetry.source === 'demo'
              ? t('SYNTHETIC DATA · DEMO')
              : t('SOURCE TIMESTAMPS')}
          <b> / </b>{' '}
          {telemetry.mode === 'LIVE' ? t('FOLLOWING LIVE EDGE') : t('SHARED TIME CURSOR')}
          {telemetry.cursor < telemetry.start ? t(' · HISTORY EXPIRED') : ''}
        </span>
        <span>
          {t('SYNC ALL VIEWS')}
          <i className="status-dot" /> {telemetry.frameMs.toFixed(2)}
          {t('ms / chart')}
        </span>
      </div>
    </div>
  );
}
