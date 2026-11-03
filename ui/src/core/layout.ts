export interface Layout {
  v: 1;
  order: string[];
  hidden: string[];
  collapsed: string[];
  robotWidth: number;
  rowSplit: number;
  navCollapsed: boolean;
  inspector: boolean;
}
export const PANEL_NAMES: Record<string, string> = {
  robot: 'ROBOT STATE',
  trend: 'JOINT TELEMETRY',
  status: 'SYSTEM STATUS',
  imu: 'INERTIAL MEASUREMENT',
  topics: 'DATA SOURCES',
};
export const defaultLayout = (): Layout => ({
  v: 1,
  order: ['robot', 'trend', 'status', 'imu', 'topics'],
  hidden: [],
  collapsed: [],
  robotWidth: 30,
  rowSplit: 60,
  navCollapsed: false,
  inspector: true,
});
export function loadLayout(): Layout {
  try {
    const value = JSON.parse(localStorage.getItem('aerospace.layout.v1') || 'null');
    if (value?.v !== 1 || !Array.isArray(value.order)) return defaultLayout();
    const order = [
      ...new Set<string>(
        value.order.filter(
          (id: unknown): id is string =>
            typeof id === 'string' && typeof id === 'string' && Object.hasOwn(PANEL_NAMES, id),
        ),
      ),
    ];
    const defaults = defaultLayout();
    return {
      ...defaults,
      order: [...order, ...defaults.order.filter((id) => !order.includes(id))],
      hidden: Array.isArray(value.hidden)
        ? value.hidden.filter(
            (id: string) => typeof id === 'string' && Object.hasOwn(PANEL_NAMES, id),
          )
        : [],
      collapsed: Array.isArray(value.collapsed)
        ? value.collapsed.filter(
            (id: string) => typeof id === 'string' && Object.hasOwn(PANEL_NAMES, id),
          )
        : [],
      robotWidth: Math.max(23, Math.min(48, Number(value.robotWidth) || 30)),
      rowSplit: Math.max(35, Math.min(80, Number(value.rowSplit) || 60)),
      navCollapsed: Boolean(value.navCollapsed),
      inspector: value.inspector !== false,
    };
  } catch {
    return defaultLayout();
  }
}
