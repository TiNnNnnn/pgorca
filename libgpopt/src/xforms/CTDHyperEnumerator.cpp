#include "gpopt/xforms/CTDHyperEnumerator.h"

#include <algorithm>
#include <map>
#include <numeric>

#include "gpos/common/CAutoRef.h"
#include "gpos/common/CBitSetIter.h"

using namespace gpopt;

namespace
{
ULONG
First(const CBitSet *nodes)
{
	CBitSetIter it(*nodes);
	const BOOL advanced GPOS_ASSERTS_ONLY = it.Advance();
	GPOS_ASSERT(advanced);
	return it.Bit();
}

void
AddAdjacent(std::vector<std::vector<ULONG>> *adj, ULONG x, ULONG y)
{
	if (x != y &&
		std::find((*adj)[x].begin(), (*adj)[x].end(), y) == (*adj)[x].end())
	{
		(*adj)[x].push_back(y);
		(*adj)[y].push_back(x);
	}
}

// Tarjan's vertex-biconnected blocks, including bridges as two-vertex
// blocks. DFS intervals also identify exactly which vertices a separating
// articulation is mandatory for (Counter Strike, Sec. 4.5.2).
struct SBlockInfo
{
	std::vector<ULONG> discovery, low, end, parent, order;
	std::vector<std::vector<ULONG>> blocks;

