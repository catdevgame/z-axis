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
| `ASSETS/UILIB.ZAL/CommonFonts.zif`: a folder named as an archive, files named as its entries | replaces just those entries of that archive (see Archives below) | yes |
| `Music/01 Song.ogg` (`.wav`, `.opus`, `.flac`, `.mp3`) | song 01 of the soundtrack | no |
| `tuning.ini` | Aggressive Inline's developer tuning values (docs/TUNING.md, docs/DEVTOOLS.md), and BMX XXX's (its GameCube release's TUNING rows: "tuning.ini" below); Dave Mirra BMX 2 reads none (in a mod for it alone, the page notes it) | yes (Aggressive Inline, BMX XXX) |
| `Textures/ai/`, `Textures/dm2/`, `Textures/bmx/` | replacement pictures for the GPU renderer, PNG or DDS (SETTINGS.md "Texture packs"): PCSX2's file names, or `.xtp` packs. A PCSX2 `textures/<serial>/` folder can be copied in as it is (`Textures/SLUS-20327/`); a mod of one game may put them in `Textures/` itself | no |
| a `[map]` section in `mod.ini` (or a `map.ini`) | a custom map for Custom Freeplay (docs/CUSTOM-LEVELS.md, SETTINGS.md "Custom Freeplay"); no other start reads it | yes |

Files for a game the mod does not list (say `Textures/bmx/` in a mod with `games = ai`) are not read; the mod's page
says so. The table's last column says which parts change game logic.

Magenta, (255, 0, 255), is the games' colour key: in a paletted or 24-bit picture a mod puts in the games' files (a
BMP, or a picture inside an archive such as a `.zif`), every magenta pixel is drawn transparent, so a picture painted
all magenta vanishes. Any other colour, (254, 0, 254) too, is drawn as it is.

## Archives

Much of the games' data is in `.ZAL` archives (`ASSETS/UILIB.ZAL`, BMX XXX's `ASSETS/CREATE/PARTS.ZAL`...). A mod can
replace a whole archive, as any file, but then it is the only mod that changes it. Instead, a mod can replace single
entries: a folder named as the archive, at the archive's path, holding a file for each entry it replaces, named as
the entry:

```
my-mod/
  mod.ini
  ASSETS/UILIB.ZAL/CommonFonts.zif
  ASSETS/UILIB.ZAL/0x0A1B2C3D
```

The game reads the archive with those entries replaced and the rest as the disc has them. Mods that replace other
entries of one archive all count; where two replace the same entry, the later mod's wins (the Mods page says so). A
whole archive in a later mod replaces them, and entries in mods after a whole archive are put in that archive.
An entry the archive does not have is not read, and the Mods page says so on the mod's page ("Aggressive Inline:
ASSETS/UILIB.ZAL has no entry KeyIcon.zif, not read."). The game's files stay an exact copy of the disc: the archive
is made in memory when the game opens it.

An entry is named by its name (any case), by `0x` and its id (the hash of its name, what the archive stores), or in a
zfile library (Aggressive Inline's `OVERLAY.ZAL`, most of Dave Mirra BMX 2's) by its group's number. An extension
after an id is allowed (`0x0A1B2C3D.zsc`, `12.bin`). `zal.txt` and files starting with a dot are passed over, so a
folder `zal unpack` made can be copied in whole, but ship only the entries you change: a mod that has every
entry wins over every mod before it.

### zal

`zal` (a target of the build, or alone in seconds: `cmake -S iop -B out/zal -DZX_IOP_DISC_ONLY=ON` then
`cmake --build out/zal --target zal`) turns an archive into a folder and back:

```
zal list ARCHIVE                          its entries: id, size, name
zal unpack ARCHIVE DIR [--names FILE]...  a file per entry and zal.txt, in a new or empty DIR
zal repack DIR ARCHIVE                    the archive made from DIR's zal.txt and files
zal verify ARCHIVE [DIR]                  unpacked and packed again it is the same file; with DIR: DIR packs into it
```

An archive unpacked and packed again with nothing changed is the same file, byte for byte: that holds for every
`.ZAL` on the three games' PS2 discs (85 of them), and `unpack` checks it each time. Most archives keep only their
entries' ids, not their names: `unpack` names an entry by any name whose hash is its id, found in the archive itself
or in the files `--names` gives (the game's program, `SLUS_203.27` for Aggressive Inline, names many: 27 of `UILIB.ZAL`'s 28), and
by `0x` and its id otherwise.

`zal.txt` lists the entries in the archive's order (`entry ID FILE`), with what the archive needs to be made again
exactly: its format, its alignment, and any bytes between entries that are not zeros (`pad`). To change an entry,
change its file; its size may change. To add one, add its file and a line `entry - FILE` (its id the hash of its
name). Files not listed are not packed (repack names them).

## tuning.ini

A `tuning.ini` at a mod's top (or loose in a game's own Mods folder) sets a game's tuning values before the game
starts; the last mod that has one wins. With the developer menu on (Settings > Game > Developer Menu), every start
also writes `tuning-reference.ini` beside the `tuning.ini` it read (in the game's own Mods folder when there is none
or it is in a zip): every row the game has, with its value and range.

- **Aggressive Inline**: the game's own tuning table, 405 entries in 20 sections (`[Camera]`, `[Physics]`...):
  every entry and what reading it changes is docs/TUNING.md, the format docs/DEVTOOLS.md "How to use".
- **BMX XXX**: the PS2 release has no tuning table, but its GameCube release has a TUNING screen of 114 rows. Here
  those rows set the PS2 game's own values: one section, `[TUNING]`, the rows by their GameCube names, as in
  `grind off balance factor = 0.2`. 87 of the 114 are paired with the PS2 value each one is, by the code that uses
  them in both releases; 77 of those are in `tuning.ini`. The other ten are not: the eight rider abilities (air,
  speed, spin, flip, balance, technique, manual, wall ride) are the riding rider's, made again from the
  character's stats at each ride, so they are on the live screen alone, and the two feature flag rows are the saved
  game's, not tuning. With the developer menu on, **F2** in a level shows the live TUNING screen, as the GameCube's:
  up and down pick a row, left and right change its value by its step. The 27 rows not paired are on neither: the
  ninth ability, land (no code of both releases reads it), the ten camera rows (none of the code using them could be
  matched between the releases), and 16 more for which no PS2 code uses a value the pairing could take (such as
  the NPC riders' rows, the far clip, the run length and the spline camera test). The PS2's values are the GameCube's defaults but for
  three floats a last bit apart.
- **Dave Mirra BMX 2** has no tuning table: it reads no `tuning.ini`.

A tuning value can change how the game plays; without a `tuning.ini` nothing is read or written.

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
  folder's name. The Mods page lists them with their folder; a folder or zip there with a `map.ini` is a custom map
  (no boot reads it as a mod), on Custom Freeplay's map page, the game the folder's when its `map.ini` names none;
- the maps folder (`Maps/<map>/` with a `map.ini`): its maps are on Custom Freeplay's map page with the Mods folder's.
