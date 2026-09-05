import * as THREE from "three";
import type { VisualPartInfo } from "../model";
import connectionHelix1Url from "../assets/wire-patterns/connection/helix_1.glb?url";
import connectionHelix2Url from "../assets/wire-patterns/connection/helix_2.glb?url";
import connectionJitter1Url from "../assets/wire-patterns/connection/jitter_1.glb?url";
import connectionJitter2Url from "../assets/wire-patterns/connection/jitter_2.glb?url";
import connectionStraight1Url from "../assets/wire-patterns/connection/straight_1.glb?url";
import connectionStraight2Url from "../assets/wire-patterns/connection/straight_2.glb?url";
import mainHelix1Url from "../assets/wire-patterns/main/helix_1.glb?url";
import mainHelix2Url from "../assets/wire-patterns/main/helix_2.glb?url";
import mainJitter1Url from "../assets/wire-patterns/main/jitter_1.glb?url";
import mainJitter2Url from "../assets/wire-patterns/main/jitter_2.glb?url";
import mainStraight1Url from "../assets/wire-patterns/main/straight_1.glb?url";
import mainStraight2Url from "../assets/wire-patterns/main/straight_2.glb?url";
import { loadGltfScene } from "./modelAssets";

export type WirePatternSection = "main" | "connection";
export type WirePatternFamily = "straight" | "jitter" | "helix";
export type WirePatternVariant = 1 | 2;

export interface WirePatternAsset {
  points: THREE.Vector3[];
  minX: number;
  maxX: number;
  sourceLength: number;
}

export interface WirePatternPiece {
  curveStart: number;
  curveEnd: number;
  variant: WirePatternVariant;
  flipAroundLongitudinalAxis: boolean;
}

export interface WirePatternCatalog {
  asset(section: WirePatternSection, family: WirePatternFamily, variant: WirePatternVariant): WirePatternAsset;
}

type WirePatternSceneLoader = (url: string) => Promise<THREE.Object3D>;

const wirePatternUrls: Readonly<Record<string, string>> = {
  "connection/helix_1": connectionHelix1Url,
  "connection/helix_2": connectionHelix2Url,
  "connection/jitter_1": connectionJitter1Url,
  "connection/jitter_2": connectionJitter2Url,
  "connection/straight_1": connectionStraight1Url,
  "connection/straight_2": connectionStraight2Url,
  "main/helix_1": mainHelix1Url,
  "main/helix_2": mainHelix2Url,
  "main/jitter_1": mainJitter1Url,
  "main/jitter_2": mainJitter2Url,
  "main/straight_1": mainStraight1Url,
  "main/straight_2": mainStraight2Url
};

function assetKey(section: WirePatternSection, family: WirePatternFamily, variant: WirePatternVariant): string {
  return `${section}/${family}_${variant}`;
}

export function extractLooseEdgeChain(root: THREE.Object3D): WirePatternAsset {
  root.updateMatrixWorld(true);
  const points: THREE.Vector3[] = [];
  const edges: Array<[number, number]> = [];
  root.traverse((object) => {
    if (!(object instanceof THREE.LineSegments)) return;
    const positions = object.geometry.getAttribute("position");
    if (!(positions instanceof THREE.BufferAttribute)) {
      throw new Error("wire pattern LINES primitive has no position attribute");
    }
    const offset = points.length;
    for (let index = 0; index < positions.count; index += 1) {
      points.push(new THREE.Vector3(
        positions.getX(index), positions.getY(index), positions.getZ(index)
      ).applyMatrix4(object.matrixWorld));
    }
    const indices = object.geometry.getIndex();
    const count = indices?.count ?? positions.count;
    if (count % 2 !== 0) throw new Error("wire pattern LINES primitive has an odd index count");
    for (let index = 0; index < count; index += 2) {
      edges.push([offset + (indices?.getX(index) ?? index), offset + (indices?.getX(index + 1) ?? index + 1)]);
    }
  });
  if (edges.length === 0) throw new Error("wire pattern contains no glTF LINES primitive");

  const adjacency = Array.from({ length: points.length }, () => [] as number[]);
  for (const [a, b] of edges) {
    adjacency[a].push(b);
    adjacency[b].push(a);
  }
  const endpoints = adjacency
    .map((neighbors, index) => ({ neighbors, index }))
    .filter(({ neighbors }) => neighbors.length === 1);
  if (endpoints.length !== 2 || adjacency.some((neighbors) => neighbors.length < 1 || neighbors.length > 2)) {
    throw new Error("wire pattern must be one non-branching loose-edge chain");
  }

  const ordered: THREE.Vector3[] = [];
  let previous = -1;
  let current = endpoints[0].index;
  while (current >= 0) {
    ordered.push(points[current]);
    const next = adjacency[current].find((candidate) => candidate !== previous) ?? -1;
    previous = current;
    current = next;
  }
  if (ordered.length !== edges.length + 1) {
    throw new Error("wire pattern LINES primitive contains multiple chains or a cycle");
  }
  if (ordered[0].x > ordered[ordered.length - 1].x) ordered.reverse();
  const minX = Math.min(...ordered.map((point) => point.x));
  const maxX = Math.max(...ordered.map((point) => point.x));
  const sourceLength = maxX - minX;
  if (!(sourceLength > 0)) throw new Error("wire pattern has no positive local X extent");
  return { points: ordered, minX, maxX, sourceLength };
}

