#!/usr/bin/env python3
"""A level script's goals for the Thor panel, read at build time from the
level's .BIN (runtime/src/game/aggressive/script/script.h has the format):
the objective table of its data block, and, by reading the handlers, what each
goal counts and which objects it wants done. Nothing here runs in the game.

A goal's handler ends in setObjectiveState(complete, index). From there:
- the counter: the `load v; sub N; ...` test before the call, N the goal's
  target, v the handler's object's variable. The Xbox release's scripts
  complete some goals later, in another object's handler (a reward
  animation's syncStart): there the counter is the test before the counting
  object's SEND to that object, the counting object being one the
  completing handler moves to another state;
- the score: what the value tested before the call is made of (the
  skater's score, the running line's points, a timed run's points), read
  from the handler's stores (`getScore; store 1; load 1; addVariable 2`);
- the targets: the objects that send the counting handler's event (a bike
  rack's grindStart sends bikeRackGrind to the helper that counts them); with
  no sender, the handler's object itself;
- a target is done when its handler moved it to another state, or set its own
  first-time flag (`push 1; store v`).

The game runs the PS2 disc's script or the Xbox release's
(aggressive_inline_careerscripts.cpp), whose variables can lie elsewhere:
each is read on its own, and the panel takes the one the game has loaded.
"""
import struct

import ai_level as al

END = 0xFFFFFFFF
OP_PUSH, OP_STORE, OP_CALL, OP_SEND, OP_SET_STATE, OP_LOAD = 0x0A, 0x14, 0x1E, 0x28, 0x32, 0x3C
OP_ADD, OP_ADD_VARIABLE, OP_SUB, OP_SUB_VARIABLE = 0x46, 0x47, 0x50, 0x51
OP_JUMP_ZERO, OP_JUMP_NOT_ZERO = 0x78, 0x6E
OP_SET_TYPE, OP_END = 0xBE, 0x1F40

SET_OBJECTIVE_STATE = al.name_hash("setObjectiveState")
CREATE_NPC_BUBBLE = al.name_hash("createNPCBubble")
GET_SCORE = al.name_hash("getScore")
GET_LINE_SCORE = al.name_hash("getPotentialScoreIncrease")
GET_CHALLENGE_SCORE = al.name_hash("getChallengeScore")
SET_CHALLENGE_SCORE = al.name_hash("setChallengeScore")
STATE_COMPLETE = al.name_hash("complete")
OBJECTIVE_RECORD = 0x2C
STATE_ENTER = al.name_hash("enter")

# GoalRecord.scoreKind: the skater's score, the running line's points, a
# timed run's points, the score with the running line's points (getScore() +
# getPotentialScoreIncrease(), the PS2's "Get N points")
SCORE_NONE, SCORE_TOTAL, SCORE_LINE, SCORE_CHALLENGE, SCORE_WITH_LINE = 0, 1, 2, 3, 4


class Handler:
    __slots__ = ("object", "state", "event", "code")

    def __init__(self, obj, state, event, code):
        self.object, self.state, self.event, self.code = obj, state, event, code


