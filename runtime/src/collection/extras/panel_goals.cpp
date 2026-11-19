// An Aggressive Inline level script's goals for the panel (panel_goals.h).
#include "panel_goals.h"

namespace ps2x::app::extras::panel_maps
{
    namespace
    {
        constexpr uint32_t kEnd = 0xFFFFFFFF;
        // The script machine's operations the goals are read from.
        constexpr uint32_t kOpPush = 0x0A, kOpStore = 0x14, kOpCall = 0x1E, kOpSend = 0x28, kOpSetState = 0x32,
                           kOpLoad = 0x3C, kOpAdd = 0x46, kOpAddVariable = 0x47, kOpSub = 0x50, kOpSubVariable = 0x51,
                           kOpEnd = 0x1F40;
        constexpr size_t kObjectiveRecord = 0x2C;

        // Where a stored value came from (tested_value's sources).
        enum Source : unsigned
        {
            FromScore = 1,
            FromLine = 2,
            FromChallenge = 4,
            FromOther = 8,
            FromBefore = 16, // a variable read before it was set here
            FromLess = 32,   // made by a subtraction
        };

        struct Names
        {
            uint32_t setObjectiveState = dm2::nameHash("setObjectiveState");
            uint32_t createNpcBubble = dm2::nameHash("createNPCBubble");
            uint32_t getScore = dm2::nameHash("getScore");
            uint32_t getLineScore = dm2::nameHash("getPotentialScoreIncrease");
            uint32_t getChallengeScore = dm2::nameHash("getChallengeScore");
            uint32_t setChallengeScore = dm2::nameHash("setChallengeScore");
            uint32_t complete = dm2::nameHash("complete");
            uint32_t enter = dm2::nameHash("enter");
            uint32_t objective = dm2::nameHash("objective");
        };
        const Names &names()
        {
            static const Names n;
            return n;
        }

        struct Site
        {
            const ScriptHandler *handler;
            size_t position;
            uint32_t state;
        };

        // (variable, target) of the last counter test before `position`.
        std::optional<std::pair<uint32_t, uint32_t>> counterBefore(const ScriptHandler &h, size_t position)
        {
            const std::vector<ScriptOp> &c = h.code;
            for (size_t k = position - 1; k >= 1 && k < c.size(); --k)
            {
                if (c[k].op == kOpSub && c[k - 1].op == kOpLoad && 1 < c[k].arg && c[k].arg < 256)
                {
                    const uint32_t var = c[k - 1].arg;
                    for (size_t i = 1; i < c.size(); ++i)
                        if (c[i].op == kOpAdd && c[i - 1].op == kOpLoad && c[i - 1].arg == var)
                            return std::make_pair(var, c[k].arg);
                }
            }
            return std::nullopt;
        }

        // The handlers that send `event` to `receiver`.
        std::vector<const ScriptHandler *> sendersOf(const LevelScript &script, uint32_t event, uint32_t receiver)
        {
            std::vector<const ScriptHandler *> out;
            for (const ScriptHandler &h : script.handlers)
                for (size_t k = 0; k < h.code.size(); ++k)
                    if (h.code[k].op == kOpSend && h.code[k].arg == receiver && k >= 2 && h.code[k - 2].op == kOpPush &&
                        h.code[k - 2].arg == event)
                        out.push_back(&h);
            return out;
        }

        // How a target's own handler marks it done.
        std::optional<DoneTest> doneTest(const LevelScript &script, const ScriptHandler &h)
        {
            const std::vector<ScriptOp> &c = h.code;
            for (size_t k = 0; k < c.size(); ++k)
            {
                if (c[k].op == kOpSetState && c[k].arg == h.object && k >= 1 && c[k - 1].op == kOpPush &&
                    c[k - 1].arg != h.state)
                    return DoneTest{script.stateVariable(h.object), c[k - 1].arg};
                if (c[k].op == kOpStore && k >= 1 && c[k - 1] == ScriptOp{kOpPush, 1})
                    return DoneTest{int64_t(script.stateVariable(h.object)) + 2 + 2 * int64_t(c[k].arg), 1};
            }
            return std::nullopt;
        }

