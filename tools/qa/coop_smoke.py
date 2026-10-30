"""Co-op smoke test (task 0.5.8): two test copies of the game connect to each other without a person.

  python tools/qa/coop_smoke.py [--scenario together|diverged|routes|commander|hangar] [--no-build]

1. Builds the mod and the server, installs them into .testinstall/slot1 (host) and slot2 (client).
2. Both copies load a save through the test bot (together: both the "Test" save at the Bunker;
   diverged: the host the "Test" save, the client the finished save at the Resistance Camp;
   routes: the host the "Test" save (route A), the client Chapter Select as 9S (route B);
   commander: both the "Test" save, then the host talks to the Commander, TASKS 3.0.1;
   hangar: as commander, then the host goes on to the hangar until the phase changes, TASKS 3.0.1 / 3.2.1;
   client_talk: both the "Test" save, the client tries to talk to the Commander, then the host does, TASKS 3.2.3;
   death: both the "Test" save, the client dies, then the host dies too and both continue, TASKS 1.4.2 / 1.4.4;
   death_world: the same in the open world (finished save), where the Game Over shows the Body Report;
   death_host: as death, but the host dies first).
3. slot1 presses "Host Game", slot2 "Join Game" to 127.0.0.1:6969.
4. Asserts: both are in the session, slot1 leads the story and slot2 doesn't, each sees the other
   player's position close to where the other game says its player is, the phase and route are known
   to the other side; per scenario: same phase and roles / different phases / different routes.
5. Closes both and checks isolation (the owner's saves and config unchanged, nothing blocked).
Prints RESULT: PASS or RESULT: FAIL; exit code 0 on PASS.
"""
import argparse
import re
import subprocess
import sys
import time

import bot

HOST, CLIENT = "slot1", "slot2"
PORT = "6969"


def run(command, cwd=bot.REPO):
    print("$ " + " ".join(command))
    if subprocess.run(command, cwd=cwd).returncode != 0:
        sys.exit("RESULT: FAIL build step failed")


def install():
    run(["cmake", "--build", "build", "--config", "Release", "--target", "automatamp"])
    run(["go", "build", "-o", str(bot.REPO / "build" / "bin" / "server.exe"), "."], cwd=bot.REPO / "server")
    for slot in (HOST, CLIENT):
        if not (bot.slot_dir(slot) / "coop_test.ini").exists():
            run(["powershell", "-ExecutionPolicy", "Bypass", "-File",
                 str(bot.REPO / "tools" / "qa" / "make_test_install.ps1"), "-Slot", slot])
        (bot.slot_dir(slot) / "dinput8.dll").write_bytes((bot.REPO / "build" / "bin" / "automatamp" / "dinput8.dll").read_bytes())
    server_dir = bot.slot_dir(HOST) / "automatamp_server"
    server_dir.mkdir(exist_ok=True)
    (server_dir / "server.exe").write_bytes((bot.REPO / "build" / "bin" / "server.exe").read_bytes())


def parse(line, key):
    match = re.search(rf"\b{key}=(\S+)", line)
    return match.group(1) if match else None


def position(text):
    return [float(v) for v in text.split(",")] if text and text != "none" else None


def main():
    # Game logs carry graph names in Shift-JIS; the console may not show every character.
    sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", choices=["together", "diverged", "routes", "commander", "midjoin", "world", "hangar", "client_talk", "death", "death_world", "death_host"], default="together",
                        help="together: both load the same save; diverged: the games are at different story points; "
                             "routes: the games are on routes A and B; commander: the host talks to the Commander; "
                             "midjoin: the host talks to her before the client joins; world: open world, personal graphs stay with the host; "
                             "hangar: then the host flies off from the hangar (natural phase change); "
                             "client_talk: the client tries the host's story talk with the Commander; "
                             "death: the client dies, the host's character dies too, both continue (Bunker); "
                             "death_world: the same in the open world, with the Body Report; death_host: the host dies first")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    if not args.no_build:
        install()

    failures = []

    def expect(condition, message):
        print(("  ok   " if condition else "  FAIL ") + message)
        if not condition:
            failures.append(message)

    try:
        {"together": together, "diverged": diverged, "routes": routes, "commander": commander, "midjoin": midjoin, "world": world, "hangar": hangar,
         "client_talk": client_talk, "death": death,
         "death_world": lambda e: death(e, 0), "death_host": lambda e: death(e, 1, HOST)}[args.scenario](expect)
    finally:
        isolated = [bot.stop(slot) for slot in (HOST, CLIENT)]
        server = bot.slot_dir(HOST) / "automatamp_server" / "server.exe"
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        f"Get-Process server -ErrorAction SilentlyContinue | Where-Object Path -eq '{server}' | Stop-Process -Force"])
        if not all(isolated):
            failures.append("isolation")

    print("RESULT: PASS" if not failures else f"RESULT: FAIL ({len(failures)}): " + "; ".join(failures))
    sys.exit(0 if not failures else 1)


