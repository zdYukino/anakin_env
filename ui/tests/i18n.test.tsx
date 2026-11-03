// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import App from '../src/App';
import { LANGUAGE_KEY, getLanguage, readSavedLanguage, setLanguage, t } from '../src/core/i18n';
import { telemetry } from '../src/core/telemetry';
vi.mock('../src/components/RobotViewer', () => ({ RobotViewer: () => <div>Renderer</div> }));
vi.mock('../src/components/Chart', () => ({ TelemetryChart: () => <div>Chart</div> }));
beforeEach(() => {
  localStorage.clear();
  setLanguage('en');
  telemetry.reset('none');
});
afterEach(() => {
  cleanup();
  telemetry.dispose();
  setLanguage('en');
});
it('switches all main views from Settings without reconnecting or clearing numeric data', () => {
  telemetry.source = 'demo';
  telemetry.addChannel('/anakin/state', 'joints.position[0]');
  telemetry.ingest({ topic: '/anakin/state', t: 100, values: { 'joints.position[0]': 1.25 } });
  telemetry.snapshot({
    topic: '/anakin/state',
    t: 100,
    data: { joints: { name: ['left_hip_pitch_joint'], position: [1.25] } },
  });
  const session = telemetry.session;
  render(<App />);
  fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
  fireEvent.change(screen.getByRole('combobox', { name: 'Interface language' }), {
    target: { value: 'zh-CN' },
  });
  expect(screen.getByRole('heading', { name: '设置' })).toBeTruthy();
  expect(screen.getByText('数值精度')).toBeTruthy();
  expect(screen.getByRole('button', { name: '历史回放' })).toBeTruthy();
  expect(document.documentElement.lang).toBe('zh-Hans');
  expect(localStorage.getItem(LANGUAGE_KEY)).toBe('zh-CN');
  expect(readSavedLanguage()).toBe('zh-CN');
  expect(telemetry.session).toBe(session);
  expect(telemetry.received).toBe(1);
  expect(telemetry.buffers.get('/anakin/state::joints.position[0]')?.at(0)).toEqual([100, 1.25]);
  fireEvent.click(screen.getByRole('button', { name: '总览' }));
  expect(screen.getByRole('heading', { name: '机器人总览' })).toBeTruthy();
  expect(screen.getByText('关节角度')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: '工作台' }));
  fireEvent.click(screen.getByRole('button', { name: '工作台' }));
  expect(screen.getByText('/anakin/state')).toBeTruthy();
  expect(screen.getByRole('button', { name: '移除关节遥测' })).toBeTruthy();
  expect(screen.queryByRole('button', { name: '移除机器人状态' })).toBeNull();
  fireEvent.click(screen.getByRole('button', { name: '遥控器状态' }));
  expect(screen.getByRole('heading', { name: '遥控器状态' })).toBeTruthy();
  expect(screen.getByRole('complementary', { name: '状态切换方式' })).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: '系统诊断' }));
  expect(screen.getByText('事件日志')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: '设置' }));
  fireEvent.change(screen.getByRole('combobox', { name: '界面语言' }), { target: { value: 'en' } });
  expect(screen.getByRole('heading', { name: 'Settings' })).toBeTruthy();
  expect(document.documentElement.lang).toBe('en');
  expect(telemetry.session).toBe(session);
});
it('keeps the chosen language across remounts and displays localized offline controls', () => {
  setLanguage('zh-CN');
  const first = render(<App />);
  first.unmount();
  render(<App />);
  expect(getLanguage()).toBe('zh-CN');
  fireEvent.click(screen.getByRole('button', { name: '历史回放' }));
  expect(screen.getByText('细查每一个采样点。')).toBeTruthy();
  expect(screen.getByRole('button', { name: '打开记录' })).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: '连接数据源' }));
  expect(screen.getByRole('dialog', { name: '连接数据源' })).toBeTruthy();
  expect(screen.getByRole('button', { name: /体验演示工作区/ })).toBeTruthy();
});
it('translates known parameterized messages while preserving protocol fields and raw records', () => {
  setLanguage('zh-CN');
  expect(t('Remove channel joints.position[0]')).toBe('移除通道 joints.position[0]');
  expect(t('CSV row 2: expected 3 columns, received 1')).toContain('CSV 第 2 行');
  expect(t('Connecting to ws://robot:8765/ws')).toBe('正在连接 ws://robot:8765/ws');
  expect(t('/anakin/state')).toBe('/anakin/state');
  expect(t('anakin_sdk/msg/RobotStateFrame')).toBe('anakin_sdk/msg/RobotStateFrame');
  expect(t('rad/s')).toBe('rad/s');
  expect(t('CONNECTED')).toContain('CONNECTED');
  expect(t('{count} decimal places', { count: 3 })).toBe('3 位小数');
  const log = { text: 'Workspace initialized · read-only' };
  expect(t(log.text)).toBe('工作区已初始化 · 只读');
  expect(log.text).toBe('Workspace initialized · read-only');
  localStorage.setItem(LANGUAGE_KEY, 'unsupported');
  expect(readSavedLanguage()).toBe('en');
});
