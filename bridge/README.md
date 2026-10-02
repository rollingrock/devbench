# devbench-bridge

A stdio MCP proxy for [devbench](../README.md). Spawned by your MCP client per-session,
it forwards `tools/list`/`tools/call` to whatever Skyrim SE/AE/VR or Fallout 4 / Fallout 4
VR process is currently running devbench's REST API — so your MCP connection survives the
game restarting (the normal dev loop: rebuild → close game → relaunch), which a direct
connection to devbench's own `/mcp` endpoint does not.

It holds no port/cache state of its own — every call reads the live port from that
install's `Data/<SKSE|F4SE>/Plugins/devbench/runtime.json` and proxies straight through, so
a live call never drifts from whatever devbench build is actually running. Under a VFS mod
manager (MO2, …) that path is virtual and unreachable from outside the manager's own
hook, so devbench also mirrors `runtime.json` to
`%LOCALAPPDATA%\devbench\<se|vr|fallout4|fallout4vr>\`, which isn't virtualized — the
bridge checks both and uses whichever is freshest. `tools/list` is the one exception: it
returns that game's real core tool set, baked in at build time (`src/tools-fallback.json`
for Skyrim, `src/tools-fallback-fallout4.json` for Fallout 4), when no game is up, since an MCP client typically caches
`tools/list` for the whole session and can't be relied on to notice a
`tools/list_changed` push once the game starts — the client sees the full tool set from
its very first connection either way. When the game isn't up, `tools/call` returns a
clean `{ok:false, reason:"game not running"}` instead of killing your MCP session —
restart the game and keep calling, no reconnect.

Tools a mod registers (FRIK's `frik`, …) are not in that baked-in list: they exist only
while the game runs, and they register after devbench is already up. So the bridge
compares the live list with the one it last gave the client, and sends
`tools/list_changed` when they differ. A client that honors it re-lists mid-session. While
the game is down the bridge sends nothing, so a restart does not make tools vanish and
reappear. (devbench's own `/mcp` pushes the same notification, but the bridge talks REST,
where there is no push to forward.)

The check is adaptive, because mods register while the game starts up: every 5 s while the
game is down (to catch a launch) and for the first 2 minutes of each game process, then
every 60 s. Each check asks `/api/health` for the game's pid first, so a restart that fell
between two slow checks still restarts the fast window. Both endpoints are answered off the
game's main thread.

Each list is regenerated from a live devbench with
`node scripts/sync-tools-fallback.mjs [url]` (default `http://127.0.0.1:8920`); the script
asks the instance which game it is and writes that game's file, so a wrong port cannot
cross them. Run it and commit the result after changing that game's core tool registry:
`src/platform/skyrim/{Tools,Capture,KeyboardInput}.cpp` for Skyrim,
`src/platform/fallout4/{Tools_Fallout4,Nodes_Fallout4}.cpp` for Fallout, and for both,
`src/core/HostApi.cpp`, `src/core/ToolRegistry.h` and `src/core/tools/{CommonTools,GfxTools}.cpp`.
CI fails the build otherwise (`scripts/check-tools-fallback-sync.mjs`).

## Setup

If devbench is installed, the bridge is already at
`Data/SKSE/Plugins/devbench/devbench-bridge.exe` — nothing to download. Add one entry to
your MCP client's config (Claude Code's `.mcp.json`, Claude Desktop's config, etc.).

**Under a VFS mod manager (MO2, …)** that path is only reachable by processes the manager
itself launches — an MCP client spawning the bridge directly (the normal case) can't see
it. Extract `devbench-bridge.exe` from the release archive to a real, on-disk folder and
point `command` at that copy instead; `runtime.json`'s VFS-proof mirror (above) means the
extracted exe still finds the live game either way.

```json
{
  "mcpServers": {
    "devbench-se": {
      "command": "C:/path/to/Skyrim Special Edition/Data/SKSE/Plugins/devbench/devbench-bridge.exe",
      "args": ["--game", "se"]
    }
  }
}
```

For Fallout 4 VR use `--game fo4vr` (flat Fallout 4: `--game fo4`). The Fallout build does
not ship the exe beside the plugin; it is the same exe for every game, so point `command`
at any copy (a release archive's, or `npm run compile` here). Running from source works
too: `"command": "<node.exe>", "args": ["<repo>/bridge/dist/index.js", "--game", "fo4vr"]`.

Running more than one game at once? Add one entry per game (`--game se|vr|fo4|fo4vr`, or
`--install <path>` if it's not one of the common Steam locations). Each is its own bridge
process — they don't share state, so all can be used concurrently.

`devbench-bridge setup --game se` prints this snippet for you (never writes to your
client config itself — you paste it in).

You can also get this exact snippet live from a running game: `GET /api/tools`'s
`mcp_bridge` field, or `Data/SKSE/Plugins/devbench/mcp-bridge.json`.

## Flags

- `--game se|vr|fo4|fo4vr` — target the default Steam install for that runtime, or the
  `%LOCALAPPDATA%` mirror devbench writes once it has run.
- `--install <path>` — target a specific game install directory (required if devbench
  isn't in one of the default Steam locations and has never run). Skyrim or Fallout is
  read off the executable in that folder.
- `setup` — print the `.mcp.json` snippet instead of running as an MCP server.

## Development

```
npm install
npm run build      # tsc, for iterating
npm run compile    # bun build --compile → dist/devbench-bridge.exe (standalone, no Node/Bun required to run it)
```

`test/` holds a throwaway mock devbench server and MCP-client smoke tests for exercising
the proxy logic — not part of the shipped bridge. `test/smoke-client-fallout.mjs` is
portable: `node test/smoke-client-fallout.mjs --game fo4vr` makes one round of calls, and
`--watch 120` keeps one MCP session open and prints each up/down transition and each
`tools/list_changed` while you quit and relaunch the game. `test/list-changed.mjs` needs no
game: it runs the bridge against a mock that comes up mid-session and checks the push.
