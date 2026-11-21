# Cheats: the codes and the Settings > Cheats page

Each game keeps its own cheat codes, entered its own way (below). Codes are all the game's own, so they work in the
app and in the developer runner. Each game's Settings > Gameplay > CHEATS page adds switches for the gameplay
cheats, which leave the saved game alone, and Unlock Everything, which does what the game's unlock codes do in two
steps ("Sure?", then "Unlocked"; one step alone does nothing) and is saved with the game at its next save
(docs/SETTINGS.md).

## Aggressive Inline

How to use: Options > Cheats, then type a code on the game's keyboard (on the host keyboard `w a s d` are the arrow
keys). The game compares the typed text with its codes (`inlineMenuCheat`, `games/aggressive/front_end.cpp`).

| Code | Effect |
|---|---|
| `sddsasasddd` (D R R D L D L D R R R) | all levels in every mode, the secret skaters, the editor pieces |
| `wwssadadBABA` (Konami) | every level unlocked for every skater (the career level select) |
| `SKELETON`, `wswsadadABABS`, `aaddadswwsAI`, `KHUFU`, `BIGUPYASELF`, `JUSTIN BAILEY`, `aaaaddddadadw`, `QUEZDONTSLEEP`, `BAKABAKA` | gameplay cheats (low gravity wall rides, juice regeneration, invulnerability, perfect grind / hand plant / manuals, super spin, ...) |

The Settings page's seven switches are the game's seven named cheats, each writing the tuning entries its code
writes (docs/TUNING.md):

| Switch | Tuning entries |
|---|---|
| Low Gravity Wall Ride | Wall Ride Gravity 3 |
| Juice Regeneration | Max Juice Degradation -0.2 |
| Invulnerability | Juice Lost/Level on crash 0 |
| Perfect Grind, Perfect Handplant, Perfect Manual | Grind, Stall, Manual Balance Factor and Init 0 |
| Super Spin | Air Rotate 640 |

A typed code also sets a cheat bit in the career save, which the game reads only at level setup; the switches leave
that bit alone. BAKABAKA writes no tuning entry and has no switch.

Unlock Everything does the work of the D R R D code and the Konami code, and opens the Media Viewer's cutscenes:
every level icon shows in level select. `ZX_DEV_UNLOCK_ALL=1` does the same in the front end for test runs
(DEVTOOLS.md). The switches and Unlock Everything are `games/aggressive/overrides/aggressive_inline_gameplay.cpp`.

## Dave Mirra BMX 2

How to use: at the main menu, press the code's buttons (1 Up, 2 Down, 3 Left, 4 Right, 5 Square). The game
(`bmxCheckCheat`, games/dm2/menu/cheats.cpp) looks for each code in the last 32 presses. Codes need the second pad
port empty, which it is without a second gamepad. The general codes (`g_generalCodes`) work for characters 0-16:

| Code | Opens |
|---|---|
| `13241134125` (Up Left Down Right Up Up Left Right Up Down Square) | every level for every rider |
| `1423441234134425` | everything: the hidden riders, and every rider's bikes, looks, sponsors, levels and tricks |
| `13242124335`, `13242112445`, `13243343125` | the park editor's themes, its objects, the movies |
| `13244312145`, `13244332135`, `13242242235` | the first and second hidden riders, every bike |

Each character also has a code for its own bikes, competition, movie, sponsors, levels and first trick. The game's
codes are all unlocks; the Settings page adds Perfect Grind and Perfect Manual as switches, and its Unlock
Everything does what all the general codes do.

## BMX XXX

How to use: Extras > Cheats, then a code on the game's keyboard (letters, digits and a space; END enters it). The
game compares the typed text with its table of codes (`bmxmenuscriptCheat`, games/bmx/ui/menu_script.cpp; the table
`g_cheatCodes`: 54 rows on the PS2 and the Xbox, 55 on the GameCube), and a code that matches closes the keyboard.
What a code opens is written to the saved game, so it stays with the save. Each row has a kind and an index: a level
opened for every rider, a movie opened in the movie gallery, a rider unlocked, a rider's every bike, or a feature
opened and turned on (index -1: every one of the kind).

The level, movie, rider and bike codes are the same in all three releases; the rows marked otherwise differ.

