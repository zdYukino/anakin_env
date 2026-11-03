import { useI18n } from '../core/i18n';
import { useEffect, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { STLLoader } from 'three/addons/loaders/STLLoader.js';
import { RotateCcw, Move3D, Box } from 'lucide-react';
import { numberAt } from '../core/types';
import { telemetry } from '../core/telemetry';
import { findJoints, validGroup } from '../core/robot';

interface ModelNode {
  name: string;
  pos: number[];
  quat: number[];
  joint: { name: string; axis: number[]; pos: number[]; range: number[] } | null;
  geoms: { mesh: string; pos: number[]; quat: number[]; rgba: number[] }[];
  children: ModelNode[];
}
interface Binding {
  group: THREE.Group;
  base: THREE.Quaternion;
  axis: THREE.Vector3;
  pivot: THREE.Vector3;
  origin: THREE.Vector3;
}
export function RobotViewer({
  onJoint,
  variant = 'panel',
}: {
  onJoint?: (name: string) => void;
  variant?: 'panel' | 'hero';
}) {
  const { t } = useI18n();
  const mount = useRef<HTMLDivElement>(null),
    reset = useRef<() => void>(() => {});
  const [status, setStatus] = useState('LOADING MODEL'),
    [error, setError] = useState(''),
    [knownNames, setKnownNames] = useState<string[]>([]);
  useEffect(() => {
    const element = mount.current;
    if (!element) return;
    let renderer: THREE.WebGLRenderer;
    try {
      renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    } catch {
      setError('WebGL unavailable · use a browser with hardware acceleration');
      return;
    }
    let disposed = false,
      frame = 0;
    renderer.setPixelRatio(Math.min(devicePixelRatio, 1.5));
    renderer.setClearColor(0x101419, 0);
    element.appendChild(renderer.domElement);
    const scene = new THREE.Scene();
    const camera = new THREE.PerspectiveCamera(34, 1, 0.01, 100);
    camera.up.set(0, 0, 1);
    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.dampingFactor = 0.12;
    controls.minDistance = 1.3;
    controls.maxDistance = 6;
    controls.maxPolarAngle = Math.PI * 0.83;
    const defaultView = () => {
      if (variant === 'hero') {
        camera.position.set(2.0, -3.1, 1.7);
        controls.target.set(0, 0, 0.9);
      } else {
        camera.position.set(2.7, -3.8, 1.9);
        controls.target.set(0, 0, 0.8);
      }
      controls.update();
    };
    defaultView();
    reset.current = defaultView;
    scene.add(new THREE.HemisphereLight(0xe5f1ff, 0x33404c, 2.5));
    const key = new THREE.DirectionalLight(0xffffff, 3);
    key.position.set(2, -3, 4);
    scene.add(key);
    const rim = new THREE.DirectionalLight(0x8ac5d9, 1.3);
    rim.position.set(-2, 2, 2);
    scene.add(rim);
    const grid = new THREE.GridHelper(
      4,
      20,
      variant === 'hero' ? 0x353535 : 0x35434f,
      variant === 'hero' ? 0x1b1b1b : 0x1c2731,
    );
    grid.rotation.x = Math.PI / 2;
    grid.position.z = -0.005;
    scene.add(grid);
    const ring = new THREE.Mesh(
      new THREE.RingGeometry(0.48, 0.485, 96),
      new THREE.MeshBasicMaterial({
        color: variant === 'hero' ? 0x777777 : 0x54616c,
        transparent: true,
        opacity: 0.6,
        side: THREE.DoubleSide,
      }),
    );
    ring.position.z = 0.001;
    scene.add(ring);
    let base: THREE.Group | undefined;
    const bindings = new Map<string, Binding>(),
      geometry = new Map<string, THREE.BufferGeometry>();
    const meshes: THREE.Mesh[] = [],
      materials: THREE.Material[] = [];
    const loader = new STLLoader();
    const raycaster = new THREE.Raycaster(),
      pointer = new THREE.Vector2();
    let pointerStart = [0, 0];
    const pointerDown = (event: PointerEvent) => {
      pointerStart = [event.clientX, event.clientY];
    };
    const select = (event: PointerEvent) => {
      if (Math.hypot(event.clientX - pointerStart[0], event.clientY - pointerStart[1]) > 4) return;
      if (!onJoint || event.button !== 0) return;
      const rect = renderer.domElement.getBoundingClientRect();
      pointer.set(
        ((event.clientX - rect.left) / rect.width) * 2 - 1,
        (-(event.clientY - rect.top) / rect.height) * 2 + 1,
      );
      raycaster.setFromCamera(pointer, camera);
      const hit = raycaster.intersectObjects(meshes)[0];
      if (hit?.object.userData.joint) onJoint(hit.object.userData.joint);
    };
    renderer.domElement.addEventListener('pointerdown', pointerDown);
    renderer.domElement.addEventListener('pointerup', select);
    const quat = (q: number[]) => new THREE.Quaternion(q[1], q[2], q[3], q[0]);
    const build = async (node: ModelNode, inheritedJoint = ''): Promise<THREE.Group> => {
      if (disposed) return new THREE.Group();
      const group = new THREE.Group();
      group.position.fromArray(node.pos);
      group.quaternion.copy(quat(node.quat));
      const joint = node.joint?.name || inheritedJoint;
      if (node.joint)
        bindings.set(node.joint.name, {
          group,
          base: group.quaternion.clone(),
          axis: new THREE.Vector3().fromArray(node.joint.axis).normalize(),
          pivot: new THREE.Vector3().fromArray(node.joint.pos),
          origin: group.position.clone(),
        });
      for (const geom of node.geoms) {
        let buffer = geometry.get(geom.mesh);
        if (!buffer) {
          buffer = await loader.loadAsync(`/models/g1/${geom.mesh}`);
          if (disposed) {
            buffer.dispose();
            return group;
          }
          buffer.computeVertexNormals();
          geometry.set(geom.mesh, buffer);
        }
        const color = new THREE.Color(geom.rgba[0], geom.rgba[1], geom.rgba[2]);
        const material = new THREE.MeshStandardMaterial({
          color,
          metalness: 0.34,
          roughness: 0.57,
        });
        materials.push(material);
        const mesh = new THREE.Mesh(buffer, material);
        mesh.position.fromArray(geom.pos);
        mesh.quaternion.copy(quat(geom.quat));
        mesh.userData.joint = joint;
        meshes.push(mesh);
        group.add(mesh);
      }
      for (const child of node.children) group.add(await build(child, joint));
      return group;
    };
    fetch('/models/g1/model.json')
      .then((r) => {
        if (!r.ok) throw new Error('Model manifest unavailable');
        return r.json();
      })
      .then(async (data: { root: ModelNode; joint_names: string[] }) => {
        const model = await build(data.root);
        if (disposed) return;
        base = model;
        scene.add(model);
        setKnownNames(data.joint_names);
        setStatus('G1 / 29 DOF');
      })
      .catch((e) => {
        if (!disposed) setError(String(e));
      });
    const observer = new ResizeObserver(() => {
      const { width, height } = element.getBoundingClientRect();
      renderer.setSize(width, height);
      camera.aspect = width / Math.max(1, height);
      camera.updateProjectionMatrix();
    });
    observer.observe(element);
    let last = 0;
    const rotation = new THREE.Quaternion();
    const render = (now: number) => {
      frame = requestAnimationFrame(render);
      if (now - last < 33) return;
      last = now;
      const snapshot = telemetry.at('/anakin/state') || telemetry.at();
      const joints = findJoints(snapshot?.data);
      const angles = new Map(joints?.name.map((name, i) => [name, joints.position[i]]) || []);
      bindings.forEach((binding, name) => {
        const value = angles.get(name),
          angle = value !== undefined && Number.isFinite(value) ? value : 0;
        rotation.setFromAxisAngle(binding.axis, angle);
        binding.group.quaternion.copy(binding.base).multiply(rotation);
        binding.group.position
          .copy(binding.origin)
          .add(binding.pivot.clone().applyQuaternion(binding.base))
          .sub(binding.pivot.clone().applyQuaternion(binding.group.quaternion));
      });
      if (base) {
        const q = ['x', 'y', 'z', 'w'].map((axis) =>
          numberAt(snapshot?.data, `imu.orientation.${axis}`),
        );
        if (
          validGroup(snapshot?.data, 2) &&
          q.every((v) => v !== undefined && Number.isFinite(v)) &&
          q.reduce<number>((n, v) => n + (v ?? 0) ** 2, 0) > 0.1
        )
          base.quaternion.set(q[0]!, q[1]!, q[2]!, q[3]!).normalize();
        else base.quaternion.identity();
        const z = validGroup(snapshot?.data, 16)
          ? numberAt(snapshot?.data, 'odometry.pose.pose.position.z')
          : undefined;
        base.position.z = z !== undefined && Number.isFinite(z) ? z : 0.793;
      }
      controls.update();
      renderer.render(scene, camera);
    };
    frame = requestAnimationFrame(render);
    return () => {
      disposed = true;
      cancelAnimationFrame(frame);
      observer.disconnect();
      controls.dispose();
      renderer.domElement.removeEventListener('pointerdown', pointerDown);
      renderer.domElement.removeEventListener('pointerup', select);
      geometry.forEach((g) => g.dispose());
      materials.forEach((m) => m.dispose());
      grid.geometry.dispose();
      (grid.material as THREE.Material).dispose();
      ring.geometry.dispose();
      (ring.material as THREE.Material).dispose();
      renderer.dispose();
      renderer.domElement.remove();
    };
  }, [onJoint, variant]);
  const joints = findJoints((telemetry.at('/anakin/state') || telemetry.at())?.data);
  return (
    <div className={`robot-viewport ${variant === 'hero' ? 'hero-viewport' : ''}`}>
      <div className="viewport-tags">
        <span>
          <Box size={11} /> {t(status)}
        </span>
        <span className="muted">{t('Z-UP · m')}</span>
      </div>
      <div ref={mount} className="three-mount" />
      {error && <div className="viewport-error">{t(error)}</div>}
      <div className="viewport-bottom">
        <span>
          <Move3D size={12} />
          {t('ORBIT / SCROLL TO ZOOM')}
        </span>
        <button className="icon-button" title={t('Reset camera')} onClick={() => reset.current()}>
          <RotateCcw size={13} />
        </button>
      </div>
      <div className="axis-widget">
        <i className="axis-z">Z</i>
        <i className="axis-y">Y</i>
        <i className="axis-x">X</i>
      </div>
      <div className="model-data-status">
        {joints
          ? t(
              `${joints.name.filter((n) => knownNames.includes(n)).length} / 29 MAPPED${joints.name.some((n) => !knownNames.includes(n)) ? ' · UNKNOWN NAMES' : ''}`,
            )
          : t('AWAITING JOINT DATA')}
      </div>
    </div>
  );
}
