import { useEffect, useState } from 'react';
import { Activity, Check, Circle, Eye, EyeOff, Radio } from 'lucide-react';
import { formatNumber } from '../core/format';
import { useI18n } from '../core/i18n';
import { findJoints, validGroup } from '../core/robot';
import { jsonObject, telemetry } from '../core/telemetry';
import { numberAt } from '../core/types';
import { RobotViewer } from './RobotViewer';
import { WebControlPanel } from './WebControl';

type JointFilter = 'ALL' | 'LOWER' | 'UPPER';
const modeNames = ['DISARMED', 'ALIGNING', 'HOLD', 'ACTIVE', 'DAMPING', 'FAULT', 'PREPARING'];

function jointLabel(name: string) {
  return name
    .replace(/_joint$/, '')
    .replace(/^left_/, 'L / ')
    .replace(/^right_/, 'R / ')
    .replaceAll('_', ' ')
    .toUpperCase();
}

export function MissionOverview({
  onJoint,
  precision,
  onJoy,
  joyOpen = false,
}: {
  onJoint: (name: string) => void;
  precision: number;
  onJoy?: () => void;
  joyOpen?: boolean;
}) {
  const { t } = useI18n();
  const [filter, setFilter] = useState<JointFilter>('ALL');
  const [modelNames, setModelNames] = useState<string[]>([]);
  const [gaugesVisible, setGaugesVisible] = useState(
    () => localStorage.getItem('aerospace.overview.gauges') !== 'hidden',
  );
  useEffect(() => {
    let active = true;
    fetch('/models/g1/model.json')
      .then((response) => (response.ok ? response.json() : null))
      .then((manifest: { joint_names?: unknown } | null) => {
        if (active && Array.isArray(manifest?.joint_names))
          setModelNames(
            manifest.joint_names.filter((name): name is string => typeof name === 'string'),
          );
      })
      .catch(() => {
        // The viewer reports model loading errors; live joint names remain usable here.
      });
    return () => {
      active = false;
    };
  }, []);

  const snapshot = telemetry.at('/anakin/state') || telemetry.at();
  const data = snapshot?.data;
  const root = jsonObject(data);
  const joints = findJoints(data);
  const received =
    joints?.name.filter(
      (name, index) =>
        Number.isFinite(joints.position[index]) &&
        (!modelNames.length || modelNames.includes(name)),
    ).length ?? 0;
  const names = [
    ...modelNames,
    ...(joints?.name.filter((name) => !modelNames.includes(name)) || []),
  ];
  const positionByName = new Map(
    joints?.name.map((name, index) => [name, joints.position[index]]) || [],
  );
  const visibleNames = names.filter((name) => {
    const lower = /hip|knee|ankle/.test(name);
    return filter === 'ALL' || (filter === 'LOWER' ? lower : !lower);
  });
  const height = validGroup(data, 16) ? numberAt(data, 'odometry.pose.pose.position.z') : undefined;
  const gyroAxes = ['x', 'y', 'z'].map((axis) => numberAt(data, `imu.angular_velocity.${axis}`));
  const gyro =
    validGroup(data, 2) && gyroAxes.every((value) => value !== undefined)
      ? Math.hypot(...(gyroAxes as number[]))
      : undefined;
  const execution = numberAt(data, 'execution_mode');
  const connected =
    (telemetry.source === 'demo' || telemetry.source === 'ros2') &&
    telemetry.connection === 'CONNECTED';
  const lastArrival = snapshot && telemetry.snapshotArrival.get(snapshot.topic);
  const stale =
    telemetry.source === 'ros2' &&
    snapshot &&
    (lastArrival === undefined || performance.now() - lastArrival > 2000);
  const robotSnapshot = Boolean(snapshot && (snapshot.topic === '/anakin/state' || joints));
  const lastFrame =
    telemetry.source === 'ros2' &&
    robotSnapshot &&
    (telemetry.connection !== 'CONNECTED' || (telemetry.mode === 'LIVE' && stale));
  const sourceState =
    telemetry.source === 'demo'
      ? telemetry.connection === 'ERROR'
        ? 'DEMO ERROR'
        : 'SIMULATED'
      : telemetry.source === 'file'
        ? 'LOCAL RECORDING'
        : telemetry.connection;
  const frameState =
    telemetry.source === 'demo'
      ? telemetry.connection === 'ERROR'
        ? 'DEMO ERROR'
        : 'SIMULATED DATA'
      : telemetry.source === 'file'
        ? 'OFFLINE RECORDING'
        : lastFrame
          ? 'LAST RECEIVED FRAME'
          : robotSnapshot && telemetry.mode !== 'LIVE'
            ? 'HISTORICAL FRAME'
            : robotSnapshot && telemetry.mode === 'LIVE' && connected
              ? 'LIVE DATA'
              : 'AWAITING DATA';
  const rows = [
    {
      label: 'Data connection',
      detail: sourceState,
      active: connected || telemetry.source === 'file',
    },
    {
      label: 'Execution mode',
      detail: execution === undefined ? 'AWAITING' : modeNames[execution] || 'UNKNOWN',
      active: execution !== undefined,
    },
    {
      label: 'Joint feedback',
      detail: joints ? `${received} / 29` : 'AWAITING',
      active: received > 0,
    },
    {
      label: 'Inertial measurement',
      detail:
        gyro === undefined
          ? 'AWAITING'
          : lastFrame
            ? 'LAST RECEIVED FRAME'
            : telemetry.mode !== 'LIVE'
              ? 'HISTORICAL FRAME'
              : 'RECEIVING',
      active: gyro !== undefined && !lastFrame,
    },
    {
      label: 'Stream integrity',
      detail: !telemetry.received
        ? 'AWAITING'
        : telemetry.dropped
          ? `${telemetry.dropped} DROPPED`
          : 'NO QUEUE DROPS',
      active: telemetry.received > 0 && !telemetry.dropped,
    },
    {
      label: 'Execution authorization',
      detail:
        typeof root?.execution_authorized === 'boolean'
          ? root.execution_authorized
            ? 'AUTHORIZED'
            : 'NOT AUTHORIZED'
          : 'AWAITING',
      active: root?.execution_authorized === true,
    },
  ];
  const value = (n: number | undefined, digits = precision) =>
    formatNumber(n, digits, telemetry.notation);
  return (
    <div className="mission-overview">
      <div className="mission-stage">
        <RobotViewer onJoint={onJoint} variant="hero" />
        <div className="stage-caption" aria-live="polite">
          <span
            className={`stage-live-dot ${connected && robotSnapshot && !stale && telemetry.mode === 'LIVE' ? 'active' : ''}`}
          />
          {t(frameState)}{' '}
          {telemetry.mode !== 'LIVE' && (
            <>
              <i /> {t(telemetry.mode)}
            </>
          )}{' '}
          <i /> {t('DRAG TO ROTATE')}
          {telemetry.mode !== 'LIVE' && telemetry.source !== 'file' && (
            <button onClick={() => telemetry.live()}>{t('Return to live')}</button>
          )}
        </div>
        <div className="stage-gauge-controls">
          <span>{t('ONBOARD INSTRUMENTS')}</span>
          <button
            type="button"
            aria-label={t(gaugesVisible ? 'Hide instruments' : 'Show instruments')}
            aria-pressed={gaugesVisible}
            onClick={() => {
              const next = !gaugesVisible;
              setGaugesVisible(next);
              localStorage.setItem('aerospace.overview.gauges', next ? 'visible' : 'hidden');
            }}
          >
            {gaugesVisible ? <EyeOff size={13} /> : <Eye size={13} />}
            {t(gaugesVisible ? 'Hide' : 'Show')}
          </button>
        </div>
        {gaugesVisible && (
          <div className="mission-gauges">
            <Gauge label={t('BASE HEIGHT')} value={value(height, 2)} unit="m" index="01" />
            <Gauge label={t('ANGULAR SPEED')} value={value(gyro, 2)} unit="rad/s" index="02" />
            <Gauge
              label={t('JOINT FEEDBACK')}
              value={joints ? String(received).padStart(2, '0') : '—'}
              unit="/ 29"
              index="03"
              progress={joints ? Math.min(1, received / 29) : undefined}
            />
          </div>
        )}
      </div>
      <aside className="mission-status" aria-label={t('Main status')}>
        <div className="mission-section-heading">
          <div>
            <span>01 / {t('OVERVIEW')}</span>
            <h2>{t('SYSTEM STATUS')}</h2>
          </div>
          <Activity size={16} />
        </div>
        <div className="status-intro">
          <span>{t('G1 HUMANOID ROBOT')}</span>
          <b>UNITREE / G1</b>
        </div>
        <div className="mission-status-list">
          {rows.map((row, index) => (
            <div className="mission-status-row" key={row.label}>
              <span className={`mission-status-icon ${row.active ? 'active' : ''}`}>
                {row.active ? <Check size={11} /> : <Circle size={7} />}
              </span>
              <div>
                <span>{t(row.label)}</span>
                <strong>{t(row.detail)}</strong>
              </div>
              <small>{String(index + 1).padStart(2, '0')}</small>
            </div>
          ))}
        </div>
        <div className="mission-status-footer">
          <Radio size={12} />
          <span>{t(telemetry.webActive ? 'WEB CONTROL' : 'READ-ONLY TELEMETRY')}</span>
          <b>
            {telemetry.received ? telemetry.hz.toFixed(0) : '—'} <small>Hz</small>
          </b>
        </div>
      </aside>
      {telemetry.webRequested && onJoy ? (
        <WebControlPanel onJoy={onJoy} joyOpen={joyOpen} />
      ) : (
        <aside className="mission-joints" aria-label={t('Real-time joint angles')}>
          <div className="mission-section-heading">
            <div>
              <span>02 / {t('KINEMATICS')}</span>
              <h2>{t('JOINT ANGLES')}</h2>
            </div>
            <span className="joint-heading-unit">DEG</span>
          </div>
          <div className="joint-filter" aria-label={t('Joint group')}>
            {(['ALL', 'LOWER', 'UPPER'] as const).map((group) => (
              <button
                key={group}
                className={filter === group ? 'selected' : ''}
                aria-pressed={filter === group}
                onClick={() => setFilter(group)}
              >
                {t(group)}
              </button>
            ))}
          </div>
          <div className="joint-list-header">
            <span>{t('JOINT')}</span>
            <span>{t('POSITION')} / °</span>
          </div>
          <div className="mission-joint-list">
            {visibleNames.map((name, index) => {
              const radians = positionByName.get(name);
              const degrees =
                radians !== undefined && Number.isFinite(radians)
                  ? (radians * 180) / Math.PI
                  : undefined;
              return (
                <button
                  className="mission-joint-row"
                  key={name}
                  onClick={() => onJoint(name)}
                  disabled={degrees === undefined}
                  title={
                    degrees === undefined ? t('AWAITING JOINT DATA') : t('Select joint to plot')
                  }
                >
                  <span className="joint-index">{String(index + 1).padStart(2, '0')}</span>
                  <span className="joint-name">{t(jointLabel(name))}</span>
                  <strong>
                    {value(degrees, 1)}
                    <small>{degrees === undefined ? '' : '°'}</small>
                  </strong>
                </button>
              );
            })}
            {!visibleNames.length && <div className="joint-empty">{t('AWAITING JOINT DATA')}</div>}
          </div>
          <div className="joint-list-footer">
            <span>{t('NAMED MAPPING')}</span>
            <b>{joints ? `${received} / 29` : '— / 29'}</b>
          </div>
        </aside>
      )}
    </div>
  );
}

function Gauge({
  label,
  value,
  unit,
  index,
  progress,
}: {
  label: string;
  value: string;
  unit: string;
  index: string;
  progress?: number;
}) {
  return (
    <div className="mission-gauge">
      <span className="gauge-index">{index} / G1</span>
      <div className="gauge-dial">
        <svg viewBox="0 0 100 100" aria-hidden="true">
          <circle className="gauge-track" cx="50" cy="50" r="43" />
          <circle className="gauge-ticks" cx="50" cy="50" r="43" />
          {progress !== undefined && (
            <circle
              className="gauge-progress"
              cx="50"
              cy="50"
              r="43"
              pathLength="100"
              strokeDasharray={`${progress * 75} 100`}
            />
          )}
        </svg>
        <div>
          <strong>{value}</strong>
          <small>{unit}</small>
        </div>
      </div>
      <span className="gauge-label">{label}</span>
    </div>
  );
}
