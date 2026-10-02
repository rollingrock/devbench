// runtime.json is re-read on every call (never cached), since devbench's port can
// change across a game restart.

import { existsSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";

/** The game family a target belongs to: picks the Data path and the offline tool list. */
export type Family = "skyrim" | "fallout4";

export interface Target {
  /** Human-readable label for error messages ("SE", "FO4VR", or the --install path). */
  label: string;
  family: Family;
  /** Directory containing runtime.json — the Data-relative path or its %LOCALAPPDATA% mirror. */
  runtimeDir: string;
}

interface RuntimeJson {
  port: number;
  [key: string]: unknown;
}

interface GameSpec {
  label: string;
  family: Family;
  /** Subfolder of %LOCALAPPDATA%\devbench — must match Server.cpp's ExternalStateDir(). */
  stateDir: string;
  /** Steam's folder name under steamapps/common. */
  steamFolder: string;
}

const GAMES = {
  se: {
    label: "SE",
    family: "skyrim",
    stateDir: "se",
    steamFolder: "Skyrim Special Edition",
  },
  vr: {
    label: "VR",
    family: "skyrim",
    stateDir: "vr",
    steamFolder: "SkyrimVR",
  },
  fo4: {
    label: "FO4",
    family: "fallout4",
    stateDir: "fallout4",
    steamFolder: "Fallout 4",
  },
  fo4vr: {
    label: "FO4VR",
    family: "fallout4",
    stateDir: "fallout4vr",
    steamFolder: "Fallout 4 VR",
  },
} as const satisfies Record<string, GameSpec>;

export type Game = keyof typeof GAMES;

/** The --game values, for usage messages. */
export const GAME_NAMES = Object.keys(GAMES).join("|");

// Best-effort default Steam library locations; --install covers anything else.
const STEAM_LIBRARIES = [
  "C:/Program Files (x86)/Steam/steamapps/common",
  "C:/SteamLibrary/steamapps/common",
  "D:/SteamLibrary/steamapps/common",
  "E:/SteamLibrary/steamapps/common",
];

const EXTENDER_DIR: Record<Family, string> = {
  skyrim: "SKSE",
  fallout4: "F4SE",
};

function runtimeDirFor(installPath: string, family: Family): string {
  return join(installPath, "Data", EXTENDER_DIR[family], "Plugins", "devbench");
}

// An --install path names no game, so read it off the executable in the folder.
function familyOfInstall(installPath: string): Family {
  return ["Fallout4.exe", "Fallout4VR.exe"].some((exe) =>
    existsSync(join(installPath, exe)),
  )
    ? "fallout4"
    : "skyrim";
}

// Must match Server.cpp's ExternalStateDir() exactly: %LOCALAPPDATA%\devbench\<stateDir>,
// reachable even under a VFS mod manager (MO2) where Data/<extender>/Plugins/devbench is virtual.
function localAppDataDevbenchDir(stateDir: string): string | undefined {
  const localAppData = process.env.LOCALAPPDATA;
  return localAppData ? join(localAppData, "devbench", stateDir) : undefined;
}

// runtime.json survives after the game exits, so picking the first readable
// candidate can pin a stale install over a live one; pick the most recently
// written file instead (devbench rewrites it fresh on every boot).
function freshestExisting(candidates: string[]): string | undefined {
  let best: { dir: string; mtimeMs: number } | undefined;
  for (const dir of candidates) {
    try {
      const mtimeMs = statSync(join(dir, "runtime.json")).mtimeMs;
      if (!best || mtimeMs > best.mtimeMs) best = { dir, mtimeMs };
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code !== "ENOENT") throw e;
    }
  }
  return best?.dir;
}

/** Whether `game` is a supported --game value. */
export function isSupportedGame(game: string | undefined): game is Game {
  return game !== undefined && Object.hasOwn(GAMES, game);
}

/** Resolve a Target from parsed CLI args. Throws with a clear message if none found. */
export function resolveTarget(args: {
  game?: string;
  install?: string;
}): Target {
  if (args.install) {
    const family = familyOfInstall(args.install);
    return {
      label: args.install,
      family,
      runtimeDir: runtimeDirFor(args.install, family),
    };
  }
  if (isSupportedGame(args.game)) {
    const spec = GAMES[args.game];
    const candidates = STEAM_LIBRARIES.map((lib) =>
      runtimeDirFor(join(lib, spec.steamFolder), spec.family),
    );
    const localAppDataDir = localAppDataDevbenchDir(spec.stateDir);
    if (localAppDataDir) candidates.push(localAppDataDir);
    const found = freshestExisting(candidates);
    if (!found) {
      throw new Error(
        `Could not find a devbench install for --game ${args.game} in any default Steam ` +
          `location, and devbench has not run yet to write %LOCALAPPDATA%\\devbench\\${spec.stateDir}. ` +
          `Start the game once, or pass --install <path-to-game-folder> instead.`,
      );
    }
    return { label: spec.label, family: spec.family, runtimeDir: found };
  }
  throw new Error(
    `devbench-bridge requires --game ${GAME_NAMES} or --install <path>.`,
  );
}

/** Read the live port + base URL for a target, fresh every call. */
export function resolveBaseUrl(target: Target): string {
  const raw = readFileSync(join(target.runtimeDir, "runtime.json"), "utf-8");
  const parsed = JSON.parse(raw) as RuntimeJson;
  if (
    !Number.isInteger(parsed.port) ||
    parsed.port < 1 ||
    parsed.port > 65535
  ) {
    throw new Error(
      `runtime.json at ${target.runtimeDir} has no valid "port" field (1-65535).`,
    );
  }
  return `http://127.0.0.1:${parsed.port}`;
}
