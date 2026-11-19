# Custom levels: the Custom Freeplay mode and its maps

How custom maps work across the three games, for the people making maps and the converters. How to install and play
a map is docs/MODS.md and SETTINGS.md "Custom Freeplay". Section 1 is the mode, section 2 what the engine and the
tools give a map, section 3 how each part was built and proved, section 4 the questions settled on the way.

## 1. Custom Freeplay across the games

The user's idea: before picking a custom map, choose a player type from any of the three games: an
inline skater (AI), a Dave Mirra BMX 2 rider or a BMX XXX rider. A fourth entry on the collection's
main menu, freeplay only.

- **The player type picks the game.** Each game keeps its own physics, tricks, rider and camera, so
  "skater" boots AI's code and "rider" boots DM2's or BMX XXX's. The mix is in the collection's
  menu and in the content, not in one game's code: no skater inside BMX XXX's physics.
- **One map, every player.** The three games' engines are versions of one engine: AI is ZGE3,
  DM2 ZGE2 and BMX XXX ZGE4 (`tooling/ai/tools/dm2_level.py` docstring). A level is the same five
  files in each, with per-version chunk differences:
  - AI: `<L>.ZIT .GEO .INS .TXR .DAT`, `MOTIONS.ZAL`, and the scripts `<L>.BIN` (career) and
    `<L>T.BIN` (free skate and timed run), under `ASSETS/LEVELS/<L>/`.
  - DM2: `<L>.ZIT .GEO .INS .TXR .DAT .TXT`; its goals are not in the level but in tables in the
    executable (SLUS_201.59 at 0x233900, 0x1B80 bytes a level), so a freeplay level needs none.
  - BMX XXX: `<L>.ZIT .GEO .INS .TXR .DAT`, `ANIMS.ZAL CUTCOMP.ZAL CUTSCENE.ZAL MODELS.ZAL` and the
    goals as a text file, `OBJCTV.TXT`.
  A map is stored once, as a mod in the collection's Mods folder (`<support>/Mods/<map>/`, a
  folder or a zip read in place, with a `[map]` section in its `mod.ini`: docs/MODS.md) or in the
  older maps folder (`<support>/Maps/<map>/`), as the level files of the game it was made in, laid
  out in that game's slot (`collection/maps.h`). A player of
  that game reads the folder as it is; a player of another game gets the map converted when its
  boot starts, into its own game's files for its slot, in memory: the boot reads them as a memory
  folder (`runtime/ps2_game_files.h` `registerMemoryFolder`, over the mods), nothing is written and
  nothing is cached (80 to 350 ms a map; 3 to 9 MB held while the game runs). The slots: AI's
  free skate level 7 (folder `TUT`, reached through the Estate's entry; the slot's files are the
  map's, whatever level it is), DM2's Woodward Camp level row (`WOODWARD`: a park, with its theme's
  editor objects, left no room for a large level), BMX XXX's The Bronx (`CHL_MULY`). The maps the
  checks ride are three stock levels, one per game, each used as a map: Industrial (AI's career
  level), Woodward Camp (DM2's) and The Underground (BMX XXX's `CMP_ROOT`); of AI's career levels
  only Industrial fits both bike games' budgets. A level whose files
  are larger than the game's largest stock level's is refused before the boot (both converters'
  `LEVEL_BYTES`): it would not fit the game's 32 MB heap, and DM2's park with Woodward ran out and
  never finished loading.
- **Two converters, one result.** The tooling's Python converters (`zge_level_gltf.py` export, then
  `ai_level_convert.py`, `dm2_level_convert.py`, `bmx_level_convert.py` convert) are the authoring
  step: a glTF model, or a stock level, made into a map in one game's files. They are also the
  reference. The app's converter (`runtime/src/collection/levels`) is a native port of the same
  chain from one game's files to another's, with no glTF or PNG in between. Its files are the
  Python chain's byte for byte: `ps2xMapConvert convert` writes them to a folder to compare, and
  `ps2xMapConvert export` prints the model in between. A change to one is a change to both.
