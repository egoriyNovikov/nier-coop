# nier-coop

Fork of AutomataMP (NieR:Automata multiplayer mod) aimed at a two-player story co-op.

## Read first
- `docs/COOP_CONCEPT.md` is the source of truth for what the mod should become: roles per route, "Host leads the story", spectator mode, route C parallel play, PvP only in the Tower duel. Check every design decision against it. If a change contradicts it, update the concept (its decision log) with the user first.

- `docs/TASKS.md` is the task plan (stages and sub-tasks). Work from it and keep statuses current.
- Follow the rules of the coop-anything playbook (`coop-anything/AGENTS.md`, clone of https://github.com/cranebeaker/coop-anything; decision of the user 2026-10-18). Notes go to `coop-anything/games/nier-automata/NOTES.md` as you work, private state to `coop-anything/local/`.
- After every code change: build, then test with the bot in test copies of the game (`python tools/qa/coop_smoke.py`, see `tools/qa/`). A test instruction for the user (template in `docs/TASKS.md`) only for what the bot can't check yet, one per milestone.

## Project notes
- The user communicates in Russian; answer in Russian.
- Build: `cmake --build build --config Release --target automatamp`, output `build/bin/automatamp/dinput8.dll`.
- Game folder of the user: `D:\Steam\steamapps\common\NieRAutomata`. Never touch it or the user's saves (`Documents\My Games\NieR_Automata`, `%APPDATA%\PlatinumGames`). Test in `.testinstall\slotN` (made by `tools/qa/make_test_install.ps1`); logs there: `automatamp_log*.txt`. NVIDIA App rewrote the user's `SystemData.dat` on the first launch from a new folder until the user turned off "optimize new games" (2026-10-18, checked with slot4); `bot.py` still compares the user's file hashes after every run.
- x64dbg and Cheat Engine are allowed for reverse engineering. Put findings into `shared/sdk`.
- The repository stores text files with LF (`.gitattributes`). Write new files with LF; if an editor saves CRLF, git normalizes it on commit, but do not convert whole files, it bloats the working-tree diff.
- VPS server: `root@95.163.229.188`, key `~/.ssh/nier_vps`, service `nier-coop` (`journalctl -u nier-coop`), config with the password only on the VPS in `/opt/nier-coop/server.json`. Update with `server/deploy/deploy.sh` from Git Bash.
