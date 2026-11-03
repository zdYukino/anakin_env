import { numberAt, type Json } from './types';
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

export interface RobotPose {
  angles: Map<string, number>;
  orientation?: [number, number, number, number];
  height?: number;
}

// Retain only valid values for live rendering when a sensor group briefly drops out.
export function robotPose(data: Json | undefined, previous?: RobotPose): RobotPose {
  const angles = new Map(previous?.angles);
  const joints = findJoints(data);
  joints?.name.forEach((name, index) => {
    if (Number.isFinite(joints.position[index])) angles.set(name, joints.position[index]);
  });
  const q = ['x', 'y', 'z', 'w'].map((axis) => numberAt(data, `imu.orientation.${axis}`));
  const orientation =
    validGroup(data, 2) &&
    q.every((v) => v !== undefined && Number.isFinite(v)) &&
    q.reduce<number>((sum, v) => sum + (v ?? 0) ** 2, 0) > 0.1
      ? (q as [number, number, number, number])
      : previous?.orientation;
  const z = validGroup(data, 16) ? numberAt(data, 'odometry.pose.pose.position.z') : undefined;
  return {
    angles,
    orientation,
    height: z !== undefined && Number.isFinite(z) ? z : previous?.height,
  };
}
