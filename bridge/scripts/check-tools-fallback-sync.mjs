#!/usr/bin/env node
// CI guard: fails if a devbench core-tool registration file changed without
// the offline tool list it feeds also changing in the same diff, so the
// bridge's static fallback can't silently drift from the real tool registry.
// Each game family has its own list; a shared core file feeds both. Run
// scripts/sync-tools-fallback.mjs against a live devbench of that game to
// update one.
//
// Usage: node scripts/check-tools-fallback-sync.mjs <base-ref>

import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

const baseRef = process.argv[2];
if (!baseRef) {
  throw new Error("usage: check-tools-fallback-sync.mjs <base-ref>");
}

// Registration files that feed every family's list: tools registered from the
// shared core (ping, memory, log, ...) appear in each game's fallback.
const SHARED_FILES = [
  "src/core/HostApi.cpp",
  "src/core/ToolRegistry.h",
  "src/core/tools/CommonTools.cpp",
  "src/core/tools/GfxTools.cpp",
];
const FALLBACKS = {
  "bridge/src/tools-fallback.json": [
    "src/platform/skyrim/Tools.cpp",
    "src/platform/skyrim/Capture.cpp",
    "src/platform/skyrim/KeyboardInput.cpp",
    ...SHARED_FILES,
  ],
  "bridge/src/tools-fallback-fallout4.json": [
    "src/platform/fallout4/Tools_Fallout4.cpp",
    "src/platform/fallout4/Nodes_Fallout4.cpp",
    ...SHARED_FILES,
  ],
};

// Structural validation, independent of whether a registration file changed --
// the file-touched check below only proves the file moved, not that it's sane.
for (const FALLBACK_FILE of Object.keys(FALLBACKS)) {
  const fallback = JSON.parse(readFileSync(FALLBACK_FILE, "utf-8"));
  if (!Array.isArray(fallback.tools) || fallback.tools.length === 0) {
    console.error(`${FALLBACK_FILE}: "tools" must be a non-empty array.`);
    process.exit(1);
  }
  for (const t of fallback.tools) {
    if (typeof t.name !== "string" || !t.name) {
      console.error(
        `${FALLBACK_FILE}: a tool entry is missing a valid "name".`,
      );
      process.exit(1);
    }
    if (typeof t.description !== "string" || !t.description) {
      console.error(
        `${FALLBACK_FILE}: tool "${t.name}" is missing a "description".`,
      );
      process.exit(1);
    }
    if (t.inputSchema?.type !== "object") {
      console.error(
        `${FALLBACK_FILE}: tool "${t.name}" has no inputSchema.type === "object" ` +
          "(would fail MCP's schema validation).",
      );
      process.exit(1);
    }
  }
}

// Run from the repo root (CI does; a local run should too) so the paths below
// match git's own repo-relative output.
const changed = execFileSync(
  "git",
  ["diff", "--name-only", `${baseRef}...HEAD`],
  {
    encoding: "utf-8",
  },
)
  .split("\n")
  .filter(Boolean);

let failed = false;
for (const [FALLBACK_FILE, sources] of Object.entries(FALLBACKS)) {
  const touched = sources.filter((f) => changed.includes(f));
  if (touched.length && !changed.includes(FALLBACK_FILE)) {
    console.error(
      `A devbench core-tool file changed (${touched.join(", ")}) without ${FALLBACK_FILE}. ` +
        "Run 'node bridge/scripts/sync-tools-fallback.mjs <url>' against a live devbench of that game " +
        "and commit the result.",
    );
    failed = true;
  }
}
if (failed) process.exit(1);
console.log("tools-fallback sync check passed.");
