// Python's difflib.SequenceMatcher with no junk (sequence_matcher.h), as
// CPython 3.15 computes it: find_longest_match, get_matching_blocks and
// get_opcodes.
#include "sequence_matcher.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>

namespace ps2x::app::extras
{
    namespace
    {
        struct Match
        {
            size_t a, b, size;
        };

        class Matcher
        {
        public:
            Matcher(const std::vector<std::string> &a, const std::vector<std::string> &b) : m_a(a), m_b(b)
            {
                // __chain_b: each line of b and where it is (no junk, nothing popular).
                for (size_t j = 0; j < b.size(); ++j)
                    m_where[b[j]].push_back(j);
            }

            // The longest block a[i:i+k] == b[j:j+k] inside the bounds, the one
            // starting first in a, then first in b.
            Match findLongestMatch(size_t alo, size_t ahi, size_t blo, size_t bhi) const
            {
                size_t besti = alo, bestj = blo, bestsize = 0;
                std::unordered_map<size_t, size_t> j2len; // the match ending at b[j] and a[i-1]
                for (size_t i = alo; i < ahi; ++i)
                {
                    std::unordered_map<size_t, size_t> next;
                    const auto found = m_where.find(m_a[i]);
                    if (found != m_where.end())
                        for (size_t j : found->second)
                        {
                            if (j < blo)
                                continue;
                            if (j >= bhi)
                                break;
                            const auto before = j > 0 ? j2len.find(j - 1) : j2len.end();
                            const size_t k = next[j] = (before == j2len.end() ? 0 : before->second) + 1;
                            if (k > bestsize)
                            {
                                besti = i + 1 - k;
                                bestj = j + 1 - k;
                                bestsize = k;
                            }
                        }
                    j2len = std::move(next);
                }
                // Extended by equal lines on each end (none are junk).
                while (besti > alo && bestj > blo && m_a[besti - 1] == m_b[bestj - 1])
                {
                    --besti;
                    --bestj;
                    ++bestsize;
                }
                while (besti + bestsize < ahi && bestj + bestsize < bhi && m_a[besti + bestsize] == m_b[bestj + bestsize])
                    ++bestsize;
                return {besti, bestj, bestsize};
            }

            std::vector<Match> matchingBlocks() const
            {
                struct Range
                {
                    size_t alo, ahi, blo, bhi;
                };
                std::vector<Range> queue = {{0, m_a.size(), 0, m_b.size()}};
                std::vector<Match> blocks;
                while (!queue.empty())
                {
                    const Range r = queue.back();
                    queue.pop_back();
                    const Match m = findLongestMatch(r.alo, r.ahi, r.blo, r.bhi);
                    if (m.size)
                    {
                        blocks.push_back(m);
                        if (r.alo < m.a && r.blo < m.b)
                            queue.push_back({r.alo, m.a, r.blo, m.b});
                        if (m.a + m.size < r.ahi && m.b + m.size < r.bhi)
                            queue.push_back({m.a + m.size, r.ahi, m.b + m.size, r.bhi});
                    }
                }
                std::sort(blocks.begin(), blocks.end(),
                          [](const Match &x, const Match &y) { return std::tie(x.a, x.b, x.size) < std::tie(y.a, y.b, y.size); });
                // Adjacent blocks joined.
                std::vector<Match> joined;
                Match run{0, 0, 0};
                for (const Match &m : blocks)
                {
                    if (run.a + run.size == m.a && run.b + run.size == m.b)
                        run.size += m.size;
                    else
                    {
                        if (run.size)
                            joined.push_back(run);
                        run = m;
                    }
                }
                if (run.size)
                    joined.push_back(run);
                joined.push_back({m_a.size(), m_b.size(), 0});
                return joined;
            }

        private:
            const std::vector<std::string> &m_a, &m_b;
            std::map<std::string, std::vector<size_t>> m_where;
        };
    }

    std::vector<DiffOpcode> diffOpcodes(const std::vector<std::string> &a, const std::vector<std::string> &b)
    {
        std::vector<DiffOpcode> out;
        size_t i = 0, j = 0;
        for (const Match &m : Matcher(a, b).matchingBlocks())
        {
            if (i < m.a && j < m.b)
                out.push_back({DiffOpcode::Tag::Replace, i, m.a, j, m.b});
            else if (i < m.a)
                out.push_back({DiffOpcode::Tag::Delete, i, m.a, j, m.b});
            else if (j < m.b)
                out.push_back({DiffOpcode::Tag::Insert, i, m.a, j, m.b});
            i = m.a + m.size;
            j = m.b + m.size;
            if (m.size)
                out.push_back({DiffOpcode::Tag::Equal, m.a, i, m.b, j});
        }
        return out;
    }
}