# Phase of each test save, to notice a wrong load.
SAVE_PHASES = {0: "60_05_AB_Forest_Castle", 1: "16_AB_bunker_Order"}


def connect(expect, host_save, client_save):
    """Loads the saves, Host Game + Join Game, waits for both to have the other's data.
    A save is its index (0 = top), a function (slot) -> bool that gets the game into the world, or None if that
    copy is already in the world.
    Returns (host_coop, client_coop, host_state, client_state) or None."""
    # One copy after the other: two copies loading at once reach the title screen late.
    for slot, save in ((HOST, host_save), (CLIENT, client_save)):
        if save is None:
            continue
        loaded = False
        for attempt in range(2):
            if attempt > 0:
                print(f"{slot}: retrying with a fresh start")
                bot.stop(slot)
            if bot.start(slot) and (save(slot) if callable(save) else bot.load(slot, save)):
                # The save list cursor has picked the neighbouring save before: check where we are.
                phase = parse(bot.cmd(slot, ["state"], quiet=True)[-1], "phase")
                if isinstance(save, int) and save in SAVE_PHASES and phase != SAVE_PHASES[save]:
                    print(f"{slot}: loaded {phase}, save {save} is {SAVE_PHASES[save]}: the wrong save")
                    continue
                loaded = True
                break
        expect(loaded, f"{slot} loaded save {save.__doc__ if callable(save) else save}")
        if not loaded:
            return None

    expect(bot.cmd(HOST, [f"host {PORT}"])[-1].startswith("ok"), "Host Game started the server")
    expect(bot.cmd(CLIENT, [f"join 127.0.0.1:{PORT}"])[-1].startswith("ok"), "Join Game accepted the address")

    host_coop = client_coop = ""
    for _ in range(30):
        host_coop = bot.cmd(HOST, ["wait 2000", "coop"], quiet=True)[-1]
        client_coop = bot.cmd(CLIENT, ["coop"], quiet=True)[-1]
        if "data=1" in host_coop and "data=1" in client_coop:
            break

    expect(parse(host_coop, "session") == "1" and parse(client_coop, "session") == "1", "both are in the session")
    expect(parse(host_coop, "master") == "1", "the host leads the story")
    expect(parse(client_coop, "master") == "0", "the client doesn't lead the story")
    expect("data=1" in host_coop and "data=1" in client_coop, "both receive the other player's data")

    # ID_STORY_STATE is sent every 5 s: ask again after that.
    host_coop = bot.cmd(HOST, ["wait 6000", "coop"], quiet=True)[-1]
    client_coop = bot.cmd(CLIENT, ["coop"], quiet=True)[-1]
    host_state = bot.cmd(HOST, ["state"], quiet=True)[-1]
    client_state = bot.cmd(CLIENT, ["state"], quiet=True)[-1]
    for slot, line in ((HOST, host_coop), (CLIENT, client_coop), (HOST, host_state), (CLIENT, client_state)):
        print(f"{slot}: {line}")

    expect(parse(host_coop, "other_story") == parse(client_state, "phase"),
           f"the host knows the client's phase ({parse(host_coop, 'other_story')})")
    expect(parse(client_coop, "other_story") == parse(host_state, "phase"),
           f"the client knows the host's phase ({parse(client_coop, 'other_story')})")
    expect(parse(host_coop, "other_route") == parse(client_state, "route")
           and parse(client_coop, "other_route") == parse(host_state, "route"), "each knows the other's route")
    return host_coop, client_coop, host_state, client_state