	SBlockInfo(const std::vector<std::vector<ULONG>> &adj, ULONG start)
		: discovery(adj.size(), 0), low(adj.size(), 0), end(adj.size(), 0),
		  parent(adj.size(), adj.size())
	{
		std::vector<std::pair<ULONG, ULONG>> stack;
		std::function<void(ULONG)> visit = [&](ULONG v)
		{
			GPOS_CHECK_ABORT;
			GPOS_CHECK_STACK_SIZE;
			order.push_back(v);
			discovery[v] = low[v] = order.size();
			for (ULONG next : adj[v])
			{
				if (0 == discovery[next])
				{
					parent[next] = v;
					stack.emplace_back(v, next);
					visit(next);
					low[v] = std::min(low[v], low[next]);
					if (low[next] >= discovery[v])
					{
						std::vector<ULONG> block;
						std::pair<ULONG, ULONG> edge;
						do
						{
							edge = stack.back();
							stack.pop_back();
							block.push_back(edge.first);
							block.push_back(edge.second);
						} while (edge != std::make_pair(v, next));
						std::sort(block.begin(), block.end());
						block.erase(std::unique(block.begin(), block.end()),
									block.end());
						blocks.push_back(std::move(block));
					}
				}
				else if (next != parent[v] && discovery[next] < discovery[v])
				{
					stack.emplace_back(v, next);
					low[v] = std::min(low[v], discovery[next]);
				}
			}
			end[v] = order.size();
		};
		visit(start);
	}
};

// Enumerate connected bipartitions, keeping the smallest vertex on the left.
// When growing the left disconnects its complement, a connected final right
// must be contained in exactly one component. Absorb the other components
// into the left; excluded vertices distinguish sibling branches uniquely.
BOOL
Cuts(CMemoryPool *mp, const std::vector<std::vector<ULONG>> &adj,
	 const CBitSet *all, const CBitSet *left, const CBitSet *excluded,
	 const CTDHyperEnumerator::CutCallback &callback, ULONG *calls)
{
	GPOS_CHECK_ABORT;
	GPOS_CHECK_STACK_SIZE;
	++*calls;
	CAutoRef<CBitSet> remaining(GPOS_NEW(mp) CBitSet(mp, *all));
	remaining->Difference(left);
	if (0 == remaining->Size())
	{
		return false;
	}
	while (0 < remaining->Size())
	{
		CAutoRef<CBitSet> component(GPOS_NEW(mp) CBitSet(mp));
		std::vector<ULONG> queue{First(remaining.Value())};
		(void)component->ExchangeSet(queue[0]);
		for (size_t pos = 0; pos < queue.size(); ++pos)
		{
			for (ULONG next : adj[queue[pos]])
			{
				if (remaining->Get(next) && !component->ExchangeSet(next))
				{
					queue.push_back(next);
				}
			}
		}
		remaining->Difference(component.Value());
		if (component->Size() + left->Size() == all->Size())
		{
			if (callback(left, component.Value()))
			{
				return true;
			}
			break;
		}
		if (component->ContainsAll(excluded))
		{
			CAutoRef<CBitSet> grown(GPOS_NEW(mp) CBitSet(mp, *all));
			grown->Difference(component.Value());
			if (Cuts(mp, adj, all, grown.Value(), excluded, callback, calls))
			{
				return true;
			}
		}
		if (0 == remaining->Size())
		{
			return false;
		}
	}

	CAutoRef<CBitSet> frontier(GPOS_NEW(mp) CBitSet(mp));
	CBitSetIter it(*left);
	while (it.Advance())
	{
		for (ULONG next : adj[it.Bit()])
		{
			if (all->Get(next) && !left->Get(next) && !excluded->Get(next))
			{
				(void)frontier->ExchangeSet(next);
			}
		}
	}
	CAutoRef<CBitSet> blocked(GPOS_NEW(mp) CBitSet(mp, *excluded));
	CBitSetIter candidates(*frontier.Value());
	while (candidates.Advance())
	{
		CAutoRef<CBitSet> grown(GPOS_NEW(mp) CBitSet(mp, *left));
		(void)grown->ExchangeSet(candidates.Bit());
		if (Cuts(mp, adj, all, grown.Value(), blocked.Value(), callback, calls))
		{
			return true;
		}
		(void)blocked->ExchangeSet(candidates.Bit());
	}
	return false;
}

// A connected bipartition cuts edges in exactly one biconnected block:
// cutting two blocks would disconnect a side at their separating articulation.
// Every branch outside that block must follow its unique attachment vertex.
// Enumerate each block once and expand the attachments, rather than repeatedly
// growing cuts through the whole block-cut tree. This is a graph-aware
// partitioner optimization, not a cost-based restriction of the join space.
BOOL
BlockCuts(CMemoryPool *mp, const std::vector<std::vector<ULONG>> &adj,
		  const CBitSet *nodes, const SBlockInfo &info,
		  const CTDHyperEnumerator::CutCallback &callback,
		  CTDHyperEnumerator::SStats *stats)
{
	for (const auto &block : info.blocks)
	{
		GPOS_CHECK_ABORT;
		++stats->m_block_partitions;
		CAutoRef<CBitSet> all(GPOS_NEW(mp) CBitSet(mp));
		for (ULONG v : block)
		{
			(void)all->ExchangeSet(v);
		}
		const BOOL whole = block.size() == nodes->Size();
		std::vector<ULONG> owner;
		if (!whole)
		{
			owner.resize(adj.size(), adj.size());
			std::vector<ULONG> queue(block);
			for (ULONG v : block)
			{
				owner[v] = v;
			}
			for (size_t pos = 0; pos < queue.size(); ++pos)
			{
				const ULONG v = queue[pos];
				for (ULONG next : adj[v])
				{
					if (owner[next] == adj.size())
					{
						owner[next] = owner[v];
						queue.push_back(next);
					}
				}
			}
			GPOS_ASSERT(queue.size() == nodes->Size());
		}
		auto emit = [&](const CBitSet *l, const CBitSet *r)
		{
			if (whole)
			{
				return callback(l, r);
			}
			CAutoRef<CBitSet> left(GPOS_NEW(mp) CBitSet(mp));
			CAutoRef<CBitSet> right(GPOS_NEW(mp) CBitSet(mp));
			CBitSetIter it(*nodes);
			while (it.Advance())
			{
				(void)(l->Get(owner[it.Bit()]) ? left : right)
					->ExchangeSet(it.Bit());
			}
			return callback(left.Value(), right.Value());
		};
		CAutoRef<CBitSet> left(GPOS_NEW(mp) CBitSet(mp));
		(void)left->ExchangeSet(block.front());
		CAutoRef<CBitSet> excluded(GPOS_NEW(mp) CBitSet(mp));
		if (2 == block.size())
		{
			// A bridge has exactly one cut; no branching or connectivity BFS.
			(void)excluded->ExchangeSet(block.back());
			if (emit(left.Value(), excluded.Value()))
			{
				return true;
			}
		}
		else if (Cuts(mp, adj, all.Value(), left.Value(), excluded.Value(),
					  emit, &stats->m_cut_calls))
		{
			return true;
		}
	}
	return false;
}
} // namespace

