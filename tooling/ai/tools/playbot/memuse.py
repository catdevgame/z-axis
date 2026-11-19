#!/usr/bin/env python3
"""The game's own memory counters during a run, read through the play link: the malloc arena (newlib's malloc,
runtime/src/game/sce/libc_malloc.cpp: the arena reserveMallocArena takes at boot, its top chunk, its chunks in use)
and, from the run's log, the GS texture memory audit's lines (PS2X_GSMEM_AUDIT=1: AI, DM2 and BMX XXX) and the
runtime heap's own counters per level (PS2X_TRACE_HEAP=1, set for a park run with PARK_MEMORY: heap()).

    sample = memuse.Arena(run, game).sample()     dict(program_end, high_water, heap_reached, zero_gap)

Under this runtime the game's malloc is the runtime's heap (runtime/guest_heap.h, host side: first fit from the
program's end up), so the guest's newlib counters below stay 0 and the measure is the RAM's own: how far up from the
program's end the heap has ever written (ram_high_water). On the PS2 (newlib's malloc) the arena is malloc'd whole and freed at boot (reserveMallocArena), so sbrk is never called again: `arena` is its
size, `high` how far allocations have reached into it (the top chunk's start less the arena's start: the high-water
mark, as nothing gives memory back to sbrk), `in_use` the chunks in use now (each chunk's size, in use when the next
chunk's PREV_INUSE bit is set). AI's addresses are readable_symbols.tsv's; DM2's and BMX XXX's are those the bind
tables (GAME_BIND_DIR/<game>.tsv) give for AI's.
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..'))
import zpaths  # noqa: E402

SYMBOLS = zpaths.tree_path(zpaths.ZAXIS_MAIN, 'runtime', 'tools', 'readable_symbols.tsv')
NAMES = ('initialTop', 'g_mallocTop', 'g_sbrkBase', 'g_sbrkedMem', 'g_programEnd')


def ai_symbols():
    out = {}
    with open(SYMBOLS) as f:
        for line in f:
            w = line.rstrip('\n').split('\t')
            if len(w) >= 3 and w[0] == 'data' and w[1] in NAMES and (len(w) < 5 or not w[4]):
                out[w[1]] = int(w[2], 16)
    return out


def game_symbols(game):
    ai = ai_symbols()
    if game == 'ai':
        return ai
    path = os.path.join(zpaths.PATHS.get('GAME_BIND_DIR', os.path.join(zpaths.ZAXIS_SCRATCH, 'workdb', 'bind')), game + '.tsv')
    by_ai = {}
    with open(path) as f:
        for line in f:
            w = line.rstrip('\n').split('\t')
            if len(w) >= 4 and w[0] == 'data' and w[3]:
                by_ai[int(w[1], 16)] = int(w[3], 16)
    return {k: by_ai[v] for k, v in ai.items() if v in by_ai}


class Arena:
    def __init__(self, run, game):
        self.run = run
        self.sym = game_symbols(game)

    def sample(self, walk=False):
        """The heap's high-water mark from the RAM (ram_high_water), above the program's end; walk: newlib's chunks
        too (only where the game's own malloc runs: under this runtime its counters are 0)."""
        end = self.sym['g_programEnd']
        high, gap = ram_high_water(self.run, end)
        out = dict(program_end=end, high_water=high, heap_reached=high - end, zero_gap=gap)
        if walk:
            out.update(self.newlib())
        return out

    def newlib(self, walk=True):
        u32 = self.run.u32
        base = u32(self.sym['g_sbrkBase'])
        size = u32(self.sym['g_sbrkedMem'])
        top = u32(self.sym['g_mallocTop'])
        start = (base + 15) & ~15
        out = dict(arena=size, high=top - start, top_free=u32(top + 4) & ~7 if top else 0)
        if not walk:
            return out
        in_use, chunks, largest, p = 0, 0, 0, start
        # the chunks' headers: read the arena below the top in large pieces, walk them here
        span = top - start
        blob = bytearray()
        at = start
        while at < top:
            n = min(0x40000, top - at)
            blob += self.run.mem(at, n)
            at += n
        blob += self.run.mem(top, 8)
        while p < top:
            size_word = struct.unpack_from('<I', blob, p - start + 4)[0]
            sz = size_word & ~7
            if sz < 16 or p + sz > top + 8:
                out['walk_broken_at'] = p
                break
            nxt = p + sz
            used = struct.unpack_from('<I', blob, nxt - start + 4)[0] & 1 if nxt < top + 8 else 1
            if used:
                in_use += sz
            else:
                largest = max(largest, sz)
            chunks += 1
            p = nxt
        out.update(in_use=in_use, chunks=chunks, free_largest=max(largest, out['top_free']), walked=span)
        return out


def ram_high_water(run, start, end=0x2000000, page=0x1000):
    """The guest heap's high-water mark, from the RAM itself: the runtime's own heap (runtime/guest_heap.h) takes the
    game's malloc calls, first fit in address order, from the program's end up, so nothing of newlib's malloc
    state above is kept (its counters stay 0); the RAM starts zeroed and the heap fills it from the bottom, so the
    longest run of zero pages between `start` and `end` (the stack sits at the top) starts where the heap has never
    reached. Returns (high-water address, the zero run's length in bytes)."""
    zero = bytes(page)
    pages = []
    at = start & ~(page - 1)
    while at < end:
        n = min(0x40000, end - at)
        blob = run.mem(at, n)
        for k in range(0, n, page):
            pages.append(blob[k:k + page] == zero)
        at += n
    best, run_start, best_len = None, None, 0
    for i, z in enumerate(pages + [False]):
        if z and run_start is None:
            run_start = i
        elif not z and run_start is not None:
            if i - run_start > best_len:
                best, best_len = run_start, i - run_start
            run_start = None
    base = start & ~(page - 1)
    return (base + best * page if best is not None else end), best_len * page


HEAP = re.compile(r'\[heap\] (.*?) \((ended|so far)\): 32 MB heap peak (\d+) bytes, (\d+) in use; extended peak (\d+) '
                  r'bytes, (\d+) in use; ticks (\d+)\.\.(\d+)')


def heap(log_path):
    """The runtime's own heap counters per level from a run's log (PS2X_TRACE_HEAP=1, runtime/heap_trace.h: the guest
    heap's bytes in use and its high-water mark, in the 32 MB and in extended memory, each level's stretch logged when
    it ends and every 600 vsyncs while it runs): {level: dict(main_peak, main_in_use, extended_peak, extended_in_use,
    ended, ticks, stretches)}, the peaks the highest of the level's stretches (a level played twice), the rest its
    last line's. Exact, where ram_high_water reads how far up the heap has ever written."""
    out = {}
    if not os.path.exists(log_path):
        return out
    with open(log_path, errors='replace') as f:
        for line in f:
            m = HEAP.search(line)
            if not m:
                continue
            g = m.groups()
            level, ended = g[0], g[1] == 'ended'
            prev = out.get(level)
            new_stretch = prev is None or prev['ended'] or int(g[6]) != prev['ticks'][0]
            out[level] = dict(main_peak=max(int(g[2]), prev['main_peak'] if prev else 0), main_in_use=int(g[3]),
                              extended_peak=max(int(g[4]), prev['extended_peak'] if prev else 0),
                              extended_in_use=int(g[5]), ended=ended, ticks=(int(g[6]), int(g[7])),
                              stretches=(prev['stretches'] if prev else 0) + (1 if new_stretch else 0))
    return out


GSMEM = re.compile(r'\[gsmem\] (.*?): (\d+) allocations(?:, blocks (\d+)\.\.(\d+) \(peak(?:: ([^)]*))?\), CLUTs from (\d+))?, (\d+) over the CLUTs, (\d+) past the end \((\d+)\), overruns in (\d+) of (\d+) frames')


def gsmem(log_path):
    """The audit's last line per level from a run's log: {level: dict(...)} (a GS block is 64 words, 256 bytes)."""
    out = {}
    if not os.path.exists(log_path):
        return out
    with open(log_path, errors='replace') as f:
        for line in f:
            m = GSMEM.search(line)
            if m:
                g = m.groups()
                out[g[0]] = dict(allocations=int(g[1]), lowest=int(g[2] or 0), peak=int(g[3] or 0), peak_texture=g[4],
                                 cluts_from=int(g[5] or 0), over_cluts=int(g[6]), past_end=int(g[7]), blocks=int(g[8]),
                                 overrun_frames=int(g[9]), frames=int(g[10]))
    return out


if __name__ == '__main__':
    print(__doc__)
