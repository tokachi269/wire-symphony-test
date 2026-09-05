import fs from "node:fs/promises";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";
import * as THREE from "three";
import { GLTFLoader } from "three/examples/jsm/loaders/GLTFLoader.js";

import type { VisualPartInfo } from "../src/model";
import {
  deformWirePattern,
  extractLooseEdgeChain,
  materializeWirePattern,
  planWirePatternPieces,
  resolveWirePatternVariation,
  wirePatternFamily,
  type WirePatternAsset,
  WirePatternAssetCache,
  type WirePatternCatalog,
  type WirePatternFamily,
  type WirePatternSection,
  type WirePatternVariant
} from "../src/render/wirePatternAssets";

async function loadAsset(relativePath: string): Promise<WirePatternAsset> {
  const path = fileURLToPath(new URL(`../src/assets/wire-patterns/${relativePath}.glb`, import.meta.url));
  const bytes = await fs.readFile(path);
  const data = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  const gltf = await new GLTFLoader().parseAsync(data, "");
  return extractLooseEdgeChain(gltf.scene);
}

function part(overrides: Partial<VisualPartInfo> = {}): VisualPartInfo {
  return {
    partKey: "0:0:1:2:3:4:5:0",
    sourceVersion: "99",
    sampleOffset: 0,
    kind: 0,
    supplementalKind: 0,
    wireRadius: 0.01,
    materialStyle: 0,
    colorRgba: 0xffffffff,
    sourceNodeId: "1",
    sourceEdgeId: "2",
    sourceSpanId: "3",
    sourceBundleId: "4",
    bundleTemplateId: 5,
    laneIndex: 0,
    runId: 0,
    sampleCount: 3,
    resolvedHelixRadius: 0,
    ...overrides
  };
}

