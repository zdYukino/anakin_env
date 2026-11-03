import { useI18n } from './core/i18n';
import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from 'react';
import {
  Activity,
  ArrowDownToLine,
  ArrowRight,
  Braces,
  Check,
  ChevronRight,
  Columns3,
  FileClock,
  FileUp,
  Globe2,
  Layers,
  LayoutDashboard,
  Link2,
  PanelRightOpen,
  Play,
  Plus,
  RotateCcw,
  Save,
  Settings2,
  ShieldCheck,
  Unplug,
  X,
  AlertTriangle,
  Box,
  Cpu,
  Wifi,
  Gamepad2,
} from 'lucide-react';
import { telemetry } from './core/telemetry';
import { startDemo } from './core/demo';
import { findJoints, validGroup } from './core/robot';
import { defaultLayout, loadLayout, PANEL_NAMES } from './core/layout';
import { formatNumber } from './core/format';
import { numberAt } from './core/types';
import type { View } from './core/types';
import { Panel } from './components/Panel';
import { ControllerStatus, ControllerBindings } from './components/ControllerStatus';
import { TelemetryChart } from './components/Chart';
import { Inspector } from './components/Inspector';
import { TopicBrowser, FieldBrowser } from './components/TopicBrowser';
import { Timeline } from './components/Timeline';
import { ImportDialog } from './components/ImportDialog';
import { MissionOverview } from './components/MissionOverview';
import { EventIndicator } from './components/EventIndicator';
import { switchEventText } from './core/events';
import { VirtualJoy } from './components/WebControl';

const navigation = [
  { name: 'Overview', icon: LayoutDashboard },
  { name: 'Telemetry', icon: Activity },
  { name: 'Workbench', icon: Columns3 },
  { name: 'Topics', icon: Braces },
  { name: 'Controllers', icon: Gamepad2 },
  { name: 'Playback', icon: FileClock },
  { name: 'Diagnostics', icon: ShieldCheck },
  { name: 'Settings', icon: Settings2 },
] as const;
const descriptions: Record<View, string> = {
  Overview: 'Robot state and synchronized telemetry',
  Workbench: 'Customizable telemetry workspace',
  Topics: 'Discover, inspect and subscribe to message fields',
  Telemetry: 'Synchronized multi-channel signal analysis',
  Controllers: 'Live Joy and Unitree remote input with runtime YAML bindings',
  Playback: 'Local recordings and historical analysis',
  Diagnostics: 'Transport health, timing and stream events',
  Settings: 'Workspace and acquisition preferences',
};

