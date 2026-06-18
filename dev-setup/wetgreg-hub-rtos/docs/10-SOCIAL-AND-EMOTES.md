# 10 — Social & Emotes (WetGreg-to-WetGreg)

This chapter covers the proximity-social feature: how one WetGreg discovers
another nearby and exchanges an **emote**, entirely **connectionless** (no BLE
pairing, no GATT connection). Files: `bt.c` / `bt.h` (radio), `main.c` (UI,
identity, log).

---

## The model in one paragraph

Each WetGreg has a persistent random **16-bit id**. It bakes that id into a
**manufacturer-specific field of its BLE advertisement** (a "beacon"). When the
**Social** scan is on, a WetGreg watches for other beacons. To "say hello," it
writes a **directed emote** into its *own* advert — `(target_id, emote_code)` —
for ~15 seconds; the target sees it while scanning. No connection is ever made,
so it never touches the secure phone-pairing path and two WetGregs never need to
exchange passkeys.

---

## Identity & names

- `g_saved.wetgreg_id` — random 16-bit id, generated once on first boot and
  persisted (see `saved_load`).
- A WetGreg's **name is derived from its id** (`wetgreg_auto_name`,
  `k_name_adj` × `k_name_noun` = 1024 combos like *Unhinged Trashpanda*). Because
  it's deterministic, **every** WetGreg computes the same name for a given id — so
  names don't need to be transmitted. A custom name (Set Name) is stored locally
  and shown on your own device; others still see the id-derived name (a future
  rev can carry the custom name in the scan response).

## Radio layer (`bt.c`)

The beacon lives in a mutable `adv_data[]` with a manufacturer-specific AD:
`[company 0xFFFF]['D']['L'][id16][target16][emote8]`.

**Threading rule (critical):** the app task only ever sets *volatile intent
flags* (`g_want_target`, `g_want_emote`, `g_want_ttl`, `g_adv_dirty`,
`g_social`). **All** BTstack calls happen in `g_social_tick()`, a single periodic
worker registered once in the run-loop (`HCI_STATE_WORKING`). It reconciles the
scan state, applies a dirty advert at most once per change, and expires the
directed emote. This is what fixed the early softlock — manipulating the run
loop's timer/advert from the app thread corrupted it after a couple of sends.

- `wetgreg_social_send_emote(target, code)` — latest-wins; just sets flags.
- `wetgreg_social_enable(on)` — sets the desired scan state; the tick starts/stops
  scanning at **~15 % duty** (30 ms window / 200 ms interval) to limit radio
  load and advert flood.
- Incoming adverts are parsed in the `GAP_EVENT_ADVERTISING_REPORT` handler into
  a one-slot latch the app reads via `wetgreg_social_poll()`.

## App layer (`main.c`)

- The octopus loop polls `wetgreg_social_poll()`. A new WetGreg → `STATE_SOCIAL_PROMPT`
  ("say hi?"); a directed emote aimed at us → play it, then `STATE_SOCIAL_RECV`.
- **Emotes** (`emote_defs[]`): WAVE / LOVE / LAUGH / PARTY / SLEEPY / WHOA. Each
  maps the octopus to a mood+expression plus an animated overlay glyph
  (`draw_emote_overlay`), drawn in `render_emote_octopus` — which keeps the
  octopus on screen and works in **both** orientations.
- **Prompts hold ~2 minutes** so there's time to react; saying yes logs the peer
  and broadcasts the reply even if they've left range.
- **Social menu**: scan on/off (persisted), **Scan Nearby** (live in-range list),
  **WetGregs Met** (the log), **Set Name**.
- **Met-log** (`g_saved.met[]`, `met_record` / `met_should_greet`): remembers
  peers by id with a **24-hour re-greet cooldown** so it never spams. Note:
  *don't* `met_record` on every sighting — that flash-writes (parks the display
  core); only log on an actual sent/received hello.
- A **Social status icon** (concentric "broadcast" rings) shows while scanning,
  placed right of the WiFi icon and clear of the Bluetooth icon.

## Flash schema note

The identity + social block was appended to `saved_store_t` and `SAVED_MAGIC` was
bumped (→ `'MOP3'`) so old flash images re-seed cleanly instead of reading stale
layout into the new fields.
