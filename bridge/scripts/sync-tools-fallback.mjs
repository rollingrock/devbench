#!/usr/bin/env node
// Regenerates a game family's offline tool list from a live devbench instance's
// real GET /api/tools response: src/tools-fallback.json for Skyrim,
// src/tools-fallback-fallout4.json for Fallout 4. Which one is read off the
// instance's own GET /api/health `game` field, so pointing this at the wrong
// port cannot write one game's tools into the other's file. Run it after any
// change to that game's core tool registry -- CI fails the build if those files
// change without their fallback, see scripts/check-tools-fallback-sync.mjs.
//
// Usage: node scripts/sync-tools-fallback.mjs [http://127.0.0.1:8920]
//   Skyrim SE 8920, Skyrim VR 8921, Fallout 4 8930, Fallout 4 VR 8931.

import { writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

// Explicit allowlists, not a "no dot in the name" filter -- devbench's C-ABI
// RegisterTool never enforces the dotted "consumer.verb" naming convention
// extension tools otherwise follow, so a bare filter could admit a rogue one
// (FRIK registers an undotted `frik`, for one). Update the set (and re-run)
// whenever a core tool is added or removed.
const FAMILIES = {
  skyrim: {
    out: "../src/tools-fallback.json",
    coreTools: new Set([
      "input",
      "menu",
      "console",
      "scenario",
      "inspect",
      "game",
      "camera",
      "papyrus",
      "capture",
      "record",
      "recordings",
      "wait",
      "sleep",
      "mcp_bridge_setup",
      "ping",
    ]),
  },
  fallout4: {
    out: "../src/tools-fallback-fallout4.json",
    coreTools: new Set([
      "ping",
      "inspect",
      "console",
      "menu",
      "nodes",
      "memory",
      "log",
      "rendertarget",
      "measure",
    ]),
  },
};

const base = process.argv[2] ?? "http://127.0.0.1:8920";

const health = await fetch(`${base}/api/health`);
if (!health.ok) {
  throw new Error(`GET ${base}/api/health -> ${health.status}`);
}
const game = (await health.json()).game;
const family = FAMILIES[game];
if (!family) {
  throw new Error(
    `${base} is devbench for game '${game}', which has no offline tool list here ` +
      `(known: ${Object.keys(FAMILIES).join(", ")}).`,
  );
}
const CORE_TOOL_NAMES = family.coreTools;

const res = await fetch(`${base}/api/tools`);
if (!res.ok) {
  throw new Error(`GET ${base}/api/tools -> ${res.status}`);
}
const body = await res.json();

const liveNames = new Set(body.tools.map((t) => t.name));
for (const expected of CORE_TOOL_NAMES) {
  if (!liveNames.has(expected)) {
    console.warn(
      `WARNING: expected core tool "${expected}" not found live -- renamed or removed?`,
    );
  }
}
for (const t of body.tools) {
  if (!t.name.includes(".") && !CORE_TOOL_NAMES.has(t.name)) {
    console.warn(
      `WARNING: undotted tool "${t.name}" isn't in the ${game} core tool list -- ` +
        "add it there if it's a real core tool, or investigate if it's a mod that should use a dotted name.",
    );
  }
}

const core = body.tools
  .filter((t) => CORE_TOOL_NAMES.has(t.name))
  .map(({ name, description, inputSchema, readOnly }) => ({
    name,
    description,
    inputSchema,
    ...(readOnly ? { readOnly } : {}),
  }));

const outPath = fileURLToPath(new URL(family.out, import.meta.url));
writeFileSync(outPath, JSON.stringify({ tools: core }, null, 2) + "\n");
console.log(`Wrote ${core.length} ${game} core tools to ${outPath}`);
