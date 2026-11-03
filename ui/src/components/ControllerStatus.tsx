import { Gamepad2, PanelRightClose, Radio, ShieldCheck } from 'lucide-react';
import { formatNumber } from '../core/format';
import { useI18n } from '../core/i18n';
import { controllerInput, INPUT_TOPIC } from '../core/inputs';
import type { ControllerSource, InputBinding } from '../core/inputs';
import { telemetry } from '../core/telemetry';

function Stick({ x, y, label }: { x?: number; y?: number; label: string }) {
  return (
    <div className="controller-stick">
      <svg viewBox="0 0 100 100" role="img" aria-label={label}>
        <circle cx="50" cy="50" r="40" className="stick-ring" />
        <circle cx="50" cy="50" r="20" className="stick-inner" />
        <path d="M10 50H90M50 10V90" className="stick-cross" />
        {x !== undefined && y !== undefined && (
          <>
            <path d={`M50 50L${50 + x * 34} ${50 - y * 34}`} className="stick-vector" />
            <circle cx={50 + x * 34} cy={50 - y * 34} r="6" className="stick-position" />
          </>
        )}
      </svg>
      <span>{label}</span>
    </div>
  );
}

function ControllerCard({ source, precision }: { source: ControllerSource; precision: number }) {
  const { t } = useI18n();
  const snapshot = telemetry.at(INPUT_TOPIC);
  const input = controllerInput(snapshot?.data, source);
  const config = telemetry.inputConfig;
  const arrival = telemetry.snapshotArrival.get(INPUT_TOPIC);
  const elapsed = arrival === undefined ? Infinity : Math.max(0, performance.now() - arrival);
  const historical = telemetry.mode !== 'LIVE' || telemetry.source === 'file';
  const lastFrame =
    telemetry.source === 'ros2' &&
    !historical &&
    snapshot &&
    (!['CONNECTED', 'DEGRADED'].includes(telemetry.connection) || elapsed > 500);
  const age = input?.ageMs === undefined ? undefined : input.ageMs + (historical ? 0 : elapsed);
  const status = !input
    ? 'NO INPUT DATA'
    : !input.enabled
      ? 'DISABLED'
      : lastFrame
        ? 'LAST RECEIVED FRAME'
        : !input.connected
          ? 'DISCONNECTED'
          : !input.online || (!historical && config && age !== undefined && age > config.timeoutMs)
            ? 'STALE'
            : 'LIVE';
  const usable = Boolean(input?.online && (historical || status === 'LIVE'));
  const axes = usable ? input?.axes : undefined;
  const name = source === 'gamepad' ? 'Joy gamepad' : 'Unitree remote';
  const Icon = source === 'gamepad' ? Gamepad2 : Radio;
  return (
    <section className={`controller-card ${usable ? 'input-active' : ''}`} aria-label={t(name)}>
      <header>
        <div>
          <Icon size={20} />
          <div>
            <span>{source === 'gamepad' ? 'JOY / SDL' : 'UNITREE / RC'}</span>
            <h2>{t(name)}</h2>
          </div>
        </div>
        <span className={`input-badge ${status === 'LIVE' && !historical ? 'live' : ''}`}>
          {t(historical && input ? 'HISTORICAL FRAME' : status)}
        </span>
      </header>
      <div className="controller-facts">
        <span>{t(input?.primary ? 'PRIMARY INPUT' : 'MONITOR INPUT')}</span>
        <code>
          {config
            ? source === 'gamepad'
              ? `device ${config.deviceIndex}`
              : config.remoteChannel
            : '—'}
        </code>
      </div>
      <div className="controller-sticks">
        <Stick label={t('LEFT STICK')} x={axes?.left_x} y={axes?.left_y} />
        <Stick
          label={t('RIGHT STICK')}
          x={axes?.right_x}
          y={
            axes?.right_y === undefined
              ? undefined
              : source === 'gamepad'
                ? -axes.right_y
                : axes.right_y
          }
        />
      </div>
      <dl className="controller-axes">
        {['left_x', 'left_y', 'right_x', 'right_y'].map((axis) => (
          <div key={axis}>
            <dt>{axis}</dt>
            <dd>{formatNumber(axes?.[axis], precision, telemetry.notation)}</dd>
          </div>
        ))}
      </dl>
      {source === 'gamepad' && (
        <div className="controller-triggers" aria-label={t('Analog triggers')}>
          {['LT', 'RT'].map((trigger) => {
            const value = usable ? input?.triggers[trigger] : undefined;
            return (
              <div key={trigger}>
                <span>{trigger}</span>
                <div
                  className="trigger-track"
                  role="meter"
                  aria-label={trigger}
                  aria-valuemin={0}
                  aria-valuemax={1}
                  aria-valuenow={value}
                  aria-valuetext={
                    value === undefined ? t('UNAVAILABLE') : `${Math.round(value * 100)}%`
                  }
                >
                  <i style={{ width: `${(value ?? 0) * 100}%` }} />
                </div>
                <code>{value === undefined ? '—' : `${Math.round(value * 100)}%`}</code>
              </div>
            );
          })}
        </div>
      )}
      <div className="controller-buttons" aria-label={t('Controller buttons')}>
        {['L1', 'R1', 'A', 'B', 'X', 'Y', 'Up', 'Down', 'Left', 'Right', 'Start', 'Select'].map(
          (button) => {
            const label =
              source === 'gamepad'
                ? button === 'L1'
                  ? 'LB'
                  : button === 'R1'
                    ? 'RB'
                    : button
                : button;
            const pressed = usable && input?.buttons[button] === true;
            const known = usable && input?.buttons[button] !== undefined;
            return (
              <span
                key={button}
                className={`${pressed ? 'pressed' : ''} ${known ? '' : 'unavailable'}`}
                aria-label={`${label}: ${t(!known ? 'UNAVAILABLE' : pressed ? 'PRESSED' : 'RELEASED')}`}
              >
                {label}
              </span>
            );
          },
        )}
      </div>
      {source === 'gamepad' && input?.device && (
        <details className="controller-device">
          <summary>{input.device.name}</summary>
          <code>{input.device.guid}</code>
          <code>{input.device.mapping}</code>
        </details>
      )}
      <footer>
        <span>{t('INPUT AGE')}</span>
        <code>{input && !lastFrame && Number.isFinite(age) ? `${age!.toFixed(0)} ms` : '—'}</code>
      </footer>
    </section>
  );
}