CTDHyperEnumerator::CTDHyperEnumerator(CMemoryPool *mp,
									   const CDPHyperGraph *graph,
									   IDPHyperReceiver *receiver)
	: m_mp(mp), m_graph(graph), m_receiver(receiver)
{
	GPOS_ASSERT(nullptr != mp && nullptr != graph && nullptr != receiver);
	ComputeAdjacency();
}

CTDHyperEnumerator::~CTDHyperEnumerator()
{
	for (auto &bucket : m_completed)
	{
		for (CBitSet *nodes : bucket.second)
		{
			nodes->Release();
		}
	}
}

void
CTDHyperEnumerator::ComputeAdjacency()
{
	// Fig. 9: map overlapping complex edges to a shared simple edge with
	// maximum overlap. Keep each original edge's provenance for induced
	// subgraphs: a representative is active only if BOTH full endpoints fit.
	using Link = std::pair<ULONG, ULONG>;
	std::map<Link, std::vector<ULONG>> overlap;
	m_edges.resize(m_graph->LogicalEdgeCount());
	std::iota(m_edges.begin(), m_edges.end(), 0);
	m_representatives.resize(m_graph->LogicalEdgeCount());
	std::vector<BOOL> assigned(m_graph->LogicalEdgeCount(), false);
	for (ULONG id = 0; id < m_graph->LogicalEdgeCount(); ++id)
	{
		GPOS_CHECK_ABORT;
		const auto *edge = m_graph->Edge(2 * id);
		if (1 == edge->m_left->Size() && 1 == edge->m_right->Size())
		{
			m_representatives[id] = {First(edge->m_left), First(edge->m_right)};
			assigned[id] = true;
			continue;
		}
		CBitSetIter l(*edge->m_left);
		while (l.Advance())
		{
			CBitSetIter r(*edge->m_right);
			while (r.Advance())
			{
				overlap[std::minmax(l.Bit(), r.Bit())].push_back(id);
			}
		}
	}
	while (!overlap.empty())
	{
		GPOS_CHECK_ABORT;
		auto best = overlap.end();
		size_t largest = 0;
		for (auto it = overlap.begin(); it != overlap.end(); ++it)
		{
			auto &ids = it->second;
			ids.erase(std::remove_if(ids.begin(), ids.end(),
									 [&](ULONG id)
									 {
										 return assigned[id];
									 }),
					  ids.end());
			if (ids.size() > largest)
			{
				best = it;
				largest = ids.size();
			}
		}
		if (best == overlap.end())
		{
			break;
		}
		for (ULONG id : best->second)
		{
			m_representatives[id] = best->first;
			assigned[id] = true;
		}
		overlap.erase(best);
	}
}

BOOL
CTDHyperEnumerator::Partition(const CBitSet *nodes, const CutCallback &callback)
{
	return Partition(nodes, InducedEdges(nodes, m_edges), callback);
}

std::vector<ULONG>
CTDHyperEnumerator::InducedEdges(const CBitSet *nodes,
								   const std::vector<ULONG> &parent_edges)
{
	// Sec. 4.5.3: inherit the parent's reduced graph. An edge absent from a
	// parent cannot reappear in a descendant; test BOTH complete endpoints,
	// never just the representative pair. Keep original edge ids unchanged.
	std::vector<ULONG> edges;
	if (nodes->Size() > 1)
	{
		for (ULONG id : parent_edges)
		{
			GPOS_CHECK_ABORT;
			++m_stats.m_edge_checks;
			const auto *edge = m_graph->Edge(2 * id);
			if (nodes->ContainsAll(edge->m_left) &&
				nodes->ContainsAll(edge->m_right))
			{
				edges.push_back(id);
			}
		}
	}
	return edges;
}

