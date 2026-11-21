# Custom levels: the Custom Freeplay mode and its maps

For people making maps. Installing and playing a map is docs/MODS.md and docs/SETTINGS.md "Custom Freeplay". Section 1
is the mode, section 2 what a map is made of and its limits, section 3 how to make one, section 4 notes on the level
files.

## 1. Custom Freeplay across the games

Custom Freeplay is a fourth entry on the collection's main menu, free play only, behind a setting that is off by
default (`[general] custom_freeplay`). You pick a player type from any of the three games (an inline skater from
Aggressive Inline, a rider from Dave Mirra BMX 2 or BMX XXX), then a map.

- **The player type picks the game.** Each game keeps its own physics, tricks, rider and camera: a skater boots AI,
  a rider boots DM2 or BMX XXX. The mix is in the collection's menu and in the content, never in one game's code: no
  skater inside BMX XXX's physics.
- **One map, every player.** The three games run versions of one engine: AI is ZGE3, DM2 ZGE2 and BMX XXX ZGE4
  (`tools/modding/zge_level_gltf.py` docstring). A level is the same five files in each, with chunk differences
  per version:
  - AI: `<L>.ZIT .GEO .INS .TXR .DAT`, `MOTIONS.ZAL`, and the scripts `<L>.BIN` (career) and `<L>T.BIN` (free skate
    and timed run), under `ASSETS/LEVELS/<L>/`.
  - DM2: `<L>.ZIT .GEO .INS .TXR .DAT .TXT`. Its goals are tables in the executable, not in the level, so a free play
    level needs none.
  - BMX XXX: `<L>.ZIT .GEO .INS .TXR .DAT`, `ANIMS.ZAL CUTCOMP.ZAL CUTSCENE.ZAL MODELS.ZAL`, and the goals as a text
    file, `OBJCTV.TXT`.
- **Stored once, converted at the start.** A map is the level files of the game it was made in, laid out in one of
  that game's slots (`collection/maps.h`). It lives in the collection's Mods folder (`<support>/Mods/<map>/`, a
  folder or a zip read in place, with a `[map]` section in its `mod.ini`: docs/MODS.md), in the maps folder
  (`<support>/Maps/<map>/`), or in a game's own Mods folder (`games/<id>/Mods/<map>/`). A player of that game reads
  the folder as it is. A player of another game gets the map converted into its own game's files when its boot
  starts, in memory (`runtime/game_files.h` `registerMemoryFolder`, over the mods): nothing is written and
  nothing is cached. A conversion takes 80 to 350 ms and holds 3 to 9 MB while the game runs.
- **The slots.** Each game lends free play these slots (`mapSlots`). A converted map goes into the target game's
  first slot its setup can start, unless the map names one with `slot.<game>`.
  - AI: free skate level 7 (folder `TUT`, reached through the Estate's entry; the slot's files are the map's, whatever
    level it is) and the Movie Lot (`MOVIE`, free skate's first entry, open from the start: the slot to use without
    the GameCube disc).
  - DM2: its eight FREERIDE level rows (`WOODWARD` first) and its park slot (`PARK`: a park saved in DM2's park
    editor, DM2 only; a converted level with its theme's editor objects leaves no room for a large level).
  - BMX XXX: its eight HARDCORE TOUR rows (`CHL_MULY`, The Bronx, first).

  A map in a slot its own game's setup cannot start (AI's Tutorial without the Estate) is converted into that game's
  first slot that starts, with its goals' files.
