import { useI18n } from '../core/i18n';
import { Braces, ChevronRight, Crosshair, PanelRightClose } from 'lucide-react';
import { formatNumber } from '../core/format';
import { telemetry } from '../core/telemetry';
import { FieldBrowser } from './TopicBrowser';
export function Inspector({ precision, onClose }: { precision: number; onClose: () => void }) {
  const { t } = useI18n();
  const topic = telemetry.topics.find((t) => t.name === telemetry.selectedTopic),
    snapshot = telemetry.at();
  const channel = telemetry.channels.find((c) => c.id === telemetry.selectedChannel),
    point = channel && telemetry.buffers.get(channel.id)?.nearest(telemetry.cursor);
  return (
    <aside className="inspector">
      <header>
        <span>{t('INSPECTOR')}</span>
        <button className="icon-button" onClick={onClose} title={t('Collapse inspector')}>
          <PanelRightClose size={14} />
        </button>
      </header>
      {point && channel && (
        <div className="inspected-point">
          <div className="section-label">
            <Crosshair size={11} />
            {t('SELECTED DATA POINT')}
          </div>
          <code>{channel.path}</code>
          <strong>
            {formatNumber(point[1], precision, telemetry.notation)} <small>{channel.unit}</small>
          </strong>
          <dl>
            <dt>{t('TIME / T+')}</dt>
            <dd>{(point[0] - telemetry.origin).toFixed(6)} s</dd>
          </dl>
        </div>
      )}
      <div className="inspector-topic">
        <div className="section-label">
          <Braces size={12} />
          {t('TOPIC DETAILS')}
        </div>
        <h3>{telemetry.selectedTopic}</h3>
        <code className="message-type">{topic?.type || t('No topic selected')}</code>
        <dl className="key-values">
          <dt>{t('Publishers')}</dt>
          <dd>{topic?.publishers ?? '—'}</dd>
          <dt>{t('Subscribers')}</dt>
          <dd>{topic?.subscribers ?? '—'}</dd>
          <dt>{t('Frequency')}</dt>
          <dd>{topic?.hz ? t(`${topic.hz.toFixed(1)} Hz`) : '—'}</dd>
          <dt>{t('Bandwidth')}</dt>
          <dd>
            {topic?.bytes_per_second
              ? t(`${(topic.bytes_per_second / 1024).toFixed(2)} KB/s`)
              : '—'}
          </dd>
          <dt>{t('Reliability')}</dt>
          <dd>{topic?.qos?.[0]?.reliability?.replaceAll('_', ' ') || '—'}</dd>
          <dt>{t('Durability')}</dt>
          <dd>{topic?.qos?.[0]?.durability?.replaceAll('_', ' ') || '—'}</dd>
          <dt>{t('Queue depth')}</dt>
          <dd>{topic?.qos?.[0]?.depth ?? '—'}</dd>
          <dt>{t('Time basis')}</dt>
          <dd>{t(snapshot?.clock?.toUpperCase() || '—')}</dd>
          <dt>{t('Source ns')}</dt>
          <dd title={snapshot?.source_ns || ''}>{snapshot?.source_ns || '—'}</dd>
          <dt>{t('Receive ns')}</dt>
          <dd title={snapshot?.receive_ns || ''}>{snapshot?.receive_ns || '—'}</dd>
        </dl>
      </div>
      <div className="inspector-fields-title">
        <ChevronRight size={11} /> {t('MESSAGE STRUCTURE')} <span>{t('+ TO PLOT')}</span>
      </div>
      <FieldBrowser precision={precision} />
      <footer>
        {t('PROTOCOL V1')}
        <span>{t('READ ONLY')}</span>
      </footer>
    </aside>
  );
}
