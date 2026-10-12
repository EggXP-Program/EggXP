# EggXP

**A native Egg, Inc. account and mission-history viewer that runs on Windows XP SP3 and everything after it.**

EggXP is a single ~360 KB Win32 executable written in C. You type in your Egg, Inc. ID (EID), click **Fetch**, and it downloads your cloud backup and every spaceship mission you've flown, including the loot from each one. It shows your Soul Eggs, Prophecy Eggs, Golden Eggs, Earnings Bonus and the rest of your account, and can show every field the server sends back.

It needs no installer, no .NET or runtime, and no DLLs beyond the ones Windows ships with. It doesn't use the OS certificate store either, so it works on a stock, unpatched XP SP3 box.


<table>
<tr>
<td><img src="docs/account.png" alt="Account tab"></td>
<td><img src="docs/backup.png" alt="All backup data tab"></td>
</tr>
</table>

<sub>Screenshots use a synthetic test account.</sub>

---

## Features

### Missions tab
- Lists every completed and archived mission: launch and return time, ship, duration, star level, capacity, sensor target, Home or Virtue.
- Shows the loot for each mission, grouped by item, tier and rarity.
- **Lifetime totals** shows every item you've ever pulled, across all missions.
- Click any column header to sort.
- **Export CSV…** writes one row per dropped item, ready for Excel (UTF-8 with BOM).

### Account tab
- **Eggs:** Soul Eggs, Prophecy Eggs, Truth Eggs (total and per Virtue egg), and Golden Eggs (balance, lifetime earned, lifetime spent).
- **Currency:** Shell scripts, piggy bank, unclaimed eggs.
- **Earnings Bonus and farmer role:** includes the Soul Food and Prophecy Bonus research levels.
- **Progress:** prestiges, highest egg reached, lifetime cash, drone takedowns, boosts used, achievements, global rank.
- **Contracts:** grade, total and season CXP, soul power, active and archived counts.
- **Artifacts:** crafting XP, inventory score, inventory size.
- **Eggs of Virtue:** resets and shifts.
- **Current home farm:** egg, chickens, eggs laid, cash.
- **Lists:** every epic research level and every owned boost.
- **Summary line:** SE / PE / TE / GE / EB is always visible above the tabs.