export class WirePatternAssetCache implements WirePatternCatalog {
  private readonly loaded = new Map<string, WirePatternAsset>();

  constructor(
    private readonly urls: Readonly<Record<string, string>> = wirePatternUrls,
    private readonly loadScene: WirePatternSceneLoader = loadGltfScene
  ) {}

  async loadAll(): Promise<string[]> {
    this.loaded.clear();
    const failures = (await Promise.all(Object.entries(this.urls).map(async ([key, url]) => {
      try {
        this.loaded.set(key, extractLooseEdgeChain(await this.loadScene(url)));
        return null;
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        return `${key}: ${message}`;
      }
    }))).filter((failure): failure is string => failure !== null);
    for (const section of ["main", "connection"] as const) {
      for (const family of ["straight", "jitter", "helix"] as const) {
        const firstKey = assetKey(section, family, 1);
        const secondKey = assetKey(section, family, 2);
        const first = this.loaded.get(firstKey);
        const second = this.loaded.get(secondKey);
        if (first === undefined || second === undefined) continue;
        if (Math.abs(first.sourceLength - second.sourceLength) > 1e-6) {
          failures.push(`${section}/${family}: wire pattern variants have different source extents`);
          this.loaded.delete(firstKey);
          this.loaded.delete(secondKey);
        }
      }
    }
    return failures;
  }

  asset(section: WirePatternSection, family: WirePatternFamily, variant: WirePatternVariant): WirePatternAsset {
    const key = assetKey(section, family, variant);
    const asset = this.loaded.get(key);
    if (asset === undefined) throw new Error(`wire pattern asset is not loaded: ${key}`);
    return asset;
  }
}

interface CurveTable {
  points: THREE.Vector3[];
  distances: number[];
  total: number;
}

function curveTable(samples: Float64Array): CurveTable {
  const points: THREE.Vector3[] = [];
  const distances: number[] = [];
  let total = 0;
  for (let index = 0; index + 2 < samples.length; index += 3) {
    const point = new THREE.Vector3(samples[index], samples[index + 1], samples[index + 2]);
    if (points.length > 0) total += point.distanceTo(points[points.length - 1]);
    points.push(point);
    distances.push(total);
  }
  if (points.length < 2 || !(total > 0)) throw new Error("wire curve requires two distinct Core samples");
  return { points, distances, total };
}

function sampleCurve(table: CurveTable, requestedDistance: number): { point: THREE.Vector3; tangent: THREE.Vector3 } {
  const distance = THREE.MathUtils.clamp(requestedDistance, 0, table.total);
  let high = 1;
  while (high + 1 < table.distances.length && table.distances[high] < distance) high += 1;
  const low = high - 1;
  const segmentLength = table.distances[high] - table.distances[low];
  const u = segmentLength > 0 ? (distance - table.distances[low]) / segmentLength : 0;
  const tangent = table.points[high].clone().sub(table.points[low]).normalize();
  return { point: table.points[low].clone().lerp(table.points[high], u), tangent };
}