- **The scene points across the games.** A level's DAT holds its scene points (SDPT: a kind, the
  hash of a class name; a name hash; an index; a place and a turn; the kind's record). Every kind in
  the three games' stock levels, what reads it, and what a conversion does with it:

  | kind (points: AI, DM2, BMX XXX) | read by | in a converted map |
  | --- | --- | --- |
  | Spawn PlayerOne, PlayerTwo (25+9, 17+0, 22+8) | each game's start (`placeSkaterAtLevelStart`, `spawnPlayer`); PlayerTwo split screen in AI and BMX XXX | carried; DM2 has no second start (its multiplayer takes turns), so PlayerTwo is left out there |
  | Spawn, the respawns: AI's "Respawn" (413), DM2's "Player" (2,349), BMX XXX's other names (about 290) | a fallen player put back at the nearest (AI `respawn`, DM2 `respawnAtStart`, BMX XXX `findSpawnFace`: any name) | carried as `#spawn=respawn`, written with the target's name (BMX XXX's unnamed, as its own) |
  | Spawn named by scripts or modes: AI's lessonOne ... and wickedTree, BMX XXX's sniper_shooter and sniper_target, DM2's one unnamed hash | AI's `setRespawnLocation` script native, BMX XXX's paintball | left out: no script or mode of another game names them |
  | CameraPos "idle" (0, 0, 60) | BMX XXX's idle camera behind a competition's instructions and its GAME OVER (ANOTHER RUN) screen (`idleCameraMode`: points 1 to N in turn, the first tag byte the field of view; none: the level's origin) | carried as `#camera` nodes (`#fov=N`) in all three games' files; BMX XXX's other CameraPos names (" idle", "CHL", a "van" at index 0, which `vanCameraMode` never asks for) have no reader and are left out |
  | DM2's level-point camera (no scene point: five points a level in the executable, `g_levelCameraPoints`) | DM2's mode 4 (a SESSION's or ProQuest's instructions, the leaderboard, multiplayer's time up and game select): 5 m above a point, looking at a point circling it; FREERIDE, the Custom Freeplay start, goes straight to the turn and its RUN FINISHED keeps the ground camera | `dm2_level_convert.py table-cameras` writes a level's row as its CameraPos idle points (the Woodward map has them); DM2's level camera points option (`PS2X_DM2_LEVEL_CAMERA_POINTS=1`, `levelCameraPoint`) takes a level's own in the row's place, and the DM2 slot's start turns it on. A point is the eye 5 m lower in DM2's files |
  | Globals (13, 17, 14) | the clear and fog colours, the fog and far clip (all three) | from the model's sky, as before (each game's record) |
  | Waypoint (6,079, 8,768, 952) | the paths of the level's traffic, people, boats and path men | left out: those objects are the source level's and do not come with a map |
  | AI's OAR zone boxes (1,215), NPC (130), pathhead and pathpoint (151, 189), PARTICLE (181), POINTER (5); BMX XXX's PathMan, FastPathMan, MOTIONLIST, BirdObj (12, 296, 389, 38) | the levels' props, NPCs, critters, emitters and walkers | left out: each names the source level's own models, animations or scripts |
  | AI's sound emitter (192), BMX XXX's SoundEmitter (10) | each game's ambient sounds at a place | left out: the record names the source game's own sounds |
  | the texture effects, flipbook and scroller (AI 63 + 46, BMX XXX 16 + 3) | the engine's `initTextureFx` in AI and BMX XXX (DM2 has none) | left out: they name materials and the flipbooks' picture textures, which a conversion renames or leaves out (only drawn textures go in) |
  | LensFlare (0, 9, 1) | DM2's `placeLevelLensFlare` (its colour word); BMX XXX's has no readable reader yet | left out: BMX XXX's record is not known, so DM2's cannot be written as one |
  | EditAnchor (189, 0, 278), EditLightOmni and EditLightCone (0, 37, 0); AI's unread "start", "end", Competitor and MP Industrial's motion lists | the park editors' pieces and lights; nothing | left out: park editor themes only, or no reader |

  Proved in pictures (the converted files served as a Mods folder): BMX XXX's idle camera
  behind GAME OVER on Rampage's competition row looked from the level's origin on The Underground,
  Industrial and Woodward before, and from their own cameras after (The Underground's are the stock
  level's views: the instructions screen's pictures differ from the disc's own by 6.4 and 5.1 of
  255 a channel, its people the difference); DM2's level-point camera on a SESSION's instructions in
  Woodward Camp's row looked from Woodward Camp's places (43, 3, -151) and (44, -7, 69) on every map
  before, and after from Industrial's made camera (-89, -12.3, 82.6), The Underground's own
  (-7.2, -4.5, 28.9) and (-29.3, -1.0, -76.0), and Woodward's the same places as before.

  A level with no idle camera of its own (every AI level) gets one where a game reads them (BMX XXX,
  DM2): 8 m behind its PlayerOne spawn along its facing, 3 m up, looking at a point 1 m above the
  spawn, 60 degrees. AI reads no camera (its pause and end of run stay on the follow camera), and
  its files keep a map's cameras so they go on to the other games. The cameras and respawns add
  under 300 bytes to a converted map (AI's Tutorial's lesson points left out make it smaller); the
  size guard is unchanged.
- **Freeplay only.** No career, goals or unlocks: spawn, ride, a session score and trick list,
  restart or change map. In AI that is the free skate script (`<L>T.BIN`; the Zoo's prototype
  ships 60-byte empty scripts and loads, so a minimal one is enough); in DM2 nothing; in BMX XXX an
  `OBJCTV.TXT` with no objectives. Goals can come later per game.
- **The menu.** Custom Freeplay, then the player type (three tiles with the games' art), then
  the one list of maps, each once, every one startable by every player (the row says which game
  it was made in and that it is converted when it starts); then the game's own skater or rider
  select. A map that does not convert for a game (over its budgets) does not start, and
  after its first failed start the list shows the converter's reason. A setting (`[general] custom_freeplay`, off by default) shows the mode.

## 2. What the engine and the tools give a map

- **Levels converted into AI's PS2 formats.** `runtime/tools/xbox_convert.cpp` (ps2xXboxConvert)
  makes the Zoo and the Ape from the user's Xbox files at packaging time; the app loads them from
  `Resources/xbox-converted` (`PS2X_XBOX_CONVERTED`, `collection/settings.cpp gameBootConfig`). It converts
  the textures (Xbox DXT into 8-bit BMPs with a 256-colour palette, at most 128 a side, 2,560 KB
  of texels in all, the largest shrunk first) and, in the GEO, only re-cuts the TGFM strip groups
  (60 entries each) and rewrites the MATI layers (`ps2_zaxis_xbox.cpp:810-995`); INS, DAT, ZIT,
  the scripts and MOTIONS.ZAL are the Xbox files copied as they are, and collision (TGCI) and grind
  edges (TGFE) pass through inside the GEO untouched (`runtime/include/runtime/ps2_zaxis_zoo.h:12-36,
  104-125`, `ps2_zaxis_zoo.cpp:489-611`). The closer precedent is the Estate:
  `runtime/tools/gc_convert.cpp` with `ps2_zaxis_estate.h` writes a full AI level set from the
  GameCube release (TUT.ZIT, GEO, INS, TXR, DAT, BIN, TUTT.BIN, MOTIONS.ZAL, TEXTURES.TXT,
  VISIBILITY.TXT, ICONS.UFNT, STILL.ULIB), served as Free Skate's "Estate" in level index 7. Both
  convert a level that already exists in a sister format: neither authors geometry, collision,
  edges, instances or spawns from a model (the draft called this "the engine loads level files we
  wrote"; it loads files we converted). The budget comments are the real numbers: the Zoo at 128 a
  side (3,083 KB) ran the GS texture cache over the CLUTs, hence 2,560 KB, "the PS2 levels' size".
- **Writers that were removed.** Commit 0bc955afc ("The cross-game combos out", 2026-11-05)
  removed converters from DM2's level formats into AI's (written as `LOCAL.{ZIT,GEO,INS,TXR,DAT}`
  and served as AI's level 9) and from AI's into DM2's; the reason in the commit is the user's "one
  engine, three games, nothing mixed". The old tools are in git history (`git show
  0bc955afc^:ps2xRuntime/tools/dm2_level.py` and `ai_level.py`) and are the nearest thing to a
  writer of both chunk sets. Section 7 asks whether a park of our own in three formats is "mixed".
- **Readers.** `tooling/ai/tools/ai_level.py` (AI): FORM/TGEO/TGPT/TGVP/TGFP, TGFE tag buffers,
  TGIH instances, TIDA/TII8/TII6 streams; `collision_triangles` and `grind_edges` (byte 9 bit 0,
  kind byte 8); subcommands `ground`, `edges`, `compare`, `selftest`, `info`. It loads TXR but
  never decodes it. `tooling/ai/tools/dm2_level.py` (DM2 and BMX XXX): TGHD, TGPT, TGPH, TGVP,
  TGFP, TGFN, TGCI (DM2's inline leaves and ZGE4's offset leaves), TGFT, TGFE, TGIH, ZDIR `.ZAL`
  and 4/8-bit BMPs; `tree_faces`, `collision_triangles`, `goal_places`; subcommands `selftest` and
  `info` only. Neither is a level writer (`serialize()` re-emits a parsed tree): the writers are
  `ai_level_write.py`, `dm2_level_write.py` and `bmx_level_write.py`, with their round trips (section 3, step 1). DM2's grind edges are read in
  `playbot/dm2map.py:178-181` and `dm2_xbox.py:975`, not in `dm2_level.py`.
- **The same conversion for DM2's Xbox levels.** `tooling/ai/tools/dm2_xbox.py` converts Venice and
  Greenville into DM2's PS2 formats (TXRH plus TBMP in TXRC; TGFE records of 8 bytes; TGCI leaves
  inline; TGFT dropped; `TEXTURE_BUDGET_KB` 2,560) and has its own `ground`, `edges`, `compare`.
  The workdb's notes on it are the ZGE2 vs ZGE3 differences a writer must know (`workdb.py notes
  layout`: "TGCI, the collision kd-tree", "MATI 0xA0 -> 0x9C", "DM2's collidable faces").
- **A check against the game's own collision.** `ai_level.py ground` and `edges` against the
  runtime's `PS2X_ESTATE_PROBE` and `PS2X_ESTATE_GRINDS` (SETTINGS.md), point by point. The same
  check tests a level we write; `dm2_xbox.py` has the DM2 side (`PS2X_DM2_XBOX_LEVEL_GRINDS`).
- **Three park editors, three formats.** AI's and BMX XXX's
  editor are one in `game/zge/editor` (464 shared bodies; 146 BMX and 226 AI functions stay in the games' folders),
  while DM2 keeps its own. The save formats stay per game:
  - AI: `game/aggressive/level/park_editor.cpp` and `park_*.cpp` (the front end's Park Editor,
    game type 5, four themes from the pieces file `ASSETS/EDITOR/INLINE.PEP`, each theme a grid
    from `Assets\Editor\<theme>\<theme>.map` in cells of 4.572 m (15 ft), maximum height 20; also
    the in-game editor in Cannery, Airfield and Industrial). Parks are level indices 10-13. A
    park saves as a 0x2000-byte block (a 0x48 header, then a mark per cell), on the host as
    `<saves>/Parks/Park NNN.sav`.
  - BMX XXX: `game/bmx/editor/` (AI's editor copied: terrain, rail, object, dynamics and
    test-run tools, pieces file `assets\editor\BMX3.Pep`, four themes, park level 12, the same
    0x2000-byte save). It also reads 8 preset parks from `assets\editor\presets\`, a disc-file
    route for shipping a park. No retail menu reaches the editor: `playbot/bmxedit.py` plays it,
    and `PS2X_BMX_PARK_EDITOR=1` puts it on the app's main menu.
  - DM2: `game/dm2/editor/` (DM2's own: a grid of at most 64 x 64 cells of 5 m, 25 object
    categories, 50 lights, a capacity meter of each object's vertex count plus 200, park level 8,
    a 0x2565-byte save that stays on the emulated card).
  A park built in one does not load in another.
- **A level served from a folder, and a Mods folder.** AI's Estate is served for the Tutorial's
  slot (level index 7, `ASSETS/LEVELS/TUT`) by a provider while the game is not a career, with the
  level select's name lookup wrapped (workdb: bringup-estate.md:36); the Zoo sits in index 8 of
  AI's 14-level table (index 9 was LOCAL, an unused developer level); DM2's tables hold 8 levels
  plus the park, so Venice and Greenville run in Woodward's slot; BMX XXX's `LevelEntry[13]` has
  12 levels plus Mullaly Park. That is how a custom level reaches a level select without a new
  menu path in the game's code. Files are looked up in the game's mods first (its own Mods folder, then the collection's mods that
  name it, in the Mods page's order: docs/MODS.md, `collection/mods.h`; the game gets them
  as `PS2X_MODS_DIR`), then registered providers, then the disc
  (`runtime/include/runtime/ps2_game_files.h`); custom music already lives there. The mode's
  setting is `[general] custom_freeplay`.
- **The bottom screen and the playbot.** `panel_map.py` makes `<LEVEL>.pmap` (a top-down map from
  the collidable faces, rails from `grind_edges`, goals) for AI, DM2 and BMX XXX, and already reads
  a level from a folder (`FolderLevel`, `--zoo`, `--estate`). The playbot (`playbot/nav.py`,
  `dm2map.py`, `bmxmap.py`) builds its navigation map from the disc's level files (collision,
  TGFE rails welded by kind, DAT SDPT points: spawn class 0x46BE, givers) and reports the game's
  own numbers (distance, grinds, rail metres, wall rides, tricks, bails, score). Its level lists
  are hard-coded tuples (`nav.LEVELS`, `dm2map.LEVELS`, `bmxmap.LEVELS`): a custom level is one
  entry each. A park or a level folder gets the same map from its own geometry (`playbot/parkmap.py`): each piece's collidable faces and TGFE grind edges placed as the game places them (the
  saves above), the towers' cells as ground, then nav.py's cells and welded rails; from them the
  features (each rail's straight runs, each wall piece's sides, each piece's ramp faces with the
  way up them) and their run-ups, checked clear on the map (floor at the run-up's height, no piece
  over it, no other rail within reach of its line). The three games' park players (`aipark.py`,
  `bikepark.py`) ride a tour of those features and `modlevel.py` builds a Mods folder's map with it;
  `parkmap.Reached` says which features a ride reached, from the game's state (a grind within 1.5 m
  of a rail, a wall ride, an air begun on a ramp's piece).
- **Detail above PS2 budgets on desktop: textures only.** Settings > Display > Native Meshes
  (`aggressive_inline_native.cpp:986-1000`, `PS2X_RENDER_PATH` native/strict) draws level meshes
  from host vertex buffers instead of through VU1, but from the same GEO the game opened
  (`zge/native/geo_meshes.h:3-8`); there is no host-only mesh (BMX XXX draws its meshes from the draw records, `render_path = records`). Higher-detail host content is textures: the converted Xbox originals ship
  as `.xtp` packs keyed by the PS2 ones. The draft's "richer host version" of the geometry would be
  new engine work; a richer texture set is not.
- **Memory.** The PS2 games run in one malloc arena from the program's end to the top of RAM
  (`reserveMallocArena`: AI 0x1CC94C0 bytes, about 30 MB; the same function in all three, a
  per-game constant); the level takes what it needs from that heap, there is no fixed level
  budget. Extended memory up to 64 MB exists (`runtime/guest_heap.h`) and only the Zoo (its textures
  are 8.7 MB) and DM2's Xbox levels turn it on. A frame's textures live in the GS texture cache
  (DM2: 0xA8000 words, about 2.6 MB; a level without visibility zones flushed it every other
  frame: ai_level.py once dropped the ZPVS and the Museum drew 677 of 963 parts a frame). So: a
  PVS is not optional, and the 2,560 KB texel budget is the whole level's, not a zone's. DM2's
  collision tree splits are s16 hundredths, so a level fits in about +-327 m.

## 3. How it was built

1. **Writers for what the converters do not write.** Done for AI's ZGE3:
   `tooling/ai/tools/ai_level_write.py` is the typed form of a level's GEO, INS and DAT, each
   record with `read` and `write`, the layouts taken from the engine's own loaders
   (`zge/assets/geo.cpp`, `scene/geometry_nodes.cpp`, `scene/scene_points.cpp`,
   `assets/chunk_reader.cpp`, `collision/collision.h`), not from the removed converters (which
   wrote DM2's forms of the same chunks):
   - GEO: `Geometry` { `GeoObject` (TGHD `GeoHeader`, TGTD bytes, TGFR, parts with their TGLO
     `LevelOfDetail` and TMTB, TGCI `CollisionTree`) }; `Part` (TGPH from the streams' counts;
     TGVP positions; TGVN/TGVC/TGVU/TGFN plain or TIVN/TIVC/TIVU/TIFN `IndexedStream`; TGFP
     `Face`; TGFM `StripGroup`, at most 60 entries, `from_strip` cutting a strip as the Zoo
     converter's `restrip` does; TGFE/TGFT `TagBuffer` of `EdgeTag` / `FaceTag` records, the
     grind kinds and surface kinds named from `collision.h`). `CollisionTree.build` makes a tree
     as the exporter does (median splits in hundredths, leaves of 8, depth 11).
   - INS: `Instances` of `Instance` (TGIH, with its TGIT tags as bytes); `Instance.make` places an
     object and fills the world box the exporter writes at +0x24 (the game reads neither box).
   - DAT: `SceneData` of `ScenePoint` (SDPT, kind and name hashes, the class record as bytes) and
     `ViewRegion` (PVSO, both bodies: with the flag word and the older one without it).
   - Round trips (`roundtrip --disc`): all 21 level folders on the disc (the 8 levels, LOCAL, ZOO,
     the 11 MP folders) read, written and compared: every GEO, INS and DAT byte-identical (8,408
     objects, 8,920 parts, 6,505 trees, 11,629 instances, 8,657 points, 218 zones), and
     `ai_level.py`'s collision triangles and grind edges the same from the written files. Every
     padding byte in the stock files is zero, so the writers write zero; the three unread fields
     that are not (TGCI's node byte +1, TGFT +0x0D..0x0F, TGFE +0x0A = 3) are kept in the records
     (`leftover`, `byte_0a`) so the stock files round trip exactly and a level of our own writes 0
     or 3. The TXR writer stays the Zoo converter's (`ps2_zaxis_xbox.cpp`).
   - The game loads what they write: the Tutorial rewritten through the writers and served from a
     Mods folder (`rewrite --out`, `PS2X_MODS_DIR`; the run log shows the five files opened from
     the mod) runs logiccheck's route the gate's way (`PS2X_EE_SCALE=4096` against its own
     readable-off run) to 21/21 MATCH, the gate's own rows. With the 755 indexed streams written
     plain (`--plain-streams`) the state is the same. With the 97 collision trees rebuilt by
     `CollisionTree.build` (`--rebuild-trees stock`, over exactly the faces the stock trees list)
     the route matches to ref16 and diverges at ref17-manual-done (the skater 16.7 m away): the
     tree's partition alone (which leaf lists a face, the order a query meets them) changes the
     game's physics. So a stock level keeps its stock trees, and a level of our own gets the
     builder's trees as its own collision behaviour (then checked against the game's query with
     `ai_level.py ground` / `edges`, step 5). Also found: the stock trees list no face without
     0x2000 but leave out 1,038 faces that have it (478 of the 6,505 trees), and 9,165 faces with
     0x2000 have a vertex index past their part's vertex count; what a tree lists is what is solid.
   The ZGE2 and ZGE4 forms are written (step 6: `dm2_level_write.py`,
   `bmx_level_write.py`).
2. **An authoring path.** Done for AI: `tooling/ai/tools/ai_level_convert.py`
   (with `gltf_model.py`, a standard-library glTF 2.0 and PNG reader and writer) takes a glTF model
   as Blender exports it, with the semantics in the names (or Blender custom properties): a node
   with a triangle mesh is a level object placed by its transform (`#deco`: no collision); a node
   with a line mesh marks grind edges (`#grind`, `#grind=lip|spine|vpole|hpole`, `#metal|#wood`),
   matched to the face edges lying along its segments; a material's tag is its faces' surface kind
   (`#qp`, `#spine`, `#slope=G`, `#wall`, `#2sided`) and its base colour texture the level texture;
   empty nodes are `#spawn=PlayerOne` and `#zone` (`#sees=a,b`; the box from the empty's scale).
   Axes: (x, y, z) in glTF is (x, -y, -z) in the game (a half turn about x, windings kept); UVs as
   they are (the game reads a BMP's rows top down); a spawn's identity orientation faces +z. The
   writers write TGHD/TGPT with indexed normals, colours, UVs and face normals, TGFM groups
   packed several strips each as the stock groups are (the 0x4000 flag is each triangle's true
   winding), TGFE/TGFT tag buffers, a TGCI tree per collidable object, TMTB of MATI records with
   LOCAL's opaque layer words, TXRC/TXRH/TBMP (a plain 8-bit BMP, TXRH id = the name's hash, as in
   every stock texture; TZMP with alpha), SDPT spawns, PVSO zones, the ZIT (TEHD cell 50 and the
   instances' box; the game reads only the first 0x24 bytes), a 60-byte script for `<L>.BIN` and
   `<L>T.BIN` (the Zoo prototype's shape) and an empty MOTIONS.ZAL. Budgets, checked and reported
   at conversion: texels 2,560 KB over the level (the largest textures halved first, said in the
   report), texture side 128 (halved; non-power-of-two sides resampled), strip groups of at most
   60 entries (by construction), 1,024 faces an object (the exporter's own cut: no stock part has
   more; a larger mesh is cut at the median of its faces into `name`, `name_2`, ...), 16,384
   vertices a part (a strip entry's 14 index bits), and parts in view 1,083 (the parts drawn from a
   zone: its own, the zones it does not hide and parts in no zone; the Civic Center's worst zone,
   measured over the seven levels) which the converter cannot fix and reports as an error. What it
   does not do yet: alpha-blended material state (a TZMP draws with the opaque words), lights and
   vertex colour baking (colours come from the model's COLOR_0, full white otherwise), a
   native-mesh copy for desktop. DM2's and BMX XXX's formats are step 6's. COLOR_0's red, green and
   blue are 0x80 at 1.0 and its alpha 0xFF at 1.0, the one vertex alpha every stock level of the
   three games has (at 0x80 a blended material drew at half its alpha, faint and see-through). A
   cut-out texture (clear texels, under a tenth partly clear) halved to fit stays a cut-out: each
   texel solid or clear by its coverage, in its solid texels' colour.
   It builds `ai_level_write.py`'s records and lets them write: a `Part` per material group with
   `StripGroup.from_strip` over a stripper's output, `Face.make` with 0x2000 on what collides,
   `TagBuffer`s from the edge and surface names, `CollisionTree.build` (optional in AI), an
   `Instance.make` per placed object, `ScenePoint.spawn("PlayerOne", ...)` and a `ViewRegion` per
   zone, and a ZIT from the level's name and its world box (section 4). What the records do not
   type yet, and the converter must still produce: the TMTB's MATI materials (the Zoo converter
   rewrites them from the Xbox's; a new level needs them from scratch), each object's TGTD class
   record (`Terrain` is the 4-byte hash alone), the TGFR frame flags and the TGLO distance.
   The test park (`ai_level_convert.py example`, its files scratch only): an 80 x 80 m
   concrete ground in 2 m tiles (3,200 faces, cut into 4 objects), a 12 m wooden quarter pipe
   with a 3 m transition and a metal coping, a 10 m flat bar, a 12 m concrete ledge, the spawn and
   two zones; the concrete texture 256 x 256 so the converter halves it (it did, and said so).
   Served from a mod as TUT and checked in the game: `ai_level.py ground` against
   `PS2X_ESTATE_PROBE` (with `PS2X_ESTATE_PROBE_ANY=1`) 1,600 of 1,600 points within 0.01 m; the
   grind probe found 3 of the 11 file edges, 7 "near" (the coping's six 2 m tile edges, which the
   game welds into one 12 m rail) and 1 none (one of the ledge's two edges; the playbot grinds the
   ledge). The playbot (`playbot/modlevel.py ride`, free skate through the Estate's entry) rode it
   with no help: 3,514 m in 239 s, 5 grinds (45 m, longest 10.6 m), 94 airs off the quarter pipe,
   6 bails, score 35,600 in one 14,000-tick session; hand plants on the coping. Nothing in the
   runtime changed: with no mod the game reads its disc, so logiccheck and the A/B are unchanged.
   Name hashes and props: every converter writes an object's TGHD word as the
   game finds objects by it (GeoObject::nameHash: findWorldObjectByHash, cloneDynamicsDefinition),
   the hash of the whole name, with the name field cut at 31 characters as on the discs (before,
   the word was 0, so nothing in a converted map could be found by name). `#dynamics` on a node
   makes its object a dynamics prop: the mesh's extras `dynamics` hold its ZDYN definition (the
   same form in all three games), written after the objects with the object's name hash as its
   DDEF, and the object gets the DynamicsObject type with the target game's class data (AI's
   impact sounds, BMX XXX's sound name) and its instances the game's prop flags and tags.
   `zge_level_gltf.py` exports a stock level's props so, so Woodward's cones and trash cans and
   The Underground's beer bottles and chairs are props in the other games' maps, knocked about
   by the riders. The app's converter does the same (`collection/levels` export and
   the three converters; the 12 pairs byte for byte the Python chain's).
3. **Gameplay data:** spawn points (AI: SDPT class 0x46BE; DM2: the SPAWN and PLAYER_ONE hashes
   in `dm2_level.py`); grind edges by kind; the free skate script for AI (a minimal `<L>T.BIN`;
   there is no script compiler in the repo, only the disassembler
   `tooling/checks/aggressive_script_dis.py`); the panel map (`panel_map.py` on the folder); the
   playbot's level entries. Facts from the test park: the game puts the skater on the
   ground under the spawn (a spawn 1 m up lands at y 0) facing +z for an identity orientation;
   a spawn needs no tag bytes and the level no Globals point (the camera takes its defaults) and
   no ZPVS (the Tutorial has none either); an empty MOTIONS.ZAL (a ZDIR with no entries) loads;
   the playbot builds its map from the mod folder (`modlevel.py build`) with `ai_level.py`'s
   readers as for a disc level, and finds quarter pipes by instance name (`qpipe`, `spine`,
   `halfpipe`, `bowl`): name them so. A park needs an edge (a wall or a bowl) or ground beyond
   its features: the second ride ended off the 80 m slab, falling.
4. **In the app.** The Custom Freeplay mode (section 1), behind a setting, off by default
   (defaults never change game logic). Custom levels under `<support>/Mods` (looked up before the
   disc already), mapped into a level slot as the Estate is (AI: index 7 or 9; DM2: Woodward's;
   BMX XXX: a preset park's or a level row, to settle). Never in the app bundle, never shared.

   Done for the first case (SETTINGS.md "Custom Freeplay"): the setting
   (`[general] custom_freeplay`, the app's), the fourth tile and its two pages in the menu's own
   look (the player as the three games' tiles, then the maps of the collection's Mods and Maps
   folders with their previews), the map description (`map.ini`: name, game, slot, preview, author;
   `collection/maps.h`), and the boot: the map's folder read last of the mods, the other maps
   left out, and the game started straight into the slot's free play. The player is chosen on
   the game's own skater select (the rosters live in the games' memory, nothing on the discs
   lists them, and a typed list would be a guess): the devstart (`PS2X_DEV_START_LEVEL=ESTATE`,
   `PS2X_DEV_START_SKATER=ask`, now the boot's values, not the environment's alone) presses
   from the boot to the skater select, waits there, then picks the Estate's entry and loads.
   The slot table (`mapSlots`) has three rows: AI's `TUT` through the Estate's entry, DM2's `WOODWARD`, BMX
   XXX's `CHL_MULY`. What crosses
   the games and what each case needs:
   - **Which game runs:** always the player's. The map crosses, never the player: a DM2 rider in
     AI's engine would need DM2's rider physics, animation, models and tricks inside AI's code
     (ZGE2 in ZGE3): out of scope.
   - **An AI skater on a DM2 map:** the map stays in DM2's files; the boot converts it into AI's
     (section 1, "One map, every player").
   - **A DM2 or BMX XXX rider on their own game's map:** done. Each game
     has a slot row and a start route of its own, the same menu and boot carry them (no per-game
     case in the collection beyond the table): DM2's `WOODWARD` (Woodward Camp's level row,
     `dm2_level_convert.py --level WOODWARD`'s form) started as FREERIDE's level select value 0
     (`dave_mirra_devstart.cpp`; the park slot, step 6's form, also loads its theme's editor
     objects and ran out of memory with Woodward); BMX XXX's `CHL_MULY` (The Bronx, the first
     row) started as a HARDCORE TOUR row, the row opened in memory for the pick and put back after
     (`bmx_xxx_devstart.cpp`). A challenge row has no clock (a competition row's run is two
     minutes); the map's `OBJCTV.TXT` has no objectives, and since the game counts a level's goals
     into the player's record as it loads, the start keeps The Bronx's counts and puts them back
     on the ride's end screens (game over saves) and in the menus (`PS2X_BMX_KEEP_LEVEL_GOALS`). The "ask" rider
     select is each game's own (DM2's rider and bike choices).
   - **The slot's NPCs:** a map has the people, riders and traffic its own files place. BMX XXX's
     NPC riders are the level number's (setupNpcs: the executable's records row, `def.NPR.<folder>` of
     `assets/create/defs.zal`, `NPR.Lev<short>.Rider<n>` of `assets/replay.zal`), so The Bronx's three
     stood and rode in every `CHL_MULY` map; the pedestrians (PathMan) and traffic (Car) are level
     objects, the map's own. `map.ini`'s `npcs` (none by default, `slot` for a map made from the
     slot's level) gives BMX XXX's boot `PS2X_BMX_LEVEL_NPCS` 0 or 1 (`bmx_xxx_npcs.cpp`: with 0
     setupNpcs answers as in multiplayer). AI's Tutorial's lesson givers come from its scripts (the
     map's `TUT.BIN`/`TUTT.BIN`) and DM2's park (level 8) has no NPCs, so their slots need no switch.
   - **The sky, in all three games:** `--sky SKY.png` on each converter (`ai_level_convert.py`'s
     `add_sky`: a 28 m dome with the picture, and the level's Globals point with the picture's
     zenith as the clear colour and its horizon as the fog's). Each game draws it the same way (one
     instance with the sky bit is the world's sky node; `drawSky` moves it to the camera and the
     sky program draws it behind everything); what differs is a record per game, `SkyForm`: AI's
     and BMX XXX's sky part has render class 3 (the class gives the sky type at load), DM2's keeps
     flags 0 (ZGE2 gives a sky node's part the sky type at every draw); the Globals record is AI's
     28 bytes, DM2's and BMX XXX's 20 (light or ambient, clear, fog, fog start or depth, far clip;
     BMX XXX clears to black, so without a panorama its sky is black; a DM2 park kept the theme's
     grey Globals and had no panorama). DM2's and BMX XXX's converted skies have no fog (alpha 0, as
     DM2's levels and BMX XXX's competitions): BMX XXX's fog planes at its challenge levels' alpha
     0x1C left 14% of the sky. `dm2_to_ai_level.py` carries a DM2 sky to AI's form.
   - **A DM2 map for AI's skater:** `dm2_to_ai_level.py` reads a DM2 level (a park theme's or a
     level folder's files) and writes AI's `TUT` files from the same pieces: the instanced
     objects' parts (the theme's own editor pieces, which no instance places, left out), their
     surface kinds and grind edges mapped to AI's tags, the textures, the spawn (the park's start
     cell when the level has no PlayerOne). The output is a second map folder with `game = ai`.
   - **Any game's level for any other:** `zge_level_gltf.py export --game ai|dm2|bmx FOLDER`
     writes a level of any of the three games as a glTF in the converters' tags (objects per
     instance, a separate collision part `#hidden`, the panorama `#sky`, surface kinds, `#2sided`
     or `#2sided=collide`, `#layer=` for blended or alpha-tested materials, grind edges, spawns);
     the three converters then make it a map of the target game, and it opens in Blender. A sky
     the model has as its own gives the Globals from what it shows. DM2's park slot moves the
     level so its PlayerOne stands at the start cell's centre and level, and keeps the grid
     within the park buffer (`kParkBytes`: a 64 x 64 park's stream overran it and the load hung).
5. **Prove it plays:**
   - the collision check (files against the game's own query) agrees at every probe point, and
     the grind check at every edge;
   - the playbot rides it, grinds its rails and lands tricks, reported by the game's own numbers;
   - logiccheck and the A/B are unchanged with the setting off.
6. **DM2 and BMX XXX.** The test park through their formats (the `dm2_xbox.py` conversion is the
   model: ZGE2's inline TGCI leaves and 8-byte TGFE; ZGE4's offset leaves and `OBJCTV.TXT`), the
   same checks, plus bike spawns. Then all three player types ride it.
   BMX XXX: done for the test park.
   - The formats, from BMX XXX's own loaders (the kIsBmx branches of game/zge, game/bmx): ZGE4 is
     ZGE3 except for the tag records, TGFE of 8 bytes and TGFT of 12 (DM2's sizes, read by
     game/bmx/rider/collision/tags.cpp; `tooling/ai/tools/zge_tags.py`, shared with DM2's writer:
     `EdgeTag8`, `FaceTag12`), the corner tags TGFV (`CornerTag4`: one index per face corner, on
     the fast cars only, their skitch holds), more top-level GEO chunks (ZDYN, ZSCD/ZSCR cut
     scenes, ZFPH/ZFPD/ZFPT fast paths, WSMC, ZPEC; kept as they are) and the goals in
     `OBJCTV.TXT`. The collision tree is AI's offset-leaf form: in all 3,228 stock trees the first
     word is the collision part's id, a name hash. TGIH, SDPT, PVSO, TEHD, MATI and the TXR are
     AI's layouts. `bmx_level_write.py` types them on `ai_level_write.py`'s records (its classes
     take their record types from class attributes: `BmxPart`, `BmxObject`, `BmxGeometry`), plus
     `zge_scene.SceneFile` (the ZIT, the same form in all three games: every level folder's ZIT
     round trips, BMX XXX 13, AI 21, DM2 8), `ObjectivesFile` (OBJCTV.TXT: nested `KIND_BEGIN ... KIND_END` blocks
     of `KEY "value"` pairs, the text between tokens kept), and `zge_library.py` (the engine's ZDIR
     asset libraries, all three games).
   - Round trips (`bmx_level_write.py roundtrip`): the eight levels and the park editor's five
     folders (MULLALY, UGP, RAMPAGE, ZAXIS, OBJECTS), every GEO, INS, DAT, ZIT, ZAL and OBJCTV.TXT
     byte-identical. The bike games' readers (dm2_level's collision surface and goals' places,
     dm2_xbox's grind edges) read the written files the same. The fields no BMX XXX reader reads
     that are not zero on disc are kept in the records: TGFT bytes 10-11 (5,358 of 66,872
     records), TGFV bytes 1-3 (44 of 163), and the libraries' bytes between entries (191 not
     zero). The tree nodes' leftover byte is 0 in all 216,514 nodes (AI's varies), TGFT's link
     word is 0 in every record, and TGFE's grind side is 3 in every record. 29 AI and DM2 ZDIR
     libraries round trip too; DM2's six others are an older form (DM2's writer's).
   - The game loads what they write: the Bronx (CHL_MULY, bmxlogic's level) rewritten and served
     from a Mods folder (`rewrite --out`, the log shows all ten files opened from the mod) runs
     bmxlogic the gate's way (`PS2X_EE_SCALE=4096` against its own readable-off run) to 17/17
     MATCH, every rider position bit-exact. It does the same with the 2,001 indexed streams
     written plain (`--plain-streams`) and with the 459 collision trees rebuilt over the stock
     trees' faces (`--rebuild-trees stock`). In BMX XXX, unlike AI's Tutorial, the tree's
     partition does not change the route.
   - The converter (`bmx_level_convert.py`, a `Converter` subclass on `ai_level_convert.py`'s
     hooks: `RECORDS`, `scene_file`): the same model and tags, with BMX XXX's records. The strips
     keep AI's 0x4000 winding mark, as BMX XXX's own do (310,838 of 947,927 stock entries), unlike DM2's. Surface kinds come from the stock levels' use and
     the rider's code: `#qp` 1, `#spine` 2, `#kicker=P` 3 (the rider reads 4: a take-off at P
     percent of the top speed), `#wall` 8. AI's `#slope` has no BMX XXX kind. Grind marks are
     grindable 8-byte edges, with lips linked to the quarter pipe's faces (link kind 1). The
     sound is the face material's (MATI +0x1C: metal 1, wood 2, concrete 0, as on the stock
     textures of those names), not the edge's. Ids are name hashes. The four asset libraries are
     written empty, and so is OBJCTV.TXT. The slot is a level row, CHL_MULY (section 4).
   - The test park (`ai_level_convert.py example`), converted and served from a mod in a level row,
     loads and draws (the concrete, the bar and the wooden quarter pipe in the pictures). The
     playbot's map comes from the mod folder's own files (dm2map/bmxmap with the folder as the
     disc): 3,306 triangles, 1,598 cells, 10 rails, 96 ramp faces. A tour run there lasts the
     competition's two minutes (the HUD clock counts down from 2:00, and the rider stops at
     0:00). In that time `bmx.play_session` (HARDCORE TOUR, level select entry 2) rode 1,552 m
     (seed 2) and 1,327 m (seed 3), with 18 and 16 jumps, 2 and 4 grinds (34 m and 73 m of rail),
     a stall on the coping, scores 6,376 and 7,303, and 5 and 2 bails. Every grind feature was
     reached: the flat bar, the ledge, and the coping, which seed 3 ground. A replay with the GS
     on gave the same numbers. Nothing in the runtime changed: with no mod the game reads its
     disc.

   DM2 done. `tooling/ai/tools/dm2_level_write.py` is ZGE2's typed level, built on
   `ai_level_write.py`'s records (its containers take their record classes from class attributes, so
   a game's module subclasses only what differs) and on `zge_tags.py` (the bike games' 8-byte TGFE
   and 12-byte TGFT records, shared with BMX XXX):
   - What ZGE2 has that ZGE3 has not, each from DM2's own loaders: TGCI leaves keep their faces
     inline (preorder, the greater side first, each leaf padded to 4); TGIH is 0x6C bytes with no
     region word; SDPT's index is a word; PVSO has the older body only; there are no indexed streams;
     MATI is 0x9C bytes; TGFM entries never carry the 0x4000 winding flag (a triangle's winding is
     its parity after the restart; a converted park that kept the flag drew as spikes, DM2 taking the
     index as 15 bits: `Part.unwind`; BMX XXX keeps the flag). Also typed: the TXR (`TextureLibrary`:
     TXRH and its TBMP levels, the two reserved TXRH words kept); the ZIT is `zge_scene.py`'s, one
     record for all three games (AI's converter writes it through the same record).
   - Round trips (`roundtrip --disc --xbox-levels`): all 17 PS2 folders (the 8 levels and the 9
     park themes) and the converted Venice and Greenville, every ZIT, GEO, INS, DAT and TXR
     byte-identical (95 files: 8,530 objects, 8,425 trees, 7,047 instances, 13,604 points, 3,111
     textures), and DM2's readers (`dm2_level.py`'s collision triangles, `dm2_xbox.py`'s grind
     edges, the goal places) the same from the written files. Unread leftovers kept so the stock
     files round trip: a tree node's second byte, the two bytes after an odd leaf's faces, TGFT
     bytes 10-11 (`--clean` writes 0). Every DM2 tree lists exactly its part's 0x2000 faces.
   - The game loads them: Woodward rewritten and served from a Mods folder ran dm2logic at the
     gate's clock 6/6 MATCH with the writers' bytes and 6/6 with every leftover written 0 (the
     same distances as the disc's files); with the 260 trees rebuilt over the same faces it
     diverges at ck4 (1.15 m by ck6): as in AI, a stock level keeps its trees.
   - `dm2_level_convert.py` is `ai_level_convert.py`'s converter with DM2's records and forms (a
     subclass: the record module, the budgets and the object, edge and scene writers), read off
     DM2's own park pieces: `#qp` faces surface kind 1 with launch 70 (`#launch=N`), their
     `#grind=lip` copings link kind 1 and grindable; `#spine` kind 2 with link-2 edges; `#kicker`
     kind 3; rails grindable with no link. Poles, `#wall`, `#slope` and the grind sounds have no
     named DM2 form and are reported. Budgets: texels 2,560 KB, side 128, and 682 parts drawn
     (Devil's Peak's instanced parts; DM2's levels have no zones). The park slot: the park menu
     offers the disc's nine themes and a theme opens by its SCENE.TXT scene name, so the level
     takes Warehouse's place: `EDITOR/LEWAREHO/LEWAREHO.*` (the level with the theme's 461 editor
     objects and 147 textures, which the park code makes nodes of) and THEMES.TXT's first block
     replaced (the level's grid, every cell buildable, no towers). An empty park saved on it (only
     the start, at the cell the converter reports) plays the level from FREERIDE, value 8.
     `playbot/dm2modlevel.py` makes that card in DM2's editor and rides it (`card`, `ride`, the map
     from the folder's own files).
   - Ridden: the test park (`ai_level_convert.py example`) converted for DM2, the card made
     by the playbot in DM2's editor (an empty park, its start at column 8, row 5 facing +z), and
     ridden by `bikepark.py`'s rider from FREERIDE, level 8, for 10,800 ticks: 185 s, 2,532 m, 17
     grinds and 138.7 m of rail (the flat bar, the ledge and the quarter pipe's coping), 41 jumps,
     22 tricks started, 5 bails, 8 lines banked, score 1,775; 5 of the 6 rails and the quarter
     pipe reached. The editor and the ride draw it as AI does (the ground's F marks, the wooden
     pipe, the bar and the ledge).

## 4. Questions settled

- What the ZIT holds: settled. It is the level's scene file, 136-152 bytes:
  `FSCN { TERR { TEHD, ZFTX, ZFGE, ZFIS, ZFSD } }`. TEHD (0x30 bytes) is the collision world:
  the cell size (50, 50, 50 in every stock level), the world box's low and high corners, and the
  grid counts per axis ((high - low) / cell: the Movie Lot 20 x 8 x 17 cells over x -350..650,
  y -250..150, z -450..400; the Tutorial 14 x 4 x 13); `readTehdChunk` reads the first 0x24 bytes
  and makes the collision world from them (`newCollisionWorld`), and the counts are derivable.
  ZFTX, ZFGE, ZFIS and ZFSD are the names of the TXR, GEO, INS and DAT files, read from the ZIT's
  own folder (`includeChunkFile`), the TERR handler wrapping the GEO's load (`readTerrChunk`). So
  a new level cannot ship a stock level's ZIT: its box must hold the new level's collidable
  instances and its names must be the new files'. It is 4 names and 12 numbers; the writer is
  trivial and goes with the authoring path (step 2). The Zoo ships the Xbox's because the Xbox
  level's box and names are its own.
- AI's free skate script: settled. `ai_level_convert.py` writes the Zoo prototype's
  60-byte shape (version 2.70, one object with one empty state) as `<L>.BIN` and `<L>T.BIN`; the
  test park loads and plays with it. An assembler is only needed once a level wants goals.
- Which level slot each game lends: for AI the Tutorial's 7. The files go in a mod as
  `ASSETS/LEVELS/TUT/TUT.*` (mods are read before the Estate's provider and the disc), and level 7
  is always unlocked. LOCAL's 9 is in no level select at all, and the free skate select has no
  Tutorial entry either (entries 0-6 are the seven career levels: entry 0 places the Airfield):
  today a custom level in slot 7 is reached in free skate only through the Estate's entry (the
  setting on, `PS2X_DEV_START_LEVEL=ESTATE`, or the playbot's `modlevel.py`), or through the
  career's Tutorial. Either slot needs the Custom Freeplay menu (step 4) for a real entry; 7 is
  the one that plays today. DM2: the park slot, in a park theme's place (step 6); BMX XXX has no precedent yet (its preset parks
  are the nearest). Each game
  already has a park slot its menus reach in free play, with a list of parks to choose from: AI's
  Free Skate entry 7 (the Editor Level; a Repository park plays as level 11), DM2's FREERIDE value 8
  (session level 8, from the card's list), BMX XXX's PARK EDITOR > SESSION (park level 12, behind the
  app's menu line). A custom level could take that slot and its list rather than a stock level's.
  BMX XXX: settled, a level row, CHL_MULY (The Bronx: a challenge level, no clock) by default; a
  competition row (CMP_RMPG, CMP_ROOT) is a two-minute run. Park level 12
  does not work as a level slot. It is the park editor's Mullaly theme (`ASSETS/EDITOR/MULLALY`)
  with a saved park built over it: `buildParkCells` puts a terrain tower on each of its 30 x 20
  cells, so a converted park served as `MULLALY.GEO` would have the editor's floor laid through it.
  A level row (`LevelInfo`, game/bmx/profile/players.h) is a plain level load from
  `ASSETS/LEVELS/<name>`, reached by HARDCORE TOUR's level select (the playbot's `bmx.menu_script`;
  CMP_RMPG is its entry 2, opened by its cheat IOWARULES). The files go in a mod as
  `ASSETS/LEVELS/<row>/<row>.*` (`CHL_MULY/CHL_MULY.*` for the default slot) with the four asset libraries. `OBJCTV.TXT` is optional:
  `loadLevelObjectives` reads it only in a tour session (session kind 0), and a level without one
  has no goals there.
- "Nothing mixed" (when the cross-game level converters came out) was a prioritisation call at that time, not a
  rule: mixing is in scope, and Custom Freeplay mixes the games.
- Lighting and vertex colours: bake them in the converter, since the PS2 path has no runtime
  lights.
