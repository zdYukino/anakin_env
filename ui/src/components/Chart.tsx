import { useI18n } from '../core/i18n';
import { useEffect, useRef, useState } from 'react';
import { Crosshair, Maximize2, Minus, Plus } from 'lucide-react';
import { formatNumber } from '../core/format';
import { telemetry } from '../core/telemetry';
import type { Channel } from '../core/types';

export function TelemetryChart({
  channels,
  title,
  precision,
  onMaximize,
}: {
  channels: Channel[];
  title: string;
  precision: number;
  onMaximize?: () => void;
}) {
  const { t } = useI18n();
  const canvas = useRef<HTMLCanvasElement>(null),
    host = useRef<HTMLDivElement>(null);
  const channelsRef = useRef(channels);
  channelsRef.current = channels;
  const [autoscale, setAutoscale] = useState(true);
  const lockedRange = useRef<[number, number] | null>(null);
  const bounds = useRef({ left: 55, right: 0, width: 1, top: 16, height: 1 });
  useEffect(() => {
    const element = canvas.current,
      wrapper = host.current;
    if (!element || !wrapper) return;
    const ctx = element.getContext('2d');
    if (!ctx) return;
    let width = 0,
      height = 0,
      frame = 0,
      last = 0;
    const resize = new ResizeObserver(() => {
      width = wrapper.clientWidth;
      height = wrapper.clientHeight;
      const dpr = Math.min(devicePixelRatio, 2);
      element.width = width * dpr;
      element.height = height * dpr;
      element.style.width = `${width}px`;
      element.style.height = `${height}px`;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    });
    resize.observe(wrapper);
    const cache: { key: string; series: { c: Channel; points: [number, number][] }[] } = {
      key: '',
      series: [],
    };
    const draw = (now: number) => {
      frame = requestAnimationFrame(draw);
      if (now - last < 33 || !width || !height) return;
      last = now;
      const began = performance.now();
      ctx.clearRect(0, 0, width, height);
      const left = 56,
        right = width - 18,
        top = 18,
        bottom = height - 27,
        plotH = Math.max(1, bottom - top),
        plotW = Math.max(1, right - left);
      bounds.current = { left, right, width: plotW, top, height: plotH };
      const [start, end] = telemetry.range();
      const selected = channelsRef.current.filter((c) => c.visible);
      const cacheKey = `${telemetry.session}|${start}|${end}|${plotW}|${selected
        .map((c) => {
          const b = telemetry.buffers.get(c.id);
          return `${c.id}:${b?.length}:${b?.at(Math.max(0, b.length - 1))[0]}`;
        })
        .join('|')}`;
      if (cache.key !== cacheKey) {
        cache.key = cacheKey;
        cache.series = selected.map((c) => ({
          c,
          points: telemetry.buffers.get(c.id)?.envelope(start, end, plotW) || [],
        }));
      }
      const series = cache.series;
      let min = Infinity,
        max = -Infinity;
      series.forEach((s) =>
        s.points.forEach((p) => {
          min = Math.min(min, p[1]);
          max = Math.max(max, p[1]);
        }),
      );
      if (!Number.isFinite(min)) {
        min = -1;
        max = 1;
      }
      const pad = Math.max((max - min) * 0.18, 0.01);
      min -= pad;
      max += pad;
      if (autoscale || !lockedRange.current) lockedRange.current = [min, max];
      [min, max] = lockedRange.current;
      ctx.font = '10px "IBM Plex Mono", monospace';
      ctx.lineWidth = 1;
      ctx.textBaseline = 'middle';
      for (let i = 0; i <= 4; i++) {
        const y = top + (plotH * i) / 4;
        ctx.strokeStyle = '#242c34';
        ctx.beginPath();
        ctx.moveTo(left, Math.round(y) + 0.5);
        ctx.lineTo(right, Math.round(y) + 0.5);
        ctx.stroke();
        ctx.fillStyle = '#778591';
        ctx.textAlign = 'right';
        ctx.fillText(
          formatNumber(max - ((max - min) * i) / 4, Math.min(3, precision), telemetry.notation),
          left - 11,
          y,
        );
      }
      for (let i = 0; i <= 6; i++) {
        const x = left + (plotW * i) / 6;
        ctx.strokeStyle = '#1d252e';
        ctx.beginPath();
        ctx.moveTo(Math.round(x) + 0.5, top);
        ctx.lineTo(Math.round(x) + 0.5, bottom);
        ctx.stroke();
        ctx.fillStyle = '#778591';
        ctx.textAlign = i === 0 ? 'left' : i === 6 ? 'right' : 'center';
        const t = start + (telemetry.window * i) / 6 - telemetry.origin;
        ctx.fillText(`${t >= 0 ? '+' : ''}${t.toFixed(1)}s`, x, bottom + 17);
      }
      ctx.save();
      ctx.beginPath();
      ctx.rect(left, top, plotW, plotH);
      ctx.clip();
      series.forEach(({ c, points }) => {
        ctx.strokeStyle = c.color;
        ctx.lineWidth = 1.25;
        ctx.lineJoin = 'round';
        ctx.beginPath();
        let previous = -Infinity;
        points.forEach(([t, value]) => {
          const x = left + ((t - start) / (end - start)) * plotW,
            y = top + ((max - value) / (max - min)) * plotH;
          if (t - previous > Math.max(0.15, (telemetry.window / plotW) * 4)) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
          previous = t;
        });
        ctx.stroke();
      });
      if (telemetry.cursor >= start && telemetry.cursor <= end && telemetry.mode !== 'LIVE') {
        const x = left + ((telemetry.cursor - start) / telemetry.window) * plotW;
        ctx.strokeStyle = '#8B98A5';
        ctx.setLineDash([3, 3]);
        ctx.beginPath();
        ctx.moveTo(x, top);
        ctx.lineTo(x, bottom);
        ctx.stroke();
        ctx.setLineDash([]);
        series.forEach(({ c }) => {
          const p = telemetry.buffers.get(c.id)?.nearest(telemetry.cursor);
          if (!p) return;
          const y = top + ((max - p[1]) / (max - min)) * plotH;
          ctx.fillStyle = c.color;
          ctx.beginPath();
          ctx.arc(x, y, 3, 0, Math.PI * 2);
          ctx.fill();
        });
      }
      ctx.restore();
      telemetry.frameMs = performance.now() - began;
    };
    frame = requestAnimationFrame(draw);
    return () => {
      cancelAnimationFrame(frame);
      resize.disconnect();
    };
  }, [autoscale, precision]);
  return (
    <div className="chart-component">
      <div className="chart-toolbar">
        <span className="chart-title">
          {t(title)}{' '}
          <em>
            {[...new Set(channels.map((c) => c.unit).filter(Boolean))].join(' / ') || t('value')}
          </em>
        </span>
        <div className="chart-tools">
          <button
            onClick={() => {
              telemetry.zoom(false);
            }}
            title={t('Zoom out')}
          >
            <Minus size={12} />
          </button>
          <span>{telemetry.window}s</span>
          <button
            onClick={() => {
              telemetry.zoom(true);
            }}
            title={t('Zoom in')}
          >
            <Plus size={12} />
          </button>
          <button
            className={autoscale ? 'active' : ''}
            onClick={() => setAutoscale((v) => !v)}
            title={t('Toggle Y autoscale')}
          >
            {t('AUTO')}
          </button>
          {onMaximize && (
            <button onClick={onMaximize} title={t('Maximize chart')}>
              <Maximize2 size={12} />
            </button>
          )}
        </div>
      </div>
      <div ref={host} className="chart-canvas">
        <canvas
          ref={canvas}
          aria-label={t(`${t(title)} telemetry chart`)}
          onClick={(e) => {
            if (!telemetry.received) return;
            const r = e.currentTarget.getBoundingClientRect(),
              b = bounds.current,
              [start] = telemetry.range();
            const time =
              start +
              Math.max(0, Math.min(1, (e.clientX - r.left - b.left) / b.width)) * telemetry.window;
            let chosen = channels[0],
              distance = Infinity;
            channels
              .filter((c) => c.visible)
              .forEach((c) => {
                const p = telemetry.buffers.get(c.id)?.nearest(time);
                if (!p) return;
                const range = lockedRange.current || [-1, 1];
                const y = b.top + ((range[1] - p[1]) / (range[1] - range[0])) * b.height;
                const d = Math.abs(e.clientY - r.top - y);
                if (d < distance) {
                  chosen = c;
                  distance = d;
                }
              });
            if (chosen) telemetry.selectedChannel = chosen.id;
            telemetry.seek(time);
          }}
        />
        {!channels.length && (
          <div className="chart-empty">
            <Crosshair size={19} />
            <span>{t('Select a numeric field to plot')}</span>
          </div>
        )}
      </div>
      <div className="chart-legend">
        {channels.map((c) => (
          <button
            key={c.id}
            className={c.visible ? '' : 'hidden-series'}
            onClick={() => {
              c.visible = !c.visible;
              telemetry.emit();
            }}
            title={t(`Toggle ${c.path}`)}
          >
            <i style={{ background: c.color }} />
            <span>{t(c.label)}</span>
            <b>
              {formatNumber(
                telemetry.buffers.get(c.id)?.nearest(telemetry.cursor)?.[1],
                precision,
                telemetry.notation,
              )}
            </b>
          </button>
        ))}
      </div>
    </div>
  );
}