def together(expect):
    """Both load the "Test" save (Bunker, route A): the client takes over 9S, positions match."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    host_coop, client_coop, host_state, client_state = result
    # Route from the scene state flags Route/A..C (sdk::get_route).
    expect(parse(host_state, "route") == "A" and parse(client_state, "route") == "A",
           f"both games are on route A ({parse(host_state, 'route')} / {parse(client_state, 'route')})")

    # Each game's view of the other player against where the other game says its player is.
    for viewer, view, other, other_state in ((HOST, host_coop, CLIENT, client_state), (CLIENT, client_coop, HOST, host_state)):
        # pos= is what came over the network: in the Bunker that is the ring's frame (each game turns its ring
        # differently, sdk::BunkerRing), so compare it with the other game's ring_player.
        seen, actual = position(parse(view, "pos")), position(parse(other_state, "ring_player") or parse(other_state, "player"))
        distance = sum((a - b) ** 2 for a, b in zip(seen, actual)) ** 0.5 if seen and actual else None
        expect(distance is not None and distance < 3.0,
               f"{viewer} sees {other} where {other} is (distance {distance if distance is None else round(distance, 2)} m)")
    expect(parse(host_state, "phase") == parse(client_state, "phase"), "both games are in the same phase")
    # Concept 5.1, route A: the host plays 2B (10000), the client takes over the story buddy 9S (10200).
    expect(parse(host_state, "model") == "10000", f"the host plays 2B (model {parse(host_state, 'model')})")
    expect(parse(client_state, "model") == "10200", f"the client plays 9S (model {parse(client_state, 'model')})")

    bot.cmd(HOST, ["shot coop_host"])
    bot.cmd(CLIENT, ["shot coop_client"])


def log_text(slot):
    return "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in bot.slot_dir(slot).glob("automatamp_log*.txt"))


def diverged(expect):
    """The host is in the Bunker ("Test" save), the client at the Resistance Camp (finished save).
    The mod must notice the different phases, and (concept, section 4) not pull the story buddy out of
    either scene: the other player is far from where the game put the buddy."""
    result = connect(expect, 1, 0)
    if result is None:
        return
    host_coop, client_coop, host_state, client_state = result

    expect(parse(host_state, "phase") != parse(client_state, "phase"),
           f"the games are in different phases ({parse(host_state, 'phase')} / {parse(client_state, 'phase')})")
    expect("different phases" in log_text(HOST) and "different phases" in log_text(CLIENT),
           "both logs say the phases differ")
    # handle=0: no puppet bound for the other player (the 50 m rule in NierClient::bind_puppet).
    expect(parse(host_coop, "handle") == "0", f"the host's buddy is not taken over (handle {parse(host_coop, 'handle')})")
    expect(parse(client_coop, "handle") == "0", f"the client's buddy is not taken over (handle {parse(client_coop, 'handle')})")
    # TASKS 3.1.2: the message in the Co-op menu and on screen (screenshots diverged_*).
    expect(parse(host_coop, "mismatch") == "phases" and parse(client_coop, "mismatch") == "phases",
           f"both say: different story lines, different phases ({parse(host_coop, 'mismatch')} / {parse(client_coop, 'mismatch')})")

    bot.cmd(HOST, ["shot diverged_host"])
    bot.cmd(CLIENT, ["shot diverged_client"])


def routes(expect):
    """The host on route A ("Test" save), the client on route B (Chapter Select on the finished save as 9S):
    both must see that the routes differ (concept 5.6, TASKS 3.1.1)."""
    def chapter_as_9s(slot):
        """finished save, Chapter Select as 9S"""
        return bot.chapter(slot, 0, 2, 0, "9S")

    result = connect(expect, 1, chapter_as_9s)
    if result is None:
        return
    host_coop, client_coop, host_state, client_state = result

    expect(parse(host_state, "route") == "A" and parse(client_state, "route") == "B",
           f"the host is on route A, the client on route B ({parse(host_state, 'route')} / {parse(client_state, 'route')})")
    expect(parse(client_state, "hero") == "9S", f"the client's game has 9S as the hero ({parse(client_state, 'hero')})")
    expect("different routes" in log_text(HOST) and "different routes" in log_text(CLIENT),
           "both logs say the routes differ")
    # TASKS 3.1.1 / 3.1.2: no takeover on different routes, the message in the menu and on screen.
    host_coop = bot.cmd(HOST, ["coop"], quiet=True)[-1]
    client_coop = bot.cmd(CLIENT, ["coop"], quiet=True)[-1]
    expect(parse(host_coop, "mismatch") == "routes" and parse(client_coop, "mismatch") == "routes",
           f"both say: different story lines, different routes ({parse(host_coop, 'mismatch')} / {parse(client_coop, 'mismatch')})")
    expect(parse(host_coop, "handle") == "0" and parse(client_coop, "handle") == "0",
           f"nobody takes over a buddy (handles {parse(host_coop, 'handle')} / {parse(client_coop, 'handle')})")
    expect("the story buddy is not taken over" in log_text(HOST) + log_text(CLIENT), "the log says why the buddy is not taken over")
    bot.cmd(HOST, ["shot routes_host"])
    bot.cmd(CLIENT, ["shot routes_client"])


# The Commander on the "Test" save: west of her, facing east, the talk prompt shows (found 2026-10-18).
COMMANDER_SPOT = "goto -114.06 138.37 -22.72 90"
COMMANDER_MODEL = "11020"  # the only entity with this model in the Bunker


def commander(expect):
    """TASKS 3.0.1, 3.6.1: the host talks to the Commander (a story step inside the phase 16_AB_bunker_Order),
    the client stands still. Asserts the host's step goes 130 -> 140 and that the client's game follows: the
    host sends the finished actions of its story graph (ID_STORY_ACTION), the client's game finishes the same
    ones and its step becomes 140 too (before 3.6.1 it stayed at 130, observed 2026-10-18)."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    talk_to_commander(expect, result[2], result[3])
    host_puppet_where_host_is(expect)