function stableHash(value: string): number {
  let hash = 0x811c9dc5;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 0x01000193);
  }
  return hash >>> 0;
}

export function resolveWirePatternVariation(
  partKey: string,
  family: WirePatternFamily,
  pieceIndex: number
): Pick<WirePatternPiece, "variant" | "flipAroundLongitudinalAxis"> {
  const hash = stableHash(`${partKey}:${family}:${pieceIndex}`);
  return {
    variant: ((hash & 1) + 1) as WirePatternVariant,
    flipAroundLongitudinalAxis: ((hash >>> 1) & 1) !== 0
  };
}

export function planWirePatternPieces(
  curveLength: number,
  sourceLength: number,
  partKey: string,
  family: WirePatternFamily
): WirePatternPiece[] {
  if (!(curveLength > 0) || !(sourceLength > 0)) throw new Error("wire pattern lengths must be positive");
  const count = Math.max(1, Math.ceil(curveLength / sourceLength));
  return Array.from({ length: count }, (_, index) => ({
    curveStart: curveLength * index / count,
    curveEnd: curveLength * (index + 1) / count,
    ...resolveWirePatternVariation(partKey, family, index)
  }));
}

export function wirePatternSection(kind: number): WirePatternSection {
  return kind === 1 || kind === 2 || kind === 3 ? "connection" : "main";
}

export function wirePatternFamily(part: VisualPartInfo): WirePatternFamily {
  return part.supplementalKind === 2 ? "helix" : "straight";
}

export function deformWirePattern(
  asset: WirePatternAsset,
  coreSamples: Float64Array,
  piece: WirePatternPiece
): THREE.Vector3[] {
  const table = curveTable(coreSamples);
  if (piece.curveEnd - piece.curveStart > asset.sourceLength + 1e-9) {
    throw new Error("wire pattern piece would stretch its source asset");
  }
  let previousLateral: THREE.Vector3 | null = null;
  return asset.points.map((authored) => {
    const u = THREE.MathUtils.clamp((authored.x - asset.minX) / asset.sourceLength, 0, 1);
    const sampled = sampleCurve(table, piece.curveStart + (piece.curveEnd - piece.curveStart) * u);
    let lateral = new THREE.Vector3(0, 0, 1).cross(sampled.tangent);
    if (lateral.lengthSq() <= 1e-12) {
      lateral = previousLateral?.clone() ?? new THREE.Vector3(1, 0, 0).cross(sampled.tangent);
    }
    if (lateral.lengthSq() <= 1e-12) lateral = new THREE.Vector3(0, 1, 0);
    lateral.normalize();
    previousLateral = lateral.clone();
    const up = sampled.tangent.clone().cross(lateral).normalize();
    const sign = piece.flipAroundLongitudinalAxis ? -1 : 1;
    return sampled.point
      .addScaledVector(lateral, authored.y * sign)
      .addScaledVector(up, authored.z * sign);
  });
}

export function materializeWirePattern(
  part: VisualPartInfo,
  coreSamples: Float64Array,
  family: WirePatternFamily = wirePatternFamily(part),
  cache: WirePatternCatalog = wirePatternAssetCache
): Float64Array {
  const table = curveTable(coreSamples);
  const section = wirePatternSection(part.kind);
  const sourceLength = cache.asset(section, family, 1).sourceLength;
  const pieces = planWirePatternPieces(table.total, sourceLength, part.partKey, family);
  const result: number[] = [];
  for (const piece of pieces) {
    const deformed = deformWirePattern(cache.asset(section, family, piece.variant), coreSamples, piece);
    for (let index = 0; index < deformed.length; index += 1) {
      const point = deformed[index];
      const previousOffset = result.length - 3;
      if (index === 0 && previousOffset >= 0 &&
          Math.hypot(result[previousOffset] - point.x, result[previousOffset + 1] - point.y,
            result[previousOffset + 2] - point.z) <= 1e-9) {
        continue;
      }
      result.push(point.x, point.y, point.z);
    }
  }
  return new Float64Array(result);
}

export const wirePatternAssetCache = new WirePatternAssetCache();
