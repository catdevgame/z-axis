# Mods

A mod adds to the games or changes their files. No mod is needed: with none, every game runs as it shipped. Mods go in
the collection's **Mods** folder, in its support folder:

- macOS: `~/Library/Application Support/Z-Axis Collection/Mods`
- Windows: `%APPDATA%\Z-Axis Collection\Mods`
- Android: `Android/data/io.github.catdevgame.zaxiscollection/files/support/Mods` on the device's storage

A mod is a folder there, or a `.zip`. A zip is read as it is: the app never unpacks it, so keep a mod as either the
folder or the zip, never both. A zip of one folder (what zipping a folder makes) is read from inside that folder.

## mod.ini

Each mod has a small description, `mod.ini`, at its top:

```ini
[mod]
id = hd-rails          ; unique: the settings know the mod by it
name = HD Rails        ; shown on the Mods page
version = 1.2
author = you
games = ai, dm2        ; the games that read it: ai, dm2, bmx
needs = base-pack      ; optional: mods it is read after, and only with
conflicts = old-rails  ; optional: mods it is not read with
```

A folder in Mods with no `mod.ini` naming its games is listed on the Mods page but not read.

## What a mod can hold

What a mod holds is told by where its files are. They use the paths the games already load:

| In the mod | What it does | Changes game logic |
|---|---|---|
| `ASSETS/...`, `MOVIES/...`, any disc path | replaces that file of the game's disc (names in any case) | yes |
| `Music/01 Song.ogg` (`.wav`, `.flac`, `.mp3`) | song 01 of the soundtrack | no |
| `tuning.ini` | Aggressive Inline's developer tuning values (docs/DEVTOOLS.md); in a mod for Dave Mirra BMX 2 or BMX XXX alone not read (the page notes it) | yes (Aggressive Inline) |
| `Textures/ai/`, `Textures/dm2/`, `Textures/bmx/` | replacement pictures for the GPU renderer: PCSX2's file names, or `.xtp` packs. A PCSX2 `textures/<serial>/` folder can be copied in as it is (`Textures/SLUS-20327/`); a mod of one game may put them in `Textures/` itself | no |
| a `[map]` section in `mod.ini` (or a `map.ini`) | a custom map for Custom Freeplay (docs/CUSTOM-LEVELS.md, SETTINGS.md "Custom Freeplay"); no other start reads it | yes |

Files for a game the mod does not list (say `Textures/bmx/` in a mod with `games = ai`) are not read; the mod's page
says so. The table's last column says which parts change game logic.

## The Mods page (Settings > Mods)

Every mod in one list, in the order the games read them, each with its state (On, Off, On in some games, Not read, Map).
Choose one (keys, a pad, the mouse or a touch) for its page:

- where it is, who made it and what it has;
- a switch for each game it is for (On or Off, kept in that game's `settings.ini`, `[mods]`), and why a game does not
  read it when it does not (switched off, a mod it needs missing or not read, a mod it conflicts with read, an id
  already taken);
- **Move Up** and **Move Down**: its place in the order;
- where it and another mod have the same file: which one the game reads ("wins 3 over HD Rails", "loses 1 to Big
  Pack").

A later mod's file wins over an earlier one's. A mod comes after the mods it needs whatever its place. Changes apply at
the game's next start.

## Mods from before mod.ini

These still work, unchanged:

- each game's own Mods folder (`games/<id>/Mods`): loose files there (`Mods/ASSETS/...`, `Mods/Music`,
  `Mods/tuning.ini`) are read under every mod, and each folder (or zip) there is a mod of that game alone, known by its
  folder's name. The Mods page lists them with their folder;
- the maps folder (`Maps/<map>/` with a `map.ini`): its maps are on Custom Freeplay's map page with the Mods folder's.
