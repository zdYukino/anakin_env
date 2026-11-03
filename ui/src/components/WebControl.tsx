import { useEffect, useRef, useState } from 'react';
import type { PointerEvent as ReactPointerEvent } from 'react';
import { Gamepad2, GripHorizontal, Send, X } from 'lucide-react';
import { useI18n } from '../core/i18n';
import { telemetry } from '../core/telemetry';

const motionActions = new Set(['motion_toggle', 'height_up', 'height_down']);

export function WebControlPanel({ onJoy, joyOpen }: { onJoy: () => void; joyOpen: boolean }) {
  const { t } = useI18n();
  const control = telemetry.control;
  const actions = Object.keys(control?.actions || {});
  const button = (action: string) => (
    <button
      key={action}
      className={`web-action ${telemetry.webActive && control?.actions[action] ? 'available' : ''} ${action === 'emergency_stop' ? 'emergency' : ''}`}
      disabled={!telemetry.webActive || !control?.actions[action]}
      onClick={() => telemetry.command(action)}
      title={t(control?.actions[action] ? 'Send command' : 'Unavailable in the current state')}
    >
      <span>{t(action)}</span>
      <Send size={12} />
    </button>
  );
  return (
    <aside className="mission-joints web-control-panel" aria-label={t('Web commands')}>
      <div className="mission-section-heading">
        <div>
          <span>02 / {t('WEB CONTROL')}</span>
          <h2>{t('STATE COMMANDS')}</h2>
        </div>
        <Send size={15} />
      </div>
      <div className="web-control-summary">
        <span>
          {t('Current state')}
          <b>{t(control?.state || 'AWAITING')}</b>
        </span>
        <span>
          {t('Control source')}
          <b>{t(control?.activeSource || 'none')}</b>
        </span>
        <small>
          {t(control?.mode === 'real' ? 'REAL ROBOT' : 'SIMULATION')} ·{' '}
          {t(telemetry.webActive ? 'Web control active' : 'Activating web control')}
        </small>
      </div>
      <div className="web-state-actions">
        {actions
          .filter((action) => !motionActions.has(action) && action !== 'emergency_stop')
          .map(button)}
      </div>
      <div className="web-motion-actions">
        {actions.filter((action) => motionActions.has(action)).map(button)}
      </div>
      <div className="web-control-motion-state">
        <span>{t(control?.moving ? 'Motion enabled' : 'Motion disabled')}</span>
        <b>
          {control ? control.height.toFixed(2) : '—'} <small>m</small>
        </b>
      </div>
      {actions.includes('emergency_stop') && button('emergency_stop')}
      <button
        className="web-joy-toggle"
        onClick={onJoy}
        disabled={!telemetry.webActive}
        aria-pressed={joyOpen}
      >
        <Gamepad2 size={15} />
        {t(joyOpen ? 'Close virtual Joy' : 'Open virtual Joy')}
      </button>
      <p className="web-control-note">
        {t('Allowed actions follow the running YAML and ENV guards.')}
      </p>
      {telemetry.controlMessage && (
        <p className="web-control-message" role="status">
          {t(telemetry.controlMessage)}
        </p>
      )}
    </aside>
  );
}