BOOL
CTDHyperEnumerator::Partition(const CBitSet *nodes,
								const std::vector<ULONG> &edges,
								const CutCallback &callback)
{
	if (nodes->Size() < 2)
	{
		return false;
	}
	const ULONG n = m_graph->NodeCount();
	std::vector<std::vector<ULONG>> adj(n);
	BOOL complex = false;
	for (ULONG id : edges)
	{
		const auto *edge = m_graph->Edge(2 * id);
		complex = complex || edge->m_left->Size() > 1 ||
				  edge->m_right->Size() > 1;
		const auto link = m_representatives[id];
		AddAdjacent(&adj, link.first, link.second);
	}
	CBitSetIter vertex(*nodes);
	while (vertex.Advance())
	{
		if (adj[vertex.Bit()].empty())
		{
			// The mapped graph is a connectivity relaxation. An isolated
			// vertex already disproves connectivity in the original graph;
			// no BCC analysis or compound construction can repair it.
			++m_stats.m_isolated_subproblems;
			return false;
		}
	}

	// Sec. 4.4: a bridge representing one complex hyperedge makes each of
	// its endpoints non-separable in this partition. Union-find merges
	// overlapping compounds. Never contract a shared representative or a
	// representative also supplied by a simple edge.
	// Keep the bridge condition: at a non-bridge articulation, different
	// complex exits can be alternatives. Contracting either exit's endpoint
	// can lose valid cuts even without a simple edge into the separated block.
	const SBlockInfo info(adj, First(nodes));
	if (info.order.size() != nodes->Size())
	{
		return false;
	}
	auto emit_original = [&](const CBitSet *l, const CBitSet *r)
	{
		++m_stats.m_candidates;
		return l->Get(First(nodes)) ? callback(l, r) : callback(r, l);
	};
	if (!complex)
	{
		// Sec. 4.5.3: a simple induced subgraph goes directly to the
		// graph-aware partitioner, even when its parent was complex.
		++m_stats.m_simple_subproblems;
		return BlockCuts(m_mp, adj, nodes, info, emit_original, &m_stats);
	}
	std::map<std::pair<ULONG, ULONG>, std::vector<ULONG>> provenance;
	for (ULONG id : edges)
	{
		const auto link = m_representatives[id];
		provenance[std::minmax(link.first, link.second)].push_back(id);
	}
	std::vector<ULONG> parent(n);
	std::iota(parent.begin(), parent.end(), 0);
	BOOL has_compounds = false;
	auto root = [&](ULONG v)
	{
		while (parent[v] != v)
		{
			parent[v] = parent[parent[v]];
			v = parent[v];
		}
		return v;
	};
	auto merge = [&](const CBitSet *endpoint)
	{
		const ULONG representative = root(First(endpoint));
		CBitSetIter it(*endpoint);
		while (it.Advance())
		{
			const ULONG other = root(it.Bit());
			if (representative != other)
			{
				parent[other] = representative;
				has_compounds = true;
				++m_stats.m_compound_merges;
			}
		}
	};
	for (ULONG next : info.order)
	{
		const ULONG v = info.parent[next];
		if (v != n && info.low[next] > info.discovery[v])
		{
			const auto &ids = provenance.at(std::minmax(v, next));
			if (1 == ids.size())
			{
				const auto *edge = m_graph->Edge(2 * ids[0]);
				merge(edge->m_left);
				merge(edge->m_right);
			}
		}
	}
	if (!has_compounds)
	{
		// Simple blocks already use original node ids: no union-find decoding
		// or second adjacency construction is needed on this common path.
		return BlockCuts(m_mp, adj, nodes, info, emit_original, &m_stats);
	}

	// Sec. 4.5.2: enlarge a non-separable compound only with vertices on
	// EVERY path between its members. A DFS child with low >= discovery[parent]
	// separates its subtree from the rest at parent. Prefix counts test whether
	// the compound intersects both sides, without a BFS per articulation or
	// enumerating paths. Never absorb an optional route around a cycle.
	std::vector<std::vector<ULONG>> compounds(n);
	for (ULONG v : info.order)
	{
		compounds[root(v)].push_back(v);
	}
	for (const auto &compound : compounds)
	{
		if (compound.size() < 2)
		{
			continue;
		}
		GPOS_CHECK_ABORT;
		std::vector<ULONG> count(info.order.size() + 1, 0);
		for (ULONG v : compound)
		{
			count[info.discovery[v]] = 1;
		}
		std::partial_sum(count.begin(), count.end(), count.begin());
		for (ULONG child : info.order)
		{
			const ULONG v = info.parent[child];
			if (v == n || info.low[child] < info.discovery[v])
			{
				continue;
			}
			const ULONG inside =
				count[info.end[child]] - count[info.discovery[child] - 1];
			if (0 < inside && inside < compound.size() &&
				root(v) != root(compound.front()))
			{
				parent[root(v)] = root(compound.front());
				++m_stats.m_articulation_merges;
			}
		}
	}
	std::vector<std::vector<ULONG>> contracted(n);
	CAutoRef<CBitSet> all(GPOS_NEW(m_mp) CBitSet(m_mp));
	CBitSetIter it(*nodes);
	while (it.Advance())
	{
		(void)all->ExchangeSet(root(it.Bit()));
		for (ULONG next : adj[it.Bit()])
		{
			AddAdjacent(&contracted, root(it.Bit()), root(next));
		}
	}
	auto emit = [&](const CBitSet *l, const CBitSet *)
	{
		CAutoRef<CBitSet> decoded_left(GPOS_NEW(m_mp) CBitSet(m_mp));
		CAutoRef<CBitSet> decoded_right(GPOS_NEW(m_mp) CBitSet(m_mp));
		CBitSetIter original(*nodes);
		while (original.Advance())
		{
			CBitSet *side = l->Get(root(original.Bit()))
								? decoded_left.Value()
								: decoded_right.Value();
			(void)side->ExchangeSet(original.Bit());
		}
		++m_stats.m_candidates;
		// Block-local anchors need not be the original smallest vertex.
		return decoded_left->Get(First(nodes))
				   ? callback(decoded_left.Value(), decoded_right.Value())
				   : callback(decoded_right.Value(), decoded_left.Value());
	};
	const SBlockInfo contracted_info(contracted, First(all.Value()));
	return BlockCuts(m_mp, contracted, all.Value(), contracted_info, emit,
					 &m_stats);
}

