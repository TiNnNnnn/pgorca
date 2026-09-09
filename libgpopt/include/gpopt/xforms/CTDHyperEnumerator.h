//---------------------------------------------------------------------------
// Top-down hypergraph partitioning using the framework of Fender/Moerkotte,
// "Counter Strike" (VLDB 2013), Fig. 6. The graph-aware partitioner is an
// independent connected-set branch traversal, not a port of MinCutBranch.
// No estimated-cost pruning is performed by this logical enumerator.
//---------------------------------------------------------------------------
#ifndef GPOPT_CTDHyperEnumerator_H
#define GPOPT_CTDHyperEnumerator_H

#include <unordered_map>
#include <utility>

#include "gpopt/xforms/CDPHyperGraph.h"

namespace gpopt
{
class CTDHyperEnumerator
{
public:
	using CutCallback = std::function<BOOL(const CBitSet *, const CBitSet *)>;
	struct SStats
	{
		ULONG m_subproblems = 0;
		ULONG m_cache_hits = 0;
		ULONG m_candidates = 0;
		ULONG m_rejected = 0;
		ULONG m_compound_merges = 0;
	};

private:
	CMemoryPool *m_mp;
	const CDPHyperGraph *m_graph;
	IDPHyperReceiver *m_receiver;
	std::vector<std::pair<ULONG, ULONG>> m_representatives;
	std::unordered_map<ULONG, std::vector<CBitSet *>> m_completed;
	SStats m_stats;

	void ComputeAdjacency();
	BOOL Visit(const CBitSet *nodes);
	BOOL VisitPair(const CBitSet *left, const CBitSet *right);
	BOOL Sweep(const std::vector<std::vector<ULONG>> &adjacency,
			   const CBitSet *nodes, const CBitSet *excluded);

public:
	CTDHyperEnumerator(CMemoryPool *mp, const CDPHyperGraph *graph,
					   IDPHyperReceiver *receiver);
	CTDHyperEnumerator(const CTDHyperEnumerator &) = delete;
	~CTDHyperEnumerator();

	// Returns mapped-graph cuts, decoded to original nodes. Callers must
	// validate both children and connecting hyperedges in the original graph.
	// A true callback return aborts immediately, without invoking it again.
	BOOL Partition(const CBitSet *nodes, const CutCallback &callback);

	// Eager compatibility adapter: top-down recursion, followed by a coverage
	// sweep for subgraphs not reached from the root. Preserve even dead-end
	// alternatives because DSL exploration can observe them in the Memo.
	// Receiver pairs are emitted after their children, just as for DPHyp.
	BOOL Enumerate();

	const SStats &
	Stats() const
	{
		return m_stats;
	}
};
} // namespace gpopt

#endif