def host_puppet_where_host_is(expect):
    """The Bunker's ring turns under the hero, in the client's game the host's puppet is the hero (sdk::BunkerRing).
    Before 2026-10-21 the two games turned their rings differently: after the host's teleport to the Commander the
    puppet ran around the Bunker in the client's game and ended up below the floor. Asserts that the client keeps its
    ring at the host's angle and the puppet stands where the host is."""
    host, client = bot.cmd(HOST, ["state"], quiet=True)[-1], bot.cmd(CLIENT, ["state"], quiet=True)[-1]
    host_ring, client_ring = parse(host, "ring"), parse(client, "ring")
    expect(host_ring not in (None, "none") and client_ring not in (None, "none")
           and abs(float(host_ring) - float(client_ring)) < 0.5,
           f"the client's Bunker ring is at the host's angle ({host_ring} / {client_ring} deg)")
    puppet = [l for l in bot.cmd(CLIENT, ["entities 3000", "wait 100"], quiet=True) if l.startswith("entity Player model=10000 ")]
    seen, actual = position(puppet[0].split(" pos=")[1].split()[0]) if puppet else None, position(parse(host, "player"))
    distance = sum((a - b) ** 2 for a, b in zip(seen, actual)) ** 0.5 if seen and actual else None
    expect(distance is not None and distance < 2.0,
           f"the host's puppet in the client's game stands where the host is (distance {distance if distance is None else round(distance, 2)} m)")
    # A look: the client next to the Commander too, east of her, facing west.
    bot.goto_entity(CLIENT, COMMANDER_MODEL, 1.5, 0, 1.5, 270)
    bot.cmd(CLIENT, ["wait 1500", "shot commander_client_sees_host"], quiet=True)


def talk_to_commander(expect, host_state, client_state):
    expect(parse(host_state, "step") == "130" and parse(client_state, "step") == "130",
           f"both start at step 130 ({parse(host_state, 'step')} / {parse(client_state, 'step')})")

    host_step = host_talks(client=True)
    expect(host_step == "140", f"the host's step is 140 after the talk ({host_step})")
    client_follows_to_140(expect)