        // What the handler's last `load v; sub N` (N a points number) before
        // `position` tests: (N, where v's value came from).
        std::optional<std::pair<uint32_t, unsigned>> testedValue(const ScriptHandler &h, size_t position)
        {
            const std::vector<ScriptOp> &c = h.code;
            std::unordered_map<uint32_t, unsigned> values;
            auto valueOf = [&](uint32_t var) {
                const auto it = values.find(var);
                return it == values.end() ? unsigned(FromBefore) : it->second;
            };
            std::optional<std::pair<uint32_t, unsigned>> found;
            for (size_t k = 0; k < std::min(position, c.size()); ++k)
            {
                const ScriptOp &op = c[k];
                if (op.op == kOpStore && k >= 1)
                {
                    const ScriptOp &prev = c[k - 1];
                    if (prev.op == kOpCall)
                    {
                        const Names &n = names();
                        values[op.arg] = prev.arg == n.getScore            ? FromScore
                                         : prev.arg == n.getLineScore      ? FromLine
                                         : prev.arg == n.getChallengeScore ? FromChallenge
                                                                           : FromOther;
                    }
                    else if (k >= 2 && (prev.op == kOpAddVariable || prev.op == kOpSubVariable) && c[k - 2].op == kOpLoad)
                    {
                        const unsigned both = valueOf(c[k - 2].arg) | valueOf(prev.arg);
                        values[op.arg] = prev.op == kOpAddVariable ? both : both | FromLess;
                    }
                    else if (prev.op == kOpLoad)
                        values[op.arg] = valueOf(prev.arg);
                    else
                        values[op.arg] = FromOther;
                }
                if (op.op == kOpSub && k >= 1 && c[k - 1].op == kOpLoad && 1000 <= op.arg && op.arg < 100000000)
                    found = std::make_pair(op.arg, valueOf(c[k - 1].arg));
            }
            return found;
        }

        // (kind, target) of a score test before `position`.
        std::optional<std::pair<int64_t, int64_t>> scoreBefore(const LevelScript &script, const ScriptHandler &h,
                                                               size_t position)
        {
            bool setsChallengeScore = false;
            for (const ScriptHandler &other : script.handlers)
                if (other.object == h.object)
                    for (const ScriptOp &op : other.code)
                        if (op.op == kOpCall && op.arg == names().setChallengeScore)
                            setsChallengeScore = true;
            const auto tested = testedValue(h, position);
            if (!tested)
                return std::nullopt;
            const int64_t target = tested->first;
            const unsigned sources = tested->second;
            int64_t kind;
            if (setsChallengeScore || (sources & FromChallenge))
                kind = ScoreChallenge; // a timed run: its object keeps the challenge's points
            else if (sources == (FromScore | FromLine))
                kind = ScoreWithLine; // the score with the running line's points
            else if (sources == FromLine || (sources & FromLess))
                kind = ScoreLine; // the running line's points, or the score now less the score before
            else if (sources == FromScore)
                kind = ScoreTotal;
            else
                return std::nullopt;
            // "more than 69,999": the goal's own round number
            return std::make_pair(kind, (target + 1) % 1000 == 0 ? target + 1 : target);
        }
    }

    LevelScript::LevelScript(ByteView data)
    {
        const uint32_t codeWords = data.u32(4), varWords = data.u32(8), dataBytes = data.u32(12),
                       stringBytes = data.u32(16);
        size_t at = 0x14;
        m_strings = data.slice(at, at + stringBytes);
        at += stringBytes;
        m_data = data.slice(at, at + dataBytes);
        at += dataBytes;
        if (at + 4 * size_t(varWords) > data.size)
            throw PanelError("a level script shorter than its variables");
        variableCount = varWords;
        at += 4 * size_t(varWords);
        if (at + 4 * size_t(codeWords) > data.size)
            throw PanelError("a level script shorter than its code");
        const ByteView code = data.slice(at, at + 4 * size_t(codeWords));
        auto word = [&](size_t i) -> uint32_t {
            if (i >= codeWords)
                throw PanelError("a level script's handler table past its code");
            return code.u32(4 * i);
        };
        size_t o = 0;
        while (word(o) != kEnd)
        {
            const uint32_t object = word(o);
            const size_t record = word(o + 1);
            m_objects[object] = word(record);
            for (size_t s = record + 2; word(s) != kEnd; s += 2)
            {
                const uint32_t state = word(s);
                const size_t states = record + word(s + 1);
                for (size_t e = states; word(e) != kEnd; e += 2)
                {
                    ScriptHandler h{object, state, word(e), {}};
                    for (size_t ip = states + word(e + 1);; ip += 2)
                    {
                        const uint32_t op = word(ip), arg = word(ip + 1);
                        if (op == kOpEnd)
                            break;
                        h.code.push_back({op, arg});
                    }
                    handlers.push_back(std::move(h));
                }
            }
            o += 2;
        }
    }

    std::string LevelScript::string(uint32_t offset) const
    {
        if (offset >= m_strings.size)
            return "";
        return m_strings.zeroEnded(offset);
    }