class Script:
    def __init__(self, data):
        version, code_words, var_words, data_bytes, string_bytes = struct.unpack_from("<fIIII", data, 0)
        at = 0x14
        self.strings = data[at:at + string_bytes]
        at += string_bytes
        self.data = data[at:at + data_bytes]
        at += data_bytes
        self.variables = struct.unpack_from("<%dI" % var_words, data, at)
        at += var_words * 4
        code = struct.unpack_from("<%dI" % code_words, data, at)
        self.handlers = []
        self.objects = {}    # hash: variable index (the state's word; variable k at + 1 + 2k)
        o = 0
        while code[o] != END:
            obj, rec = code[o], code[o + 1]
            self.objects[obj] = code[rec]
            s = rec + 2
            while code[s] != END:
                state, st = code[s], rec + code[s + 1]
                e = st
                while code[e] != END:
                    event, ip = code[e], st + code[e + 1]
                    ops = []
                    while True:
                        op, arg = code[ip], code[ip + 1]
                        ip += 2
                        if op == OP_END:
                            break
                        ops.append((op, arg))
                    self.handlers.append(Handler(obj, state, event, ops))
                    e += 2
                s += 2
            o += 2

    def string(self, offset):
        if offset >= len(self.strings):
            return ""
        return self.strings[offset:self.strings.index(b"\0", offset)].decode("latin1")

    def objectives(self):
        """The table: dicts of the record's fields, in the game's order."""
        out = []
        at = 0
        while at + 12 <= len(self.data):
            kind, count, size = struct.unpack_from("<III", self.data, at)
            at += 12
            if kind == al.name_hash("objective") and size == OBJECTIVE_RECORD:
                for i in range(count):
                    w = struct.unpack_from("<11I", self.data, at + i * size)
                    out.append({"index": i, "locked": w[0], "tier": w[1], "points": w[2], "state": w[3], "type": w[4],
                                "owner": w[5], "title": self.string(w[6]), "text": self.string(w[7]),
                                "title_at": w[6], "text_at": w[7],
                                "flythru": w[8], "camera": self.string(w[9])})
            at += count * size
        return out

    def state_var(self, obj):
        return self.objects[obj]


def calls_to_state(script):
    """{objective: [(handler, position of the call, state)]}."""
    out = {}
    for h in script.handlers:
        c = h.code
        for k, (op, arg) in enumerate(c):
            if op == OP_CALL and arg == SET_OBJECTIVE_STATE and k >= 2 and c[k - 1][0] == OP_PUSH and c[k - 2][0] == OP_PUSH:
                out.setdefault(c[k - 1][1], []).append((h, k, c[k - 2][1]))
    return out


def counter_before(handler, position):
    """(variable, target) of the last counter test before `position`, or None."""
    c = handler.code
    for k in range(position - 1, 0, -1):
        if c[k][0] == OP_SUB and c[k - 1][0] == OP_LOAD and 1 < c[k][1] < 256:
            var = c[k - 1][1]
            if any(c[i][0] == OP_ADD and c[i - 1][0] == OP_LOAD and c[i - 1][1] == var for i in range(1, len(c))):
                return var, c[k][1]
    return None


def senders_of(script, event, receiver):
    """The handlers that send `event` to `receiver`."""
    out = []
    for h in script.handlers:
        c = h.code
        for k, (op, arg) in enumerate(c):
            if op == OP_SEND and arg == receiver and k >= 2 and c[k - 2][0] == OP_PUSH and c[k - 2][1] == event:
                out.append(h)
    return out


def done_test(script, handler):
    """How a target's own handler marks it done: (word, value) to compare, or None."""
    c = handler.code
    for k, (op, arg) in enumerate(c):
        if op == OP_SET_STATE and arg == handler.object and k >= 1 and c[k - 1][0] == OP_PUSH and c[k - 1][1] != handler.state:
            return script.state_var(handler.object), c[k - 1][1]
        if op == OP_STORE and k >= 1 and c[k - 1] == (OP_PUSH, 1):
            return script.state_var(handler.object) + 2 + 2 * arg, 1
    return None


def tested_value(handler, position):
    """What the handler's last `load v; sub N` (N a points number) before
    `position` tests, as (N, sources): the natives and earlier values its
    variable was stored from ('score', 'line', 'challenge', 'before' for a
    variable read before it was set here), or None."""
    c = handler.code
    values = {}
    found = None
    for k in range(min(position, len(c))):
        op, arg = c[k]
        if op == OP_STORE and k >= 1:
            prev_op, prev_arg = c[k - 1]
            if prev_op == OP_CALL:
                values[arg] = {GET_SCORE: {"score"}, GET_LINE_SCORE: {"line"},
                               GET_CHALLENGE_SCORE: {"challenge"}}.get(prev_arg, {"other"})
            elif k >= 2 and prev_op in (OP_ADD_VARIABLE, OP_SUB_VARIABLE) and c[k - 2][0] == OP_LOAD:
                both = values.get(c[k - 2][1], {"before"}) | values.get(prev_arg, {"before"})
                values[arg] = both if prev_op == OP_ADD_VARIABLE else both | {"less"}
            elif prev_op == OP_LOAD:
                values[arg] = set(values.get(prev_arg, {"before"}))
            else:
                values[arg] = {"other"}
        if op == OP_SUB and k >= 1 and c[k - 1][0] == OP_LOAD and 1000 <= arg < 100000000:
            found = (arg, frozenset(values.get(c[k - 1][1], {"before"})))
    return found


