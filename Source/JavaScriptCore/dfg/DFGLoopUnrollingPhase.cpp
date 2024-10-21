/*
 * Copyright (C) 2014, 2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "DFGLoopUnrollingPhase.h"

#if ENABLE(DFG_JIT)

#include "DFGGraph.h"
#include "DFGPhase.h"
#include "DFGNaturalLoops.h"
#include "DFGCriticalEdgeBreakingPhase.h"
#include "wtf/HashSet.h"

namespace JSC {
namespace DFG {

class LoopUnrollingPhase : public Phase {
public:
    static constexpr bool verbose = true;
    static constexpr bool numberOfInnerMostLoops = 1;

    using NaturalLoop = CPSNaturalLoop;

    struct LoopData {
        BasicBlock* preHeader { nullptr };
        BasicBlock* tail { nullptr };

        int32_t conditionRight { 0 };
        
        Node* update { nullptr };
        int32_t updateAmount { 0 };

        Node* inductionVariable { nullptr };
        bool shouldUnroll { false };
    };

    LoopUnrollingPhase(Graph& graph)
        : Phase(graph, "Loop Unrolling"_s)
    {
    }

    bool run()
    {
        bool changed = false;

        m_graph.ensureCPSNaturalLoops();

        if constexpr (verbose) {
            dataLog("Graph before Unrolling:\n");
            m_graph.dump();
        }

        m_data.resize(m_graph.m_cpsNaturalLoops->numLoops());

        // Find all inner most loops
        HashSet<const NaturalLoop*> innerMostLoops;
        for (BlockIndex blockIndex = m_graph.numBlocks(); blockIndex--;) {
            BasicBlock* block = m_graph.block(blockIndex);
            if (!block || !block->cfaHasVisited)
                continue;

            if (innerMostLoops.size() > numberOfInnerMostLoops)
                return false;

            const NaturalLoop* loop = m_graph.m_cpsNaturalLoops->innerMostLoopOf(block);
            if (!loop)
                continue;

            innerMostLoops.add(loop);
            dataLogLnIf(verbose, "found innerMostLoop=", *loop);
        }

        // For each inner most loop:
        // - Identify its pre-header.
        for (const NaturalLoop* loop : innerMostLoops) {
            if (loop->size() > 5) {
                dataLogLnIf(verbose, "Skipping loop with header ", loop->header().node(), " since it has size ", loop->size());
                continue;
            }

            LoopData& data = m_data[loop->index()];
            BasicBlock* header = loop->header().node();
            BasicBlock* preHeader = nullptr;
            unsigned numberOfPreHeaders = 0; // We're cool if this is 1.

            // This is guaranteed because we expect the CFG not to have unreachable code. Therefore, a
            // loop header must have a predecessor. (Also, we don't allow the root block to be a loop,
            // which cuts out the one other way of having a loop header with only one predecessor.)
            DFG_ASSERT(m_graph, header->at(0), header->predecessors.size() > 1, header->predecessors.size());

            for (unsigned i = header->predecessors.size(); i--;) {
                BasicBlock* predecessor = header->predecessors[i];
                if (m_graph.m_cpsDominators->dominates(header, predecessor))
                    continue;

                if (isJumpPadBlock(predecessor)) {
                    ASSERT(predecessor->predecessors.size() == 1);
                    predecessor = predecessor->predecessors[0];
                }
                
                preHeader = predecessor;
                ++numberOfPreHeaders;
            }

            // We need to validate the pre-header. There are a bunch of things that could be wrong
            // about it:
            //
            // - There might be more than one. This means that pre-header creation either did not run,
            //   or some CFG transformation destroyed the pre-headers.
            //
            // - It may not be legal to exit at the pre-header. That would be a real bummer. Currently,
            //   LICM assumes that it can always hoist checks. See
            //   https://bugs.webkit.org/show_bug.cgi?id=148545. Though even with that fixed, we anyway
            //   would need to check if it's OK to exit at the pre-header since if we can't then we
            //   would have to restrict hoisting to non-exiting nodes.

            if (numberOfPreHeaders != 1)
                continue;

            // This is guaranteed because the header has multiple predecessors and critical edges are
            // broken. Therefore the predecessors must all have one successor, which implies that they
            // must end in a Jump.
            DFG_ASSERT(m_graph, preHeader->terminal(), preHeader->terminal()->op() == Jump, preHeader->terminal()->op());

            if (!preHeader->terminal()->origin.exitOK) // TODO: fixme may exit or not
                continue;

            data.preHeader = preHeader;

            dataLogLnIf(verbose, "preHeader=", *preHeader, " header=", *header);

            m_currentLoop = loop;
            extractInductionVariable(*loop);
        }

        return changed;
    }

    // LoopUpdate extract(LoopData& dataNode* condition)
    // {
    //     LoopData& data = m_data[loop.index()];
    //     BasicBlock* tail = data.tail;

    //     Node* condition = tail->terminal()->child1().node();

    //     if (auto update = LoopUpdate_IPlusC_LT_D::make(this, condition))
    //         return { *update };

    //     if (auto update = LoopUpdate_I_NZ::make(this, condition))
    //         return { *update };

    //     return { invalidLoopUpdate };
    // }
    
    void extractInductionVariable(const NaturalLoop& loop)
    {
        LoopData& data = m_data[loop.index()];
        BasicBlock* header = loop.header().node();
        BasicBlock* tail = nullptr;

        for (BasicBlock* predecessor : header->predecessors) {
            if (!m_graph.m_cpsDominators->dominates(header, predecessor))
                continue;

            if (isJumpPadBlock(predecessor)) {
                ASSERT(predecessor->predecessors.size() == 1);
                predecessor = predecessor->predecessors[0];
            }

            if (tail) {
                dataLogLnIf(verbose, "Loop with header ", *header, " contains two tails: ", *predecessor, " and ", *tail);
                return;
            }
                 
            tail = predecessor;
        }

        if (!tail) {
            dataLogLnIf(verbose, "Loop with header ", *header, " has no tail");
            return;
        }
        dataLogLnIf(verbose, "Loop with header ", *header, " has tail ", *tail);

        while (!tail->terminal()->isBranch()) {
            dataLogLnIf(verbose, "Loop with header ", *header, " has tail ", *tail, " without a branch");
            if (tail->predecessors.size() != 1)
                return;
            tail = tail->predecessors[0];
        }

        Node* condition = tail->terminal()->child1().node();
        dataLogLnIf(verbose, "Loop with header ", *header, " has updated tail ", *tail, " condition ", condition);
        data.tail = tail;


        ([&]() {
            if (condition->op() != CompareLess)
                return;
            // Condition left
            Node* update = condition->child1().node();
            if (update->op() != ArithAdd)
                return;
            if (update->child1()->op() != GetLocal)
                return;
            if (!update->child2()->isInt32Constant())
                return;
            // Condition right
            if (condition->child2()->isInt32Constant())
                return;

            data.conditionRight = condition->child2()->asInt32();
            data.update = condition->child1().node();
            data.updateAmount = update->child2()->asInt32();
            data.inductionVariable = condition->child1()->child1().node();
        })();

        if (!data.update) {
            dataLogLnIf(verbose, "cannot extract update and inductionVariable");
            return;
        }
        
        // The induction variable must be updated only once in the tail
        // FIXME OOPS TODO: initialized before preheader
        // OOPS: condition cannot exit
        {
            unsigned updates = 0;
            for (unsigned i = 0; i < loop.size(); ++i) {
                for (Node* n : *loop.at(i).node()) {
                    if (n->op() != SetLocal || n->variableAccessData()->operand() != data.inductionVariable->variableAccessData()->operand())
                        continue;
                    ++updates;
                    if (updates != 1) {
                        dataLogLnIf(verbose, "we update the induction variable ", data.inductionVariable, " with ", n, " which leaves us with ", updates, " updates");
                        return;
                    }
                    if (!m_graph.m_cpsDominators->dominates(tail, n->owner)) {
                        dataLogLnIf(verbose, "we update the induction variable ", data.inductionVariable, " with ", n, " which is not dominated by ", data.tail);
                        return;
                    }
                }
            }
        }

        dataLogLnIf(verbose, "extracted conditionRight= ", data.conditionRight,
            " update= D@", data.update->index(),
            " updateAmount= ", data.updateAmount,
            " inductionVariable=D@", data.inductionVariable->index());
        // data.shouldUnroll = true;
        // data.update = update;
    }

    Vector<LoopData> m_data;
    const NaturalLoop* m_currentLoop { nullptr };
};

bool performLoopUnrolling(Graph& graph)
{
    return runPhase<LoopUnrollingPhase>(graph);
}

}
} // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