def client_talk(expect):
    """TASKS 3.2.3, concept 5.8: story talks are the host's. The client holds B next to the Commander (the objective
    "Speak with the Commander" points at her): nothing starts, its step stays 130, the log and the screen say the host
    does it. Before 2026-10-22 the client's 9S talked to her and its step went to 140 on its own. Then the host talks
    to her as usual and the client's step follows to 140."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    host_state, client_state = result[2], result[3]
    expect(parse(host_state, "step") == "130" and parse(client_state, "step") == "130",
           f"both start at step 130 ({parse(host_state, 'step')} / {parse(client_state, 'step')})")

    if not bot.goto_entity(CLIENT, COMMANDER_MODEL, -1.5, 0, 0, 90):
        bot.cmd(CLIENT, [COMMANDER_SPOT], quiet=True)
    bot.cmd(CLIENT, ["wait 3000", "pad B 1500", "wait 700", "shot client_talk_hint", "wait 1800", "shot client_talk_after_b"])
    # What the host's talk takes (A advances lines): the client's step must not move.
    client = bot.cmd(CLIENT, ["pad A 120", "wait 2300"] * 4 + ["state"], quiet=True)[-1]
    print(f"  observed: client {client}")
    expect(parse(client, "step") == "130", f"the client's step stays 130 after its try ({parse(client, 'step')})")
    expect(int(parse(client, "talk_blocked") or 0) >= 1, f"the client's talk was blocked ({parse(client, 'talk_blocked')})")
    tried = [l for l in log_text(CLIENT).splitlines() if "[Talk] Client tried to talk" in l]
    for line in tried[:3]:
        print("    client log: " + line.split("] ", 2)[-1])
    expect(bool(tried), "the client's log says the talk is the host's")
    host = bot.cmd(HOST, ["state"], quiet=True)[-1]
    expect(parse(host, "step") == "130", f"the host's step is still 130 ({parse(host, 'step')})")

    # The client steps aside, the host talks: the host still can, and the client's game follows.
    bot.cmd(CLIENT, ["goto -69.5 142.1 0.1 0", "wait 1000"], quiet=True)
    host_step = host_talks(client=False)
    expect(host_step == "140", f"the host's step is 140 after its talk ({host_step})")
    client_follows_to_140(expect)


def host_talks(client):
    """The host talks to the Commander, returns its step afterwards."""
    host_step = "130"
    for attempt in range(2):
        # Hold B next to her; the first try in a fresh session sometimes comes before the talk prompt.
        # The Bunker turns: next to her where she is now (west of her, facing east), the old fixed spot as a fallback.
        if not bot.goto_entity(HOST, COMMANDER_MODEL, -1.5, 0, 0, 90):
            bot.cmd(HOST, [COMMANDER_SPOT], quiet=True)
        bot.cmd(HOST, ["wait 3000", "pad B 1500", "wait 2500", f"shot commander_host_talk{attempt}"])
        if attempt == 0 and client:
            bot.cmd(CLIENT, ["shot commander_client_during"])
        # A advances the lines. Stop as soon as the step changes: after the talk A jumps, and 2B can fall off.
        for _ in range(12):
            host_step = parse(bot.cmd(HOST, ["pad A 120", "wait 2300"] * 3 + ["state"], quiet=True)[-1], "step")
            if host_step != "130":
                break
        if host_step != "130":
            break

    bot.cmd(HOST, ["shot commander_host_after"])
    return host_step


def client_follows_to_140(expect):
    client_step = None
    for _ in range(10):
        client_step = parse(bot.cmd(CLIENT, ["wait 1000", "state"], quiet=True)[-1], "step")
        if client_step == "140":
            break
    bot.cmd(CLIENT, ["wait 2000", "shot commander_client_after"])
    for slot in (HOST, CLIENT):
        lines = [l for l in log_text(slot).splitlines() if "[StoryAction]" in l and "Hooked" not in l]
        print(f"  {slot} story actions: {len(lines)}")
        for line in lines[:40]:
            print("    " + line.split("] ", 2)[-1] if "] " in line else line)
    expect(client_step == "140", f"the client's step follows the host's to 140 ({client_step}); screenshot commander_client_after")

# After the Commander: the phase graph waits for an area (AreaAction a9864814) that holds the spot where the
# "Test" save starts (the hangar entrance), then near the host's flight unit (model 10010) the launch cutscene ev0200
# starts; after about 40 s of flight the player character asks for the next phase (+0x81a440) and the game
# loads 20_AB_buker_Aerial_battle (found 2026-10-19; walking the hangar floor reached the area only by chance).
FLIGHT_UNIT = "10010"


def midjoin(expect):
    """TASKS 3.6.1, decision 2026-10-20: the host talks to the Commander alone (step 130 -> 140), then the client
    loads the same save and joins in the middle of the phase. Asserts that the host sends what it has done in the
    phase so far and the client's step catches up to 140."""
    loaded = bot.start(HOST) and bot.load(HOST, 1)
    expect(loaded, "the host loaded save 1")
    if not loaded:
        return
    host_step = host_talks(client=False)
    expect(host_step == "140", f"the host's step is 140 after the talk, before the client joins ({host_step})")
    bot.cmd(HOST, ["goto -69.5 142.1 0.1 0", "wait 2000"], quiet=True)

    result = connect(expect, None, 1)
    if result is None:
        return
    expect(parse(result[3], "step") in ("130", "140"), f"the client starts at step 130 ({parse(result[3], 'step')})")
    client_follows_to_140(expect)


