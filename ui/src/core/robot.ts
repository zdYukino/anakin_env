import type { Json } from './types';
import { jsonObject } from './telemetry';
export interface JointData {
  name: string[];
  position: number[];
  velocity: number[];
  effort: number[];
}
export function findJoints(data: Json | undefined): JointData | undefined {
  const root = jsonObject(data);
  if (!root || !validGroup(data, 1)) return;
  const nested = jsonObject(root.joints) || jsonObject(root.joint_state) || root;
  if (
    !Array.isArray(nested.name) ||
    !nested.name.every((n) => typeof n === 'string') ||
    !Array.isArray(nested.position) ||
    new Set(nested.name).size !== nested.name.length
  )
    return;
  return {
    name: nested.name.filter((n): n is string => typeof n === 'string'),
    position: nested.position.map(finite),
    velocity: Array.isArray(nested.velocity) ? nested.velocity.map(finite) : [],
    effort: Array.isArray(nested.effort) ? nested.effort.map(finite) : [],
  };
}

const finite = (value: Json) => (typeof value === 'number' && Number.isFinite(value) ? value : NaN);
export function validGroup(data: Json | undefined, mask: number): boolean {
  const root = jsonObject(data);
  return !root || typeof root.validity !== 'number' || (root.validity & mask) === mask;
}
