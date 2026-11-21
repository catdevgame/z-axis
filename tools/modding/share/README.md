# zaxis_texdump

Dump, edit and put back the textures of three Z-Axis games that share one
engine:

- **Aggressive Inline**
- **Dave Mirra Freestyle BMX 2**
- **BMX XXX**

on all three consoles they came out on: **PS2, Xbox and GameCube**.

One Python file, no installation. It reads a backup image of a disc you
own and writes PNG files (or the pictures as the game stores them). Edited
PNGs can be put back into the game's files, and those files into a copy of
the disc image, for texture mods.

> **Only for your own disc backups.** The tool never opens anything but the
> image you give it, and it only reads that image. The pictures it writes
> are the games' art: keep them to yourself, and share mods as instructions
> or as your own edited PNGs, not as dumps or patched disc images.

## Requirements

- Python 3.8 or newer, standard library only (Windows, macOS, Linux).
- GameCube RVZ images compressed with zstd (Dolphin's default) also need
  one of: Python 3.14 or newer, the `zstandard` package
  (`pip install zstandard`), or the `zstd` command. Without them the tool
  says so; or convert the image to ISO in Dolphin first.

## Dumping

```
python3 zaxis_texdump.py IMAGE OUT [options]
```

`IMAGE` can be:

| console  | image                                                        |
|----------|--------------------------------------------------------------|
| PS2      | `.iso`, or a raw `.bin` CD image (or its `.cue`)               |
| Xbox     | an XISO (`.iso`), or a full image with the game partition     |
| GameCube | `.rvz` (Dolphin), or a plain `.iso` / `.gcm`                  |
| any      | a folder with a disc's extracted files                        |

The console and the game are detected from the image. `OUT` is a folder;
the tool refuses to write next to the image.

Options:

- `--png` (default) PNG files; `--raw` each picture as the game stores it
  (`.bmp`, `.tga`, `.dds`, `.xpr`, `.raw`); `--both`.
- `--only PATTERN` only matching files or textures. The pattern is matched
  against `disc/path` and `disc/path/texture name`, any case; without `*`
  or `?` any part matches (`--only TUT.TXR`, `--only "*/skaters/*"`).
  May be given more than once.
- `--list` list the textures (format, size, mip levels), write nothing.
- `--no-mips` skip the smaller mip levels.
- `--keep-key` show the PS2 colour key (magenta) opaque instead of
  transparent.
- `--game ai|dm2|bmxxxx` if detection gets it wrong.
- `-q` no per-file progress.

Output: one folder per game file, named and placed like the file on the
disc, holding one PNG per texture named after the texture
(`OUT/ASSETS/LEVELS/CIVIC/CIVIC.TXR/c_brik03.png`). Mip levels are
`name.1.png`, `name.2.png` and so on. A name that occurs twice in one file
gets `~2`, `~3`. `OUT/zaxis_texdump.json` records where every PNG came
from (`import` needs it). At the end the tool prints the files scanned,
the textures by format, and any failures.

Rough times on a recent laptop: PS2 images 1 to 4 s, GameCube 7 to 20 s,
Xbox 24 to 40 s (the DXT pictures are decoded in Python).

### What it reads

Every file that holds textures: level libraries (`.TXR`), front-end and UI
archives (`.ZAL`, `.ZIF`), rider, skater and model files (`.ZIT`), and
anything else with `TXRC` chunks; BMX XXX's LZO-compressed Xbox files; and
loose pictures on the discs (`.bmp`, `.tga`, `.dds`, `.xpr`, and
Aggressive Inline's 24-bit `.RAW` loading screens).

| chunk | console | format |
|-------|---------|--------|
| TBMP  | all     | Windows BMP, 4 or 8 bit with a palette (also 24 bit). Opaque; a palette entry of exactly magenta (255, 0, 255) is the colour key, shown transparent |
| TZMP  | all     | the same with the palette's alpha (0-255 in the files; the game halves it to the PS2's 0-128 range when it loads it) |
| TTGA  | all     | uncompressed TGA, 24 or 32 bit |
| TDDS  | DM2 Xbox, all GameCube | a DDS file after a 4-byte word: DXT1, DXT3, or 32-bit RGB. The GameCube release keeps PC-style DXT1 in its files (the game converts it when it loads) |
| TXPR  | AI and BMX XXX Xbox (and a few in BMX XXX PS2) | an XPR0 bundle with one Direct3D texture: DXT1/DXT3/DXT5, or uncompressed formats (A8R8G8B8, A8B8G8R8, R5G6B5, A1R5G5B5, A4R4G4B4, L8, A8, A8L8, V8U8, ...), swizzled or linear; volume textures (slices stacked top to bottom in one PNG) |

## Modding: putting textures back

1. **Dump** to PNG (not `--raw`): `python3 zaxis_texdump.py MyDisc.iso dump`
2. **Edit** the PNGs you want to change in any editor. Keep the size and
   the file name. Leave the others alone (or delete them: a missing PNG
   counts as unchanged).
   - Transparency: draw it with alpha. In PS2 `TBMP` textures anything
     more than half transparent becomes the colour key (fully see-through);
     `TZMP`, DXT3/DXT5 and 32-bit textures keep graded alpha; DXT1 has
     on/off alpha.
   - Mip levels (`name.1.png`, ...): if you only edit `name.png`, the
     smaller levels are redone from it where your edit reaches; edit them
     yourself if you want them a particular way.
3. **Import**: `python3 zaxis_texdump.py import dump mods`
   (`--image IMAGE` if the disc image moved since the dump) writes a modified copy of each game file that changed into `mods`, at
   the same path as on the disc. The original image is only read. Each
   picture is encoded back to the texture's own format and size, so every
   file keeps its exact size (except BMX XXX's compressed Xbox files, see
   below):
   - PS2 palette textures: if your edit uses only colours already in the
     palette, the palette stays as it was; new colours go into palette
     entries no pixel used, if there is room. Otherwise a new 16- or
     256-colour palette is made from the picture (median cut, no
     dithering; the rest of the texture can shift slightly), keeping the
     colour key. A texture's mip levels share its palette, as in the game.
   - DXT (Xbox, GameCube): only the 4x4 blocks you changed are encoded
     again; the rest is copied, so there is no loss outside your edit.
   - Swizzled Xbox textures are swizzled again.
   - A PNG of the wrong size is refused; `--resize` scales it to fit.
   - BMX XXX's Xbox level files are LZO-compressed chunks: an edited
     chunk is compressed again (a few seconds a texture). If it comes out
     smaller it is padded to its old size (the game ignores the padding);
     if it comes out larger the file grows and its `.ZIX` index (the chunk
     offsets) is updated and written too.
4. **Patch a copy of the image**:
   `python3 zaxis_texdump.py patch-image mods MyDisc.iso MyDisc-mod.iso`
   - PS2 ISO and Xbox XISO: a copy of the image with the files written in
     place.
   - A PS2 `.bin` CD image becomes a plain 2048-byte-sector `.iso` copy.
   - A GameCube RVZ becomes a plain GCM/ISO copy (RVZ cannot be patched
     in place; Dolphin plays the ISO, and can convert it back to RVZ).
   - A file that grew: on Xbox it is moved to the end of the image copy;
     on PS2 and GameCube it must fit in its old space, otherwise nothing
     is written and the tool says which file does not fit.
   - Every patched file is read back from the new image and compared.
   - `--force` overwrites `MyDisc-mod.iso` if it exists.

Then play the patched copy in your emulator (PCSX2, xemu, Dolphin) or on
your own hardware. The Z-Axis Collection app needs no patched image: put the
files `import` writes in a mod (`docs/MODS.md`, a disc path such as
`ASSETS/...`), or a texture pack in a mod's `Textures/`.

`import --reencode-all` treats every PNG as edited; it is a self-test of
the encoders (an unchanged dump comes back byte for byte).

## Not handled

- Xbox palettized (P8) and cube-map textures, and compressed volume
  textures: none of these occur on the three games' discs.
- The 16-bit legal and loading screens of DM2 and BMX XXX (`.raw` files
  other than Aggressive Inline's 24-bit ones): their layout is not known.
- PCSX2 texture replacement names: PCSX2 names a replacement by a hash of
  the texture as it sits in GS memory after the game converted it (these
  games scale the palette colours by a brightness value at run time and
  halve the alpha), which cannot be worked out from the disc files.
  Patch the image instead.

## Licence

MIT (see the top of `zaxis_texdump.py`).
