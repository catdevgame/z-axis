# Manual tests

Checks that would take an agent more than about 30 minutes to reach (long
play, career progress, rare events), left for a person with the app. Each:
what to do, what should happen, the build or setting it needs. Once an
entry is seen, take it out, with the date and app build in the commit message.

## Aggressive Inline

- **The Thor panel's screens by feel (thorpanel).** On the Thor, in a career level: skate; the default
  needs no touch. Pause: the goal list and the whole map come forward; unpause: back to follow. Open
  TRICKS by hand, then start a line: it should give way; with HOLD on it should stay. Judge whether
  the reference leaving on a line feels right or sudden, and whether the second HUD reads in a glance
  during a line. No project found does this automatic switching, so only play can say. A timed run
  (Kelly in the Movie Lot) should show its clock and points; a goal finished, its DONE card for 4 s.
- **Android: real fingers and two real pads (thor).** Only injected events so far. On the Thor: drag and pinch the panel with fingers while steering with
  the built-in pad: the pad stays with the game and B does not close the panel. Connect a second pad, start Multiplayer (it is hidden with one pad) and
  check that it plays as player 2. The screen off and on, and the AYN button in the middle of a level: the game resumes and the panel comes back.
- **The collection on the Thor by hand (thor1).** Install the APK and push the games (BRINGUP "Android"); the app opens on the
  collection menu. With the built-in pad: Left/Right and A choose a game; each boots to its front end and a level; A/B/X/Y are
  the game's Cross/Circle/Square/Triangle (Xbox prompts show). Check the sound of each game (intro music, the level's
  music and effects, the volume keys). The bottom screen: blank in the collection menu and the front ends; in a level
  AI's and DM2's map with the rider's arrow, cars (AI), the goal list on pause (DM2's challenges, AI's career goals), the
  trick table by touch; BMX XXX's stays blank (no panel yet). Exit to Collection in DM2's and BMX's pause menus and AI's
  quit item come back to the menu, where the next game boots. Saves: play a career goal, quit, boot again: the progress is
  there (files/support/games/<id>/Saves). Frame time by feel: the Movie Lot, Woodward and the Bronx hold 60 with short dips
  (the numbers are in $ZAXIS_SCRATCH/thor1/progress.md).
- **Wall Tap (paths8, paths9).** The skater's state 13 (WallTap: stateWallTapEnter 0x1A2888, wallTapHold,
  wallTapJump, stateWallTapExit 0x1A2BA8) was not entered by any route, level or script found; the
  game sets Player::wallTapping from a surface whose roll sound's low byte is 0x0D. paths9's test route
  `cov-walltap` forces the state (setSkaterState(player, 13) by guest call from the Wall Ride's update: all four
  functions run and replay identical), which says nothing about whether a level has a surface that does it
  on its own. Skate every level's walls for a move that sticks the skater to a wall for a moment (Wall Ride
  and Wall Jump are states 12 and the Tutorial's lesson; a Wall Tap, if a level has the surface, would show
  as the skater's `state` 13 in `tools/routes.py where`). If it exists, record it (`PS2X_FNREC`) and hand the
  file over.
- **The memory card's pulled and swapped dialogs (paths8, paths9).** The runtime's card is formatted or not
  from the start (`PS2X_MC_SLOT1/2=unformatted` or `none`). With an unformatted card the save asks to format it
  (DOWN, CROSS on the dialog formats it: formatMemoryCard, memoryCardFormatted; `tools/routes.py carddialog`,
  routes `cov-mcdlg-NN`), and `PS2X_MC_FAIL=CMD:N:CODE` makes the Nth libmc command fail the way a card does
  that is full, denied, changed or pulled in the middle of a save (routes `cov-mcsave-NN`). Not reached: the
  job thread's own flows (the unregistered bodies at 0x11D608 ..: endSaveJob, findMemoryCard,
  memoryCardTestWrite, appendRequiredSpaceText) that the game's auto-load and save choice start; with a real
  card reader or a runtime swap command, pull the card during a pause > Save and during the career's
  auto-load at the boot and see the dialogs.

- **The sweep's Tested drafts in play (readable round 14).** With
  `PS2X_READABLE=tested` (about 2,100 swept functions on, engine, SDK and
  game) play each career level, split screen and the Zoo for a few minutes,
  paced, with the GPU renderer, and save and load on a memory card: look for
  a hang at a level load, missing sound or music, a crash, a wrong picture.
  Each is identical to the original on every recorded call (at most 200 a
  route), and the boot, logiccheck and the career input were checked with
  them on, but nothing else was played; a function that misbehaves is found
  by halving the Tested list (`PS2X_READABLE=on,<addresses>`) and is set
  back with `readable_sweep.py manifest-demote`.
- **Recording on Windows (readable round 14).** The recorder's page
  protection (VirtualProtect and a vectored handler) was built and its unit
  test passed on Windows, but no game run recorded there: record one route
  (`PS2X_FNREC=@list PS2X_FNREC_DIR=...` over the logiccheck input to vsync
  2500) and check it with ps2xFnCheck.exe; a file read into RAM while a
  call records would fail there if a reader did not say so first.

- **Textures with the readable VIF1 handler (readable12).** The VIF1
  interrupt that uploads textures to the GS (FUN_001085B0) and the GIF and
  VIF packet library under it are readable and on by default. With the GPU
  renderer on the strict PS2 path and on the native path, play each level
  for a few minutes (and split screen, the front end, a level loaded
  twice) with the defaults and with
  `PS2X_READABLE=-vif1Interrupt,-gifPacketCall` and look for textures that
  are missing, wrong or late (a black or untextured surface, a texture of
  another object, a flicker when the camera turns): there should be no
  difference. The recorded calls are identical to the original's, I/O
  register accesses included, but no run compared the pictures.