BOOL
CTDHyperEnumerator::VisitPair(const CBitSet *left, const CBitSet *right,
								const std::vector<ULONG> &parent_edges)
{
	std::vector<ULONG> edges;
	for (ULONG id : parent_edges)
	{
		++m_stats.m_edge_checks;
		const auto *edge = m_graph->Edge(2 * id);
		if ((left->ContainsAll(edge->m_left) &&
			 right->ContainsAll(edge->m_right)) ||
			(right->ContainsAll(edge->m_left) &&
			 left->ContainsAll(edge->m_right)))
		{
			edges.push_back(edge->m_edge_id);
		}
	}
	if (edges.empty())
	{
		++m_stats.m_rejected;
		return false;
	}
	if (Visit(left, parent_edges) || Visit(right, parent_edges))
	{
		return true;
	}
	// Original-hypergraph connectivity includes the receiver's non-inner
	// applicability filter. A cached completed subproblem can be unreachable.
	if (!m_receiver->HasSeen(left) || !m_receiver->HasSeen(right))
	{
		++m_stats.m_rejected;
		return false;
	}
	for (ULONG edge : edges)
	{
		if (m_receiver->FoundSubgraphPair(left, right, edge))
		{
			return true;
		}
	}
	return false;
}

BOOL
CTDHyperEnumerator::Visit(const CBitSet *nodes,
						  const std::vector<ULONG> &parent_edges)
{
	GPOS_CHECK_ABORT;
	GPOS_CHECK_STACK_SIZE;
	const ULONG hash = nodes->HashValue();
	auto found = m_completed.find(hash);
	if (found != m_completed.end())
	{
		for (const CBitSet *completed : found->second)
		{
			if (completed->Equals(nodes))
			{
				++m_stats.m_cache_hits;
				return false;
			}
		}
	}
	++m_stats.m_subproblems;
	const auto edges = InducedEdges(nodes, parent_edges);
	if (Partition(nodes, edges,
				  [&](const CBitSet *l, const CBitSet *r)
				  {
					  return VisitPair(l, r, edges);
				  }))
	{
		return true;
	}
	m_completed[hash].push_back(GPOS_NEW(m_mp) CBitSet(m_mp, *nodes));
	return false;
}

