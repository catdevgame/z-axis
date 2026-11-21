#pragma once

// BMX XXX's Xbox and GameCube releases' content in its PS2 release's formats,
// made from the player's discs as tools/bmx_releases.py makes it (the
// reference: the same bytes but for the movies, which the game plays as the
// Xbox disc has them). The game override serves them, each behind its
// setting (game_overrides/bmx_xxx_releases.cpp, ZX_BMX_RELEASES_DIR).
// Under <out>:
//
//   challenges/xbox/<LEVEL>/OBJCTV.TXT
//       The challenge scripts of the four levels whose Xbox and GameCube
//       scripts (the same files) differ from the PS2's: CHL_MNCO, CHL_NASA,
//       CHL_SHEP, CHL_SYRA. They play the challenges' success and failure
//       lines, enable Sheep Hills' four pickup tethers, and fix a NASA and a
//       Syracuse step; as they are.
//   challenges/ps2-timers/<LEVEL>/OBJCTV.TXT
//       The same with each TIMEOUT value the PS2's (Syracuse's two 45 s
//       timers are 60 s on the Xbox and GameCube): the balance is the PS2's
//       unless the player picks the Xbox's (Gameplay Tuning).
//   strings/<table>
//       The French, German and Spanish string tables ("string.common.fre",
//       ...: the PS2 STRING.ZAL's entries by those names) with each text the
//       PS2 leaves in English that the GameCube (else the Xbox) translates,
//       where that release's English is the PS2's word for word. Only the
//       tables something is added to.
//   movies/ASSETS/MOVIES/MENU/<LEVEL>.XMV, movies/ASSETS/MOVIES/ZAXIS.XMV,
//   movies/ASSETS/MOVIES/COMPEDGE/TUROK.XMV, VEXX.XMV
//       The level select's preview movies, the Z-Axis logo movie and the two
//       Competitive Edge trailers only the Xbox has, the Xbox's
//       movies/menu/*.xmv, movies/zaxis.xmv and movies/CompEdge/*.xmv as
//       they are (WMV2); the game's movie player plays them for the PSM and
//       PSS files the PS2's level select, movie list and FrameAKAdemos name.
//   editor/OBJECT.OBJ
//       The park editor's piece list: the PS2's, with the pieces the
//       GameCube's (or the Xbox's) adds after the PS2's last, so every piece
//       keeps its number (a saved park names its pieces by number).
//   levels/ASSETS/LEVELS/<LEVEL>/<FILE>
//       The four levels only the GameCube disc has files for, whose rows the
//       PS2's level table already holds: CMP_SCRT (the Secret Level),
//       TST_FLAT, TST_GRND and TST_VERT, as they are (the PS2's formats),
//       names in capitals; a level with a texture not a TBMP is left out.
//   create/PARTS.ZAL, create/TEXTUREA.ZAL .. TEXTURED.ZAL, create/added.txt
//       The create-a-rider libraries: the PS2's, each with the entries the
//       other releases have and it lacks after its own, so every PS2 entry
//       keeps its place. Parts: the GameCube's two more, its 8x8 texture the
//       PS2's; textures: the Xbox's (else the GameCube's), but for one going
//       on a part these parts lack. added.txt names the textures added.
//   music/<NN>.WMA, music/songs.txt
//       The songs on the Xbox disc its music bank names none of, as the disc
//       has them (the Python tool's are MP3s), and their list: each one's
//       file, its Xbox file, its title (that file's name) and play time. The
//       game override serves them as MUSIC.ZSD's waves 20-27.
//   manifest.txt
//       What was made, with counts.

#include "collection/extras/source_files.h"

#include <filesystem>
#include <string>
#include <vector>

namespace zx::app::extras
{
    // `xbox` or `gc` may be null (not both). False with *error when a file
    // cannot be read or written, or the PS2 disc has no STRING.ZAL.
    bool makeBmxReleases(const SourceFiles &ps2, const SourceFiles *xbox, const SourceFiles *gc,
                         const std::filesystem::path &out, std::vector<std::string> &log, std::string *error);
}