- **mash's play on screen (readable round 14).** `tools/routes.py mash`
  drives the recording runs; whether its play looks sensible was measured
  (bails, states reached), not watched. Paced, with the GPU renderer:
  `RUNNER=<runner> python3 tools/routes.py start m PS2X_UNPACED=0
  PS2X_GS_BACKEND=gpu PS2X_DEV_UNLOCK_ALL=1`, then `python3 tools/routes.py
  m "boot; career 1; mash 3600 7"`, and `stop m`. Watch the minute of play:
  grabs and spins should end before the skater lands, grinds and manuals
  should hold for a second or more without falling, the teleports should
  put the skater on the ground (or rolling into a rail or pipe) and never
  inside a wall or high in the air; note anything that looks stuck or
  random (the same move over and over, the skater pressed against a wall).
- **Readable game side round 13 on screen.** Run the app with the defaults
  and with `PS2X_READABLE=off` and look for any difference in: framed menu
  boxes (the pause menu, the front end's panels; a box whose corner or
  edge texture is missing draws a mirrored partner), sparks and streaks
  (particles, with and without a texture), scrolling and animated textures
  (water, screens, signs), the levels' moving carts and cars on paths (one
  that hits the skater vanishes and restarts; one that waits starts when
  the skater comes near), traffic stopping at lights (amber and red), the
  music moving on to the next playlist track, and the camera in every
  state (down, rolling, grinding, air, quarter pipe, spine transfer,
  vertical and horizontal poles, wall ride, skitch, split screen, the
  right stick's look-around). The agent checked the logged state and RAM
  with the null GS, not pictures. (readable-game13.)
- **Windows career A/B, a few more times.** Round 10 saw one Windows
  career run (careercheck's input, build 8096c4c, the defaults) part from
  its pair (the new functions off) at state-log tick 64,244: a collision
  query's flags (0xFC for 0xF4) at query 443,802, then the game mode (the
  pair went to mode 3 there, it stayed in 2). Round 11 rebuilt 8096c4c
  from the same source on Windows and ran the pair again: 77,421 ticks
  and 538,986 queries identical, and identical to round 10's other run;
  round 10's three later Windows pairs were identical too. So one run of
  eight parted from the others; nothing in the code has been found to
  explain it (a host race on Windows, or a missed pad-script input, are
  the leads). Run the career pair a few more times on Windows
  (the Windows worker's scratch readable11\career-pair.ps1 with any
  build; tools: /tmp/ai-readable11/tools/abcompare.py, tracecmp.py) and
  note any run that parts, with its tick. (readable11.)
- **Camera Xbox in motion, against the Xbox.** xemu (the console set to
  widescreen, as the user's is) and the app at 16:9 with Settings >
  Camera: Xbox, the same skater and level: skate the same route, turn
  hard, jump off a ramp, ride into a wall. Expect the same framing and the
  same lag and pull-in on both (the camera code is the same; only the
  projection was changed); the Xbox picture is about 10 % wider in xemu
  (its TV pixel shape). (xboxleft; at rest compared by agent, moving shots
  only roughly.)
- **Ape career with card saves.** With `PS2X_SAVES=card`, play some of the
  Ape's career, quit, restart: the Ape's progress is not kept (known; only
  native saves keep it). Native saves (the default): it is kept.
- **Split-screen Zoo, a full round of each game.** Egg Hunt to the results
  (eggs on the eight spots, pickups count), Twenty One, Most Points 2:00;
  the Safari gate opens and closes; NPCs move near both players. Check on
  Windows too (only the Mac was measured). (zoonpc.)
- **The Zoo and the Ape from xbox-converted on Windows.** A Windows app
  built with make-windows.ps1 and -XboxIso: split screen in the Zoo (Most
  Points and Egg Hunt: eggs on the spots, the Safari gate opens), and the
  Ape picked in the skater select (last; needs the Mummy unlocked) in any
  level. Expect the same as on the Mac. Seen 2026-10-24 by agent (appcheck)
  on the packaged Windows app of main 74d043b: the log's "[zoo] on:" names
  the app's xbox-converted folder and "[ape] on: the Ape is skater 0 (Xbox
  files in ...\xbox)"; the split screen and the Ape in play not run.
  (zoobuild: Windows checked in free skate only.)
- **Xbox career scripts over a whole career.** Settings > Game > Career
  Scripts: Xbox. Play a level's career to its end; challenges and unlocks
  as on the Xbox; switching to PS2 scripts later keeps the skater's saved
  progress on the scripts it was made with.

- **Readable game side: career challenges past the first minutes.** The
  default build (the readable script natives on). Play the Movie Lot, the
  Hangar and the Civic Center careers through several challenges each,
  including timed ones (Kelly's points, the grave run) and ones that count
  tricks (a points target). Expect them to start, time out, fail and
  complete as with `PS2X_READABLE=off`, the HUD timer and challenge score
  shown, the pause menu (Challenges, Retry Challenge, Level Select)
  responding. (readable-game3.) Checked by agent on PS2Recomp
  readable-game8: Kelly's "Get 40,000 points in 60 seconds" played to its
  end both ways (tools/routes.py `challenge`, the points poked): every
  native and skater function it runs identical, and now Verified.
  getTimerTime, getTimerState, getChallengeScore, getNum180s and
  extractFloatFromTagData are called by no script on the disc, so no
  challenge needs them. Left for a person: other challenges, played by
  hand.
- **Native meshes in other levels.** The readable meshes check only ran
  over the Tutorial (the logiccheck input). With `PS2X_NATIVE_MESHES_CHECK=1`
  and `PS2X_READABLE=verified` in the environment (a terminal launch of the
  app or `ps2EntryRunner`), load each level (free skate is enough: a minute
  each, the Zoo and split screen too) and look in the log for
  `[native-meshes] check: N parts, N the same from the file, 0 differ`;
  any `[native-meshes] part ...` line names a part whose mesh from the GEO
  data differs from its packet (readable3 or later).
- **Collision queries of kinds 0, 2 and 3.** shapeQueryVisit (FUN_00125318)
  is Draft: the Tutorial run never starts a query of those kinds. Play
  other levels and modes with `PS2X_FNREC=shapeQueryVisit
  PS2X_FNREC_DIR=/tmp/rec-shape` until the log's `[fncheck]` line counts
  calls, then `ps2xFnCheck --cycles /tmp/rec-shape`; expect every call
  identical (readable3 or later).

- **Skating on slopes and walls with the readable physics.** The default
  build. Skate a level's quarter pipes, slopes and walls for a few
  minutes: pushing, braking after a bail, rolling down hills, sliding
  along walls and bouncing off them should feel as with
  `PS2X_READABLE=-groundPhysics,-skaterCollisionResponse`.
  (readable-game4.)

- **Wall rides in every level with the readable wall search.** The
  default build (the wall search FUN_00189138 and the level's collision
  set-up are readable since readable10). In each level, wall ride a few
  different walls (jump at a wall with the wall-ride button held), also
  in the levels from the tenth on (their wall search passes over a wall
  point above the skater) and on walls marked as the preferred kind: the
  skater should take and leave the walls as with
  `PS2X_READABLE=-findWall,-setupCollisionPart`. The agents' routes reach
  only the Tutorial's wall-ride lesson (6 recorded calls).

- **The menus and HUD with zgui's readable interpreter** (readable-game12:
  the UI scripts' interpreter, its commands, animations and the pads'
  input are readable and on by default). Go through every front-end menu
  (career and free skate setup, options, the park editor's menus, the
  memory card screens, the pause menu in a level, a career challenge's
  start and end panels, split screen's menus and HUD): text, colours,
  fades and slides, the cursor's moves and held-button repeats should be
  as with `PS2X_READABLE=on,-runScript,-runStatement,-updateZgui`. Also
  move the cursor with the left stick where a menu takes it. The agents'
  recordings cover the logiccheck and careercheck inputs and a split
  screen game (identical on every recorded call); the stick as d-pad, the
  colour and vector arithmetic of the scripts and a pickup collected
  were not reached.

- **Estate's tutorial by hand.** An app built with the GameCube image,
  Settings > Game > Estate Level on, restart. Checked 2026-10-24 by agent
  (appcheck) on the packaged apps of main 74d043b (PS2Recomp a49dc5b):
  Estate is entry 18 of 20 in Free Skate, its own icon (the courtyard in the
  oval, the blue rim when selected) and its own still behind the menu (Mac
  shot); it loads in 1.6-3.3 s (Mac), 2.4 s (Windows); lesson 1's giver
  answered Circle (the skater moved up to it, ACTION sent to the giver, then
  put at lesson 2's start); the pause menu is the Tutorial's list and its
  Sharp Turns item starts Estate's lesson 3 ("Hit the circle button to talk
  to Chris"); records: Estate's top speed 26.18 and the Tutorial's 23.45
  each kept apart, through a Tutorial career session, an Estate session
  after it and two restarts (Mac); on Windows Estate's own top speed 23.52
  kept across a restart. Left for a person: play the lessons' tasks in order
  (talk to each giver with Circle and do what the texts ask): expect 1 to 8,
  then 10 to 25 (9 skipped, 21 never comes; 2 runs on its own, 11 and 22 in
  the bowl, 17 and 19 say they are not implemented), note any that does not
  start with Circle or cannot be finished; the pause list's boxes (after
  lesson 1 was done by its giver, all 23 boxes were drawn alike, so whether
  a finished lesson gets its tick was not seen); Restart Lesson; and the
  Tutorial then Estate in one session: both levels' floors and rails their
  own (loaded and skated both ways, not looked at closely). (estate,
  estate2.)

- **Unreached swept functions that need a person (triggers1, 2026-10-26).**
  Of the 2,816 swept Drafts and Planned no recorded route reaches, 67
  functions (9,880 bytes) are reached only by play an agent route cannot
  do today: career challenges that need skilled play, and a memory card
  swapped while the game runs. For each, run the app or a runner with the
  recorder on for that feature's list
  (`PS2X_READABLE=off PS2X_FNREC=@<list> PS2X_FNREC_DIR=<scratch dir>`,
  the list from the functions' `trigger` notes, `workdb.py notes trigger`; paced
  is fine) and hand the recordings to the readable agents. Card
  `recomp/run/mc-sweep-unlocked` has every level open.
  - Civic Center, "Fetch the Three Parrots to the Chef" (Hayes FAQ 2.2,
    Wilikai IV level two challenge 21): pick up each parrot (by the statue
    of challenge 5, on the power line by the broken buses, by the two-arms
    statue) and bring it to the chef with Circle; also fail once (drop a
    parrot). Validates 28 (the parrot game natives parrotStartGame,
    parrotEndGame and the motion loading under them), list `ch-parrots`.
    The route `cov-parrots` (paths3) talks to the Chef (the challenge
    starts: "Fetch the parrots.") and to him again, which reaches 5 of
    them; picking a parrot up and bringing it back is what is left.
    paths6's `cov-parrots-w` (the same route on drafter23's recorder)
    records the natives parrotStartGame, parrotHitBase and parrotGetScore.
    Most of the list's other rows are not parrot code (zcompres, the
    motion loading, named after the nearest parrot native); what only a
    person reaches is a crash while carrying a parrot (it flies off:
    0x1C6148, 0x1DB8D8, 0x1DBD30 from skaterCrash) and parrotEndGame at
    the game's end.
  - Career challenges and level events that hide the traffic
    (setTrafficVisible). Every level script on the disc disassembled
    (paths3): setTrafficVisible is called only by level events, each
    started by a move on a level object (initializeAudioSync, then its
    syncStart and anim_event): the Movie Lot's boulder (grind its three
    chains: "Grind the Boulder Chains", Hayes FAQ 2.1) and what it sets off
    (the boulder's second part, WickedGround); Civic Center's globe,
    the truck dance, the hotel gutter, the fireworks ("Grind the Fireworks
    Rail", 2.2) and Venus (a wall ride on the statue). Do each and watch
    the traffic vanish and come back. lockNPCFromAI, createNPCPointer,
    destroyNPCPointer and setVehicleVolume are called by no script on the
    disc (unlockNPCFromAI only by ASSETS/LEVELS/TUT/MOVIE.BIN, which no
    level loads): their parts are dead in the shipped game. Validates the
    setTrafficVisible and traffic-light rows of list `ch-traffic`.
    paths6's `cov-syncs-1` and `cov-syncs-2` send these objects their
    syncStart and play events by guest call (test only): all ten
    setTrafficVisible parts are recorded. The traffic-light rows are the
    TrafficLight object type's, which only DM2's COMMERCI level has (now
    `data-dead`). Playing the challenges is still the natural path.
  - Challenges with audio sync (initializeAudioSync: 36 calls in the seven
    careers, a grind, hand plant or wall ride on a level object: Cannery's
    swordfish grind (with the Cannery key), Civic Center's gargoyle hand
    plants, the scales of justice and wings grinds, Venus's wall ride).
    The other natives of list `ch-objectives` (displayObjectiveText,
    displayObjectiveTitle, setTimerState, getTimerState,
    extractFloatFromTagData, addBonusTrickToTrickQueue) are called by no
    script on the disc, and setBonusObjectiveState only by TUT/MOVIE.BIN
    and LOCAL/LOCALT.BIN: 0x144F60, 0x1BE3F8, 0x1BE438, 0x1DB2E0 and
    0x1DB318 (each called only by one of those natives) are dead in the
    shipped game; 0x1B3A00, 0x1B2060, 0x1BE308 have other callers.
    paths7's `cov-textobj` and `cov-local` record those three (text trick
    objects put in the queue, LOCAL's bonus objective set: test only).
  - The careers' in-level editors, played to their ends. The routes
    `cov-crane`, `cov-pisser`, `cov-vacuum` and `cov-construct` (paths3,
    tools/routes.py `keys`, `giver`, `tool`, `dochallenge`, `bank`) reach
    the three editors through their givers on mc-sweep-unlocked with the
    keys poked (all of `ch-construct` and `pe-vacuum`, 10 of 13 `pe-crane`,
    5 of 7 `pe-pisser`, 48 `pe-core` rows) and complete Cannery's
    "Construct and Score" (a banked trick score, test only). Left for a
    person: Industrial "Clear the Junkyard" to its end (the points count
    only inside the junkyard, past the doors behind the machine; drop a
    piece in a melt pit first), Airfield "Place and Trick on 6/8/10 Items"
    to its end (one line over every placed piece) and the Pisser Tool's
    every button with pieces in place.
  - Level objects' explosions and splashes from their own events. The
    natives createExplosion and createSplash are called only in level
    objects' anim events, which start when a challenge is done (paths4,
    from the level scripts): Cannery's propane tank (AnimObj_Propanetank,
    after "Grind Transfer the Pipes" by the propane engineer, Wilikai
    level five challenge 7) and its harpoon (animObj_FireHarpoon, the
    harpoon challenges 10 and 12), Boardwalk's boulders (AnimObj_Boulders,
    explosion and splash), Airfield's snail (AnimObj_SnailExplode) and
    Museum's tank (animObj_FireThe_tank). Do each with the recorder on for
    list `fx` and watch the explosion. The routes `cov-emitters-K` and
    `cov-splash` reach the effects' code by guest calls of the natives'
    callees (test only), not through the natives themselves.
    paths6's `cov-syncs-5` sends Cannery's propane trigger its
    objectiveComplete and the harpoon its syncStart (test only): the
    natives createExplosion and createSplash and their internals are
    recorded. In `cov-syncs-4`, `-6` and `-7` the Boardwalk's boulders,
    Airfield's snail and the Museum's tank did not reach the anim events
    that explode: each object handles syncStart only in its state `active`
    and was still in `init` (paths7). paths7's `cov-explode-4`, `-6` and
    `-7` set the state (or send Airfield's two getReady events) first and
    the three explode (createExplosion 6 times each, no code the corpus
    lacked); playing the challenges (Boardwalk's DamAssigner with the key,
    Airfield's balloon switch and glass, the Museum's bankRobber) is still
    the natural path and the one to watch.
  - Level objects' messages from animation events (paths5): the handlers
    FUN_001E0B80 (SOUND_EVENT objects' "SOUND_EVENT"), FUN_001E0AE8 (their
    0x7002), FUN_001E1C48 / FUN_001E1E60 (NPCs' 0x7002 and
    "ListenForActions") and FUN_001E69F0 (an object's "play") are sent by
    animation event tracks, not level scripts; play the challenges whose
    completion runs a level animation (the explosions above, Boardwalk's
    boats, the Museum's tank) with the recorder on for list `npc`.
    paths7's `cov-npcmsg` sends four of them by guest call (the Movie
    Lot's kelly and madScientist, AnimObj_BoulderPartOne: test only); the
    "play" handler FUN_001E69F0 waits for an object of its class, which
    none of those is.
  - Memory card swap: with `PS2X_SAVES=card` on PCSX2 (or a PS2) with two
    cards, save, swap the card in slot 1 for another, then load or save
    again ("Different memory card inserted, mount lost"). Validates 1
    (0x120E28), list `card-swap`; the runtime has no card swap, which would
    make this automatable.

- **Xbox Skaters in the app.** An app built with the Xbox files (the
  pack's models/ folder). Settings > Display > Xbox Skaters is On by
  default (readable5). Play each of the ten Xbox skaters for a while: Grob,
  Fogarty, Boekhorst, Salerno, the Mummy, and the five that need the
  readable packet builder, Khris, Lindenmuth, Chrissy (HOOTIE), the
  Goddess (STATUE) and Jordan (a level, tricks, bails, a split-screen
  round with two of them, the career level select's skater preview): the
  skater as on the Xbox (the Mummy's headdress, Fogarty's sleeves),
  animations and shadow as with the PS2 model, no missing limbs, also
  with the GPU renderer's strict path and with the CPU renderer. Off: the
  PS2 skaters from the next level load. Checked by agent: one career level
  each, native and strict shots, the drawn part's triangles equal to the
  file's (readable5).

- **The readable skater states past the Tutorial's first goals.** The
  default build. Checked by agent on PS2Recomp readable-game6
  (tools/routes.py `moves`): hand plants, both poles, skitching, wall rides
  and the jump off the wall, vaults, with every readable skater function
  identical in them and the wall ride functions Verified; readable-game7
  added double grinds (the Museum's double rails, `doublegrind`) and made
  every one of these states' callbacks readable and Verified;
  readable-game8 made the rest of the state machine readable and Verified
  and added routes to a spine transfer (`spine`) and a traffic hit
  (`traffic`). Left for a person: double grinds on the Boardwalk's roller
  coaster track, landings and take-offs off ramps with launch values (no
  route found one); expect them to feel as with `PS2X_READABLE=off`. Wall
  taps cannot happen on the PS2 (the state is never entered).
  readable-game9 made the code under the states readable (the move's
  collisions and traffic, the trick input, the grind searches, the
  slide-off, the score); readable-game10 made it Verified and on by
  default, with the grind trick combining and the hazards (water, fire,
  out of bounds) checked by agent (tools/routes.py `grindtricks`,
  `hazards`). Left for a person: tricks in lines with the score banked on
  landing, the HUD's score counter and trick text, changing grinds on a
  rail (d-pad and TRIANGLE while grinding), a hit by a car, falling into
  water, grinds onto the next rail, double grinds, a grind into a wall;
  expect them as with `PS2X_READABLE=off`.
- **The bone sway with every skater.** Skaters 1 and 3 have the swayed
  body bones and were recorded; no skater tried has the bob or up/down
  controllers (Player +0x910 / +0x91C). Pick each skater in free skate
  with `PS2X_FNREC=skaterSway PS2X_FNREC_DIR=/tmp/rec-sway`, skate a
  minute, and run `ps2xFnCheck --cycles /tmp/rec-sway`: expect every call
  identical; calls charging more than 136 cycles are the bob paths
  (readable-game5).
- **No hitches riding into new areas** (PS2Recomp gsspikes; checked with
  scratch runners on the Mac and the Windows worker, a runner built as
  make-windows.ps1 builds the app). In the Windows app (GPU renderer, Xbox
  Textures on), ride the Museum through its first rooms for a minute or
  two: no stutter when a new room comes into view. The same on the Mac app.
  Note (appcheck, 2026-10-24): the packages of main 74d043b were built
  before gsspikes, so this needs the next package. Those packages measured
  as the "before" of gsspikes (developer start, three skate rounds, a 90 s
  frame trace from PS2X_CONTROL; the Windows app writes no PS2X_PERF_LOG
  lines): the Museum's first turn 95 and 102 ms in two Windows runs (112
  and 101 textures made on the GS worker at once); one of those runs also
  stood still 0.52 s every 1.76 s over its last 60 s (the game, not the
  window: 37 frames over 50 ms), not seen in the other run, so not
  reproduced (firstframes, 2026-10-24: not in 6 more Windows Museum rides
  of 182 s with gsspikes' code, 4 with its defaults and 2 with its
  old-behaviour switches; watch for it on the next package); Mac 60.0 fps
  average, lowest second 59.5. On the next package:
  `PS2X_DEV_START_LEVEL=6 PS2X_DEV_START_SKATE=3 PS2X_DEV_START_QUIT=3
  PS2X_DEV_UNLOCK_ALL=1` and a PS2X_CONTROL file with `ftrace start` /
  `ftrace stop FILE` give the same numbers through tools/framespikes.py.

## Dave Mirra BMX 2

- **The held-register table's frame time on the Thor (tools18).** ee/held.h works out, at each guest call of a
  readable function, the callee-saved registers the original holds there: about 43 ns of host time a call on the
  Mac (AI's career, about 12,600 calls a tick: some 3 % of a 60 fps frame). The Thor is far slower. On the Thor,
  in DM2's Woodward (Free Ride, the first minute of riding), read the frame time (the debug overlay's frame ms, or
  `PS2X_FNPROF`'s total) with the table on (the default) and with `PS2X_READABLE_HELD=0`, the same build, the same
  minute; then the same for AI's Industrial. Note both numbers and whether either side drops below 60 fps; a
  table that costs the Thor frames needs its per-call work cut (the register values handed to each guest call are
  most of it).
- **Venice's and Greenville's career by hand** (about an hour; Settings >
  Game > Xbox Levels on). Seen 2026-10-24 by agent (appcheck) on the
  collection apps of main 74d043b (Mac; Windows a scratch copy): in a new
  career ProQuest lists Venice (8 Rights from Woodward, 18 gaps, 5,000
  respect) and Greenville (9 Rights, 10 gaps), both "Play more to unlock
  this level", and Free Ride shows them locked too; with
  `PS2X_DEV_UNLOCK_ALL=1` both load in ProQuest (Mac 7.1-7.2 s, Windows 7.8
  and 8.2 s), show their Xbox instructions and rookie goal ("clear 100,000
  points in a single run") and ride (Mac 60.0 fps, lowest second 59.0-59.5);
  on Windows Greenville's local pro (Ryan Nyquist) answered "You should work
  on passing the rookie test". The "all levels" cheat then got every key
  twice ('1133224411113344112255') by any input; fixed 2026-10-24
  (cheatkeys): by pad script, host keyboard
  script, control channel and one SDL virtual gamepad the keys come once
  and Venice opens in ProQuest. Left for a person: the cheat at the main
  menu (Up Left Down Right Up Up Left Right Up Down Square) with one real
  gamepad and, separately, with the keyboard: the message comes and
  Venice and Greenville open; then ride each: the rookie
  score goal, talk to the local pros, a few amateur and pro goals (Venice:
  the statue heads, the barges; Greenville: the gates, the soda machines,
  the courthouse stall), a few gaps, a competition; save, quit, load: the
  progress is there, Woodward Camp's is its own. Without the cheat:
  finishing Airport's pro tier opens Venice, the Commercial District's opens
  Greenville (and Galloon).

- **One real gamepad, then two (cheatkeys, about 15 minutes, both games).**
  Since 2026-10-24 the second pad port is empty without a second gamepad.
  With one real gamepad (and the keyboard): DM2's main menu greys
  Multiplayer and Options' Controller 2; DM2's all-levels cheat (above)
  opens the levels; AI's main menu skips the MULTIPLAYER icon; AI's cheat
  keyboard (Options > Cheats) takes a code (`sddsasasddd` opens every
  level). Plug a second real gamepad in at the main menu: Multiplayer comes
  back in both games; in AI's split screen (Movie Lot, Most Points) each
  pad drives only its own skater, and player 2's skater select takes only
  pad 2; in DM2's multiplayer (any game) player 2's rider select and turn
  take only pad 2. Unplug pad 2 in a split-screen run: say what the game
  does (a PS2 game may pause and ask for the controller). Checked by agent
  with SDL virtual gamepads only (`PS2X_VIRTUAL_PADS`).

- **A real gamepad, step by step (both platforms, about 20 minutes).** Only SDL virtual
  gamepads have been tried. With a real pad:

  1. Start the game and plug the pad in. The log (`~/Library/Logs/Aggressive
     Inline/aggressive-inline.log`; Windows: `%APPDATA%\Aggressive
     Inline\Logs`) shows `[pad] gamepad connected:
     player 1, "<name>", type ..., mapping yes, rumble yes`; Settings >
     Controls > Gamepads shows its name.
  2. Main menu: every face button, the d-pad, both sticks, L1/R1, both
     triggers, Start, Select (Back), L3/R3. The prompts switch between the
     keyboard's and the pad's style as you alternate.
  3. Settings > Controls > Test Rumble, Cross: the large motor, then the
     small one. Cross on Gamepads writes the pad's details to the log.
  4. Options > Controller > Vibration ON, skate Movie Lot and bail: the pad
     vibrates, then stops. With Vibration OFF it does not.
  5. Unplug the pad while skating, then in the pause menu: the log says
     "gamepad removed", nothing keeps vibrating, the keyboard keeps working.
     Plug it back: "connected" again, the pad and its prompts come back.
  6. Two pads: the first connected is player 1 (light 1), the second player
     2. In split screen each pad drives only its own skater and vibrates
     only for its own player. Swap Pads 1 and 2 exchanges them. Unplugging
     player 1's pad makes the other one player 1.
  7. Settings > Controls > Gamepad Off: no pad input, no vibration. On again:
     the pads work.
  8. A `gamecontrollerdb.txt` next to controls.ini, restart: the log shows
     "gamepad mappings from ...: N new" and "gamepad remapped" for a pad it
     covers.
  9. Steam Input: with Steam running a pad may appear as a generic Xbox pad
     (Xbox prompts, rumble through Steam). Set Button Prompts to the pad's
     style, or turn Steam Input off for the game.
  10. Photo mode on the pad: Photo Mode in the pause menu, sticks move and
      look, L1/R1 roll, Cross saves a picture, Circle goes back.

  Report the log lines of steps 1, 3 and 5 if something does not work.

- **Parks, replays and the boot card check** still use the memory card
  dialogs (native saves cover the game save only): save and load a park
  and a replay; the dialogs should work and the files survive a restart.
- **Z-Axis Collection on Windows, a long session:** switch games a few
  times (back to the collection's menu, one process), play each for a
  while; saves stay per game, and Task Manager's memory for the app comes
  back to about the same after each return to the menu.
- **Restart in a ProQuest run and in a Session.** Pause > Restart (above
  End Run) was checked in Freeride only (rider back at the start, rides
  on). In a ProQuest level with goals and in a timed Session run: Restart,
  then expect the run's timer, score and goal progress back at their start
  values and no leftover trick or bail state; End Run afterwards still
  ends normally. (dm2 round 4, `PS2X_RESTART=0` turns it off.)
- **Z-Axis Collection, File > Switch Game… and the Switch Game key (collmenu2).** In the
  collection app, play Aggressive Inline, choose File > Switch Game…: the game ends (settings
  saved) and the collection's menu comes back in the same window (no new process, no picker
  box); pick Dave Mirra. Turn widescreen on there, go back to the menu and into Aggressive
  Inline: widescreen is on there too (display, graphics, audio and general settings are shared
  at the collection's root; each game's options and controls stay in games/<id>/settings.ini
  and controls.ini). On the menu File > Switch Game… and View > Widescreen are greyed out.
  Windows has no menu bar: there (and on the Mac too) the Switch Game key does the same, F8 by
  default (`[hotkeys] switch_game` in each game's controls.ini); F8 twice quickly leaves once.
- **Mouse-look in Dave Mirra BMX 2 with a real mouse** (PS2Recomp dm2-mouse;
  checked by host input scripts on the Mac only, in Freeride at Woodward
  Camp). Ride with the mouse turning the view: slow turns and fast flicks
  all the way round, in each Options > Camera Type (Wide, Behind, Above),
  past rails and walls (the game's camera collision pushes the eye and it
  eases back), up a quarter pipe with the view turned 180 (the camera
  trails the rider, as the game's camera does). Expect the view to follow
  the mouse with nothing wrong or missing drawn. Pause: mouse motion
  dropped, the angle kept; Restart: back behind the rider; R or the middle
  button recentres; [mouse] mode = stick (Settings > Controls > Mouse
  Camera: Stick) leaves the camera to the game. Also: a ProQuest level and
  a Session run (other camera modes there were not seen), two players
  (only player 1's camera turns), and on Windows.
- **Dave Mirra BMX 2's Xbox levels, Venice and Greenville** (PS2Recomp
  dm2xbox). In an app built with `GAMES="ai dm2"` and the DM2 Xbox disc
  image: Settings > Game shows Xbox Levels (on by default; it applies at
  the next start) only in Dave Mirra. Seen 2026-10-24 by agent (appcheck) on
  the collection apps of main 74d043b (Mac and a Windows scratch copy, Xbox
  Levels on in settings.ini): Free Ride's level select goes past the park
  editor to "Venice, Italy" with its Xbox picture and text; since the career
  round (dm2xcareer) both are locked in a new career ("Play more to unlock
  this level") until unlocked as on the Xbox; with `PS2X_DEV_UNLOCK_ALL=1`
  Venice loads in Free Ride and rides on both (Windows shots; Mac: loaded in
  7.2 s, 60.0 fps, lowest second 59.5), and both levels in ProQuest (see
  Venice's and Greenville's career above). Left for a person: in Greenville
  ride the dirt park, freeway and campus; look for textures missing or
  wrong, see-through ground or walls, grass drawn transparent, triangles
  missing or stretched, places where the rider falls through or hits
  something invisible. With Xbox Textures off (Settings > Display in
  Aggressive Inline; the collection shares it) the converted textures; on,
  the sharper Xbox ones. End Run returns to the menu; Woodward still loads
  its own level; View Records shows each level's own.
- **Greenville against the Xbox** (xemu, DM2 Xbox disc; about an hour: no
  snapshot exists). Unlock Greenville in the Xbox career, stand at its
  start and a few landmarks (the dirt park, the freeway, the campus) and
  take xemu's F12 shots; then the same places in the port (GPU renderer,
  Xbox Textures on, widescreen off). Expect the same geometry and
  textures; known differences: the Xbox's grass blades and reflections are
  not drawn, the cannon does not work, semi-transparent decals are cut out
  or opaque.
- **Dave Mirra BMX 2's level meshes drawn natively** (PS2Recomp ailevel6;
  checked with scripted pads on the Mac runner and a Windows scratch runner,
  one player, not in a packaged app). In an app built with `GAMES="ai dm2"`
  (and the DM2 Xbox disc), GPU renderer: Woodward and Greenville look as
  with Settings > Display > Native Meshes off (through the PS2's VU1): no
  missing or flickering walls, the sky and the far panorama in place,
  nothing cut off near the camera when riding close past a wall or under a
  ramp, textures (Xbox Textures on and off) on the right faces. Switch
  Native Meshes off and on while riding: the picture stays the same. Also
  on Windows.
- **The collection's menu on the screen (collmenu2, about 15 minutes).** Headless runs drive the
  menu by script only (menu -> ai -> menu -> dm2 -> menu -> bmx -> menu -> quit, one process); its picture and its window are for a person.
  Start the collection app (no `--game`): it opens on the menu, titled Z-Axis Collection: three
  tiles in a row on a dark copy of the chosen game's picture, each the game's own (Aggressive
  Inline's loading screen logo, Dave Mirra's title from its legal screen, BMX XXX's start
  screen), the name under the chosen tile and Quit below, all text in Aggressive Inline's menu
  font. Judge whether it looks like part of the games, whether text is sharp at the window's
  size and in full screen, and whether the pictures are the right ones to stand for each game.
  Then: the arrows, Enter, Escape (to Quit), the mouse (hover chooses, click plays), a gamepad's
  D-pad, left stick, Cross and Circle. From the menu: Aggressive Inline, a level, its own
  Quit to Desktop item (main menu): back to the menu, not out of the app; Dave Mirra Freestyle
  BMX 2, a level, F8; BMX XXX, the Bronx in HARDCORE TOUR, File > Switch Game. Each time the
  menu comes back on the game just played, the window title follows the game (the game's name,
  then Z-Axis Collection on the menu), each game starts on its own riders and maps, its saves,
  options and controls are its own (`games/<id>`) and the shared settings carry over. Full
  screen (View > Enter Full Screen, Alt+Enter, Borderless in Settings) stays on through every
  switch, in and out of games; the window's size too. The window's close button and Cmd+Q quit
  the app from the menu and from inside a game (Aggressive Inline's autosave runs). With the
  GPU renderer, the menu replaces the last game frame at once (no stale picture under it).
- **Exit to Collection in each game's own menus (exitgame1, about 15 minutes).** Headless runs chose
  every item by pad script in one process (menu -> ai -> menu -> dm2 twice -> bmx twice -> menu -> quit) and took CPU-renderer shots of each; left for a person:
  how they look at the window's size and in full screen, with a real gamepad and the keyboard. In the
  collection app: Aggressive Inline's main menu, Back (Escape or Triangle): the question reads Exit to
  Collection? (French Retour à la collection ?, German Zur Sammlung?, Spanish ¿Ir a la colección? with
  Settings > Game > Language), Yes: back to the collection's menu; in a level, Pause: Exit to Collection
  after Exit Game (Career, Free Skate, the Tutorial's lesson list, split screen), Are you sure? Yes: the
  menu (the autosave runs). The single-game Aggressive Inline app still says Quit to Desktop and quits.
  Dave Mirra BMX 2: the main menu's last line Exit to Collection (the frame is a line taller and wider:
  judge whether it still looks like the game's; with one pad Multiplayer greyed above it), and Pause >
  EXIT TO COLLECTION after End Run (Freeride, a ProQuest level, a Session run). BMX XXX: the main
  menu's last line EXIT TO COLLECTION (it reaches the box's edges: judge the fit; the picture and text
  on the right keep the line above's), and Pause > EXIT TO COLLECTION after End Run (HARDCORE TOUR, and
  a MULTIPLAYER game with two pads). Each returns to the collection's menu on that game; F8 and File >
  Switch Game still do too, and the window's close button and Cmd+Q still quit the app.
- **BMX XXX's FREE RIDE with two pads (about 10 minutes).** BMX XXX lists FREE RIDE among its
  multiplayer games (with SESSION, RANDOM SPOT, HIGH FIVE, SKILLZ, B-M-X); MULTIPLAYER is greyed
  in agents' runs, which have one pad, and agents have played HARDCORE TOUR only (an open level with its challenge givers).
  With two real gamepads: MULTIPLAYER, FREE RIDE, a rider each, a level: both ride BMX XXX's riders
  on BMX XXX's map, each pad its own rider.

## BMX XXX

- **Exit to Collection in BMX XXX's menus (enh2, about 10 minutes).** The pause menu's lines are now
  added after the game's handler has made the menu (the handler and the main menu's builder are
  readable code, whose calls carry no return address of the original's, so the old return-address
  match never fired). In the collection app: BMX XXX's main menu has EXIT TO COLLECTION last and it
  leaves for the collection's menu; in a level (HARDCORE TOUR, the Bronx) the pause menu has RESTART
  after Exit Game (ux1: the collection's lines are one block after the game's) and EXIT TO COLLECTION last; RESTART puts the rider back at the start with the
  level's goals screen, EXIT TO COLLECTION leaves. Also on Windows.
  With the app's settings on (enh4), PHOTO MODE comes between End Run and EXIT TO COLLECTION: the
  nine lines all inside the pause panel (eight measured in the runner, the ninth only in the app), and
  PHOTO MODE starts photo mode with the pause menu back after it.
- **BMX XXX's park editor from the main menu (enh4, about 10 minutes).** In the app, BMX XXX's
  settings: Park Editor on (it asks for a restart). The main menu has PARK EDITOR third (after
  MULTIPLAYER); PARK EDITOR > NEW PARK > a theme opens the editor; pieces placed, the pause's SAVE
  (NEW, the two keyboards' END) writes Saves/Parks/Park 000.sav; EXIT, then PARK EDITOR > SESSION
  lists the park with its name and author and rides it. Off: the main menu as the game made it.

## The Windows app

Agents run the Windows app headless only (null GS, no window, no pad, no sound): each game boots, its
front end walks its Settings page, Exit to Collection reaches the menu and the menu quits (winapp3).
By hand, in `dist\Z-Axis Collection\Z-Axis Collection.exe` (its files in `%APPDATA%\Z-Axis Collection`):

- **Gamepads (about 10 minutes).** An Xbox pad and a DualSense or DualShock 4, by USB and Bluetooth.
  The collection's menu moves with the D-pad and the stick and starts a game; in each game the front
  end, riding or skating, the pause menu (Start) and vibration. The button prompts follow the pad last
  used (Xbox letters, PlayStation shapes; the keyboard's keys after a key press). Unplug the pad in a
  level and plug it back: it carries on with the same pad. A pad SDL does not know works once its line
  is in `%APPDATA%\Z-Axis Collection\gamecontrollerdb.txt`.
- **The Settings pages (about 10 minutes).** In each game, Options > Settings and the pause menu's
  Settings: every section, with the pad, the keyboard and the mouse. Display Mode window, borderless
  and full screen (also Alt+Enter, F11) on the monitor in use; internal resolution, vsync and the GPU
  renderer; a key rebound in Controls works at once. Quit and start again: the values are kept
  (`games\<id>\settings.ini`, `controls.ini`).
- **Saves and profiles (about 15 minutes).** In each game save progress (AI's career, DM2's
  ProQuest, BMX XXX's Hardcore Tour), Exit to Collection, quit, start again: it loads (`games\<id>\Saves`).
  A second save profile keeps its own progress. With a PCSX2 card that holds the game's save in
  `Documents\PCSX2\memcards`, the game's first start offers the import once and the save loads.
- **Paced play and sound (about 10 minutes).** Each game for a few minutes with the window and sound:
  steady frame rate, no crackle, the movies play; a song in `games\ai\Mods\Music` (01.ogg ...) plays
  in its place. The window's close button and Alt+F4 quit the app with no process left behind.

## The first-run setup
- **Android: a game set up from its PS2 disc image through the documents UI (thorapp4, about 15 minutes).** The Thor
  had no PS2 images in shared storage, so only the refusals were seen there: an Xbox image picked for AI ("is an
  Xbox disc") and the Xbox ROM folder picked whole ("No PS2 disc found"). The copy itself was checked by the unit
  tests' documents. On an Android device with the USA PS2 images in shared storage, Settings > Games: pick AI's ISO
  (Set Up From Image), DM2's BIN and CUE together, then a folder holding all three (Set Up From Folder). Each game
  should be "From <name>, <size>" and should boot. Then quit, start the app again and check that Repair From <name>
  is offered: the grant has to last a restart.

- **Setting the games up by hand (setup1).** A collection built with `NO_GAME_DATA=1` (`-NoGameData` on Windows), started
  with an empty support folder, on the Mac and on Windows: the menu shows the three tiles without pictures, in the
  built-in font. Click Aggressive Inline: the system's file dialog asks for its disc; pick the ISO: the notice line counts
  "Copying files" up to 100 % (Escape stops it, leaving nothing), then says it is set up; the tile gets its
  picture and the menu its usual font. Drag the Dave Mirra CUE (or the folder holding the BIN and CUE) onto the window:
  it sets up DM2 without asking which game. Drag the Xbox or GameCube image: the notice says to use the PS2 disc. Drag
  the European or another game's disc: the notice names what it is. Settings > Games: each game's
  line ("From <image>, <size>"), Check Files (a progress count, then "All files match"), Repair From <image>,
  Remove Files (asks first; afterwards the game's tile asks for its disc again). Each game then boots and plays.