BOOL
CTDHyperEnumerator::Sweep(const std::vector<std::vector<ULONG>> &adj,
						  const CBitSet *nodes, const CBitSet *excluded)
{
	GPOS_CHECK_ABORT;
	GPOS_CHECK_STACK_SIZE;
	// This walk grows subsets instead of descending. Previously inactive
	// edges can become active, so each coverage seed uses the original graph.
	if (Visit(nodes, m_edges))
	{
		return true;
	}
	CAutoRef<CBitSet> frontier(GPOS_NEW(m_mp) CBitSet(m_mp));
	CBitSetIter it(*nodes);
	while (it.Advance())
	{
		for (ULONG next : adj[it.Bit()])
		{
			if (!nodes->Get(next) && !excluded->Get(next))
			{
				(void)frontier->ExchangeSet(next);
			}
		}
	}
	CAutoRef<CBitSet> blocked(GPOS_NEW(m_mp) CBitSet(m_mp, *excluded));
	CBitSetIter candidates(*frontier.Value());
	while (candidates.Advance())
	{
		CAutoRef<CBitSet> grown(GPOS_NEW(m_mp) CBitSet(m_mp, *nodes));
		(void)grown->ExchangeSet(candidates.Bit());
		if (Sweep(adj, grown.Value(), blocked.Value()))
		{
			return true;
		}
		(void)blocked->ExchangeSet(candidates.Bit());
	}
	return false;
}

BOOL
CTDHyperEnumerator::Enumerate()
{
	CAutoRef<CBitSet> all(GPOS_NEW(m_mp) CBitSet(m_mp));
	for (ULONG node = 0; node < m_graph->NodeCount(); ++node)
	{
		if (m_receiver->FoundSingleNode(node))
		{
			return true;
		}
		(void)all->ExchangeSet(node);
	}
	if (0 == all->Size() || Visit(all.Value(), m_edges))
	{
		return 0 != all->Size();
	}
	// In a connected simple graph every connected subset can be extended to
	// the root: peel off the connected components of its complement one at a
	// time. Complete top-down partitioning therefore already visited every
	// subset, even with receiver filters (Visit runs before HasSeen). Complex
	// hypergraphs do not have this extension property: keep their coverage
	// sweep, including dead ends that later DSL rules can still observe.
	BOOL simple = true;
	for (ULONG id = 0; id < m_graph->LogicalEdgeCount(); ++id)
	{
		const auto *edge = m_graph->Edge(2 * id);
		if (edge->m_left->Size() != 1 || edge->m_right->Size() != 1)
		{
			simple = false;
			break;
		}
	}
	if (simple && m_receiver->HasSeen(all.Value()))
	{
		return false;
	}
	std::vector<std::vector<ULONG>> adj(m_graph->NodeCount());
	for (auto link : m_representatives)
	{
		AddAdjacent(&adj, link.first, link.second);
	}
	CAutoRef<CBitSet> excluded(GPOS_NEW(m_mp) CBitSet(m_mp));
	for (ULONG node = 0; node < m_graph->NodeCount(); ++node)
	{
		CAutoRef<CBitSet> seed(GPOS_NEW(m_mp) CBitSet(m_mp));
		(void)seed->ExchangeSet(node);
		if (Sweep(adj, seed.Value(), excluded.Value()))
		{
			return true;
		}
		(void)excluded->ExchangeSet(node);
	}
	return false;
}