| Code | What it opens | Releases |
|---|---|---|
| `XXX RATED CHEAT` | every level (the GameCube's Secret Level and test levels' rows 8-11 too), movie, rider and bike; on the PS2 every feature as well | all (the Xbox's and GameCube's open no feature) |
| `MASS HYSTERIA` | every level (rows 0-11) for every rider | all |
| `CHAMPAGNE ROOM` | every movie | all |
| `I LOVE WOOD` | every rider | all |
| `65 SWEET RIDES` | every rider's every bike | all |
| `YOU A PIMP` | every feature (the PS2's six) | PS2 only |
| `BRONXCHEER`, `THATDAMLEVEL`, `BAABAA`, `BOYBANDSSUCK`, `SHOWMETHEMONEY`, `SHOWMETHEMONKEY`, `IOWARULES`, `UNDERGROUND` | one level each: The Bronx (open from the start), The Dam, Sheep Hills, Syracuse, Las Vegas, Launch Pad 69, Rampage Skatepark, UGP Roots Jam | all |
| `XXXINTRO`, `Z AXIS` | movie rows 0 (BMX TRAILER) and 1 (the Z-Axis logo, which the PS2's gallery does not list); both open in a new game | all |
| `FREESAMPLE`, `LAPDANCE`, `STRIPTEASE` | The Bronx's movies 3, 1 and 2 | all |
| `BOING`, `THONG` | The Dam's movies 1 and 2 | all |
| `THISISBMX`, `BURLESQUE` | Rampage's bonus movie 1 (RA_01) and movie 2 | all |
| `ONEDOLLAR`, `69` | Sheep Hills' movies 1 and 2 | all |
| `FUZZYKITTY`, `MICHAELHUNT` | Syracuse's movies 1 and 2 | all |
| `KEEPITDIRTY`, `BOOTYCALL` | UGP Roots Jam's bonus movie 2 (UG_01) and movie 2 | all |
| `HIGHBEAMS`, `TASSLE` | Las Vegas's movies 1 and 2 | all |
| `IFLINGPOO`, `PEACH` | Launch Pad 69's movies 1 and 2 | all |
| `DDUULRRLDRSQUARE` | the FINAL MOVIE | all |
| `ELECTRICITYBAD` | the rider Amish Boy (character 12, the one locked in a new game) | all |
| `TWAN187`, `MIKA362436`, `RAVE10`, `LATEY411`, `ITCHI594`, `KARMA311`, `MANUEL415`, `HELLKITTY487`, `TRIPLEDUB922`, `JOYRIDE18`, `SKEETER666`, `NUTTER290`, `AMISHBOY1699` | the named rider's every bike (characters 0-12: their bikes 1 to 5) | all |
| `GHOSTCONTROL` | feature 0, GUIDED GHOSTRIDES (the stick steers a ghost ride) | all |
| `HEAVYPETTING` | feature 1, SUPER CRASH MODE | all |
| `PARABOLIC` | feature 2, VISIBLE GAPS | all |
| `3RD SOG` | feature 3, NIGHT VISION MODE | all |
| `FLUFFYBUNNY` | feature 4, HAPPY BUNNY MODE | all |
| `BULLETPOINT` | PS2: feature 5, THE PARK EDITOR (STRING.ZAL KSTRSP_REWARD_FEATURE05, KSTR_FEATURENAME_05), which no code of the PS2 game reads; Xbox and GameCube: feature 7, the Xbox's park editor (its main menu builder adds PARK EDITOR on it; the GameCube has no park editor and no text for 7) | all, as said |
| `MAKEMEANGRY` | feature 5, GREEN SKIN (THE GREEN MEN) | Xbox and GameCube |
| `NICE TATAS` | feature 6, TOPLESS GIRL | GameCube only (the Xbox has its texts and no code row) |

Features 2-4 have no reward that gives them: their codes are the only way to them. The rows' movie numbers are the
saved game's movie rows (SavedGame::movies); the PS2 gallery lists 19 of the 20 (mediaMovies), Z-Axis not.

In the app, BULLETPOINT on the PS2 sets its feature bit and nothing reads it, so it shows nothing. With Settings >
Unused Menu Lines on (`[extras] menu_lines`, off by default) the main menu gets its PARK EDITOR line once BULLETPOINT
has been entered, as the PS2's text for feature 5 says it should (bmx_xxx_exit.cpp); with Park Editor on (`[extras]
park_editor`) the line is there anyway.

The game's codes are all unlocks; the Settings page adds Perfect Grind and Perfect Manual as switches, and its
Unlock Everything does what every code in the game's table does (the special modes opened, not turned on).

## The added content and the cheats

When a game's unlock-everything runs, the content the collection adds opens the way that game's own content opens.
A toggle that is off hides its content even when the content is unlocked, and no cheat turns a toggle on.

- Aggressive Inline: the Zoo with Zoo Unlock set to Museum is a skater's level bit 8. The Konami code and Unlock
  Everything open it. The D R R D code does not, and it leaves the game's own free-skate levels locked too. The Ape
  unlocks with the Mummy, which the D R R D code and Unlock Everything open. Estate, Xbox Free Skate, tuning,
  scripts and camera have no lock.
- Dave Mirra BMX 2: Venice and Greenville open for the riders a cheat covers. The levels and everything codes open
  them through `dave_mirra_xcareer.cpp`'s hook, and Unlock Everything opens them the same way. With Create Player
  on, riders 17-20 get whatever a code or Unlock Everything opened for every one of riders 0-16: levels (Venice and
  Greenville too), bikes, looks, sponsors and tricks (`dave_mirra_cheats.cpp`). Back from a created rider's level
  select goes to Create Rider. The Xbox themes and Retro are open whenever their toggles are on.
- BMX XXX: XXX RATED CHEAT, MASS HYSTERIA and Unlock Everything open level rows 8-11 (the GameCube levels) for every
  rider. CHAMPAGNE ROOM and Unlock Everything open every movie row, the Z-Axis logo's (row 1, open from a new game)
  among them. Pool and Trash Can Pieces is the one exception to the toggle rule: with the toggle off, a saved park
  that uses those pieces still turns them on, so the park loads.
- Custom Freeplay is the collection's own tile (`[general] custom_freeplay`). It has no lock.
