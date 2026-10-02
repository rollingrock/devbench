// Self-contained check (no game) that the bridge pushes tools/list_changed when the
// live tool list stops matching what the client was given. Run `npm run build` first.
//
//   node test/list-changed.mjs
//
// The scenario is the one the push exists for: the session starts while the game is
// down (the client gets the offline list), then the game comes up with a mod-registered
// tool the offline list cannot know about. Then a mod registers one more tool mid-run.
// Then the poll cadence: fast in each game process's startup window, slow after it, and
// fast again when the pid changes, i.e. a restart the bridge never saw go down. The env
// knobs shrink the real 5 s / 60 s / 2 min to test speed; the ratios are what matter.
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { ToolListChangedNotificationSchema } from "@modelcontextprotocol/sdk/types.js";

const tool = (name) => ({
  name,
  description: `${name} (mock)`,
  inputSchema: { type: "object" },
});
let liveTools = [tool("ping"), tool("nodes"), tool("frik")];
let gamePid = 100;
let toolListRequests = 0;
const FAST = 250;
const SLOW = 2_000;
const STARTUP = 1_500;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// A fake Fallout install: the executable decides the family, runtime.json the port.
const install = mkdtempSync(join(tmpdir(), "devbench-bridge-test-"));
writeFileSync(join(install, "Fallout4VR.exe"), "");
const runtimeDir = join(install, "Data", "F4SE", "Plugins", "devbench");
mkdirSync(runtimeDir, { recursive: true });

// Reserve a free port, then leave it closed: the "game" is not running yet.
const probe = createServer();
await new Promise((resolve) => probe.listen(0, "127.0.0.1", resolve));
const port = probe.address().port;
await new Promise((resolve) => probe.close(resolve));
writeFileSync(join(runtimeDir, "runtime.json"), JSON.stringify({ port }));

let failed = 0;
const check = (ok, what) => {
  console.log(`${ok ? "PASS" : "FAIL"}  ${what}`);
  if (!ok) failed++;
};

const client = new Client({ name: "list-changed-test", version: "0.0.0" });
let notified = 0;
client.setNotificationHandler(ToolListChangedNotificationSchema, async () => {
  notified++;
});
await client.connect(
  new StdioClientTransport({
    command: process.execPath,
    args: [
      fileURLToPath(new URL("../dist/index.js", import.meta.url)),
      "--install",
      install,
    ],
    env: {
      ...process.env,
      DEVBENCH_BRIDGE_FAST_POLL_MS: String(FAST),
      DEVBENCH_BRIDGE_SLOW_POLL_MS: String(SLOW),
      DEVBENCH_BRIDGE_STARTUP_WINDOW_MS: String(STARTUP),
    },
  }),
);

const names = async () =>
  (await client.listTools()).tools.map((t) => t.name).sort();
const waitForNotice = async (count, ms) => {
  const until = Date.now() + ms;
  while (notified < count && Date.now() < until)
    await new Promise((resolve) => setTimeout(resolve, 100));
  return notified >= count;
};

try {
  const offline = await names();
  check(
    offline.includes("nodes") && !offline.includes("frik"),
    `game down: the offline Fallout list (${offline.length} tools, no frik)`,
  );

  const game = createServer((req, res) => {
    res.writeHead(200, { "Content-Type": "application/json" });
    if (req.url === "/api/health") {
      res.end(JSON.stringify({ ok: true, pid: gamePid }));
      return;
    }
    toolListRequests++;
    res.end(JSON.stringify({ tools: liveTools }));
  });
  await new Promise((resolve) => game.listen(port, "127.0.0.1", resolve));

  check(await waitForNotice(1, 6_000), "game up: tools/list_changed pushed");
  const live = await names();
  check(live.includes("frik"), `re-list sees the mod tool: ${live.join(", ")}`);

  liveTools = [...liveTools, tool("later.mod")];
  check(
    await waitForNotice(2, SLOW + 3_000),
    "a tool registered mid-session: pushed again",
  );
  check((await names()).includes("later.mod"), "re-list sees it");

  await sleep(SLOW + 500);
  check(notified === 2, `no repeat notice while nothing changes (${notified})`);

  // Well past the startup window now: one list fetch per SLOW, where fast would be ~16.
  toolListRequests = 0;
  await sleep(2 * SLOW + 200);
  check(
    toolListRequests <= 3,
    `after the startup window it backs off (${toolListRequests} fetches in ${2 * SLOW + 200} ms)`,
  );

  // A new pid with no down in between: fast again. Slow-only would fetch at most twice
  // in this span, whenever within one SLOW the new pid gets noticed.
  gamePid = 200;
  toolListRequests = 0;
  await sleep(SLOW + STARTUP);
  check(
    toolListRequests >= 4,
    `a restart (new pid) resumes fast polling (${toolListRequests} fetches in ${SLOW + STARTUP} ms)`,
  );

  await new Promise((resolve) => game.close(resolve));
  game.closeAllConnections();
  await sleep(3 * FAST + 2_500); // a refused localhost connect takes ~2 s on Windows
  check(
    notified === 2,
    "game down again: no notice, the client keeps its list",
  );
} finally {
  await client.close();
  rmSync(install, { recursive: true, force: true });
}

console.log(failed ? `${failed} FAILED` : "all passed");
process.exit(failed ? 1 : 0);