export function ControllerStatus({ precision }: { precision: number }) {
  const { t } = useI18n();
  return (
    <div className="controller-view">
      <div className="controller-notice">
        <ShieldCheck size={13} />
        {t('Live input monitoring')}
        <span>{t('READ ONLY')}</span>
      </div>
      <div className="controller-grid">
        <ControllerCard source="gamepad" precision={precision} />
        <ControllerCard source="remote" precision={precision} />
      </div>
      <p className="controller-hint">
        {t('Controller axes and buttons reflect the normalized input received by env.')}
      </p>
    </div>
  );
}

function BindingList({ bindings }: { bindings: InputBinding[] }) {
  const { t } = useI18n();
  return (
    <ul className="input-binding-list">
      {bindings.map((binding, index) => (
        <li key={index}>
          <div className="binding-combination">
            {(Array.isArray(binding.shoulders)
              ? [...binding.shoulders, binding.control]
              : [binding.control]
            ).map((key, i) => (
              <span key={i}>
                {i > 0 && <small>+</small>}
                <kbd>{key === 'L1' ? 'LB / L1' : key === 'R1' ? 'RB / R1' : key}</kbd>
              </span>
            ))}
            <em>{t(binding.trigger)}</em>
          </div>
          <div className="binding-action">
            <strong>{t(binding.action)}</strong>
            {
              <span>
                {t(
                  binding.shoulders === 'any'
                    ? 'ANY SHOULDERS'
                    : binding.shoulders.length
                      ? 'EXACT COMBINATION'
                      : 'NO SHOULDERS',
                )}
              </span>
            }
          </div>
        </li>
      ))}
    </ul>
  );
}

export function ControllerBindings({ onClose }: { onClose: () => void }) {
  const { t } = useI18n();
  const config = telemetry.inputConfig;
  return (
    <aside className="inspector input-bindings" aria-label={t('State switching guide')}>
      <header>
        <span>{t('STATE SWITCHING')}</span>
        <button className="icon-button" onClick={onClose} title={t('Collapse inspector')}>
          <PanelRightClose size={14} />
        </button>
      </header>
      {!config ? (
        <div className="input-config-empty">
          <Radio size={22} />
          <strong>{t('Runtime input configuration unavailable')}</strong>
          <p>{t('Waiting for configuration from the running env node.')}</p>
          {telemetry.inputConfigMessage && <code>{telemetry.inputConfigMessage}</code>}
        </div>
      ) : (
        <>
          <div className="input-config-source">
            <span className="section-label">{t('RUNNING YAML')}</span>
            <code title={config.path}>{config.path}</code>
            <dl>
              <dt>{t('PRIMARY INPUT')}</dt>
              <dd>{config.primary}</dd>
              <dt>{t('Input timeout')}</dt>
              <dd>{config.timeoutMs} ms</dd>
            </dl>
            <p>
              {t(
                'Ordinary actions use the primary input. Emergency stops use every enabled input.',
              )}
            </p>
            <details>
              <summary>{t('Configuration details')}</summary>
              <code>{config.node}</code>
              <code>{config.modePath}</code>
              <span>
                {t('Enabled inputs')}: {config.enabledSources.join(', ')}
              </span>
            </details>
          </div>
          <section className="input-binding-section">
            <h3>{t('CONTROLLER BINDINGS')}</h3>
            <p>{t('Shared by Joy and Unitree remote')}</p>
            <p>{t('Joy: LB/RB = L1/R1. LT/RT are independent analog triggers.')}</p>
            <BindingList bindings={config.controllerBindings} />
          </section>
          <section className="input-binding-section">
            <h3>{t('WEB ACTIONS')}</h3>
            <p>{config.webActions.map((action) => t(action)).join(' · ')}</p>
          </section>
          <section className="input-axis-config">
            <h3>{t('MOTION AXES')}</h3>
            <span>
              {t('Deadzone')}: {config.deadzone}
            </span>
            {config.velocityAxes.map((binding) => (
              <div key={binding.component}>
                <span>{t(binding.component)}</span>
                <code>
                  {binding.axis} × {binding.scale}
                </code>
              </div>
            ))}
          </section>
          <p className="input-config-note">
            {t('Bindings reflect the loaded configuration. File edits apply after env restarts.')}
          </p>
        </>
      )}
    </aside>
  );
}