- **The size limit.** A level whose files are larger than the game's largest stock level's is refused before the boot
  (each converter's `LEVEL_BYTES`): it would not fit the game's heap. Of AI's career levels only Industrial fits both
  bike games' limits.
- **The bottom screen's map** of a map is made at its boot from the files the game reads, in memory
  (`collection/extras/panel_maps.h` `makeMapPanelFile`, served in place of the slot's `.pmap`).
- **Two converters, one result.** The tooling's Python converters (`zge_level_gltf.py` export, then
  `ai_level_convert.py`, `dm2_level_convert.py`, `bmx_level_convert.py` convert) are the authoring step: a glTF
  model, or a stock level, made into a map in one game's files. The app's converter (`collection/levels`)
  is a native port of the same chain from one game's files to another's, with no glTF or PNG in between. Its files
  are the Python chain's byte for byte: `zx-map-convert convert` writes them to a folder to compare, and
  `zx-map-convert export` prints the model in between. A change to one is a change to both.
- **The scene points across the games.** A level's DAT holds its scene points (SDPT: a kind, the hash of a class
  name; a name hash; an index; a place and a turn; the kind's record). What a conversion does with each kind:

  | kind | read by | in a converted map |
  | --- | --- | --- |
  | Spawn PlayerOne, PlayerTwo | each game's start (`placeSkaterAtLevelStart`, `spawnPlayer`); PlayerTwo is split screen in AI and BMX XXX | carried; DM2 has no second start (its multiplayer takes turns), so PlayerTwo is left out there |
  | Spawn, the respawns: AI's "Respawn", DM2's "Player", BMX XXX's other names | a fallen player is put back at the nearest (AI `respawn`, DM2 `respawnAtStart`, BMX XXX `findSpawnFace`: any name) | carried as `#spawn=respawn`, written with the target's name (BMX XXX's unnamed, as its own) |
  | Spawn named by scripts or modes: AI's lessonOne ... lessonTwentyFive and wickedTree, BMX XXX's sniper_shooter and sniper_target, DM2's one unnamed hash | AI's `setRespawnLocation` script native, BMX XXX's paintball | carried by name (`#spawn=lessonOne`; `SCRIPT_SPAWNS`, `levels/scene.h scriptSpawns`) and written only into the files of the game that reads them; left out of the others (BMX XXX would take a lesson point as a respawn); the unnamed hash is left out |
  | CameraPos "idle" | BMX XXX's idle camera behind a competition's instructions and its GAME OVER (ANOTHER RUN) screen (`idleCameraMode`: points 1 to N in turn, the first tag byte the field of view; none: the level's origin) | carried as `#camera` nodes (`#fov=N`) in all three games' files; BMX XXX's other CameraPos names (" idle", "CHL", a "van" at index 0) have no reader and are left out |
  | DM2's level-point camera (no scene point: five points a level in the executable, `g_levelCameraPoints`) | DM2's mode 4 (a SESSION's or ProQuest's instructions, the leaderboard, multiplayer's time up and game select): 5 m above a point, looking at a point circling it; FREERIDE, the Custom Freeplay start, goes straight to the turn and its RUN FINISHED keeps the ground camera | `dm2_level_convert.py table-cameras` writes a level's row as its CameraPos idle points; DM2's level camera points option (`ZX_DM2_LEVEL_CAMERA_POINTS=1`, `levelCameraPoint`) takes a level's own in the row's place, and the DM2 slot's start turns it on. A point is the eye 5 m lower in DM2's files |
  | Globals | the clear and fog colours, the fog and far clip (all three) | from the model's sky (each game's record) |
  | Waypoint | the paths of the level's traffic, people, boats and path men | left out: those objects are the source level's |
  | AI's OAR zone boxes, NPC, pathhead and pathpoint, PARTICLE, POINTER; BMX XXX's PathMan, FastPathMan, MOTIONLIST, BirdObj | the levels' props, NPCs, critters, emitters and walkers | left out: each names the source level's own models, animations or scripts |
  | AI's sound emitter, BMX XXX's SoundEmitter | each game's ambient sounds at a place | left out: the record names the source game's own sounds |
  | the texture effects, flipbook and scroller (AI, BMX XXX) | the engine's `initTextureFx` in AI and BMX XXX (DM2 has none) | left out: they name materials and the flipbooks' textures, which a conversion renames or leaves out (only drawn textures go in) |
  | LensFlare (DM2, BMX XXX) | DM2's `placeLevelLensFlare` (its colour word); BMX XXX's has no readable reader yet | left out: BMX XXX's record is not known, so DM2's cannot be written as one |
  | EditAnchor, EditLightOmni and EditLightCone; AI's unread "start", "end", Competitor and MP Industrial's motion lists | the park editors' pieces and lights; nothing | left out: park editor themes only, or no reader |

  A level with no idle camera of its own (every AI level) gets one where a game reads them (BMX XXX, DM2): 8 m behind
  its PlayerOne spawn along its facing, 3 m up, looking at a point 1 m above the spawn, 60 degrees. AI reads no camera
  (its pause and end of run stay on the follow camera); its files keep a map's cameras so they go on to the other
  games. The cameras and respawns add under 300 bytes to a converted map.
- **Tag words.** The faces' and edges' tag words go with a map too: the word a TGFT or TGFE record carries (AI's
  FaceTag event word and EdgeTag event id, the bike games' `tag`). The games' goals and events find faces and edges by
  them: DM2's gaps (`g_levelGapDefs`, by face tags, in FREERIDE too), BMX XXX's PROXIMITY, TRANSFER and GRIND
  objectives, AI's Jump and Land events. The export writes them as `#tag=0x<word>` on the faces' materials and on the
  grind marks (grind edges only: a word on an edge that is not ground is left out); a model's `#tag=<name>` gives a
  name's hash. Every face word goes through, and every edge word on a collidable face. A word on an edge of a face that
  does not collide is lost: the export puts a collidable object's loose faces in a `#deco` object of their own, which
  has no collision part, and the converters tag only the collision parts' face edges.
- **Free play, with a map's own goals.** No career or unlocks: spawn, ride, a session score and trick list, restart or
  change map. A map's goals are its own files' and run in its own game: AI's free skate script (`<L>T.BIN`; the
  Estate's has its lessons; a converted map gets a 60-byte empty one, which loads), BMX XXX's `OBJCTV.TXT` (the tour
  reads it; a converted map's has no objectives); DM2's are the executable's and FREERIDE runs none. What they mark is
  not kept:
  - AI: the start puts the level's records back (top speed and the rest, a row of the career save:
    `ZX_AI_KEEP_LEVEL_RECORDS`, so the Estate's own records file is kept too).
  - DM2: free ride writes no level record, so there is nothing to keep; `ZX_DM2_KEEP_LEVEL_RECORDS` puts the row back
    should a ride ever set one. DM2's game over offers no save line.
  - BMX XXX: the start puts the row's level record back (counts, flags, marks) and its saved records (bests), in a
    save written meanwhile too.

  A map moved into another slot of its own game takes the files its goals are in along (AI's scripts and MOTIONS.ZAL,
  BMX XXX's OBJCTV.TXT and asset libraries). Goals are not translated between the games' systems.
- **The menu.** Custom Freeplay, then the player type (three tiles with the games' art), then one list of maps (the
  collection's Mods folder's, each game's own Mods folder's, the Maps folder's), each once, every one startable by
  every player (the row says which game it was made in and that it is converted when it starts), then the game's own
  skater or rider select. A map that does not convert for a game (over its limits) does not start, and after its first
  failed start the list shows the converter's reason.

## 2. What a map is made of, and its limits

- **Budgets.** The converters check these and report them at conversion (each converter's header lists them):
  - texels: 2,560 KB over the whole level, a byte a texel (the largest textures are halved first until it fits, and
    the report says so). The whole level's, not a zone's;
  - texture side: 128 (larger ones are halved; a side that is not a power of two is resampled);
  - strip groups: at most 60 entries (cut by construction);
  - faces: 1,024 an object (a larger mesh is cut at the median of its faces into `name`, `name_2`, ...);
  - vertices: 16,384 a part (a strip entry's 14 index bits);
  - parts in view: 1,083 for AI and BMX XXX (the parts drawn from a zone: its own, the zones it does not hide, and
    parts in no zone; the Civic Center's worst zone), 682 for DM2 (DM2's levels have no zones). The converter cannot
    fix this one: the model needs more zones, and it is an error unless `--force`;
  - level size: `LEVEL_BYTES` (section 1).
- **Memory.** The games run in one malloc arena from the program's end to the top of RAM (`reserveMallocArena`: AI
  0x1CC94C0 bytes, about 30 MB; the same function in all three, a constant per game). The level takes what it needs
  from that heap: there is no fixed level budget. Extended memory up to 64 MB exists (`runtime/guest_heap.h`), and
  only the Zoo and DM2's Xbox levels turn it on. A frame's textures live in the GS texture cache (DM2: 0xA8000 words,
  about 2.6 MB), and a level without visibility zones flushes it every other frame. So zones (a PVS) are not optional.
- **Size in the world.** DM2's collision tree splits are s16 hundredths, so a DM2 level fits in about +-327 m.
- **Collision trees.** A stock level keeps its stock trees. In AI and DM2 the tree's partition alone (which leaf lists
  a face, the order a query meets them) changes the game's physics, so rebuilding a stock level's trees changes how it
  plays; in BMX XXX it does not. A level of your own gets the converter's trees (`CollisionTree.build`) as its own
  collision. What a tree lists is what is solid.
- **No lights at run time.** The PS2 path has no runtime lights: vertex colours come from the model's `COLOR_0`
  (full white otherwise). Bake lighting into them.
- **Detail above the PS2's: textures only.** Settings > Graphics > Native Meshes (`aggressive_inline_native.h`
  `installNativeDraw`, `ZX_RENDER_PATH` native/strict) draws level meshes from host vertex buffers instead of through
  VU1, but from the same GEO the game opened (`zge/native/geo_meshes.h`); there is no host-only mesh (BMX XXX draws
  its meshes from the draw records, `render_path = records`). Higher detail on the host is textures: texture packs
  (docs/MODS.md).
- **How files are found.** In a game's mods first (its own Mods folder, then the collection's mods that name it, in
  the Mods page's order: docs/MODS.md, `collection/mods.h`; the game gets them as `ZX_MODS_DIR`), then registered
  providers, then the disc (`ps2/runtime/game_files.h`). That is how a map reaches a level select with
  no new menu path in the game's code.
- **Three park editors, three formats.** AI's and BMX XXX's editor are one in `engine/zge/editor`; DM2 keeps its own.
  The save formats stay per game, and a park built in one does not load in another:
  - AI: `games/aggressive/level/park_editor.cpp` and `park_*.cpp` (the front end's Park Editor, game type 5, four themes
    from the pieces file `ASSETS/EDITOR/INLINE.PEP`, each theme a grid from `Assets\Editor\<theme>\<theme>.map` in
    cells of 4.572 m (15 ft), maximum height 20; also the in-game editor in Cannery, Airfield and Industrial). Parks
    are level indices 10-13. A park saves as a 0x2000-byte block (a 0x48 header, then a mark per cell), on the host as
    `<saves>/Parks/Park NNN.sav`.
  - BMX XXX: `games/bmx/editor/` (AI's editor: terrain, rail, object, dynamics and test-run tools, pieces file
    `assets\editor\BMX3.Pep`, four themes, park level 12, the same 0x2000-byte save). It also reads 8 preset parks
    from `assets\editor\presets\`, a disc-file route for shipping a park. No retail menu reaches the editor:
    `ZX_BMX_PARK_EDITOR=1` (the setting `[extras] park_editor`) puts it on the game's main menu as PARK EDITOR.
  - DM2: `games/dm2/editor/` (a grid of at most 64 x 64 cells of 5 m, 25 object categories, 50 lights, a capacity meter
    of each object's vertex count plus 200, park level 8, a 0x2565-byte save, on the host as `<saves>/Parks/Park NN.sav`, NN the park list's slot: the
    slot's name, author and theme, then the park, as the card's park library keeps each slot).
- **The tools.**
  - Readers: `tools/modding/ai_level.py` (AI; subcommands `ground`, `edges`, `compare`, `selftest`, `info`) and
    `tools/modding/dm2_level.py` (DM2 and BMX XXX; `selftest` and `info`). DM2's grind edges are read in
    `games/dm2/tools/playbot/dm2map.py` (`build`) and `tools/modding/dm2_xbox.py` (`grind_edges`).
  - Writers, the typed form of each game's level files with exact round trips of the stock levels:
    `ai_level_write.py`, `dm2_level_write.py`, `bmx_level_write.py` (with `zge_tags.py`, `zge_scene.py` for the ZIT,
    the same form in all three games, and `zge_library.py` for the ZDIR asset libraries).
  - Converters: `ai_level_convert.py`, `dm2_level_convert.py`, `bmx_level_convert.py` (a glTF model into a map),
    `zge_level_gltf.py` (any game's level into a glTF), `dm2_to_ai_level.py` (a DM2 level into AI's files). Each one's
    header lists its commands, the model's tags and its budgets.
  - The bottom screen's map: `panel_map.py` makes `<LEVEL>.pmap` from a level folder (`FolderLevel`) for all three
    games.
  - The playbot (`tools/playbot/nav.py`, `games/dm2/tools/playbot/dm2map.py`, `games/bmx/tools/playbot/bmxmap.py`)
    builds its map from a level's files and rides it; `tools/playbot/parkmap.py` does the same for a park or a level
    folder, and the park players (`games/aggressive/tools/playbot/aipark.py`, `tools/playbot/bikepark.py`) ride a tour
    of its features. `games/aggressive/tools/playbot/modlevel.py` builds a Mods folder's map with it.

## 3. Making a map

1. **The model.** Make the level in Blender (or anything that writes glTF 2.0) and export it as glTF. The semantics
   are in the names, as `#word` or `#key=value` suffixes ("ledge#grind#metal"), or in Blender's custom properties:
   - a node with a triangle mesh is a level object placed by its transform; `#deco` does not collide, `#hidden`
     collides but is not drawn, `#sky` is the sky panorama;
   - a node with a line mesh marks grind edges: `#grind` (a ledge or rail), `#grind=lip|spine|vpole|hpole`, with
     `#metal` or `#wood` for the sound;
   - a material's tag is its faces' surface kind (`#qp`, `#spine`, `#slope=G`, `#wall`, `#2sided`; the
     bikes' `#kicker`: step 6), its base colour texture (a PNG) the level texture;
   - empty nodes: `#spawn=PlayerOne` (and `#spawn=respawn`, named points), `#camera` (`#fov=N`), `#zone`
     (`#sees=a,b`; the box from the empty's scale);
   - `#dynamics` on a node makes a prop the rider knocks about; `#tag=` gives faces or edges a tag word (section 1).

   Axes: (x, y, z) in glTF is (x, -y, -z) in the game (a half turn about x, windings kept). A spawn's identity
   orientation faces +z. `ai_level_convert.py`'s header has the full list, and `ai_level_convert.py example --out
   DIR` writes a small test park (a ground, a quarter pipe with a coping, a flat bar, a ledge, a spawn, two zones) to
   start from.
2. **Convert it.** For the game you make it in:

   ```
   python3 -I ai_level_convert.py convert MODEL.gltf --out DIR [--slot TUT] [--sky SKY.png]
   python3 -I dm2_level_convert.py convert MODEL.gltf --out DIR --disc <DM2 disc> [--level WOODWARD] [--sky SKY.png]
   python3 -I bmx_level_convert.py convert MODEL.gltf --out DIR [--slot CHL_MULY] [--sky SKY.png]
   ```

   DIR gets the level's files in a Mods folder's layout for the slot (`ASSETS/LEVELS/<slot>/<slot>.*`; DM2's default
   is the park slot, below). `info MODEL.gltf` shows what the converter sees and the budgets, with nothing written.
   The report says what was changed to fit (textures halved) and what is over a limit.

   `--sky SKY.png` makes the picture the level's sky: a 28 m dome, and the level's Globals point with the picture's
   zenith as the clear colour and its horizon as the fog's. All three games draw it the same way. DM2's and BMX XXX's
   converted skies have no fog. BMX XXX clears to black, so without a sky its sky is black.

   For AI the converter writes a 60-byte free skate script as `<L>.BIN` and `<L>T.BIN` and an empty MOTIONS.ZAL. There
   is no script compiler, only the disassembler `tools/checks/aggressive_script_dis.py`: a map with goals of its own
   in AI needs a script written another way.
3. **Gameplay data.** What the games need from a map, and what they do with it:
   - The game puts the player on the ground under the spawn (a spawn 1 m up lands at y 0), facing +z for an identity
     orientation. A spawn needs no tag bytes.
   - A level needs no Globals point (the camera takes its defaults) and loads without zones (the Tutorial has none),
     but see "Memory" in section 2.
   - A park needs an edge (a wall or a bowl) or ground beyond its features, or a rider rides off it and falls.
   - The playbot finds quarter pipes by instance name (`qpipe`, `spine`, `halfpipe`, `bowl`): name them so.
   - A map's people, riders and traffic are those its own files place. BMX XXX's NPC riders belong to the level row,
     not the level files, so a map leaves them out unless its `map.ini` says `npcs = slot` (a map made from the slot's
     own level).
4. **In the app.** Put DIR in the collection's Mods folder with a `[map]` section in its `mod.ini` (or a `map.ini`):
   `name`, `game` (the game it was made in), `slot`, and optionally `slot.<game>`, `park`, `preview`, `author`, `npcs`
   (docs/SETTINGS.md "Custom Freeplay" shows the file). Turn Custom Freeplay on in the collection's Settings. The map is in
   the list for every player.
   - **Which game runs:** always the player's. The map crosses, never the player.
   - **A map made for another game:** nothing to do. The boot converts it (section 1, "Stored once, converted at the
     start").
   - **A DM2 map for AI's skater:** `dm2_to_ai_level.py convert FOLDER --out DIR` reads a DM2 level (a park theme's or
     a level folder's files) and writes AI's `TUT` files from the same pieces: the instanced objects' parts (the
     theme's own editor pieces, which no instance places, left out), their surface kinds and grind edges mapped to
     AI's tags, the textures, the spawn (the park's start cell when the level has no PlayerOne). The output is a
     second map folder with `game = ai`. It carries a DM2 sky to AI's form.
   - **Any game's level for any other:** `zge_level_gltf.py export --game ai|dm2|bmx FOLDER --out MODEL.gltf` writes a
     level of any of the three games as a glTF in the converters' tags (objects per instance, a separate collision
     part `#hidden`, the panorama `#sky`, surface kinds, `#2sided` or `#2sided=collide`, `#layer=` for blended or
     alpha-tested materials, grind edges, spawns, props, cameras, tag words). It opens in Blender, and the three
     converters make it a map of any game.
5. **Check it plays.**
   - Collision: `ai_level.py ground` and `edges` compare the files with the game's own query, point by point
     (`ZX_ESTATE_PROBE`, `ZX_ESTATE_GRINDS`; DM2: `dm2_xbox.py` with `ZX_DM2_XBOX_LEVEL_GRINDS`).
   - Riding: the playbot (`games/aggressive/tools/playbot/modlevel.py ride` for AI; the park players for the bikes)
     rides it, grinds its rails and lands tricks, and reports the game's own numbers (distance, grinds, rail metres,
     tricks, bails, score).
6. **DM2 and BMX XXX.** The same model and tags, with each game's records and forms:
   - BMX XXX (`bmx_level_convert.py`): `#qp` 1, `#spine` 2, `#kicker=P` 3 (a take-off at P percent of the top speed),
     `#wall` 8. AI's `#slope` has no BMX XXX kind and stays ground. Grind marks are grindable edges, with lips linked to
     the quarter pipe's faces. The sound is the face material's (metal, wood, concrete), not the edge's. The four
     asset libraries and OBJCTV.TXT are written empty. The default slot is a level row, CHL_MULY (The Bronx: a
     challenge level with no clock); a competition row (CMP_RMPG, CMP_ROOT) is a two-minute run. `OBJCTV.TXT` is
     optional: `loadLevelObjectives` reads it only in a tour session, and a level without one has no goals there.
     The start keeps The Bronx's goal counts and puts them back on the ride's end screens and in the menus
     (`ZX_BMX_KEEP_LEVEL_GOALS`).
   - DM2 (`dm2_level_convert.py`): `#qp` faces surface kind 1 with launch 70 (`#launch=N`), their `#grind=lip` copings
     link kind 1 and grindable; `#spine` kind 2 with link-2 edges; `#kicker` kind 3; rails grindable with no link.
     Poles, `#wall`, `#slope` and the grind sounds have no DM2 form and are reported. By default the level takes the
     park slot, in a park theme's place (Warehouse: `EDITOR/LEWAREHO/LEWAREHO.*`, with the theme's editor objects and
     THEMES.TXT's first block replaced by the level's grid, every cell buildable). An empty park saved on it (only the
     start, at the cell the converter reports) plays the level from FREERIDE, value 8;
     `games/dm2/tools/playbot/dm2modlevel.py card` makes that park in DM2's editor. `--level L` puts it in a level row
     instead. DM2's park slot moves the level so its PlayerOne stands at the start cell's centre and level, and keeps
     the grid within the park buffer (`kParkBytes`).

## 4. Notes on the level files

- **The ZIT** is the level's scene file, 136-152 bytes: `FSCN { TERR { TEHD, ZFTX, ZFGE, ZFIS, ZFSD } }`. TEHD (0x30
  bytes) is the collision world: the cell size (50, 50, 50 in every stock level), the world box's low and high
  corners, and the grid counts per axis ((high - low) / cell); `readTehdChunk` reads the first 0x24 bytes and makes
  the collision world from them (`newCollisionWorld`). ZFTX, ZFGE, ZFIS and ZFSD are the names of the TXR, GEO, INS
  and DAT files, read from the ZIT's own folder (`includeChunkFile`). So a new level cannot ship a stock level's ZIT:
  its box must hold the new level's collidable instances and its names must be the new files'. The converters write
  it.
- **AI's slot.** The files go in a mod as `ASSETS/LEVELS/TUT/TUT.*` (mods are read before the Estate's provider and
  the disc), and level 7 is always unlocked. The free skate select has no Tutorial entry (entries 0-6 are the seven
  career levels; entry 0 is the Movie Lot), so slot 7 is reached in free skate through the Estate's entry; Custom
  Freeplay does that for you.
- **The park slots.** Each game has a park slot its menus reach in free play, with a list of parks: AI's Free Skate
  entry 7 (the Editor Level; a Repository park plays as level 11), DM2's FREERIDE value 8 (session level 8, from the
  card's list), BMX XXX's PARK EDITOR > SESSION (park level 12, behind the main menu's PARK EDITOR line,
  `[extras] park_editor`). BMX XXX's park level 12 does not work as a map slot: it is the park editor's Mullaly theme
  (`ASSETS/EDITOR/MULLALY`) with a saved park built over it, and `buildParkCells` puts a terrain tower on each of its
  30 x 20 cells, so the editor's floor would be laid through a converted level. A BMX XXX map takes a level row
  (`LevelInfo`, games/bmx/profile/players.h): a plain level load from `ASSETS/LEVELS/<name>`, reached by HARDCORE
  TOUR's level select. The files go in a mod as `ASSETS/LEVELS/<row>/<row>.*` (`CHL_MULY/CHL_MULY.*` for the default
  slot) with the four asset libraries.
