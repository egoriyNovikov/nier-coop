# nier-coop

Fork of AutomataMP (NieR:Automata multiplayer mod) aimed at a two-player story co-op.

## Read first
- `docs/COOP_CONCEPT.md` is the source of truth for what the mod should become: roles per route, "Host leads the story", spectator mode, route C parallel play, PvP only in the Tower duel. Check every design decision against it. If a change contradicts it, update the concept (its decision log) with the user first.

- `docs/TASKS.md` is the task plan (stages and sub-tasks). Work from it and keep statuses current.
- After every code change: build, then send the user a testing instruction in Russian using the template in `docs/TASKS.md` ("Шаблон инструкции по тестированию"). Mark the task `[?]` until the user confirms it works in game.

## Project notes
- The user communicates in Russian; answer in Russian.
- Build: `cmake --build build --config Release --target automatamp`, output `build/bin/automatamp/dinput8.dll`.
- Game folder: `D:\Steam\steamapps\common\NieRAutomata`. Logs there: `automatamp_log*.txt` (one per running game instance). If the game is running the DLL is locked: rename the old one to a backup, then copy the new one; it loads on the next game start.
- x64dbg and Cheat Engine are allowed for reverse engineering. Put findings into `shared/sdk`.
- The repository stores text files with LF (`.gitattributes`). Write new files with LF; if an editor saves CRLF, git normalizes it on commit, but do not convert whole files, it bloats the working-tree diff.
- VPS server: `root@95.163.229.188`, key `~/.ssh/nier_vps`, service `nier-coop` (`journalctl -u nier-coop`), config with the password only on the VPS in `/opt/nier-coop/server.json`. Update with `server/deploy/deploy.sh` from Git Bash.
