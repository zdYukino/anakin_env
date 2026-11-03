import { useI18n } from '../core/i18n';
import { useState } from 'react';
import { ChevronRight, Search, Radio, Plus, Check, ChevronDown } from 'lucide-react';
import { formatNumber } from '../core/format';
import { telemetry } from '../core/telemetry';
import { flatten } from '../core/types';
import type { Json, SchemaField } from '../core/types';

export function TopicBrowser({ compact = false }: { compact?: boolean }) {
  const { t } = useI18n();
  const [search, setSearch] = useState('');
  const topics = telemetry.topics.filter((t) =>
    `${t.name} ${t.type}`.toLowerCase().includes(search.toLowerCase()),
  );
  return (
    <div className={`topic-browser ${compact ? 'compact' : ''}`}>
      <label className="search-input">
        <Search size={13} />
        <input
          placeholder={t('Filter topics or message types…')}
          aria-label={t('Filter topics')}
          value={search}
          onChange={(e) => setSearch(e.target.value)}
        />
        <kbd>/</kbd>
      </label>
      <div className="table-scroll">
        <table className="data-table topic-table">
          <thead>
            <tr>
              <th>{t('TOPIC')}</th>
              {!compact && <th>{t('MESSAGE TYPE')}</th>}
              <th className="numeric">{t('RATE')}</th>
              {!compact && <th className="numeric">{t('BW')}</th>}
              <th />
            </tr>
          </thead>
          <tbody>
            {topics.map((topic) => (
              <tr
                key={topic.name}
                className={telemetry.selectedTopic === topic.name ? 'selected' : ''}
                onClick={() => telemetry.inspectTopic(topic.name)}
                tabIndex={0}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') telemetry.inspectTopic(topic.name);
                }}
              >
                <td>
                  <span className={`topic-dot ${topic.hz ? 'online' : ''}`} />
                  <code>{topic.name}</code>
                </td>
                {!compact && (
                  <td className="muted">
                    <code>{topic.type}</code>
                  </td>
                )}
                <td className="numeric">
                  {topic.hz ? topic.hz.toFixed(0) : '—'}
                  <small> Hz</small>
                </td>
                {!compact && (
                  <td className="numeric">
                    {topic.bytes_per_second ? (topic.bytes_per_second / 1024).toFixed(1) : '—'}
                    <small> KB/s</small>
                  </td>
                )}
                <td>
                  <ChevronRight size={12} />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        {!topics.length && (
          <div className="empty-state">
            <Radio size={20} />
            <span>
              {telemetry.source === 'none'
                ? t('Connect a gateway to discover topics')
                : t('No matching topics')}
            </span>
          </div>
        )}
      </div>
      <div className="table-footer">
        <span>
          {topics.length} {t('TOPICS')}
        </span>
        <span>
          {telemetry.source === 'demo' ? t('DEMO CATALOG · SIMULATED') : t('READ-ONLY DISCOVERY')}
        </span>
      </div>
    </div>
  );
}
function SchemaTree({ fields, prefix = '' }: { fields: SchemaField[]; prefix?: string }) {
  const { t } = useI18n();
  return (
    <>
      {fields.map((field) => {
        const path = prefix ? `${prefix}.${field.name}` : field.name;
        return field.children.length ? (
          <details key={path} className="schema-node">
            <summary>
              <ChevronRight size={11} />
              <span>
                {field.name}
                {field.array ? '[]' : ''}
              </span>
              <small>{field.type}</small>
            </summary>
            <div>
              <SchemaTree fields={field.children} prefix={field.array ? `${path}[0]` : path} />
            </div>
          </details>
        ) : (
          <div key={path} className="field-row">
            <code>{field.name}</code>
            <small>{field.type}</small>
            {/float|double|int|octet/.test(field.type) && (
              <button
                className="icon-button"
                title={t(`Plot ${path}`)}
                onClick={() =>
                  telemetry.addChannel(telemetry.selectedTopic, field.array ? `${path}[0]` : path)
                }
              >
                <Plus size={11} />
              </button>
            )}
          </div>
        );
      })}
    </>
  );
}
export function FieldBrowser({ precision }: { precision: number }) {
  const { t } = useI18n();
  const [filter, setFilter] = useState('');
  const snapshot = telemetry.at(),
    flat = snapshot ? flatten(snapshot.data) : {};
  const schema = telemetry.schemas.get(telemetry.selectedTopic);
  const entries = Object.entries(flat).filter(([key]) =>
    key.toLowerCase().includes(filter.toLowerCase()),
  );
  return (
    <div className="field-browser">
      <label className="search-input">
        <Search size={12} />
        <input
          aria-label={t('Filter message fields')}
          placeholder={t('Find a field…')}
          value={filter}
          onChange={(e) => setFilter(e.target.value)}
        />
      </label>
      {snapshot ? (
        <div className="field-list">
          {entries.slice(0, 400).map(([path, value]) => {
            const numeric = typeof value === 'number' && Number.isFinite(value),
              selected = telemetry.channels.some(
                (c) => c.topic === telemetry.selectedTopic && c.path === path,
              );
            return (
              <div key={path} className={`field-row ${selected ? 'field-selected' : ''}`}>
                <code title={path}>{path}</code>
                <b title={String(value)}>
                  {numeric ? formatNumber(value, precision, telemetry.notation) : display(value)}
                </b>
                {numeric && (
                  <button
                    className="icon-button"
                    disabled={selected}
                    title={t(selected ? 'Already plotted' : `Plot ${path}`)}
                    onClick={() => telemetry.addChannel(telemetry.selectedTopic, path)}
                  >
                    {selected ? <Check size={11} /> : <Plus size={11} />}
                  </button>
                )}
              </div>
            );
          })}
          {entries.length > 400 && (
            <p className="muted fine-print">
              {t('Showing 400 fields. Filter to narrow the selection.')}
            </p>
          )}
        </div>
      ) : schema ? (
        <div className="field-list">
          <div className="inline-note">{t('Schema available · awaiting message values')}</div>
          <SchemaTree fields={schema.fields} />
        </div>
      ) : (
        <div className="empty-state">
          <ChevronDown size={16} />
          <span>{t('Select a topic to inspect its schema')}</span>
        </div>
      )}
    </div>
  );
}
function display(value: Json) {
  if (value === null) return 'null';
  if (typeof value === 'string') return value.length > 28 ? value.slice(0, 25) + '…' : value;
  return String(value);
}
