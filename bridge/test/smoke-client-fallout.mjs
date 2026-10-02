// Smoke test for the bridge against Fallout 4 / Fallout 4 VR: spawns dist/index.js with
// this same Node and drives it over stdio the way an MCP client does. Portable (no
// machine paths). Run `npm run build` first.
//
//   node test/smoke-client-fallout.mjs [--game fo4vr|fo4] [--watch <seconds>]
//
// One-shot: tools/list, then inspect + nodes calls. --watch keeps ONE MCP session open
// and polls `inspect kind=health` every 2 s, printing each up/down transition. Quit and
// relaunch the game while it runs: the session should report "game not running" and then
// answer again, without reconnecting. That is the whole reason the bridge exists. Start it
// with the game down to see mod tools (FRIK's `frik`) arrive by tools/list_changed.
import { fileURLToPath } from "node:url";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";
import { ToolListChangedNotificationSchema } from "@modelcontextprotocol/sdk/types.js";

const argv = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = argv.indexOf(name);
  return i >= 0 ? argv[i + 1] : fallback;
};
const game = opt("--game", "fo4vr");
const watchSeconds = Number(opt("--watch", "0"));

const transport = new StdioClientTransport({
  command: process.execPath,
  args: [
    fileURLToPath(new URL("../dist/index.js", import.meta.url)),
    "--game",
    game,
  ],
});
const client = new Client({ name: "smoke-test-fallout", version: "0.0.0" });
// Re-list on the bridge's push, as a real client does, and say what arrived: a session
// that started while the game was down should gain the mod tools here.
client.setNotificationHandler(ToolListChangedNotificationSchema, async () => {
  const names = (await client.listTools()).tools.map((t) => t.name);
  console.log(
    `${new Date().toLocaleTimeString()}  tools/list_changed -> ${names.length} tools: ${names.join(", ")}`,
  );
});
await client.connect(transport);

// A tool result's text is devbench's JSON (or the bridge's { ok:false, reason }).
const call = async (name, args) => {
  const r = await client.callTool({ name, arguments: args });
  const text = r.content?.[0]?.text ?? "null";
  return { isError: r.isError === true, body: JSON.parse(text) };
};

const tools = await client.listTools();
console.log(
  `tools/list: ${tools.tools.length} tools: ${tools.tools.map((t) => t.name).join(", ")}`,
);

if (watchSeconds > 0) {
  const until = Date.now() + watchSeconds * 1000;
  let last;
  while (Date.now() < until) {
    const r = await call("inspect", { kind: "health" });
    const state = r.isError
      ? `DOWN (${r.body.reason})`
      : `UP   pid=${r.body.pid} port=${r.body.port} frame=${r.body.frame}`;
    const key = r.isError ? "down" : `up:${r.body.pid}`;
    if (key !== last)
      console.log(`${new Date().toLocaleTimeString()}  ${state}`);
    last = key;
    await new Promise((resolve) => setTimeout(resolve, 2000));
  }
} else {
  const state = await call("inspect", { kind: "state" });
  console.log("inspect state:", JSON.stringify(state.body));
  const hmd = await call("nodes", { action: "get", root: "hmdNode" });
  console.log(
    "nodes get hmdNode:",
    hmd.isError
      ? JSON.stringify(hmd.body)
      : `${hmd.body.name} (${hmd.body.type}) at ${hmd.body.address}`,
  );
  const reg = await call("inspect", { kind: "registrants" });
  console.log("registrants consumers:", JSON.stringify(reg.body.consumers));
}

await client.close();
process.exit(0);
