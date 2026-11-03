import { useI18n } from '../core/i18n';
import type { ReactNode } from 'react';
import { ChevronDown, ChevronRight, GripVertical, Maximize2, Minimize2, X } from 'lucide-react';
export function Panel({
  id,
  title,
  tag,
  children,
  collapsed,
  maximized,
  onToggle,
  onMaximize,
  onRemove,
  onReorder,
  className = '',
  action,
}: {
  id: string;
  title: string;
  tag?: string;
  children: ReactNode;
  collapsed?: boolean;
  maximized?: boolean;
  onToggle?: () => void;
  onMaximize?: () => void;
  onRemove?: () => void;
  onReorder?: (from: string, to: string) => void;
  className?: string;
  action?: ReactNode;
}) {
  const { t } = useI18n();
  return (
    <section
      className={`panel ${collapsed ? 'panel-collapsed' : ''} ${maximized ? 'panel-maximized' : ''} ${className}`}
      onDragOver={(e) => e.preventDefault()}
      onDrop={(e) => {
        e.preventDefault();
        const from = e.dataTransfer.getData('text/panel');
        if (from) onReorder?.(from, id);
      }}
    >
      <header className="panel-header">
        <div className="panel-heading">
          {onReorder && (
            <span
              className="panel-grip"
              draggable
              onDragStart={(e) => e.dataTransfer.setData('text/panel', id)}
              title={t('Drag to reorder panel')}
            >
              <GripVertical size={13} />
            </span>
          )}
          {onToggle && (
            <button
              className="panel-collapse"
              onClick={onToggle}
              title={t(`${collapsed ? 'Expand' : 'Collapse'} ${t(title)}`)}
            >
              {collapsed ? <ChevronRight size={12} /> : <ChevronDown size={12} />}
            </button>
          )}
          <h2>{t(title)}</h2>
          {tag && <span className="panel-tag">{t(tag)}</span>}
        </div>
        <div className="panel-actions">
          {action}
          {onMaximize && (
            <button
              title={t(`${maximized ? 'Restore' : 'Maximize'} ${t(title)}`)}
              onClick={onMaximize}
            >
              {maximized ? <Minimize2 size={12} /> : <Maximize2 size={12} />}
            </button>
          )}
          {onRemove && (
            <button title={t(`Remove ${t(title)}`)} onClick={onRemove}>
              <X size={12} />
            </button>
          )}
        </div>
      </header>
      {!collapsed && <div className="panel-content">{children}</div>}
    </section>
  );
}