export default function App() {
  const { t, language, setLanguage } = useI18n();
  useSyncExternalStore(telemetry.subscribe, telemetry.getVersion);
  const [view, setView] = useState<View>('Overview'),
    [layout, setLayout] = useState(loadLayout),
    [maximized, setMaximized] = useState('');
  const [connectOpen, setConnectOpen] = useState(false),
    [importFile, setImportFile] = useState<File | null>(null),
    [toast, setToast] = useState('');
  const [mobileInspectorOpen, setMobileInspectorOpen] = useState(false);
  const [joyOpen, setJoyOpen] = useState(false);
  useEffect(() => {
    if (!telemetry.webRequested) setJoyOpen(false);
  }, [telemetry.webRequested]);
  const [precision, setPrecision] = useState(() => {
    const n = Number(localStorage.getItem('aerospace.precision') ?? '3');
    return Math.max(0, Math.min(8, n || 0));
  });
  const [workspace, setWorkspace] = useState(
    () => localStorage.getItem('aerospace.workspace') || 'G1 DEVELOPMENT',
  );
  const [gatewayUrl, setGatewayUrl] = useState(
    () =>
      localStorage.getItem('aerospace.gateway') ||
      `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`,
  );
  const [gridHeight, setGridHeight] = useState(0);
  const fileInput = useRef<HTMLInputElement>(null),
    grid = useRef<HTMLDivElement>(null);
  useEffect(() => {
    telemetry.notation =
      localStorage.getItem('aerospace.notation') === 'scientific' ? 'scientific' : 'fixed';
    telemetry.startClock();
    return () => telemetry.dispose();
  }, []);
  useEffect(() => {
    const release = () => {
      telemetry.releaseWeb();
      setJoyOpen(false);
      telemetry.emit();
    };
    const visibility = () => {
      if (document.hidden) release();
    };
    window.addEventListener('blur', release);
    window.addEventListener('pagehide', release);
    document.addEventListener('visibilitychange', visibility);
    return () => {
      window.removeEventListener('blur', release);
      window.removeEventListener('pagehide', release);
      document.removeEventListener('visibilitychange', visibility);
    };
  }, []);
  useEffect(() => {
    localStorage.setItem('aerospace.precision', String(precision));
  }, [precision]);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        setConnectOpen(false);
        setImportFile(null);
        setMaximized('');
      }
      if (e.key === '/' && !(e.target instanceof HTMLInputElement)) {
        e.preventDefault();
        setView('Topics');
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);
  useEffect(() => {
    if (!toast) return;
    const id = setTimeout(() => setToast(''), 3000);
    return () => clearTimeout(id);
  }, [toast]);
  useEffect(() => {
    if (view !== 'Workbench' || !grid.current || typeof ResizeObserver === 'undefined') return;
    const observer = new ResizeObserver((entries) => setGridHeight(entries[0].contentRect.height));
    observer.observe(grid.current);
    return () => observer.disconnect();
  }, [view]);
  useEffect(() => {
    if (!connectOpen && !importFile) return;
    const previous = document.activeElement as HTMLElement | null;
    const dialog = document.querySelector('[role="dialog"]');
    const controls = () =>
      Array.from(
        dialog?.querySelectorAll<HTMLElement>(
          'button:not(:disabled), input:not(:disabled), select:not(:disabled), a[href]',
        ) || [],
      );
    controls()[0]?.focus();
    const trap = (event: KeyboardEvent) => {
      if (event.key !== 'Tab') return;
      const items = controls(),
        first = items[0],
        last = items.at(-1);
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last?.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first?.focus();
      }
    };
    document.addEventListener('keydown', trap);
    return () => {
      document.removeEventListener('keydown', trap);
      previous?.focus();
    };
  }, [connectOpen, importFile]);
  const loaded = useCallback(() => {
    setImportFile(null);
    setView('Playback');
  }, []);
  const jointSelected = useCallback((name: string) => {
    const snapshot = telemetry.at('/anakin/state') || telemetry.at();
    const data = findJoints(snapshot?.data);
    const index = data?.name.indexOf(name) ?? -1;
    if (index >= 0) {
      const root = snapshot?.data;
      const topic = snapshot?.topic || telemetry.selectedTopic;
      const path =
        root && typeof root === 'object' && !Array.isArray(root) && 'joints' in root
          ? `joints.position[${index}]`
          : root && typeof root === 'object' && !Array.isArray(root) && 'joint_state' in root
            ? `joint_state.position[${index}]`
            : `position[${index}]`;
      telemetry.addChannel(
        topic,
        path,
        name.replace('_joint', '').replaceAll('_', ' ').toUpperCase(),
      );
      telemetry.selectedChannel =
        telemetry.channels.find((c) => c.topic === topic && c.path === path)?.id || '';
      telemetry.inspectTopic(topic);
      telemetry.emit();
    }
  }, []);
  const saveWorkspace = () => {
    localStorage.setItem('aerospace.layout.v1', JSON.stringify(layout));
    localStorage.setItem('aerospace.channels.v1', JSON.stringify(telemetry.channels));
    setToast('Workspace layout and channels saved');
  };
  const restoreWorkspace = () => {
    setLayout(loadLayout());
    try {
      const saved = JSON.parse(localStorage.getItem('aerospace.channels.v1') || 'null');
      const channels = Array.isArray(saved)
        ? saved.filter(
            (c) =>
              c &&
              telemetry.topics.some((t) => t.name === c.topic) &&
              (telemetry.source !== 'file' || telemetry.buffers.has(`${c.topic}::${c.path}`)),
          )
        : null;
      if (
        Array.isArray(channels) &&
        channels.length > 0 &&
        channels.length <= 12 &&
        telemetry.source !== 'none'
      ) {
        const wanted = new Set(channels.map((c) => `${c?.topic}::${c?.path}`));
        if (telemetry.source !== 'file')
          telemetry.buffers.forEach((_, id) => {
            if (!wanted.has(id)) telemetry.buffers.delete(id);
          });
        telemetry.channels = [];
        for (const c of channels) {
          if (c && typeof c.topic === 'string' && typeof c.path === 'string') {
            telemetry.addChannel(
              c.topic,
              c.path,
              typeof c.label === 'string' ? c.label : undefined,
            );
            const restored = telemetry.channels.at(-1);
            if (restored) {
              restored.visible = c.visible !== false;
              if (typeof c.unit === 'string') restored.unit = c.unit;
            }
          }
        }
        telemetry.syncSubscriptions();
        telemetry.emit();
      }
    } catch {
      /* An invalid saved configuration keeps the current channels. */
    }
    setToast('Saved workspace restored');
  };
  const chooseFile = () => fileInput.current?.click();
  const notify = (text: string) => setToast(text);
  const data = (telemetry.at('/anakin/state') || telemetry.at())?.data,
    joints = findJoints(data);
  const height = validGroup(data, 16) ? numberAt(data, 'odometry.pose.pose.position.z') : undefined;
  const statusClass =
    telemetry.source === 'demo' && telemetry.connection !== 'ERROR'
      ? 'demo'
      : telemetry.connection.toLowerCase();
  const values = (value: number | undefined, digits = precision) =>
    formatNumber(value, digits, telemetry.notation);
  const togglePanel = (id: string) =>
    setLayout((l) => ({
      ...l,
      collapsed: l.collapsed.includes(id)
        ? l.collapsed.filter((x) => x !== id)
        : [...l.collapsed, id],
    }));
  const reorder = (from: string, to: string) =>
    setLayout((l) => {
      const order = [...l.order],
        a = order.indexOf(from),
        b = order.indexOf(to);
      if (a < 0 || b < 0) return l;
      [order[a], order[b]] = [order[b], order[a]];
      return { ...l, order };
    });
  const rowPixels = gridHeight
    ? Math.min(gridHeight - 162, Math.max(203, ((gridHeight - 11) * layout.rowSplit) / 100))
    : 0;
  const startRowResize = (event: React.PointerEvent) => {
    event.preventDefault();
    const element = event.currentTarget;
    element.setPointerCapture(event.pointerId);
    const move = (e: PointerEvent) => {
      const rect = grid.current?.getBoundingClientRect();
      if (rect)
        setLayout((l) => ({
          ...l,
          rowSplit: Math.max(
            35,
            Math.min(80, ((e.clientY - rect.top) / Math.max(1, rect.height - 11)) * 100),
          ),
        }));
    };
    const end = () => {
      element.removeEventListener('pointermove', move as EventListener);
      element.removeEventListener('pointerup', end);
    };
    element.addEventListener('pointermove', move as EventListener);
    element.addEventListener('pointerup', end, { once: true });
  };
  const startResize = (event: React.PointerEvent) => {
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    const element = event.currentTarget;
    const move = (e: PointerEvent) => {
      const rect = grid.current?.getBoundingClientRect();
      if (rect)
        setLayout((l) => ({
          ...l,
          columnWidth: Math.min(75, Math.max(25, ((e.clientX - rect.left) / rect.width) * 100)),
        }));
    };
    const up = () => {
      element.removeEventListener('pointermove', move as EventListener);
      element.removeEventListener('pointerup', up);
    };
    element.addEventListener('pointermove', move as EventListener);
    element.addEventListener('pointerup', up, { once: true });
  };
  const positionChannels = telemetry.channels.filter((c) => !/imu\.|gyro|accel/.test(c.path)),
    imuChannels = telemetry.channels.filter((c) => /imu\.|gyro|accel/.test(c.path));
  const max = (id: string) => setMaximized((v) => (v === id ? '' : id));
  const renderPanel = (id: string, slot: number) => (
    <Panel
      key={id}
      id={id}
      title={t(PANEL_NAMES[id])}
      tag={id === 'trend' ? 'POSITION' : undefined}
      collapsed={layout.collapsed.includes(id)}
      maximized={maximized === id}
      onToggle={() => togglePanel(id)}
      onMaximize={() => max(id)}
      onRemove={() => setLayout((l) => ({ ...l, hidden: [...l.hidden, id] }))}
      onReorder={reorder}
      className={`slot-${slot} panel-${id}`}
      action={
        id === 'trend' ? (
          <span className="panel-meta">
            {telemetry.mode === 'LIVE' ? 'LIVE' : 'SYNC'} <i className="status-dot" />
          </span>
        ) : undefined
      }
    >
      {id === 'trend' && (
        <TelemetryChart
          channels={positionChannels}
          title={t('Joint position')}
          precision={precision}
          onMaximize={() => max(id)}
        />
      )}
      {id === 'imu' && (
        <TelemetryChart
          channels={imuChannels}
          title={t('Angular velocity')}
          precision={precision}
          onMaximize={() => max(id)}
        />
      )}
      {id === 'topics' && <TopicBrowser compact />}
      {id === 'status' && (
        <div className="system-status">
          <StatusRow
            label={t('Gateway transport')}
            value={telemetry.source === 'demo' ? 'SIMULATED' : t(telemetry.connection)}
            good={telemetry.connection === 'CONNECTED'}
            warn={telemetry.source === 'demo'}
          />
          <StatusRow
            label={t('Joint feedback')}
            value={
              joints ? `${joints.position.filter(Number.isFinite).length} / 29 RECEIVED` : 'NO DATA'
            }
            good={Boolean(joints)}
          />
          <StatusRow
            label={t('Message integrity')}
            value={
              telemetry.received
                ? telemetry.dropped
                  ? `${telemetry.dropped} DROPPED`
                  : 'NO QUEUE DROPS'
                : 'AWAITING'
            }
            good={telemetry.received > 0 && !telemetry.dropped}
          />
          <StatusRow label={t('Motor temperature')} value="N/A" />
          <StatusRow label={t('Motor voltage')} value="N/A" />
          <div className="read-only-note">
            <ShieldCheck size={12} /> {t('OBSERVATION ONLY')} <span>{t('NO CONTROL OUTPUT')}</span>
          </div>
        </div>
      )}
    </Panel>
  );

  return (
    <div
      className={`app-shell ${layout.navCollapsed ? 'nav-collapsed' : ''} ${layout.inspector ? '' : 'inspector-closed'} ${mobileInspectorOpen ? 'mobile-inspector-open' : ''}`}
    >
      <input
        ref={fileInput}
        className="visually-hidden"
        type="file"
        accept=".csv,.json,.mcap"
        aria-label={t('Import telemetry file')}
        onChange={(e) => {
          const file = e.target.files?.[0];
          if (file) setImportFile(file);
          e.target.value = '';
        }}
      />
      <header className="global-bar mission-header">
        <a
          className="mission-brand"
          href="#"
          aria-label={t('Aerospace overview')}
          onClick={(event) => {
            event.preventDefault();
            setView('Overview');
          }}
        >
          <span className="brand-mark">
            G<span>1</span>
          </span>
          <span className="brand-divider" />
          <span className="brand-copy">
            <strong>UNITREE G1</strong>
            <small>{t('ROBOTICS TELEMETRY')}</small>
          </span>
        </a>
        <div className="mission-header-title">
          <span>
            {t('TELEMETRY /')}{' '}
            {String(navigation.findIndex((item) => item.name === view) + 1).padStart(2, '0')}
          </span>
          {view === 'Overview' ? <h1>{t('Robot overview')}</h1> : <strong>{t(view)}</strong>}
        </div>
        <div className="mission-header-actions">
          {telemetry.source === 'demo' && (
            <span className="mission-demo">{t('SIMULATED DATA')}</span>
          )}
          {telemetry.source === 'file' && (
            <span className="mission-demo">{t('OFFLINE RECORDING')}</span>
          )}
          <span className={`mission-connection ${statusClass}`}>
            <i />
            {telemetry.source === 'demo'
              ? t(telemetry.connection === 'ERROR' ? 'DEMO ERROR' : 'DEMO ACTIVE')
              : telemetry.source === 'file'
                ? t('OFFLINE')
                : t(telemetry.connection)}
          </span>
          <button className="mission-connect" onClick={() => setConnectOpen(true)}>
            <Link2 size={13} />
            {telemetry.source === 'none' ? t('Connect source') : t('Data source')}
          </button>
          {telemetry.source === 'ros2' &&
            ['CONNECTED', 'DEGRADED'].includes(telemetry.connection) && (
              <button
                className={`mission-web-control ${telemetry.webActive ? 'active' : ''}`}
                aria-pressed={telemetry.webRequested}
                disabled={
                  !telemetry.webRequested &&
                  (!telemetry.canWebControl || Boolean(telemetry.control?.leased))
                }
                title={t(
                  telemetry.control?.leased && !telemetry.webRequested
                    ? 'Another browser holds web control'
                    : 'Web control',
                )}
                onClick={() => {
                  if (telemetry.webRequested) {
                    telemetry.releaseWeb();
                    setJoyOpen(false);
                    telemetry.emit();
                  } else telemetry.activateWeb();
                }}
              >
                <Gamepad2 size={14} />
                {t(telemetry.webRequested ? 'Disable web control' : 'Web control')}
              </button>
            )}
        </div>
      </header>
      {joyOpen && telemetry.webActive && <VirtualJoy onClose={() => setJoyOpen(false)} />}
      <main className={`main-workspace ${view === 'Overview' ? 'overview-workspace' : ''}`}>
        {view !== 'Overview' && (
          <div className="workspace-heading">
            <div>
              <div className="breadcrumb">
                {t('WORKSPACE')}
                <ChevronRight size={9} /> {workspace} <ChevronRight size={9} />{' '}
                <b>{t(view).toUpperCase()}</b>
              </div>
              <div className="page-title">
                <h1>{t(view)}</h1>
                {telemetry.source === 'demo' && (
                  <span className="demo-tag">{t('SIMULATED DATA')}</span>
                )}
                {telemetry.source === 'file' && (
                  <span className="offline-tag">{t('OFFLINE RECORDING')}</span>
                )}
              </div>
              <p>{t(descriptions[view])}</p>
            </div>
            <div className="workspace-actions">
              <button
                className="button mobile-inspector-button"
                onClick={() => {
                  setLayout((current) => ({ ...current, inspector: true }));
                  setMobileInspectorOpen(true);
                }}
              >
                <PanelRightOpen size={13} />
                {t('INSPECTOR')}
              </button>
              <button className="button" title={t('Import CSV or JSON')} onClick={chooseFile}>
                <FileUp size={13} />
                <span>{t('Import')}</span>
              </button>
            </div>
          </div>
        )}
        {telemetry.error && (
          <div className="error-banner">
            <AlertTriangle size={13} />
            <span>{t(telemetry.error)}</span>
            <button
              className="icon-button"
              title={t('Dismiss error')}
              onClick={() => {
                telemetry.error = '';
                telemetry.emit();
              }}
            >
              <X size={12} />
            </button>
          </div>
        )}
        {view === 'Overview' && (
          <MissionOverview
            onJoint={jointSelected}
            precision={precision}
            onJoy={() => setJoyOpen((open) => !open)}
            joyOpen={joyOpen}
          />
        )}
        {view === 'Workbench' && (
          <>
            <div className="metric-strip">
              <Metric
                label={t('STREAM FREQUENCY')}
                value={telemetry.received ? telemetry.hz.toFixed(0) : '—'}
                unit="Hz"
                foot={t(
                  telemetry.source === 'demo'
                    ? 'SYNTHETIC / 500 Hz TARGET'
                    : 'ACTUAL RECEIVED RATE',
                )}
                icon={<Activity size={14} />}
              />
              <Metric
                label={t('ACTIVE CHANNELS')}
                value={telemetry.channels.length.toString().padStart(2, '0')}
                unit="/ 12"
                foot={t(`${telemetry.topics.length} TOPICS DISCOVERED`)}
                icon={<Braces size={14} />}
              />
              <Metric
                label={t('BASE HEIGHT')}
                value={values(height)}
                unit="m"
                foot={t('ODOMETRY / POSITION Z')}
                icon={<Box size={14} />}
              />
              <Metric
                label={t('STREAM INTEGRITY')}
                value={telemetry.received ? telemetry.dropped.toString() : '—'}
                unit="dropped"
                foot={t(`${telemetry.received.toLocaleString()} MESSAGES RECEIVED`)}
                good={!telemetry.dropped && telemetry.received > 0}
                icon={<ShieldCheck size={14} />}
              />
            </div>
            <div className="layout-toolbar">
              <span>
                <Columns3 size={12} /> {t('MISSION WORKSPACE')} <b>01</b>
              </span>
              <div>
                <select
                  aria-label={t('Add data view')}
                  value=""
                  onChange={(e) => {
                    const id = e.target.value;
                    setLayout((l) => ({ ...l, hidden: l.hidden.filter((x) => x !== id) }));
                  }}
                >
                  <option value="" disabled>
                    {t('＋ Add view')}
                  </option>
                  {layout.hidden.map((id) => (
                    <option key={id} value={id}>
                      {t(PANEL_NAMES[id])}
                    </option>
                  ))}
                </select>
                <button
                  title={t('Save layout')}
                  onClick={() => {
                    saveWorkspace();
                  }}
                >
                  <Save size={12} />
                  <span>{t('Save layout')}</span>
                </button>
                <button
                  title={t('Restore saved layout')}
                  onClick={() => {
                    restoreWorkspace();
                  }}
                >
                  <RotateCcw size={12} />
                </button>
                {!layout.inspector && (
                  <button
                    title={t('Open inspector')}
                    onClick={() => setLayout((l) => ({ ...l, inspector: true }))}
                  >
                    <PanelRightOpen size={13} />
                  </button>
                )}
              </div>
            </div>
            <div
              ref={grid}
              className="workspace-grid"
              style={
                {
                  '--column-width': `${layout.columnWidth}%`,
                  ...(gridHeight ? { gridTemplateRows: `${rowPixels}px minmax(151px,1fr)` } : {}),
                } as React.CSSProperties
              }
            >
              {layout.order.filter((id) => !layout.hidden.includes(id)).map(renderPanel)}
              <div
                className="column-resizer"
                role="separator"
                aria-label={t('Resize workspace columns')}
                aria-orientation="vertical"
                tabIndex={0}
                style={{ left: `calc(${layout.columnWidth}% + 1px)` }}
                onPointerDown={startResize}
                onKeyDown={(e) => {
                  if (e.key === 'ArrowLeft' || e.key === 'ArrowRight')
                    setLayout((l) => ({
                      ...l,
                      columnWidth: Math.min(
                        75,
                        Math.max(25, l.columnWidth + (e.key === 'ArrowLeft' ? -1 : 1)),
                      ),
                    }));
                }}
              />
              <div
                className="row-resizer"
                role="separator"
                aria-label={t('Resize panel rows')}
                aria-orientation="horizontal"
                tabIndex={0}
                style={{ top: rowPixels ? rowPixels + 1 : '60%' }}
                onPointerDown={startRowResize}
                onKeyDown={(e) => {
                  if (e.key === 'ArrowUp' || e.key === 'ArrowDown')
                    setLayout((l) => ({
                      ...l,
                      rowSplit: Math.max(
                        35,
                        Math.min(80, l.rowSplit + (e.key === 'ArrowUp' ? -2 : 2)),
                      ),
                    }));
                }}
              />
              {layout.hidden.length === Object.keys(PANEL_NAMES).length && (
                <div className="empty-state">
                  {t('All views removed. Use Add view to restore a panel.')}
                </div>
              )}
            </div>
          </>
        )}
        {view === 'Topics' && (
          <div className="full-view topics-view">
            <Panel
              id="topic-browser"
              title={t('TOPIC BROWSER')}
              tag={`${telemetry.topics.length} DISCOVERED`}
            >
              <TopicBrowser />
            </Panel>
            <Panel id="topic-fields" title={t('MESSAGE FIELDS')} tag={t('SELECT TO PLOT')}>
              <FieldBrowser precision={precision} />
            </Panel>
          </div>
        )}
        {view === 'Telemetry' && (
          <div className="full-view analysis-view">
            <Panel
              id="analysis"
              title={t('SIGNAL ANALYSIS')}
              tag={`${telemetry.channels.length} CHANNELS`}
            >
              <TelemetryChart
                channels={telemetry.channels}
                title={t('Selected channels')}
                precision={precision}
              />
            </Panel>
            <ChannelTable precision={precision} />
          </div>
        )}
        {view === 'Controllers' && <ControllerStatus precision={precision} />}
        {view === 'Playback' && (
          <div className="full-view playback-view">
            {telemetry.source === 'file' ? (
              <>
                <div className="recording-bar">
                  <FileClock size={22} />
                  <div>
                    <strong>{telemetry.fileName}</strong>
                    <span>
                      {telemetry.received.toLocaleString()} {t('records ·')}{' '}
                      {(telemetry.end - telemetry.start).toFixed(3)}
                      {t('s · local source')}
                    </span>
                  </div>
                  <button
                    className="button"
                    onClick={() => {
                      telemetry.window = Math.max(0.001, telemetry.end - telemetry.start);
                      telemetry.viewEnd = telemetry.end;
                      telemetry.emit();
                    }}
                  >
                    {t('Fit recording')}
                  </button>
                  <button className="button" onClick={chooseFile}>
                    <FileUp size={12} />
                    {t('Open another')}
                  </button>
                </div>
                <Panel id="recording" title={t('RECORDING ANALYSIS')} tag={t('SOURCE TIME')}>
                  <TelemetryChart
                    channels={telemetry.channels}
                    title={t('Recorded channels')}
                    precision={precision}
                  />
                </Panel>
                <ChannelTable precision={precision} />
              </>
            ) : (
              <div
                className="import-empty"
                onDragOver={(e) => e.preventDefault()}
                onDrop={(e) => {
                  e.preventDefault();
                  const file = e.dataTransfer.files[0];
                  if (file) setImportFile(file);
                }}
              >
                <div className="file-illustration">
                  <FileClock size={34} strokeWidth={1} />
                  <span>t</span>
                </div>
                <span className="eyebrow">{t('HISTORICAL ANALYSIS')}</span>
                <h2>{t('A closer look at every sample.')}</h2>
                <p>
                  {t('Open a recording to inspect signals, compare channels')}
                  <br />
                  {t('and replay robot state on a shared timeline.')}
                </p>
                <button className="button primary" onClick={chooseFile}>
                  <FileUp size={14} />
                  {t('Open recording')}
                </button>
                <div className="format-tags">
                  <span>CSV</span>
                  <span>JSON</span>
                  <span className="muted">{t('≤ 50 MB · LOCAL PROCESSING')}</span>
                </div>
                <div className="playback-note">
                  <span>{t('MCAP SUPPORT')}</span>
                  <p>{t('Planned for a later phase, alongside native DDS ingestion.')}</p>
                </div>
                <a href="/samples/joint-telemetry.csv" download>
                  <ArrowDownToLine size={12} />
                  {t('Download sample CSV')}
                </a>
              </div>
            )}
          </div>
        )}
        {view === 'Diagnostics' && (
          <div className="full-view diagnostics-view">
            <div className="diagnostic-stats">
              <Metric
                label={t('RECEIVED MESSAGES')}
                value={telemetry.received.toLocaleString()}
                foot={t('CURRENT SOURCE SESSION')}
              />
              <Metric
                label={t('DROPPED SAMPLES')}
                value={String(telemetry.dropped)}
                foot={t('GATEWAY QUEUE OVERFLOW')}
              />
              <Metric
                label={t('CHART FRAME TIME')}
                value={telemetry.frameMs.toFixed(2)}
                unit="ms"
                foot={t('LAST CANVAS DRAW')}
              />
              <Metric
                label={t('NUMERIC BUFFER')}
                value={(
                  Array.from(telemetry.buffers.values()).reduce(
                    (n, b) => n + b.times.byteLength + b.values.byteLength,
                    0,
                  ) / 1e6
                ).toFixed(2)}
                unit="MB"
                foot={t('ALLOCATED TYPED ARRAYS')}
              />
            </div>
            <Panel id="events" title={t('EVENT LOG')} tag={t('SESSION')}>
              <div className="table-scroll">
                <table className="data-table log-table">
                  <thead>
                    <tr>
                      <th>{t('TIME / UTC')}</th>
                      <th>{t('LEVEL')}</th>
                      <th>{t('EVENT')}</th>
                    </tr>
                  </thead>
                  <tbody>
                    {[...telemetry.logs].reverse().map((log, i) => (
                      <tr key={`${log.time}-${i}`}>
                        <td className="mono">{new Date(log.time).toISOString().slice(11, 23)}</td>
                        <td>
                          <span className={`log-level ${log.level.toLowerCase()}`}>
                            {log.level}
                          </span>
                        </td>
                        <td title={log.controlEvent?.detail}>
                          {log.controlEvent ? switchEventText(log.controlEvent, t) : t(log.text)}
                          {log.controlEvent && (
                            <small className="event-detail">
                              {t('Current state')}: {t(log.controlEvent.current)} ·{' '}
                              {t(log.controlEvent.detail)}
                            </small>
                          )}
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </Panel>
            <div className="diagnostic-footnote">
              <ShieldCheck size={14} />
              {t(
                'Source and receive timestamps are preserved. Cross-host latency requires synchronized clocks. Hardware execution is not inferred from telemetry.',
              )}
            </div>
          </div>
        )}
        {view === 'Settings' && (
          <div className="full-view settings-view">
            <section>
              <div className="settings-heading">
                <Layers size={18} />
                <div>
                  <h2>{t('Workspace')}</h2>
                  <p>{t('Local preferences for this browser')}</p>
                </div>
              </div>
              <label className="setting-row">
                <span>
                  {t('Workspace name')}
                  <small>{t('Displayed in the global status bar')}</small>
                </span>
                <input
                  value={workspace}
                  maxLength={32}
                  onChange={(e) => {
                    setWorkspace(e.target.value);
                    localStorage.setItem('aerospace.workspace', e.target.value);
                  }}
                />
              </label>
              <label className="setting-row">
                <span>
                  {t('Language')}
                  <small>{t('Applies immediately and is remembered in this browser')}</small>
                </span>
                <select
                  aria-label={t('Interface language')}
                  value={language}
                  onChange={(e) => setLanguage(e.target.value === 'zh-CN' ? 'zh-CN' : 'en')}
                >
                  <option value="en">English</option>
                  <option value="zh-CN">简体中文</option>
                </select>
              </label>
              <label className="setting-row">
                <span>
                  {t('Numeric precision')}
                  <small>{t('Tabular readouts and point inspection')}</small>
                </span>
                <select value={precision} onChange={(e) => setPrecision(Number(e.target.value))}>
                  {[0, 1, 2, 3, 4, 5, 6, 7, 8].map((n) => (
                    <option key={n} value={n}>
                      {t('{count} decimal places', { count: n })}
                    </option>
                  ))}
                </select>
              </label>
              <label className="setting-row">
                <span>
                  {t('Numeric notation')}
                  <small>
                    {t('Large or very small values automatically use scientific notation')}
                  </small>
                </span>
                <select
                  value={telemetry.notation}
                  onChange={(e) => {
                    telemetry.notation = e.target.value as 'fixed' | 'scientific';
                    localStorage.setItem('aerospace.notation', telemetry.notation);
                    telemetry.emit();
                  }}
                >
                  <option value="fixed">{t('Fixed / automatic')}</option>
                  <option value="scientific">{t('Scientific')}</option>
                </select>
              </label>
              <div className="setting-row">
                <span>
                  {t('Workspace layout')}
                  <small>{t('Panel order, visibility and column width')}</small>
                </span>
                <div className="button-group">
                  <button
                    className="button"
                    onClick={() => {
                      saveWorkspace();
                    }}
                  >
                    <Save size={12} />
                    {t('Save')}
                  </button>
                  <button
                    className="button"
                    onClick={() => {
                      setLayout(defaultLayout());
                      notify('Default layout restored');
                    }}
                  >
                    <RotateCcw size={12} />
                    {t('Reset')}
                  </button>
                </div>
              </div>
            </section>
            <section>
              <div className="settings-heading">
                <Globe2 size={18} />
                <div>
                  <h2>{t('Data acquisition')}</h2>
                  <p>{t('Read-only subscriptions through an independent gateway')}</p>
                </div>
              </div>
              <label className="setting-row">
                <span>
                  {t('Gateway endpoint')}
                  <small>{t('WebSocket URL on the ROS2 host')}</small>
                </span>
                <input
                  value={gatewayUrl}
                  onChange={(e) => {
                    setGatewayUrl(e.target.value);
                    localStorage.setItem('aerospace.gateway', e.target.value);
                  }}
                />
              </label>
              <label className="setting-row">
                <span>
                  {t('Subscription reliability')}
                  <small>{t('Auto matches the discovered publishers')}</small>
                </span>
                <select
                  value={telemetry.qos}
                  onChange={(e) => {
                    telemetry.qos = e.target.value;
                    telemetry.syncSubscriptions();
                    telemetry.emit();
                  }}
                >
                  <option value="auto">{t('Auto / compatible')}</option>
                  <option value="best_effort">{t('Best effort')}</option>
                  <option value="reliable">{t('Reliable')}</option>
                </select>
              </label>
              <div className="setting-row">
                <span>
                  {t('Live retention')}
                  <small>{t('Selected numeric channels')}</small>
                </span>
                <code>{t('60 seconds / 32,000 samples per channel')}</code>
              </div>
              <div className="setting-row">
                <span>
                  {t('Display update target')}
                  <small>{t('Independent from source sampling rate')}</small>
                </span>
                <code>{t('30 FPS / Canvas')}</code>
              </div>
            </section>
            <section className="system-info">
              <ShieldCheck size={20} />
              <div>
                <h3>
                  Aerospace Robotics Telemetry Viewer <span>0.1.0</span>
                </h3>
                <p>{t('Engineering workstation · ROS2 gateway v1 · G1 29 DOF')}</p>
                <p>
                  {t('Native DDS and MCAP are scheduled for later phases. Startup instructions:')}{' '}
                  <code>ui/README.md</code>.
                </p>
              </div>
            </section>
          </div>
        )}
      </main>
      {view === 'Controllers' && layout.inspector && (
        <ControllerBindings
          onClose={() => {
            setLayout((l) => ({ ...l, inspector: false }));
            setMobileInspectorOpen(false);
          }}
        />
      )}
      {view !== 'Overview' && view !== 'Controllers' && layout.inspector && (
        <Inspector
          precision={precision}
          onClose={() => {
            setLayout((l) => ({ ...l, inspector: false }));
            setMobileInspectorOpen(false);
          }}
        />
      )}
      {view !== 'Overview' && (
        <div className="timeline-container">
          <Timeline />
        </div>
      )}
      <nav className="mission-navigation" aria-label={t('Workspace navigation')}>
        <div className="navigation-context">
          <span>G1 / SYSTEM</span>
          <strong>{t(telemetry.webActive ? 'WEB CONTROL' : 'READ ONLY')}</strong>
        </div>
        <div className="mission-nav-items">
          {navigation.map(({ name, icon: Icon }, index) => (
            <button
              key={name}
              aria-label={t(name)}
              aria-current={view === name ? 'page' : undefined}
              className={view === name ? 'selected' : ''}
              onClick={() => {
                setView(name);
                setMobileInspectorOpen(false);
              }}
            >
              <Icon size={17} strokeWidth={1.5} />
              <span>{t(name)}</span>
              <small>{String(index + 1).padStart(2, '0')}</small>
            </button>
          ))}
        </div>
        <EventIndicator onOpen={() => setView('Diagnostics')} />
      </nav>
      {!layout.inspector && view !== 'Overview' && view !== 'Workbench' && (
        <button
          className="inspector-reopen icon-button"
          title={t('Open inspector')}
          onClick={() => setLayout((l) => ({ ...l, inspector: true }))}
        >
          <PanelRightOpen size={16} />
        </button>
      )}
      {toast && (
        <div className="toast" role="status">
          <Check size={13} />
          {t(toast)}
        </div>
      )}
      {connectOpen && (
        <div
          className="modal-backdrop"
          onMouseDown={(e) => {
            if (e.target === e.currentTarget) setConnectOpen(false);
          }}
        >
          <section
            className="modal source-modal"
            role="dialog"
            aria-modal="true"
            aria-labelledby="source-title"
          >
            <header>
              <div>
                <span className="eyebrow">{t('ACQUISITION')}</span>
                <h2 id="source-title">{t('Connect a data source')}</h2>
              </div>
              <button
                className="icon-button"
                title={t('Close data sources')}
                onClick={() => setConnectOpen(false)}
              >
                <X size={17} />
              </button>
            </header>
            <div className="modal-body">
              <div className="source-option">
                <div className="source-icon">
                  <Wifi size={20} />
                </div>
                <div>
                  <h3>
                    {t('ROS2 Gateway')}
                    <span>{t('LIVE')}</span>
                  </h3>
                  <p>{t('Discover topics and stream telemetry from your ROS2 network.')}</p>
                </div>
              </div>
              <label className="form-label">
                {t('WEBSOCKET ENDPOINT')}
                <input
                  value={gatewayUrl}
                  onChange={(e) => setGatewayUrl(e.target.value)}
                  placeholder="ws://robot-host:8765/ws"
                />
              </label>
              <div className="source-connect-actions">
                <span>
                  <ShieldCheck size={11} />
                  {t('TELEMETRY CONNECTION')}
                </span>
                {telemetry.source === 'ros2' && (
                  <button
                    className="button"
                    onClick={() => {
                      telemetry.disconnect();
                      setConnectOpen(false);
                    }}
                  >
                    <Unplug size={12} />
                    {t('Disconnect')}
                  </button>
                )}
                <button
                  className="button primary"
                  onClick={() => {
                    localStorage.setItem('aerospace.gateway', gatewayUrl);
                    telemetry.connect(gatewayUrl);
                    setConnectOpen(false);
                  }}
                >
                  <Link2 size={13} />
                  {t('Connect gateway')}
                </button>
              </div>
              <div className="source-divider">{t('OR OPEN A LOCAL SOURCE')}</div>
              <button
                className="source-choice"
                onClick={() => {
                  setConnectOpen(false);
                  startDemo().catch((e) => {
                    telemetry.error = String(e);
                    telemetry.connection = 'ERROR';
                    telemetry.emit();
                  });
                }}
              >
                <div className="source-icon demo-icon">
                  <Play size={18} />
                </div>
                <span>
                  <b>
                    {t('Explore demo workspace')}
                    <em>{t('SIMULATED')}</em>
                  </b>
                  <small>{t('Deterministic G1 telemetry · 500 Hz · no gateway required')}</small>
                </span>
                <ArrowRight size={16} />
              </button>
              <button
                className="source-choice"
                onClick={() => {
                  setConnectOpen(false);
                  chooseFile();
                }}
              >
                <div className="source-icon">
                  <FileUp size={18} />
                </div>
                <span>
                  <b>{t('Open a recording')}</b>
                  <small>{t('CSV / JSON · processed locally · up to 50 MB')}</small>
                </span>
                <ArrowRight size={16} />
              </button>
              <div className="coming-next">
                <Cpu size={13} />
                <span>{t('Native DDS adapter and MCAP import')}</span>
                <b>{t('NEXT PHASE')}</b>
              </div>
            </div>
          </section>
        </div>
      )}
      {importFile && (
        <ImportDialog file={importFile} close={() => setImportFile(null)} loaded={loaded} />
      )}
    </div>
  );
}
function Metric({
  label,
  value,
  unit,
  foot,
  good,
  icon,
}: {
  label: string;
  value: string;
  unit?: string;
  foot: string;
  good?: boolean;
  icon?: React.ReactNode;
}) {
  const { t } = useI18n();
  return (
    <div className="metric">
      <div className="metric-label">
        {t(label)}
        {icon}
      </div>
      <div className={`metric-value ${good ? 'status-good' : ''}`}>
        {t(value)}
        <small>{unit}</small>
      </div>
      <div className="metric-foot">
        {good && <i className="status-dot" />}
        {t(foot)}
      </div>
    </div>
  );
}
function StatusRow({
  label,
  value,
  good,
  warn,
}: {
  label: string;
  value: string;
  good?: boolean;
  warn?: boolean;
}) {
  const { t } = useI18n();
  return (
    <div className="status-row">
      <span>{t(label)}</span>
      <b className={warn ? 'status-warn' : good ? 'status-good' : 'muted'}>
        <i />
        {t(value)}
      </b>
    </div>
  );
}
function ChannelTable({ precision }: { precision: number }) {
  const { t } = useI18n();
  return (
    <div className="channel-table-wrap">
      <table className="data-table channel-table">
        <thead>
          <tr>
            <th>{t('CHANNEL')}</th>
            <th>{t('SOURCE FIELD')}</th>
            <th>{t('VALUE')}</th>
            <th>{t('UNIT')}</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {telemetry.channels.map((c) => (
            <tr key={c.id}>
              <td>
                <label>
                  <input
                    type="checkbox"
                    checked={c.visible}
                    onChange={(e) => {
                      c.visible = e.target.checked;
                      telemetry.emit();
                    }}
                  />
                  <i style={{ background: c.color }} />
                  {t(c.label)}
                </label>
              </td>
              <td className="muted">
                <code>
                  {c.topic} / {c.path}
                </code>
              </td>
              <td className="numeric">
                {formatNumber(
                  telemetry.buffers.get(c.id)?.nearest(telemetry.cursor)?.[1],
                  precision,
                  telemetry.notation,
                )}
              </td>
              <td>
                <input
                  className="unit-input"
                  aria-label={t(`Unit for ${c.path}`)}
                  value={c.unit}
                  onChange={(e) => {
                    c.unit = e.target.value;
                    telemetry.emit();
                  }}
                />
              </td>
              <td>
                <button
                  className="icon-button"
                  title={t(`Remove channel ${c.path}`)}
                  onClick={() => telemetry.removeChannel(c.id)}
                >
                  <X size={12} />
                </button>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      {!telemetry.channels.length && (
        <div className="empty-state">
          <Plus size={16} />
          <span>{t('Add channels from the Topic inspector')}</span>
        </div>
      )}
    </div>
  );
}