export function VirtualJoy({ onClose }: { onClose: () => void }) {
  const { t } = useI18n();
  const [position, setPosition] = useState(() => ({
    x: Math.max(8, window.innerWidth - 440),
    y: Math.max(72, window.innerHeight - 430),
  }));
  const [shoulders, setShoulders] = useState(0);
  const [held, setHeld] = useState<boolean[]>(Array<boolean>(10).fill(false));
  const drag = useRef<{ id: number; x: number; y: number; left: number; top: number } | null>(null);
  const panel = useRef<HTMLElement>(null);
  const clamp = (x: number, y: number) => ({
    x: Math.max(8, Math.min(x, window.innerWidth - (panel.current?.offsetWidth || 420) - 8)),
    y: Math.max(8, Math.min(y, window.innerHeight - (panel.current?.offsetHeight || 380) - 8)),
  });
  useEffect(() => {
    const resize = () => setPosition((p) => clamp(p.x, p.y));
    window.addEventListener('resize', resize);
    return () => {
      window.removeEventListener('resize', resize);
      telemetry.neutralWeb();
    };
  }, []);
  const axis = (offset: 0 | 2, x: number, y: number) => {
    const axes = [...telemetry.webInput.axes] as [number, number, number, number];
    axes[offset] = x;
    axes[offset + 1] = y === 0 ? 0 : offset === 0 ? -y : y;
    telemetry.setWebInput({ ...telemetry.webInput, axes });
  };
  const press = (index: number, value: boolean) => {
    const buttons = [...telemetry.webInput.buttons];
    buttons[index] = value;
    setHeld(buttons);
    telemetry.setWebInput({ ...telemetry.webInput, buttons });
  };
  const pad = (label: string, index: number) => (
    <PadButton
      key={index}
      label={label}
      active={held[index]}
      onPress={(down) => press(index, down)}
    />
  );
  return (
    <aside
      ref={panel}
      className="virtual-joy"
      role="dialog"
      aria-modal="false"
      aria-label={t('Virtual Joy')}
      style={{ left: position.x, top: position.y }}
    >
      <header
        onPointerDown={(event) => {
          if ((event.target as HTMLElement).closest('button') || event.button !== 0) return;
          drag.current = {
            id: event.pointerId,
            x: event.clientX,
            y: event.clientY,
            left: position.x,
            top: position.y,
          };
          event.currentTarget.setPointerCapture?.(event.pointerId);
        }}
        onPointerMove={(event) => {
          const start = drag.current;
          if (start?.id === event.pointerId)
            setPosition(
              clamp(start.left + event.clientX - start.x, start.top + event.clientY - start.y),
            );
        }}
        onPointerUp={() => {
          drag.current = null;
        }}
        onPointerCancel={() => {
          drag.current = null;
        }}
      >
        <Gamepad2 size={16} />
        <strong>{t('Virtual Joy')}</strong>
        <GripHorizontal size={16} />
        <button title={t('Close virtual Joy')} onClick={onClose}>
          <X size={16} />
        </button>
      </header>
      <div className="virtual-joy-source">
        <i />
        {t('web')} → {t(telemetry.control?.activeSource || 'none')}
        <span>{t('Unitree > Joy > Web')}</span>
      </div>
      <div className="virtual-joy-shoulders">
        {[
          ['LB / L1', 1],
          ['RB / R1', 2],
        ].map(([label, mask]) => (
          <button
            key={label}
            aria-pressed={Boolean(shoulders & Number(mask))}
            onClick={() => {
              const next = shoulders ^ Number(mask);
              setShoulders(next);
              telemetry.setWebInput({ ...telemetry.webInput, shoulders: next });
            }}
          >
            {label}
          </button>
        ))}
      </div>
      <div className="virtual-joy-sticks">
        <Stick label={t('Left stick')} onMove={(x, y) => axis(0, x, y)} />
        <Stick label={t('Right stick')} onMove={(x, y) => axis(2, x, y)} />
      </div>
      <div className="virtual-joy-buttons">
        <div className="virtual-dpad">
          {pad('↑', 4)}
          {pad('←', 6)}
          {pad('↓', 5)}
          {pad('→', 7)}
        </div>
        <div className="virtual-face">
          {pad('X', 2)}
          {pad('Y', 3)}
          {pad('A', 0)}
          {pad('B', 1)}
        </div>
      </div>
      <div className="virtual-joy-system">
        {pad('Select', 9)}
        {pad('Start', 8)}
      </div>
      <p>{t('Toggle shoulders for combinations. Release sticks to center.')}</p>
    </aside>
  );
}

function PadButton({
  label,
  active,
  onPress,
}: {
  label: string;
  active: boolean;
  onPress: (down: boolean) => void;
}) {
  const pointer = useRef<number | null>(null);
  const release = () => {
    if (pointer.current !== null) {
      pointer.current = null;
      onPress(false);
    }
  };
  return (
    <button
      aria-label={label}
      aria-pressed={active}
      className={active ? 'held' : ''}
      onPointerDown={(event) => {
        if (event.button !== 0 || pointer.current !== null) return;
        event.preventDefault();
        pointer.current = event.pointerId;
        event.currentTarget.setPointerCapture?.(event.pointerId);
        onPress(true);
      }}
      onPointerUp={release}
      onPointerCancel={release}
      onLostPointerCapture={release}
      onKeyDown={(event) => {
        if ((event.key === ' ' || event.key === 'Enter') && !event.repeat) {
          event.preventDefault();
          onPress(true);
        }
      }}
      onKeyUp={(event) => {
        if (event.key === ' ' || event.key === 'Enter') {
          event.preventDefault();
          onPress(false);
        }
      }}
      onBlur={() => onPress(false)}
    >
      {label}
    </button>
  );
}

function Stick({ label, onMove }: { label: string; onMove: (x: number, y: number) => void }) {
  const pointer = useRef<number | null>(null),
    knob = useRef<HTMLSpanElement>(null);
  const move = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (pointer.current !== event.pointerId) return;
    const rect = event.currentTarget.getBoundingClientRect(),
      radius = Math.max(1, rect.width / 2 - 16);
    let x = (event.clientX - rect.left - rect.width / 2) / radius,
      y = (event.clientY - rect.top - rect.height / 2) / radius;
    const length = Math.max(1, Math.hypot(x, y));
    x /= length;
    y /= length;
    if (knob.current) knob.current.style.transform = `translate(${x * radius}px, ${y * radius}px)`;
    onMove(x, y);
  };
  const release = () => {
    if (pointer.current === null) return;
    pointer.current = null;
    if (knob.current) knob.current.style.transform = 'translate(0, 0)';
    onMove(0, 0);
  };
  return (
    <div className="virtual-stick-group">
      <div
        className="virtual-stick"
        role="group"
        aria-label={label}
        onPointerDown={(event) => {
          if (event.button !== 0 || pointer.current !== null) return;
          event.preventDefault();
          pointer.current = event.pointerId;
          event.currentTarget.setPointerCapture?.(event.pointerId);
          move(event);
        }}
        onPointerMove={move}
        onPointerUp={release}
        onPointerCancel={release}
        onLostPointerCapture={release}
      >
        <i />
        <span ref={knob} />
      </div>
      <small>{label}</small>
    </div>
  );
}