# Graph names of personal interactions and side quests (co-op concept 5.8, decision 2026-10-20), as StoryActionSync
# keeps them to the host.
NOT_SHARED = ["宝箱", "採取", "ショップ", "武器屋", "道具屋", "自販機", "釣り", "実績", "アイテム"]


def world(expect):
    """Concept 5.8, decision 2026-10-20: both load the finished save (open world, 60_05_AB_Forest_Castle; its
    graphs include chests, shops, fishing spots and the giver of side quest q210). The host sends the client what it
    has done in the phase when the client comes in; asserts that none of it is from a personal or side quest graph."""
    result = connect(expect, 0, 0)
    if result is None:
        return
    bot.cmd(HOST, ["wait 10000"], quiet=True)
    sent = [l for l in log_text(HOST).splitlines() if "[StoryAction] Sent" in l]
    kept = [l for l in log_text(HOST).splitlines() if "not shared" in l]
    wrong = [l for l in sent if any(w in l for w in NOT_SHARED) or re.search(r'"[^"]*q\d{3}', l)]
    print(f"  observed: the host sent {len(sent)} actions, kept {len(kept)} to itself")
    for line in (wrong or sent)[:15] + kept[:10]:
        print("    " + line.split("] ", 2)[-1])
    expect(any("came into our phase" in l for l in log_text(HOST).splitlines()), "the host sent the catch-up when the client came in")
    expect(not wrong, f"no personal or side quest graph was sent ({len(wrong)})")


def hangar(expect):
    """TASKS 3.0.1 / 3.2.1 / 3.6.1: the host talks to the Commander, goes to the hangar and flies off (a natural
    phase change), the client stands still. Asserts that the host's phase changes and that the client's game follows
    through the story actions the host sends: the launch cutscene ev0200 and the same next phase (before 3.6.1 the
    client's game stayed at 16_AB_bunker_Order, step 130, observed 2026-10-19)."""
    result = connect(expect, 1, 1)
    if result is None:
        return
    host_start = parse(result[2], "player")
    talk_to_commander(expect, result[2], result[3])

    # Back to where the save started: the area the graph waits for.
    bot.cmd(HOST, [f"goto {host_start.replace(',', ' ')} 0", "wait 3000"], quiet=True)
    expect(bot.goto_entity(HOST, FLIGHT_UNIT, 2.2, 0.6, 0, 90), "the host is next to the flight unit")

    host_phase, client_phase, client_cutscene, client_lines = "16_AB_bunker_Order", "", False, []
    for _ in range(30):
        host = bot.cmd(HOST, ["wait 3000", "state"], quiet=True)[-1]
        client = bot.cmd(CLIENT, ["state"], quiet=True)[-1]
        host_phase, client_phase = parse(host, "phase"), parse(client, "phase")
        client_cutscene = client_cutscene or parse(client, "event") == "ev0200"
        line = f"host {host_phase} step {parse(host, 'step')} event {parse(host, 'event')} loading {parse(host, 'loading')} | "                f"client {parse(client, 'phase')} step {parse(client, 'step')} event {parse(client, 'event')} loading {parse(client, 'loading')}"
        if not client_lines or client_lines[-1] != line:
            client_lines.append(line)
            print("  " + line)
        if host_phase != "16_AB_bunker_Order" and parse(host, "loading") == "0" and client_phase != "16_AB_bunker_Order":
            break

    bot.cmd(HOST, ["shot hangar_host_after"])
    bot.cmd(CLIENT, ["shot hangar_client_after"])
    expect(host_phase == "20_AB_buker_Aerial_battle", f"the host's phase changed to 20_AB_buker_Aerial_battle ({host_phase})")
    expect(client_cutscene, "the client's game played the launch cutscene ev0200 too")
    expect(client_phase == "20_AB_buker_Aerial_battle", f"the client's phase followed to 20_AB_buker_Aerial_battle ({client_phase})")
    client_coop = bot.cmd(CLIENT, ["wait 6000", "coop"], quiet=True)[-1]
    print(f"  observed: client coop: {client_coop}")
    for line in log_text(CLIENT).splitlines():
        if any(k in line for k in ("[StoryAction] Finished", "skipped", "[Story] Phase")):
            print("    client log: " + line.split("] ", 2)[-1])
    fly_together(expect)


