import { Activity } from 'lucide-react';
import { useI18n } from '../core/i18n';
import { switchEventText, switchEventRoute } from '../core/events';
import { telemetry } from '../core/telemetry';

export function EventIndicator({ onOpen }: { onOpen: () => void }) {
  const { t } = useI18n();
  const log = telemetry.latestEvent;
  const message = log
    ? log.controlEvent
      ? switchEventText(log.controlEvent, t)
      : t(log.text)
    : t('Waiting for events');
  const detail = log?.controlEvent?.detail;
  return (
    <button
      className={`navigation-event ${log?.controlEvent?.result || log?.level.toLowerCase() || ''}`}
      aria-label={t('Open event log')}
      onClick={onOpen}
      title={detail ? `${message}\n${t(detail)}` : message}
    >
      <span>
        <Activity size={11} />
        {t('LATEST EVENT')}
        <time>{log ? new Date(log.time).toLocaleTimeString([], { hour12: false }) : '—'}</time>
      </span>
      <strong role="status" aria-live="polite" aria-atomic="true">
        <span>{log?.controlEvent ? switchEventRoute(log.controlEvent, t) : message}</span>
        {log?.controlEvent && <em>{` · ${t(log.controlEvent.result)}`}</em>}
      </strong>
    </button>
  );
}
