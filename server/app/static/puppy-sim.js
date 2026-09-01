import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

const host = document.querySelector("#puppy-preview");
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x17211d);

const camera = new THREE.PerspectiveCamera(34, 1, 0.1, 100);
camera.position.set(7.4, 6.1, 8.7);
camera.lookAt(0, 1.3, 0);

const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 1.5));
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;
renderer.outputColorSpace = THREE.SRGBColorSpace;
host.appendChild(renderer.domElement);

const controls = new OrbitControls(camera, renderer.domElement);
controls.target.set(0, 1.8, 0);
controls.enableDamping = true;
controls.enablePan = false;
controls.minDistance = 7;
controls.maxDistance = 18;
controls.minPolarAngle = Math.PI * 0.16;
controls.maxPolarAngle = Math.PI * 0.48;

scene.add(new THREE.HemisphereLight(0xe9fff6, 0x18201d, 2.5));
const keyLight = new THREE.DirectionalLight(0xffffff, 3.2);
keyLight.position.set(5, 9, 6);
keyLight.castShadow = true;
scene.add(keyLight);

const orange = new THREE.MeshStandardMaterial({ color: 0xe96f25, roughness: 0.48, metalness: 0.08 });
const dark = new THREE.MeshStandardMaterial({ color: 0x292f35, roughness: 0.62, metalness: 0.12 });
const rearDark = new THREE.MeshStandardMaterial({ color: 0x1d2328, roughness: 0.68, metalness: 0.1 });
const black = new THREE.MeshStandardMaterial({ color: 0x111518, roughness: 0.42 });
const white = new THREE.MeshStandardMaterial({ color: 0xe8edef, roughness: 0.58 });
const faceWhite = new THREE.MeshBasicMaterial({ color: 0xffffff });

function box(parent, size, position, material, rotation = [0, 0, 0]) {
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(...size), material);
  mesh.position.set(...position);
  mesh.rotation.set(...rotation);
  mesh.castShadow = true;
  mesh.receiveShadow = true;
  parent.add(mesh);
  return mesh;
}

const robot = new THREE.Group();
robot.rotation.y = -0.08;
scene.add(robot);

const frontBody = new THREE.Group();
robot.add(frontBody);
box(frontBody, [3.45, 1.3, 1.7], [0, 2.7, 0.82], dark);
box(frontBody, [0.22, 0.72, 0.72], [0, 2.7, -0.08], orange);
box(frontBody, [3.15, 2.4, 1.35], [0, 3.65, 1.48], white, [-0.08, 0, 0]);
box(frontBody, [1.8, 1.55, 0.16], [0, 3.65, 2.2], orange);
box(frontBody, [1.45, 1.2, 0.12], [0, 3.65, 2.31], black);
box(frontBody, [0.36, 0.11, 0.06], [-0.38, 3.86, 2.39], faceWhite, [0, 0, 0.25]);
box(frontBody, [0.36, 0.11, 0.06], [0.38, 3.86, 2.39], faceWhite, [0, 0, -0.25]);
box(frontBody, [0.5, 0.1, 0.06], [0, 3.4, 2.39], faceWhite);
box(frontBody, [0.46, 0.75, 0.48], [-1.02, 4.88, 1.48], white, [0, 0, -0.08]);
box(frontBody, [0.46, 0.75, 0.48], [1.02, 4.88, 1.48], white, [0, 0, 0.08]);
box(frontBody, [0.18, 0.48, 0.08], [-1.02, 4.9, 1.74], orange);
box(frontBody, [0.18, 0.48, 0.08], [1.02, 4.9, 1.74], orange);
box(frontBody, [1.45, 0.26, 0.56], [0, 5.6, 0.76], dark);

const waist = new THREE.Group();
waist.position.set(0, 2.7, -0.08);
robot.add(waist);
box(waist, [3.45, 1.3, 1.7], [0, 0, -0.9], rearDark);
box(waist, [1.35, 0.25, 1.15], [0, 0.75, -0.08], orange);

const legPivots = [];
const legDefinitions = [
  { parent: robot, position: [-1.72, 2.55, 1.28] },
  { parent: robot, position: [1.72, 2.55, 1.28] },
  { parent: waist, position: [-1.72, -0.15, -1.2] },
  { parent: waist, position: [1.72, -0.15, -1.2] },
];

legDefinitions.forEach(({ parent, position: [x, y, z] }, index) => {
  const pivot = new THREE.Group();
  pivot.position.set(x, y, z);
  parent.add(pivot);
  box(pivot, [0.62, 0.62, 0.68], [0, 0, 0], dark);
  box(pivot, [0.54, 2.35, 0.58], [0, -1.08, 0.25], orange, [-0.22, 0, 0]);
  box(pivot, [0.68, 0.72, 0.72], [0, -2.18, 0.52], dark);
  pivot.userData.rear = index > 1;
  legPivots.push(pivot);
});

const floor = new THREE.Mesh(
  new THREE.PlaneGeometry(30, 30),
  new THREE.MeshStandardMaterial({ color: 0x202b27, roughness: 0.9 })
);
floor.rotation.x = -Math.PI / 2;
floor.position.y = -0.3;
floor.receiveShadow = true;
scene.add(floor);

const grid = new THREE.GridHelper(20, 20, 0x53635c, 0x34423c);
grid.position.y = -0.29;
scene.add(grid);

function updatePose(pose) {
  const angles = [pose.angle1, pose.angle2, pose.angle3, pose.angle4];
  legPivots.forEach((pivot, index) => {
    pivot.rotation.x = THREE.MathUtils.degToRad(angles[index] * 0.5);
  });
  waist.rotation.z = THREE.MathUtils.degToRad(-pose.angle5);
}

window.updatePuppySimulation = updatePose;
updatePose({
  angle1: Number(document.querySelector("#motor-angle-1").value),
  angle2: Number(document.querySelector("#motor-angle-2").value),
  angle3: Number(document.querySelector("#motor-angle-3").value),
  angle4: Number(document.querySelector("#motor-angle-4").value),
  angle5: Number(document.querySelector("#motor-angle-5").value),
});

function resize() {
  const width = host.clientWidth;
  const height = host.clientHeight;
  renderer.setSize(width, height, false);
  camera.aspect = width / height;
  camera.updateProjectionMatrix();
}

new ResizeObserver(resize).observe(host);
resize();

function render() {
  controls.update();
  renderer.render(scene, camera);
  requestAnimationFrame(render);
}
render();