describe("wire pattern assets", () => {
  it("reports invalid assets without rejecting application startup", async () => {
    const cache = new WirePatternAssetCache(
      { "main/helix_1": "invalid.glb" },
      async () => new THREE.Group()
    );

    await expect(cache.loadAll()).resolves.toEqual([
      "main/helix_1: wire pattern contains no glTF LINES primitive"
    ]);
  });

  it("extracts only one non-branching LINES chain and ignores triangle face edges", () => {
    const root = new THREE.Group();
    const lineGeometry = new THREE.BufferGeometry();
    lineGeometry.setAttribute("position", new THREE.Float32BufferAttribute([
      0, 0, 0, 1, 0, 0, 2, 0, 0
    ], 3));
    lineGeometry.setIndex([0, 1, 1, 2]);
    root.add(new THREE.LineSegments(lineGeometry));
    root.add(new THREE.Mesh(new THREE.BoxGeometry(20, 20, 20)));

    const asset = extractLooseEdgeChain(root);
    expect(asset.points.map((point) => point.toArray())).toEqual([
      [0, 0, 0], [1, 0, 0], [2, 0, 0]
    ]);
    expect(asset.sourceLength).toBe(2);
  });

  it("ships seam-compatible assets for every family and serial variant", async () => {
    for (const [section, expectedLength] of [["main", 4], ["connection", 1]] as const) {
      for (const family of ["straight", "jitter", "helix"] as const) {
        const variants = await Promise.all([1, 2].map((variant) =>
          loadAsset(`${section}/${family}_${variant}`)));
        for (const asset of variants) {
          expect(asset.sourceLength).toBeGreaterThan(0);
          expect(asset.points.length).toBeGreaterThanOrEqual(5);
          expect(Math.abs(asset.points[0].y - (asset.points.at(-1)?.y ?? Number.NaN))).toBeLessThan(1e-6);
          expect(Math.abs(asset.points[0].z - (asset.points.at(-1)?.z ?? Number.NaN))).toBeLessThan(1e-6);
          if (family !== "helix") {
            expect(asset.minX).toBeCloseTo(0, 6);
            expect(asset.maxX).toBeCloseTo(expectedLength, 6);
            expect(asset.sourceLength).toBeCloseTo(expectedLength, 6);
          }
        }
        expect(variants[0].sourceLength).toBeCloseTo(variants[1].sourceLength, 7);
      }
    }
  });

  it("keeps shape family separate from serial variant selection", () => {
    expect(wirePatternFamily(part())).toBe("straight");
    expect(wirePatternFamily(part({ supplementalKind: 2, resolvedHelixRadius: 0.2 }))).toBe("helix");
    const straight = resolveWirePatternVariation("part", "straight", 0);
    const jitter = resolveWirePatternVariation("part", "jitter", 0);
    expect(straight.variant === 1 || straight.variant === 2).toBe(true);
    expect(jitter.variant === 1 || jitter.variant === 2).toBe(true);
  });

  it("plans from measured source extent without gaps, overlap, or stretch", () => {
    const pieces = planWirePatternPieces(9, 3.7, "part", "straight");
    expect(pieces).toHaveLength(3);
    expect(pieces[0].curveStart).toBe(0);
    expect(pieces.at(-1)?.curveEnd).toBe(9);
    pieces.forEach((piece, index) => {
      expect(piece.curveEnd - piece.curveStart).toBeLessThanOrEqual(3.7);
      if (index > 0) expect(piece.curveStart).toBe(pieces[index - 1].curveEnd);
    });
    expect(planWirePatternPieces(0.4, 3.7, "part", "straight")).toHaveLength(1);
  });

  it("selects presentation variation deterministically without sourceVersion", () => {
    const first = resolveWirePatternVariation("stable-part", "jitter", 7);
    const repeated = resolveWirePatternVariation("stable-part", "jitter", 7);
    expect(repeated).toEqual(first);
    expect(resolveWirePatternVariation("stable-part", "jitter", 8)).not.toEqual(first);
  });

  it("maps intermediate straight stations onto the Core sag curve", async () => {
    const asset = await loadAsset("main/straight_1");
    const sag = new Float64Array([0, 0, 0, 1.5, 0, -0.5, 3, 0, 0]);
    const sagLength = 2 * Math.sqrt(2.5);
    const deformed = deformWirePattern(asset, sag, {
      curveStart: 0,
      curveEnd: sagLength,
      variant: 1,
      flipAroundLongitudinalAxis: false
    });
    const middle = deformed.reduce((best, point) => Math.abs(point.x - 1.5) < Math.abs(best.x - 1.5) ? point : best);
    expect(middle.x).toBeCloseTo(1.5, 6);
    expect(middle.z).toBeCloseTo(-0.5, 6);
  });

  it("rotates around local X without exchanging piece endpoints", () => {
    const asset: WirePatternAsset = {
      points: [new THREE.Vector3(0, 0, 0), new THREE.Vector3(1, 0.2, 0.3), new THREE.Vector3(2, 0, 0)],
      minX: 0,
      maxX: 2,
      sourceLength: 2
    };
    const curve = new Float64Array([0, 0, 0, 2, 0, 0]);
    const normal = deformWirePattern(asset, curve, {
      curveStart: 0, curveEnd: 2, variant: 1, flipAroundLongitudinalAxis: false
    });
    const flipped = deformWirePattern(asset, curve, {
      curveStart: 0, curveEnd: 2, variant: 1, flipAroundLongitudinalAxis: true
    });
    expect(flipped[0].toArray()).toEqual(normal[0].toArray());
    expect(flipped.at(-1)?.toArray()).toEqual(normal.at(-1)?.toArray());
    expect(flipped[1].x).toBeCloseTo(normal[1].x, 7);
    expect(flipped[1].y).toBeCloseTo(-normal[1].y, 7);
    expect(flipped[1].z).toBeCloseTo(-normal[1].z, 7);
  });

  it("joins adjacent pieces and preserves authored helix offsets", async () => {
    const assets = new Map<string, WirePatternAsset>();
    for (const family of ["straight", "helix"] as const) {
      for (const variant of [1, 2] as const) {
        assets.set(`main/${family}_${variant}`, await loadAsset(`main/${family}_${variant}`));
      }
    }
    const catalog: WirePatternCatalog = {
      asset(section: WirePatternSection, family: WirePatternFamily, variant: WirePatternVariant) {
        const asset = assets.get(`${section}/${family}_${variant}`);
        if (asset === undefined) throw new Error("missing synthetic catalog asset");
        return asset;
      }
    };
    const curve = new Float64Array([0, 0, 0, 4, 0, -1, 8, 0, 0]);
    const straight = materializeWirePattern(part(), curve, "straight", catalog);
    expect(straight[0]).toBeCloseTo(0, 7);
    expect(straight.at(-3)).toBeCloseTo(8, 7);

    const authoredRadius = 0.025;
    const authoredHelix: WirePatternAsset = {
      points: [
        new THREE.Vector3(0, authoredRadius, 0),
        new THREE.Vector3(1, 0, authoredRadius),
        new THREE.Vector3(2, authoredRadius, 0)
      ],
      minX: 0,
      maxX: 2,
      sourceLength: 2
    };
    assets.set("main/helix_1", authoredHelix);
    assets.set("main/helix_2", authoredHelix);
    const straightAxis = new Float64Array([0, 0, 0, 8, 0, 0]);
    const helix = materializeWirePattern(
      part({ supplementalKind: 2, resolvedHelixRadius: 0.18 }), straightAxis, "helix", catalog);
    let maxOffset = 0;
    for (let index = 0; index + 2 < helix.length; index += 3) {
      maxOffset = Math.max(maxOffset, Math.hypot(helix[index + 1], helix[index + 2]));
    }
    expect(maxOffset).toBeCloseTo(authoredRadius, 6);
  });
});