def entities(slot, commands=()):
    """Output of `commands` + `entities 3000`, read until the whole list is there (bot.cmd returns as soon as it has
    as many lines as commands, the list can come cut)."""
    root = bot.slot_dir(slot)
    before = len(bot.out_lines(root))
    bot.cmd(slot, list(commands) + ["entities 3000"], quiet=True)
    for _ in range(50):
        lines = bot.out_lines(root)[before:]
        if any(l.startswith("ok entities") for l in lines):
            return lines
        time.sleep(0.1)
    return bot.out_lines(root)[before:]


def fly_together(expect):
    """TASKS 1.3.13, decision 2026-10-21: in the flight from the Bunker each game flies two Flight Units, the other
    player's puppet sat on one that the game's AI flew in formation (owner's report 2026-10-21: they fly apart).
    In a flight the buddy is the game's and its unit follows the other player. Asserts that in each game the buddy's
    unit is where the other player is, that the flight reaches its battle seen from above (camera far away), and that the client's game does not
    wait in the dark for the next block of the flight (20_AB_buker_Aerial_battle is loaded again under the same name;
    before 2026-10-22 the client's held phase request waited for a phase name change that never came)."""
    errors = {HOST: [], CLIENT: []}
    cameras = {HOST: [], CLIENT: []}
    for _ in range(12):
        for slot in (HOST, CLIENT):
            lines = entities(slot, ["state", "coop"])
            me, net = position(parse(lines[0], "player")), position(parse(lines[1], "pos"))
            if parse(lines[0], "cam_dist"):
                cameras[slot].append(float(parse(lines[0], "cam_dist")))
            units = [position(l.split(" pos=")[1].split()[0]) for l in lines if l.startswith("entity") and " model=10010 " in l]
            units = [u for u in units if me and abs(u[0]) < 100]
            if me and net and len(units) >= 2:
                other = max(units, key=lambda u: sum((a - b) ** 2 for a, b in zip(u, me)))
                errors[slot].append(sum((a - b) ** 2 for a, b in zip(other, net)) ** 0.5)
        bot.cmd(HOST, ["wait 1500"], quiet=True)
    bot.cmd(HOST, ["shot fly_host"], quiet=True)
    bot.cmd(CLIENT, ["shot fly_client"], quiet=True)
    for slot, who in ((HOST, "the client's"), (CLIENT, "the host's")):
        e = sorted(errors[slot])
        median = e[len(e) // 2] if e else None
        print(f"  observed: {slot} puppet unit off the network position: {[round(x, 2) for x in e]}")
        expect(len(e) >= 6 and median < 1.0,
               f"in {slot}'s game {who} Flight Unit flies where that player is (median {median if median is None else round(median, 2)} m of {len(e)})")
    # Owner's report 2026-10-22: with the buddy as a puppet the flight never turned into the battle from above (camera
    # 13-16 m, the unit stayed a mech and fired down). Alone the camera goes from 20 m to ~46 m after ~12 s of each block.
    for slot in (HOST, CLIENT):
        far = max(cameras[slot]) if cameras[slot] else None
        print(f"  observed: {slot} camera distances: {[round(x, 1) for x in cameras[slot]]}")
        expect(far is not None and far > 35.0, f"in {slot}'s game the flight reaches the battle seen from above (camera up to {far if far is None else round(far, 1)} m)")
    held = [l for l in log_text(CLIENT).splitlines() if "Client tried to change phase" in l]
    released = [l for l in log_text(CLIENT).splitlines() if "Letting the held phase request" in l]
    print(f"  observed: the client's phase requests held {len(held)}, let through {len(released)}")
    state = bot.cmd(CLIENT, ["state"], quiet=True)[-1]
    expect(parse(state, "phase_held") == "0" or len(released) >= len(held) - 1,
           f"the client's game is not stuck with a held phase request (held {len(held)}, let through {len(released)})")



DEATH_STATES = {"0x33", "0x34", "0x35", "0x36"}


def death(expect, save=1, first=CLIENT):
    """TASKS 1.4.2 / 1.4.4, co-op concept 6 point 5 (decision 2026-10-23): both load the "Test" save (Bunker, route A),
    the client plays 9S, which is the host's puppet. The puppet can't be hurt in the host's game. The client dies
    (bot `die`, the way the game kills): the host's character dies too, the host's puppet doesn't play the death, the
    client's Game Over waits for the host's. Both are back in the world and in the session: in the Bunker the Game
    Over loads at once, in the open world (save 0: finished save) each presses Continue on the Body Report.
    first=HOST: the host dies, the client's character dies too and its Game Over follows the host's."""
    other = HOST if first == CLIENT else CLIENT
    result = connect(expect, save, save)
    if result is None:
        return
    if save == 1:
        lines = entities(HOST)
        puppet = [l for l in lines if l.startswith("entity") and " model=10200 " in l]
        print("  observed: 9S in the host's game: " + (puppet[0].split(" rot=")[0] + " " + " ".join(puppet[0].split()[-3:]) if puppet else "none"))
        expect(bool(puppet) and float(parse(puppet[0], "muteki") or 0) >= 250, "the client's puppet in the host's game can't be hurt (muteki timer kept up)")

    expect(bot.cmd(first, ["die"], quiet=True)[-1].startswith("ok"), f"{first}'s character dies (bot die)")
    other_dying = False
    for _ in range(15):
        state = bot.cmd(other, ["wait 1000", "state"], quiet=True)[-1]
        other_dying = other_dying or parse(state, "pl_state") in DEATH_STATES
        if other_dying:
            break
    print(f"  observed: {other} state " + str(parse(state, "pl_state")))
    expect(other_dying, f"{other}'s character dies too (shared death)")
    expect("dies too (shared death)" in log_text(first), f"{first}'s mod sent its death as shared")
    expect("their puppet here doesn't" in log_text(other), f"{first}'s puppet in {other}'s game doesn't play the death")

    # Each dying ends in state 0x36, which asks for the Game Over; the client's request waits for the host's.
    for _ in range(30):
        if "Letting the held phase request" in log_text(CLIENT):
            break
        bot.cmd(HOST, ["wait 1000"], quiet=True)
    for line in [l for l in log_text(CLIENT).splitlines() if "[Death]" in l or "Client died" in l or "Letting the held" in l][-5:]:
        print("    client: " + line.split("] ", 2)[-1])
    for line in [l for l in log_text(HOST).splitlines() if "[Death]" in l or "asks for the next phase" in l][-5:]:
        print("    host: " + line.split("] ", 2)[-1])
    expect("the Game Over waits for the host's" in log_text(CLIENT), "the client's Game Over waited for the host's")
    expect(re.search(r"Letting the held phase request .* the host's Game Over loaded", log_text(CLIENT)) is not None,
           "the client's Game Over went on after the host's")

    # The Body Report (open world) waits for Continue, the second item; the Bunker loads at once.
    back, reported = {}, set()
    for _ in range(40):
        for slot in (HOST, CLIENT):
            state = bot.cmd(slot, ["state"], quiet=True)[-1]
            if parse(state, "loading") == "0" and parse(state, "pl_state") not in DEATH_STATES | {None}:
                back[slot] = state
            elif parse(state, "loading") == "0" and parse(state, "pl_state") == "0x36" and slot not in reported:
                # The menu comes a few seconds after the report text; presses before it are lost. The Bunker's
                # Game Over is in 0x36 for a moment too, then loads: no report there.
                state = bot.cmd(slot, ["wait 6000", "state"], quiet=True)[-1]
                if parse(state, "loading") == "0" and parse(state, "pl_state") == "0x36":
                    bot.cmd(slot, [f"shot body_report_{slot}", "pad DOWN 120", "pad A 120"], quiet=True)
                    reported.add(slot)
        if len(back) == 2:
            break
        bot.cmd(HOST, ["wait 2000"], quiet=True)
    for slot in (HOST, CLIENT):
        print(f"  observed: {slot} after the Game Over{' (Body Report, Continue)' if slot in reported else ''}: "
              + (back.get(slot) or bot.cmd(slot, ["state"], quiet=True)[-1])[:200])
        expect(slot in back, f"{slot} is back in the world after the Game Over")
    if save == 0:
        expect(reported == {HOST, CLIENT}, "both games showed the Body Report")
    # TASKS 1.4.3: after the reload the client takes over 9S again.
    expect(parse(back.get(CLIENT, ""), "model") == "10200", f"the client plays 9S again ({parse(back.get(CLIENT, ''), 'model')})")
    host_coop = bot.cmd(HOST, ["wait 6000", "coop"], quiet=True)[-1]
    client_coop = bot.cmd(CLIENT, ["coop"], quiet=True)[-1]
    expect(parse(host_coop, "session") == "1" and parse(client_coop, "session") == "1", "both are still in the session")
    expect(parse(host_coop, "other_story") == parse(back.get(CLIENT, ""), "phase"), f"the games are in the same phase again ({parse(host_coop, 'other_story')})")


if __name__ == "__main__":
    main()