def score_before(script, handler, position):
    """(kind, target) of a score test before `position`: the value the last
    `sub N` (N a points number) tests, or None."""
    natives = {arg for h in script.handlers if h.object == handler.object for op, arg in h.code if op == OP_CALL}
    tested = tested_value(handler, position)
    if tested is None:
        return None
    target, sources = tested
    if SET_CHALLENGE_SCORE in natives or "challenge" in sources:
        kind = SCORE_CHALLENGE             # a timed run: its object keeps the challenge's points
    elif sources == {"score", "line"}:
        kind = SCORE_WITH_LINE             # the score with the running line's points
    elif sources == {"line"} or "less" in sources:
        kind = SCORE_LINE                  # the running line's points, or score now less the score before
    elif sources == {"score"}:
        kind = SCORE_TOTAL
    else:
        return None
    # "more than 69,999": the goal's own round number
    return kind, target + 1 if (target + 1) % 1000 == 0 else target


def bubbles(script):
    """The NPCs' challenge bubbles: (NPC, the word of the handler's object's
    state, the state): the bubble shows while that object is in that state."""
    out = []
    for h in script.handlers:
        if h.event != STATE_ENTER:
            continue
        c = h.code
        for k, (op, arg) in enumerate(c):
            if op == OP_CALL and arg == CREATE_NPC_BUBBLE and k >= 1 and c[k - 1][0] == OP_PUSH:
                out.append((c[k - 1][1], script.state_var(h.object), h.state))
    return out


def senders_to(script, handler):
    """A goal completed in another object's handler (the Xbox scripts' reward
    animations): (handler, position) of each SEND to the completing
    handler's object from an object that handler moves to another state
    (the object that counted, or ran the timed run)."""
    moved = {arg for op, arg in handler.code if op == OP_SET_STATE and arg != handler.object}
    for h in script.handlers:
        if h.object in moved:
            for k, (op, arg) in enumerate(h.code):
                if op == OP_SEND and arg == handler.object:
                    yield h, k


def analyse(script):
    """Per objective: {counter: (word, target) or None, counted: (object,
    variable) or None, score: (kind, target) or None, targets: [(object
    hash, done test)]}."""
    sites = calls_to_state(script)
    goals = []
    for g in script.objectives():
        counter = None
        counted = None
        score = None
        targets = {}
        for h, k, state in sites.get(g["index"], ()):
            if state != STATE_COMPLETE:
                continue
            # the completing handler, then the handlers that lead to it (the
            # goal's own object's, where it has one, is the place, not the reward's)
            chain = list(senders_to(script, h))
            counting = next((c for c, _ in chain if c.object == g["owner"]), h)
            for c, at in [(h, k)] + chain:
                found = counter_before(c, at)
                if found and counter is None:
                    counting = c
                    counter = (script.state_var(c.object) + 2 + 2 * found[0], found[1])
                    counted = (c.object, found[0])
                if score is None and g["type"] in (0, 1):
                    score = score_before(script, c, at)
            senders = senders_of(script, counting.event, counting.object)
            for s in senders or [counting]:
                targets.setdefault(s.object, done_test(script, s) if senders else None)
        goals.append(dict(g, counter=counter, counted=counted, score=score, targets=targets))
    return goals