### All backup data tab
- Shows **every field the server returned**, decoded and named using the community [`ei.proto`](https://github.com/carpetsage/egg).
- Enums are shown by name, big numbers in game notation (`5.000T`, `3.200s`), and timestamps as local dates.
- Repeated entries show their contents inline, e.g. `epic_research[0]  {ResearchItem}  id: "soul_eggs", level: 140`.
- Fields the proto doesn't know yet still appear, as raw values.
- **Save all data…** writes the summary plus the complete decoded backup to a text file.

### Fetch behavior
- Downloads 4 missions at a time, with retries and backoff.
- **Stop** and resume.
- Caches mission loot locally, so a later Fetch only downloads new missions.

---

## Download and use

1. Download `EggXP.exe` from the [Releases](../../releases) page.
2. Put it in a folder it can write to (it keeps a cache folder next to itself), then run it.
3. Enter your EID (`EI` followed by digits; it's in the game's settings menu) and press **Fetch**.

Requirements:

- Windows XP SP3 or newer, 32- or 64-bit.
- A reasonably correct system clock. The server's certificate is checked against the current date.
- A direct internet connection. HTTP proxies aren't supported.

> [!WARNING]
> Your EID is effectively the key to your account data: anyone who has it can pull everything EggXP shows. Don't post it or leave it visible in screenshots. EggXP masks it by default (tick **Show** to reveal it).

---

## How it works

EggXP talks to the same unofficial Egg, Inc. API that the game and community tools like [EggLedger](https://github.com/DavidArthurCole/EggLedger) use:

| Endpoint | Request | Response |
|---|---|---|
| `POST /ei/bot_first_contact` | `EggIncFirstContactRequest` | `EggIncFirstContactResponse` with your full cloud `Backup` |
| `POST /ei_afx/complete_mission` | `MissionRequest` (one per mission) | `AuthenticatedMessage` wrapping `CompleteMissionResponse` (optionally zlib-compressed) |

- **Host:** `ctx-dot-auxbrainhome.appspot.com`, Auxbrain's Google App Engine backend.
- **Request bodies:** form-encoded `data=<base64 protobuf>`.
- **Responses:** base64 text.
- **Client identity:** requests identify as the iOS client 1.35.7 (build 111343, client version 72), matching EggLedger.

### Making it work on XP

- **TLS 1.2 in-process.** XP's SChannel only speaks TLS 1.0, which Google's front ends refuse. EggXP uses [BearSSL](https://bearssl.org/) for TLS 1.2, with an embedded copy of the Mozilla root store (including Google Trust Services GTS Root R1/R3/R4), so XP's out-of-date certificate store doesn't matter.
- **No runtime dependencies.** Static MinGW-w64 build, `msvcrt.dll` CRT, PE subsystem version 5.01. Every imported function exists on XP SP3.
- **Hand-written protobuf.** Requests are encoded and responses decoded by a small C protobuf reader/writer. The "All backup data" view uses reflection tables generated from `ei.proto`.
- **Stack alignment.** 32-bit Windows only guarantees 4-byte stack alignment for new threads. Everything is built with `-mincoming-stack-boundary=2`, so BearSSL's AES-NI / PCLMUL / SSE2 paths realign the stack before using aligned stack operands. Without it, the program crashes during the TLS handshake on any CPU with AES-NI; that was the 1.0 bug.

### Formulas

- **Earnings Bonus** uses the same community formula as EggLedger:

  ```
  EB% = SE × (10 + Soul Food level) × (1.05 + Prophecy Bonus level / 100)^PE × 1.01^TE
  ```

- **Farmer role** follows the usual community scale (Farmer I → Kilofarmer → … → Infinifarmer), one tier per order of magnitude of EB%.

---

## Data and privacy

- EggXP only connects to the Egg, Inc. API host. It has no telemetry, no analytics and no update check.
- **Mission cache:** raw mission replies are stored in `EggXP-cache\<EID>\` next to the exe. If that folder isn't writable, they go in `%APPDATA%\EggXP\cache`. Delete the folder to force a full re-download.
- **Saved EID:** the last EID is stored in `HKCU\Software\EggXP\LastEID`, in plain text.

---

## Building

The release is cross-compiled on Linux with MinGW-w64:

```sh
sudo apt install mingw-w64        # Debian / Ubuntu
./build.sh                        # -> EggXP.exe
```

`build.sh` compiles BearSSL once into `build/`, then builds and statically links `EggXP.exe`.

### Regenerating the field tables

If `ei.proto` gains new fields:

```sh
pip install grpcio-tools
cp /path/to/new/ei.proto proto/ei.proto
python3 tools/gen_schema.py       # rewrites src/ei_schema.inc
./build.sh
```

### Source layout

```
src/app.c             Win32 GUI, worker threads, cache, CSV / text export
src/net.c             sockets + BearSSL TLS 1.2 + small HTTP/1.1 keep-alive client
src/ei.c              Egg, Inc. requests/responses, mission + loot parsing, display names
src/pbx.c             schema-driven backup decoder + account stats
src/util.c            buffers, base64, protobuf reader/writer
src/cli.c             console test harness for the core (not part of EggXP.exe)
src/ei_schema.inc     generated from proto/ei.proto by tools/gen_schema.py
src/trust_anchors.inc Mozilla root store, generated with BearSSL's `brssl ta`
third_party/BearSSL   TLS (MIT)
third_party/tinf      zlib inflate (zlib license)
```

---

## Known limitations

- **No live server data.** EggXP shows what's in your cloud backup. Live data such as current co-op status, active events and the contract list come from other endpoints, which aren't implemented yet.
- **No proxy support.** IPv4 and direct connections only.
- **Fixed client identity.** The app version it reports is hard-coded. If Auxbrain starts rejecting old clients, it will need bumping in `src/ei.c`.

---

## Credits

- [EggLedger](https://github.com/DavidArthurCole/EggLedger), for documenting the mission-history API flow this is based on.
- [carpetsage/egg](https://github.com/carpetsage/egg), for the community-maintained `ei.proto`.
- [BearSSL](https://bearssl.org/) by Thomas Pornin, and [tinf](https://github.com/jibsen/tinf) by Jørgen Ibsen.

## License

Third-party components keep their own licenses: BearSSL is MIT (`third_party/BearSSL/LICENSE.txt`) and tinf is zlib (`third_party/tinf/LICENSE`).

<!-- TODO: choose a license for EggXP's own code (e.g. MIT) and add a LICENSE file. -->

## Disclaimer

EggXP is an unofficial fan project. It isn't affiliated with or endorsed by Auxbrain Inc. It uses an undocumented API that Auxbrain hasn't sanctioned for third-party use; use it at your own risk. *Egg, Inc.* is a trademark of Auxbrain Inc.
