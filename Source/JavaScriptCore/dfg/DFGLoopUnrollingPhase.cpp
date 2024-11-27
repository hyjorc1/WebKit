/*
 * Cloneright (C) 2014, 2015 Apple Inc. All rights reserved.
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
#include "DFGBlockInsertionSet.h"
#include "DFGCFAPhase.h"
#include "bytecode/CodeOrigin.h"
#include "dfg/DFGBasicBlock.h"
#include "dfg/DFGNodeOrigin.h"
#include "dfg/DFGNodeType.h"
#include "dfg/DFGUseKind.h"
#include "wtf/Assertions.h"
#include "wtf/Compiler.h"
#include "wtf/DataLog.h"
#include <climits>
#include <cstdint>

#if ENABLE(DFG_JIT)

#include "DFGCriticalEdgeBreakingPhase.h"
#include "DFGGraph.h"
#include "DFGInPlaceAbstractState.h"
#include "DFGNaturalLoops.h"
#include "DFGPhase.h"
#include "wtf/HashSet.h"

namespace JSC {
namespace DFG {

class LoopUnrollingPhase : public Phase {
public:
    bool verbose = Options::useLoopUnrollingVerbose();

    static constexpr uint32_t MaxNumberOfUnrolledLoops = 2;
    static constexpr uint32_t MaxNumberOfIterations = 5;
    static constexpr uint32_t MaxSizeOfLoopBody = 5;

    using NaturalLoop = CPSNaturalLoop;

    // for (i = initialValue; condition(i, operand); i = update(i, updateValue)) { ... }
    struct LoopData {
        BasicBlock* header() const { return loop->header().node(); }
        Node* condition() const { return tail->terminal()->child1().node(); }
        bool isInductionVariable(Node* node) { return node->operand() == inductionVariable->operand(); }
        void dump(PrintStream& out) const;

        const NaturalLoop* loop { nullptr };
        BasicBlock* preHeader { nullptr };
        BasicBlock* tail { nullptr };
        BasicBlock* next { nullptr };

        Node* inductionVariable { nullptr };
        int32_t initialValue { INT_MIN };
        int32_t operand { INT_MIN };
        Node* update { nullptr };
        int32_t updateValue { INT_MIN };
        uint32_t numberOfIterations { 0 };
    };

    LoopUnrollingPhase(Graph& graph)
        : Phase(graph, "Loop Unrolling"_s)
        , m_blockInsertionSet(graph)
        , m_state(graph)
    {
    }

    bool run()
    {
        bool changed = false;

        if (verbose) {
            dataLogLn("Graph before Loop Unrolling Phase:");
            m_graph.dump();
        }

        uint32_t unrolledCount = 0;
        for (const NaturalLoop* loop = findDeepestNestedLoop(); loop; loop = findDeepestNestedLoop()) {
            if (unrolledCount >= MaxNumberOfUnrolledLoops)
                break;
            if (!tryUnroll(loop))
                break;

            ++unrolledCount;
            changed = true;
        }

        dataLogLnIf(verbose, "Successfully unrolled ", unrolledCount, " loops.");
        return changed;
    }

    const NaturalLoop* findDeepestNestedLoop()
    {
        m_graph.ensureCPSNaturalLoops();

        const NaturalLoop* target = nullptr;
        int32_t maxDepth = INT_MIN;

        UncheckedKeyHashMap<const NaturalLoop*, int32_t> depthCache;
        for (unsigned loopIndex = m_graph.m_cpsNaturalLoops->numLoops(); loopIndex--;) {
            const NaturalLoop& loop = m_graph.m_cpsNaturalLoops->loop(loopIndex);

            if (loop.size() > MaxSizeOfLoopBody) {
                dataLogLnIf(verbose, "Skipping the candidate loop with header ", *loop.header().node(), " since MaxSizeOfLoopBody=", MaxSizeOfLoopBody);
                continue;
            }

            int32_t depth = 0;
            const NaturalLoop* current = &loop;
            while (current) {
                // TODO: fixme to avoid double look up
                if (depthCache.contains(current)) {
                    depth += depthCache.get(current);
                    break;
                }
                ++depth;
                current = m_graph.m_cpsNaturalLoops->innerMostOuterLoop(*current);
            }
            depthCache.add(&loop, depth);

            if (depth > maxDepth) {
                maxDepth = depth;
                target = &loop;
            }
        }
        return target;
    }

    bool tryUnroll(const NaturalLoop* loop)
    {
        if (verbose) {
            const NaturalLoop* outerLoop = m_graph.m_cpsNaturalLoops->innerMostOuterLoop(*loop);
            dataLogLnIf(verbose, "Try unroll innerMostLoop=", *loop, " with innerMostOuterLoop=", outerLoop ? *outerLoop : NaturalLoop());
        }

        LoopData data = { loop };

        // PreHeader                          PreHeader
        //  |                                  |
        // Header <---                        HeaderBodyTailGraph_0 <-- original loop
        //  |        |      unrolled to        |
        // Body      |   ================>    HeaderBodyTailGraph_1 <-- 1st copy
        //  |        |                         |
        // Tail ------                        ...
        //  |                                  |
        // Next                               HeaderBodyTailGraph_n <-- n_th copy
        //                                     |
        //                                    Next
        //
        // Note that NaturalLoop's body includes Header, Body, and Tail. The unrolling
        // process appends the HeaderBodyTailGraph copy reversely (from n_th to 1st).

        if (!findPreHeader(data))
            return false;
        dataLogLnIf(verbose, "After findPreHeader LoopData=", data);

        if (!findTail(data))
            return false;
        dataLogLnIf(verbose, "After findTail LoopData=", data);

        if (!findInductionVariable(data))
            return false;
        dataLogLnIf(verbose, "After findInductionVariable LoopData=", data);

        if (!isValid(data))
            return false;
        dataLogLnIf(verbose, "After isValid LoopData=", data);

        unrollLoop(data);

        if (verbose) {
            dataLogLn("Graph after Loop Unrolling for loop header=", *data.header());
            m_graph.dump();
        }

        if (Options::printEachUnrolledLoop()) {
            dataLogLn("In function ", m_graph.m_codeBlock->inferredName(), ", successfully unrolled the loop with LoopData=", data);
        }
        return true;
    }

    bool findPreHeader(LoopData& data)
    {
        BasicBlock* preHeader = nullptr;
        BasicBlock* header = data.header();

        // This is guaranteed because we expect the CFG not to have unreachable code. Therefore, a
        // loop header must have a predecessor. (Also, we don't allow the root block to be a loop,
        // which cuts out the one other way of having a loop header with only one predecessor.)
        DFG_ASSERT(m_graph, header->at(0), header->predecessors.size() > 1, header->predecessors.size());

        uint32_t numberOfPreHeaders = 0;
        for (uint32_t i = header->predecessors.size(); i--;) {
            BasicBlock* predecessor = header->predecessors[i];
            if (m_graph.m_cpsDominators->dominates(header, predecessor))
                continue;

            if (isJumpPadBlock(predecessor)) { // TODO: do we need this?
                ASSERT(predecessor->predecessors.size() == 1);
                predecessor = predecessor->predecessors[0];
            }

            preHeader = predecessor;
            ++numberOfPreHeaders;
        }

        if (numberOfPreHeaders != 1)
            return false;

        data.preHeader = preHeader;
        return true;
    }

    bool findTail(LoopData& data)
    {
        BasicBlock* header = data.header();
        BasicBlock* tail = nullptr;

        for (BasicBlock* predecessor : header->predecessors) {
            if (!m_graph.m_cpsDominators->dominates(header, predecessor))
                continue;

            if (isJumpPadBlock(predecessor)) { // TODO: do we still need this?
                ASSERT(predecessor->predecessors.size() == 1);
                predecessor = predecessor->predecessors[0];
            }

            if (tail) {
                dataLogLnIf(verbose, "Loop with header ", *header, " contains two tails: ", *predecessor, " and ", *tail);
                return false;
            }

            tail = predecessor;
        }

        if (!tail) {
            dataLogLnIf(verbose, "Loop with header ", *header, " has no tail");
            return false;
        }

        // PreHeader                          PreHeader
        //  |                                  |
        // Header <---                        Header_0
        //  |        |       unrolled to       |
        //  |       Tail  =================>  Branch_0
        //  |        |                         |
        // Branch ----                        Tail_0
        //  |                                  |
        // Next                               Header_1
        //                                     |
        //                                    Branch_1
        //                                     |
        //                                    Next
        //
        // FIXME: This is not supported yet.
        if (!tail->terminal()->isBranch()) {
            dataLogLnIf(verbose, "Loop with header ", *header, " has a non-branch tail");
            return false;
        }

        ASSERT(tail->terminal()->isBranch() && tail->successors().size() == 2);
        for (BasicBlock* successor : tail->successors()) {
            if (data.loop->contains(successor))
                continue;
            data.next = successor;
        }
        data.tail = tail;
        return true;
    }

    bool findInductionVariable(LoopData& data)
    {
        Node* condition = data.condition();
        auto isValidCondition = [&]() {
            // TODO: currently supported pattern: i + constant < constant
            if (condition->op() != CompareLess)
                return false;
            // Condition left
            Node* update = condition->child1().node();
            if (update->op() != ArithAdd)
                return false;
            if (update->child1()->op() != GetLocal)
                return false;
            if (!update->child2()->isInt32Constant())
                return false;
            // Condition right
            if (!condition->child2()->isInt32Constant())
                return false;

            data.operand = condition->child2()->asInt32();
            data.update = condition->child1().node();
            data.updateValue = update->child2()->asInt32();
            data.inductionVariable = condition->child1()->child1().node();
            return true;
        };
        if (!isValidCondition()) {
            dataLogLnIf(verbose, "Invalid loop condition node D@", condition->index());
            return false;
        }

        auto hasValidInitialValue = [&]() {
            Node* initialization = nullptr;
            for (Node* n : *data.preHeader) {
                if (n->op() != SetLocal || !data.isInductionVariable(n))
                    continue;
                initialization = n;
            }
            if (!initialization || !initialization->child1()->isInt32Constant())
                return false;
            data.initialValue = initialization->child1()->asInt32();
            return true;
        };
        if (!hasValidInitialValue()) {
            dataLogLnIf(verbose, "Invalid initial value");
            return false;
        }

        auto isValidInductionVariable = [&]() {
            // TODO condition cannot exit
            uint32_t updateCount = 0;
            for (uint32_t i = 0; i < data.loop->size(); ++i) {
                BasicBlock* body = data.loop->at(i).node();
                for (Node* node : *body) {
                    if (node->op() != SetLocal || !data.isInductionVariable(node))
                        continue;
                    dataLogLnIf(verbose, "Induction variable ", data.inductionVariable->index(), " is updated at node ", node->index(), " at ", *body);
                    ++updateCount;
                    if (updateCount != 1) {
                        dataLogLnIf(verbose, "Induction variable is updated multiple times at ", *body);
                        return false;
                    }
                    if (!m_graph.m_cpsDominators->dominates(data.tail, body)) {
                        dataLogLnIf(verbose, "Tail ", *data.tail, " doesn't dominate ", *body);
                        return false;
                    }
                }
            }
            return true;
        };
        if (!isValidInductionVariable()) {
            dataLogLnIf(verbose, "Invalid induction variable");
            return false;
        }

        // Compute the number of iterations in the loop.
        {
            auto getCompareFunction = [&](Node* condition) -> std::function<bool(int, int)> {
                switch (condition->op()) {
                case CompareLess:
                    return [](auto a, auto b) { return a < b; };
                // TODO: add more
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    return [](auto, auto) { return false; };
                }
            };
            auto compareFunction = getCompareFunction(condition);

            auto getUpdateFunction = [&](Node* update) -> std::function<int(int, int)> {
                switch (update->op()) {
                case ArithAdd:
                    return [](auto a, auto b) { return a + b; };
                // TODO: add more
                default:
                    RELEASE_ASSERT_NOT_REACHED();
                    return [](auto, auto) { return 0; };
                }
            };
            auto updateFunction = getUpdateFunction(data.update);

            uint32_t numberOfIterations = 0;
            for (int32_t i = data.initialValue; compareFunction(i, data.operand); i = updateFunction(i, data.updateValue)) {
                ++numberOfIterations;
                if (numberOfIterations > MaxNumberOfIterations) {
                    dataLogLnIf(verbose, "Skipping the candidate loop with header ", *data.header(), " since MaxNumberOfIterations=", MaxNumberOfIterations);
                    return false;
                }
            }
            data.numberOfIterations = numberOfIterations;
        }
        return true;
    }

    bool isValid(LoopData& data)
    {
        const NaturalLoop* const loop = data.loop;

        HashSet<Node*> cloneAble;
        HashSet<Node*> visiting;
        for (uint32_t i = 0; i < loop->size(); ++i) {
            BasicBlock* body = loop->at(i).node();
            // if (!body->cfaDidFinish) {
            //     dataLogLnIf(verbose, "block ", *body, " does not have cfa info, perhaps it was hoisted already?");
            //     return false;
            // }

            for (Node* node : *body) {
                if (!canCloneNode(cloneAble, visiting, node))
                    return false;
            }
        }
        return true;
    }

    BasicBlock* makeBlock(uint32_t executionCount = 0)
    {
        auto* block = m_blockInsertionSet.insert(m_graph.numBlocks(), executionCount);
        block->cfaHasVisited = false;
        block->cfaDidFinish = false;
        return block;
    }

    void unrollLoop(LoopData& data)
    {
        const NaturalLoop* const loop = data.loop;
        BasicBlock* const header = data.header();
        BasicBlock* const tail = data.tail;

        const NodeOrigin tailTerminalOrigin = data.tail->terminal()->origin;
        const CodeOrigin tailTerminalOriginSemantic = tailTerminalOrigin.semantic;
        dataLogLnIf(verbose, "tailTerminalOriginSemantic ", tailTerminalOriginSemantic);

        // Mapping from the origin to the clones.
        HashMap<BasicBlock*, BasicBlock*> blockClones;
        HashMap<Node*, Node*> nodeClones;

        auto replaceOperands = [&](auto& iter) {
            for (uint32_t i = 0; i < iter.size(); ++i) {
                if (iter.at(i) && nodeClones.contains(iter.at(i)))
                    iter.at(i) = nodeClones.get(iter.at(i));
            }
        };

        BasicBlock* next = data.next;
        for (uint32_t numberOfClones = data.numberOfIterations - 1; numberOfClones--;) {
            blockClones.clear();
            nodeClones.clear();

            // 1. Initialize all block clones.
            for (uint32_t i = 0; i < loop->size(); ++i) {
                BasicBlock* body = loop->at(i).node();
                blockClones.add(loop->at(i).node(), makeBlock(body->executionCount));
            }

            for (uint32_t i = 0; i < loop->size(); ++i) {
                BasicBlock* const body = loop->at(i).node();
                BasicBlock* const clone = blockClones.get(body);

                // 2. Clone Phis.
                clone->phis.resize(body->phis.size());
                for (size_t i = 0; i < body->phis.size(); ++i) {
                    Node* bodyPhi = body->phis[i];
                    Node* phiClone = m_graph.addNode(bodyPhi->prediction(), bodyPhi->op(), bodyPhi->origin, OpInfo(bodyPhi->variableAccessData()));
                    nodeClones.add(bodyPhi, phiClone);
                    ASSERT(!phiClone->child1()); // TODO: What's this?
                    clone->phis[i] = phiClone;
                }

                // 3. Clone nodes.
                for (uint32_t i = 0; i < body->size(); ++i) {
                    Node* bodyNode = body->at(i);
                    // Ignore the branch nodes at the end of the tail since we can directly jump to next (See step 5).
                    if (body == data.tail && bodyNode->origin.semantic == tailTerminalOriginSemantic)
                        continue;
                    cloneNode(nodeClones, clone, bodyNode);
                }

                // 4. Clone variables and tail and head.
                clone->variablesAtTail = body->variablesAtTail;
                replaceOperands(clone->variablesAtTail);
                clone->variablesAtHead = body->variablesAtHead;
                replaceOperands(clone->variablesAtHead);

                // 5. Clone successors. (predecessors will be fixed in resetReachability below)
                if (body == data.tail) {
                    clone->appendNode(m_graph, SpecNone, Jump, tailTerminalOrigin, OpInfo(next));
                } else {
                    for (uint32_t i = 0; i < body->numSuccessors(); ++i) {
                        auto& successor = clone->successor(i);
                        ASSERT(successor == body->successor(i));
                        if (loop->contains(successor))
                            successor = blockClones.get(successor);
                    }
                }
            }

            next = blockClones.get(header);
        }

        // 6. Replace the original loop tail branch with a jump to the last header clone.
        {
            for (Node* node : *tail) {
                if (node->origin.semantic == tailTerminalOriginSemantic)
                    node->removeWithoutChecks();
            }
            tail->appendNode(m_graph, SpecNone, Jump, tailTerminalOrigin, OpInfo(next));
        }

        // Done clone.
        m_blockInsertionSet.execute();

        // if (verbose) {
        //     dataLogLn("Graph after m_blockInsertionSet.execute header=", *data.header());
        //     m_graph.dump();
        // }

        // Disable AI for the original loop body.
        // for (uint32_t i = 0; i < loop->size(); ++i) {
        //     BasicBlock* body = loop->at(i).node();
        //     body->cfaHasVisited = false;
        //     body->cfaDidFinish = false;
        // }

        // TODO: these two are done in BlockInsertionSet::execute.
        // m_graph.dethread();
        // m_graph.invalidateCFG();

        m_graph.resetReachability();
        m_graph.killUnreachableBlocks();
        ASSERT(m_graph.m_form == LoadStore);
    }

    bool canCloneNode(HashSet<Node*>& cloneAble, HashSet<Node*>& visiting, Node*);
    Node* cloneNode(HashMap<Node*, Node*>& nodeClones, BasicBlock*, Node* into, BasicBlock* expectedBB = nullptr);
    Node* cloneNodeImpl(HashMap<Node*, Node*>& nodeClones, BasicBlock* into, Node*);

private:
    BlockInsertionSet m_blockInsertionSet;
    InPlaceAbstractState m_state;
};

bool performLoopUnrolling(Graph& graph)
{
    return runPhase<LoopUnrollingPhase>(graph);
}

void LoopUnrollingPhase::LoopData::dump(PrintStream& out) const
{
    out.print(*loop);

    out.print(" preHeader=");
    if (preHeader)
        out.print(*preHeader);
    else
        out.print("<null>");
    out.print(", ");

    out.print("tail=");
    if (tail)
        out.print(*tail);
    else
        out.print("<null>");
    out.print(", ");

    out.print("next=");
    if (tail)
        out.print(*next);
    else
        out.print("<null>");
    out.print(", ");

    out.print("inductionVariable=");
    if (inductionVariable)
        out.print("D@", inductionVariable->index());
    else
        out.print("<null>");
    out.print(", ");

    out.print("initValue=", initialValue, ", ");
    out.print("operand=", operand, ", ");

    out.print("update=");
    if (update)
        out.print("D@", update->index());
    else
        out.print("<null>");
    out.print(", ");

    out.print("updateValue=", updateValue, ", ");

    out.print("numberOfIterations=", numberOfIterations);
}

bool LoopUnrollingPhase::canCloneNode(HashSet<Node*>& cloneAble, HashSet<Node*>& visiting, Node* node)
{
    if (cloneAble.contains(node))
        return true;

    // FIXME: It seems only this node can have the recursive cycle situation.
    if (visiting.contains(node)) {
        dataLogLnIf(verbose, "canCloneNode found a recursive cycle for node D@", node->index(), " with op ", node->op());
        return true;
    }
    visiting.add(node);

    bool result = true;
    switch (node->op()) {
    case JSConstant:
    case LoopHint:
    case PhantomLocal:
    case SetArgumentDefinitely:
    case Jump:
    case Branch:
    case MovHint:
    case ExitOK:
    case ZombieHint:
    case InvalidationPoint:
    case Check:
    case CheckVarargs:
    case Flush:
    case Phi:
    case GetLocal:
    case SetLocal:
    case GetButterfly:
    case CheckArray:
    case AssertNotEmpty:
    case CheckStructure:
    case FilterCallLinkStatus:
    case ArrayifyToStructure:
    case ArithAdd:
    case ArithSub:
    case ArithMul:
    case ArithDiv:
    case ArithMod:
    case ArithBitAnd:
    case ArithBitOr:
    case ArithBitNot:
    case ArithBitRShift:
    case ArithBitLShift:
    case ArithBitXor:
    case BitURShift:
    case CompareLess:
    case CompareEq:
    case PutByVal:
    case PutByValAlias:
    case GetByVal: {
        m_graph.doToChildren(node, [&](Edge& e) {
            result &= canCloneNode(cloneAble, visiting, e.node());
        });
        break;
    }
    default:
        result = false;
        dataLogLnIf(verbose, "canCloneNode found node D@", node->index(), " with op ", node->op(), " is not cloneAble");
        break;
    }
    if (result)
        cloneAble.add(node);

    visiting.remove(node);
    return result;
}

Node* LoopUnrollingPhase::cloneNode(HashMap<Node*, Node*>& nodeClones, BasicBlock* into, Node* node, BasicBlock* parentOwner)
{
    if (!node) {
        dataLogLnIf(verbose, " cloneNode case 1: node is a nullptr");
        return node;
    }
    if (nodeClones.contains(node)) // TODO: double hash?
        return nodeClones.get(node);
    if (parentOwner && node->owner != parentOwner) {
        dataLogLn("found the case!!!");
        CRASH();
        dataLogLnIf(verbose, " cloneNode case 2: into=", *into, " expectedBB=", *parentOwner, " node D@", node->index(), " with op ", node->op());
        return node;
    }
    Node* result = cloneNodeImpl(nodeClones, into, node);
    ASSERT(result);
    nodeClones.add(node, result);
    return result;
}

Node* LoopUnrollingPhase::cloneNodeImpl(HashMap<Node*, Node*>& nodeClones, BasicBlock* into, Node* node)
{
    BasicBlock* owner = node->owner;
    switch (node->op()) {
    case Phi: {
        ASSERT_NOT_REACHED(); // Should already be in the map
        return nullptr;
    }
    case ExitOK:
    case LoopHint:
    case InvalidationPoint:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin);
    case GetButterfly: {
        Edge edge = Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind());
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, edge);
    }
    case PutByVal:
    case GetByVal:
    case PutByValAlias: {
        size_t firstChild = m_graph.m_varArgChildren.size();
        size_t children = 0;
        m_graph.doToChildren(node, [&](Edge& e) {
            m_graph.m_varArgChildren.append(Edge(cloneNode(nodeClones, into, e.node(), owner), e.useKind()));
            ++children;
        });
        ASSERT(children);
        // TODO: Leave room for the other possible children. why?
        for (size_t i = children; i < 5; ++i)
            m_graph.m_varArgChildren.append(Edge());
        return into->appendNode(m_graph, node->prediction(), Node::VarArg, node->op(), node->origin,
            OpInfo(node->arrayMode().asWord()), node->hasECMAMode() ? OpInfo(node->ecmaMode()) : OpInfo(node->arrayMode().speculation()),
            firstChild, children);
    }
    case JSConstant:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(node->constant()));
    case Jump:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(node->successor(0)));
    case Branch: {
        Edge child1 = Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind());
        BranchData clone = *node->branchData();
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(m_graph.m_branchData.add(WTFMove(clone))), child1);
    }
    case CompareLess:
    case CompareEq: {
        Edge child1 = Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind());
        Edge child2 = Edge(cloneNode(nodeClones, into, node->child2().node(), owner), node->child2().useKind());
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, child1, child2);
    }
    case CheckStructure:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(&node->structureSet()),
            Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()));
    case ArithBitNot:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(),
            Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()));
    case ArrayifyToStructure:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(node->structure()),
            Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()),
            Edge(cloneNode(nodeClones, into, node->child2().node(), owner), node->child2().useKind()));
    case ArithBitAnd:
    case ArithBitOr:
    case ArithBitRShift:
    case ArithBitLShift:
    case ArithBitXor:
    case BitURShift: {
        Edge child1 = Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind());
        Edge child2 = Edge(cloneNode(nodeClones, into, node->child2().node(), owner), node->child2().useKind());
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, child1, child2);
    }
    case ArithAdd:
    case ArithSub:
    case ArithMul:
    case ArithDiv:
    case ArithMod: {
        Edge child1 = Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind());
        Edge child2 = Edge(cloneNode(nodeClones, into, node->child2().node(), owner), node->child2().useKind());
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(node->arithMode()), child1, child2);
    }
    case CheckArray:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(node->arrayMode().asWord()),
            Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()));
    case FilterCallLinkStatus:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(node->callLinkStatus()),
            Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()));
    case GetLocal:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin,
            OpInfo(node->variableAccessData()),
            node->child1() ? Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()) : Edge());
    case MovHint:
    case Flush:
    case ZombieHint:
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin,
            node->hasUnlinkedOperand() ? OpInfo(node->unlinkedOperand()) : OpInfo(node->variableAccessData()),
            Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()));
    case SetLocal:
    case PhantomLocal: {
        auto* v = into->appendNode(m_graph, node->prediction(), node->op(), node->origin,
            OpInfo(node->variableAccessData()),
            Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()),
            node->child2() ? Edge(cloneNode(nodeClones, into, node->child2().node(), owner), node->child2().useKind()) : Edge());
        return v;
    }
    case Check: {
        Edge edge = node->child1() ? Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind()) : Edge();
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, edge);
    }
    case AssertNotEmpty: {
        ASSERT(node->child1());
        Edge edge = Edge(cloneNode(nodeClones, into, node->child1().node(), owner), node->child1().useKind());
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, edge);
    }
    case SetArgumentDefinitely: {
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, OpInfo(node->variableAccessData()));
    }
    case CheckVarargs: {
        return into->appendNode(m_graph, node->prediction(), node->op(), node->origin, m_graph.copyVarargChildren(node));
    }
    default:
        dataLogLn("Could not clone node: ", node, " into ", into);
        RELEASE_ASSERT_NOT_REACHED();
        return nullptr;
    }
}

}
} // namespace JSC::DFG

#endif // ENABLE(DFG_JIT)
