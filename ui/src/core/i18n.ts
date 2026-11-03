import { useSyncExternalStore } from 'react';
import zhCN from '../locales/zh-CN.json';

export type Language = 'en' | 'zh-CN';
export const LANGUAGE_KEY = 'aerospace.locale.v1';
const catalog: Readonly<Record<string, string>> = zhCN;
const listeners = new Set<() => void>();

export function readSavedLanguage(): Language {
  try {
    return localStorage.getItem(LANGUAGE_KEY) === 'zh-CN' ? 'zh-CN' : 'en';
  } catch {
    return 'en';
  }
}
let language: Language = readSavedLanguage();
function updateDocument() {
  if (typeof document === 'undefined') return;
  document.documentElement.lang = language === 'zh-CN' ? 'zh-Hans' : 'en';
  document.title =
    language === 'zh-CN' ? 'Aerospace · 机器人遥测' : 'Aerospace · Robotics Telemetry';
}
updateDocument();
export const getLanguage = () => language;
export function setLanguage(next: Language) {
  language = next === 'zh-CN' ? 'zh-CN' : 'en';
  try {
    localStorage.setItem(LANGUAGE_KEY, language);
  } catch {
    /* Session-only when storage is unavailable. */
  }
  updateDocument();
  listeners.forEach((fn) => fn());
}
const escapeRegex = (value: string) => value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
// Logs and worker errors keep their original text; known messages are translated at display time.
const patterns = Object.entries(catalog)
  .filter(([key]) => /\{\w+\}/.test(key))
  .sort(([a], [b]) => b.replace(/\{\w+\}/g, '').length - a.replace(/\{\w+\}/g, '').length)
  .map(([key, value]) => {
    const names = [...key.matchAll(/\{(\w+)\}/g)].map((m) => m[1]);
    return {
      regex: new RegExp(
        '^' +
          key
            .split(/\{\w+\}/)
            .map(escapeRegex)
            .join('(.+?)') +
          '$',
      ),
      names,
      value,
    };
  });
export function t(message: string, values: Record<string, string | number> = {}): string {
  let translated = language === 'zh-CN' ? catalog[message] : message;
  if (language === 'zh-CN' && translated === undefined) {
    for (const pattern of patterns) {
      const match = message.match(pattern.regex);
      if (match) {
        translated = pattern.value.replace(
          /\{(\w+)\}/g,
          (_, key: string) => match[pattern.names.indexOf(key) + 1] || '',
        );
        break;
      }
    }
  }
  return (translated ?? message).replace(/\{(\w+)\}/g, (token, key: string) =>
    Object.hasOwn(values, key) ? String(values[key]) : token,
  );
}
export function useI18n() {
  const current = useSyncExternalStore(
    (fn) => {
      listeners.add(fn);
      return () => {
        listeners.delete(fn);
      };
    },
    getLanguage,
    () => 'en' as Language,
  );
  return { language: current, setLanguage, t };
}