    std::vector<Objective> LevelScript::objectives() const
    {
        std::vector<Objective> out;
        uint64_t at = 0;
        while (at + 12 <= m_data.size)
        {
            const uint32_t kind = m_data.u32(size_t(at)), count = m_data.u32(size_t(at + 4)),
                           size = m_data.u32(size_t(at + 8));
            at += 12;
            if (kind == names().objective && size == kObjectiveRecord)
                for (uint32_t i = 0; i < count; ++i)
                {
                    const size_t r = size_t(at + uint64_t(i) * size);
                    if (r + kObjectiveRecord > m_data.size)
                        throw PanelError("a level script's objective table past its data");
                    Objective g;
                    g.index = i;
                    g.locked = m_data.u32(r);
                    g.tier = m_data.u32(r + 4);
                    g.points = m_data.u32(r + 8);
                    g.state = m_data.u32(r + 12);
                    g.type = m_data.u32(r + 16);
                    g.owner = m_data.u32(r + 20);
                    g.titleAt = m_data.u32(r + 24);
                    g.textAt = m_data.u32(r + 28);
                    g.title = string(g.titleAt);
                    g.text = string(g.textAt);
                    string(m_data.u32(r + 36)); // the fly-through camera's name, read as the reference reads it
                    out.push_back(std::move(g));
                }
            at += uint64_t(count) * size;
        }
        return out;
    }

    uint32_t LevelScript::stateVariable(uint32_t object) const
    {
        const auto it = m_objects.find(object);
        if (it == m_objects.end())
            throw PanelError(formatted("a level script names an object it lacks (0x%08x)", object));
        return it->second;
    }

    std::vector<ScriptGoal> analyseGoals(const LevelScript &script)
    {
        // {objective: [(handler, position of the call, state)]}: the calls that set an objective's state
        std::unordered_map<uint32_t, std::vector<Site>> sites;
        for (const ScriptHandler &h : script.handlers)
            for (size_t k = 0; k < h.code.size(); ++k)
                if (h.code[k].op == kOpCall && h.code[k].arg == names().setObjectiveState && k >= 2 &&
                    h.code[k - 1].op == kOpPush && h.code[k - 2].op == kOpPush)
                    sites[h.code[k - 1].arg].push_back({&h, k, h.code[k - 2].arg});

        std::vector<ScriptGoal> goals;
        for (const Objective &g : script.objectives())
        {
            ScriptGoal goal;
            goal.objective = g;
            const auto found = sites.find(g.index);
            if (found != sites.end())
                for (const Site &site : found->second)
                {
                    if (site.state != names().complete)
                        continue;
                    const ScriptHandler &h = *site.handler;
                    // the completing handler, then the handlers that lead to it
                    // (the SENDs to its object from an object it moves to another
                    // state: the Xbox scripts' reward animations)
                    std::vector<std::pair<const ScriptHandler *, size_t>> chain;
                    std::vector<uint32_t> moved;
                    for (const ScriptOp &op : h.code)
                        if (op.op == kOpSetState && op.arg != h.object)
                            moved.push_back(op.arg);
                    for (const ScriptHandler &other : script.handlers)
                        if (std::find(moved.begin(), moved.end(), other.object) != moved.end())
                            for (size_t k = 0; k < other.code.size(); ++k)
                                if (other.code[k].op == kOpSend && other.code[k].arg == h.object)
                                    chain.emplace_back(&other, k);
                    // (the goal's own object's, where it has one, is the place, not the reward's)
                    const ScriptHandler *counting = &h;
                    for (const auto &[c, at] : chain)
                        if (c->object == g.owner)
                        {
                            counting = c;
                            break;
                        }
                    chain.insert(chain.begin(), {&h, site.position});
                    for (const auto &[c, at] : chain)
                    {
                        const auto counter = counterBefore(*c, at);
                        if (counter && !goal.counter)
                        {
                            counting = c;
                            goal.counter = std::make_pair(int64_t(script.stateVariable(c->object)) + 2 +
                                                              2 * int64_t(counter->first),
                                                          int64_t(counter->second));
                        }
                        if (!goal.score && (g.type == 0 || g.type == 1))
                            goal.score = scoreBefore(script, *c, at);
                    }
                    const std::vector<const ScriptHandler *> senders = sendersOf(script, counting->event, counting->object);
                    const std::vector<const ScriptHandler *> doers =
                        senders.empty() ? std::vector<const ScriptHandler *>{counting} : senders;
                    for (const ScriptHandler *s : doers)
                    {
                        const std::optional<DoneTest> done = senders.empty() ? std::nullopt : doneTest(script, *s);
                        bool known = false;
                        for (const auto &t : goal.targets)
                            known = known || t.first == s->object;
                        if (!known)
                            goal.targets.emplace_back(s->object, done);
                    }
                }
            goals.push_back(std::move(goal));
        }
        return goals;
    }

    std::vector<NpcBubble> npcBubbles(const LevelScript &script)
    {
        std::vector<NpcBubble> out;
        for (const ScriptHandler &h : script.handlers)
        {
            if (h.event != names().enter)
                continue;
            for (size_t k = 0; k < h.code.size(); ++k)
                if (h.code[k].op == kOpCall && h.code[k].arg == names().createNpcBubble && k >= 1 &&
                    h.code[k - 1].op == kOpPush)
                    out.push_back({h.code[k - 1].arg, script.stateVariable(h.object), h.state});
        }
        return out;
    }
}
