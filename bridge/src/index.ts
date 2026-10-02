#!/usr/bin/env node
import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";

import { callTool, GameUnavailableError, health, listTools } from "./proxy.js";
import {
  type Family,
  GAME_NAMES,
  isSupportedGame,
  resolveTarget,
} from "./runtime.js";
import { printSetupSnippet } from "./setup.js";
import fallout4ToolsFallback from "./tools-fallback-fallout4.json" with { type: "json" };
import skyrimToolsFallback from "./tools-fallback.json" with { type: "json" };

interface DevbenchTool {
  name: string;
  description: string;
  inputSchema: Record<string, unknown>;
  readOnly?: boolean;
}

// devbench's REST shape uses a bare `readOnly`; MCP's is `annotations.readOnlyHint`.
// Shared by both tools/list paths so they can't drift out of sync with each other.
function toMcpTool(t: DevbenchTool) {
  return {
    name: t.name,
    description: t.description,
    inputSchema: t.inputSchema,
    ...(t.readOnly ? { annotations: { readOnlyHint: true } } : {}),
  };
}

// How often the live tool list is compared with the one the client has. Mods register
// their tools while the game starts up (FRIK's lands ~15 s after devbench), so poll fast
// while the game is down, to catch a launch, and for the first minutes of each game
// process; after that a late registration is rare and a minute's delay is fine. Both
// endpoints polled are answered on devbench's listener thread, never the game's main
// thread. The env overrides exist for test/list-changed.mjs, which cannot wait minutes.
function pollMs(name: string, fallback: number): number {
  const value = Number(process.env[`DEVBENCH_BRIDGE_${name}_MS`]);
  return Number.isFinite(value) && value > 0 ? value : fallback;
}
const FAST_POLL_MS = pollMs("FAST_POLL", 5_000);
const SLOW_POLL_MS = pollMs("SLOW_POLL", 60_000);
const STARTUP_WINDOW_MS = pollMs("STARTUP_WINDOW", 120_000);

// Each game family registers a different core tool set, so each has its own offline list.
const TOOLS_FALLBACK: Record<Family, { tools: DevbenchTool[] }> = {
  skyrim: skyrimToolsFallback,
  fallout4: fallout4ToolsFallback,
};

// A compiled standalone executable's embedded entry script lives under this
// virtual path; a plain `node dist/index.js` invocation does not.
function isCompiledExecutable(): boolean {
  return process.argv[1]?.includes("~BUN") ?? false;
}

function parseArgs(argv: string[]): {
  game?: string;
  install?: string;
  setup: boolean;
} {
  let game: string | undefined;
  let install: string | undefined;
  let setup = false;
  for (let i = 0; i < argv.length; i++) {
    switch (argv[i]) {
      case "--game":
        game = argv[++i];
        break;
      case "--install":
        install = argv[++i];
        break;
      case "setup":
        setup = true;
        break;
    }
  }
  return { game, install, setup };
}

async function main(): Promise<void> {
  const args = parseArgs(process.argv.slice(2));

  if (args.setup) {
    if (!args.install && !isSupportedGame(args.game)) {
      throw new Error(
        `devbench-bridge setup requires --game ${GAME_NAMES} or --install <path>.`,
      );
    }
    const scriptArgs = isCompiledExecutable() ? [] : [process.argv[1]];
    printSetupSnippet(process.execPath, scriptArgs, args);
    return;
  }

  const target = resolveTarget(args);

  const server = new Server(
    { name: "devbench-bridge", version: "0.1.0" },
    { capabilities: { tools: { listChanged: true } } },
  );

  // The tool list last handed to the client, serialized, so the poll below can tell
  // when the live one no longer matches it. Undefined until the client first lists.
  let reported: string | undefined;

  server.setRequestHandler(ListToolsRequestSchema, async () => {
    let tools;
    try {
      tools = (await listTools(target)).map(toMcpTool);
    } catch (e) {
      if (!(e instanceof GameUnavailableError)) throw e;
      // A not-yet-running game reports the static fallback, not an empty list --
      // a client typically fetches tools/list only once per session. A call still
      // fails live with GameUnavailableError's own message.
      tools = TOOLS_FALLBACK[target.family].tools.map(toMcpTool);
    }
    reported = JSON.stringify(tools);
    return { tools };
  });

  // Mod-registered tools (FRIK's `frik`, ...) exist only while the game runs, and they
  // register after devbench is already up, so a session that listed from the fallback
  // -- or before the mod loaded -- would never see them. devbench itself pushes
  // tools/list_changed over its own /mcp, but that channel is not the one the bridge
  // uses, so poll the live list and push the notification ourselves. A poll that finds
  // the game down changes nothing: the client keeps the last list, and a restart does
  // not churn tools out and back in.
  //
  // Each poll asks /api/health first (a few hundred bytes) for the game's pid: a new pid
  // is a new game process, whose mods register all over again, so it restarts the fast
  // window even when the restart fell between two slow polls. Returns the next delay.
  let gamePid: number | undefined;
  let gameSeenAt = 0;
  const pollToolList = async (): Promise<number> => {
    let pid: number | undefined;
    try {
      pid = (await health(target)).pid;
    } catch {
      gamePid = undefined;
      return FAST_POLL_MS; // down or unreachable: watch for the next launch
    }
    if (gamePid === undefined || pid !== gamePid) {
      gamePid = pid;
      gameSeenAt = Date.now();
    }
    const next =
      Date.now() - gameSeenAt < STARTUP_WINDOW_MS ? FAST_POLL_MS : SLOW_POLL_MS;
    if (reported === undefined) return next;
    let live: string;
    try {
      live = JSON.stringify((await listTools(target)).map(toMcpTool));
    } catch {
      return next;
    }
    if (live !== reported) {
      reported = live; // don't repeat the notice to a client that has not re-listed yet
      await server.sendToolListChanged();
    }
    return next;
  };
  // A timeout chain rather than setInterval, so a slow poll never overlaps the next;
  // unref'd, so polling never keeps the bridge alive after the client hangs up.
  const schedulePoll = (delayMs: number): void => {
    setTimeout(() => {
      void pollToolList()
        .catch(() => FAST_POLL_MS)
        .then(schedulePoll);
    }, delayMs).unref();
  };
  schedulePoll(FAST_POLL_MS);

  server.setRequestHandler(CallToolRequestSchema, async (request) => {
    try {
      const result = await callTool(
        target,
        request.params.name,
        request.params.arguments ?? {},
      );
      return {
        content: [{ type: "text", text: JSON.stringify(result ?? null) }],
      };
    } catch (e) {
      const message =
        e instanceof GameUnavailableError
          ? `game not running (target: ${target.label})`
          : (e as Error).message;
      return {
        content: [
          {
            type: "text",
            text: JSON.stringify({ ok: false, reason: message }),
          },
        ],
        isError: true,
      };
    }
  });

  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((e) => {
  console.error(`devbench-bridge: ${(e as Error).message}`);
  process.exit(1);
});
