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

// Enumerate connected bipartitions, keeping the smallest vertex on the left.
// When growing the left disconnects its complement, a connected final right
// must be contained in exactly one component. Absorb the other components
// into the left; excluded vertices distinguish sibling branches uniquely.
BOOL
Cuts(CMemoryPool *mp, const std::vector<std::vector<ULONG>> &adj,
	 const CBitSet *all, const CBitSet *left, const CBitSet *excluded,
	 const CTDHyperEnumerator::CutCallback &callback)
{
	GPOS_CHECK_ABORT;
	GPOS_CHECK_STACK_SIZE;
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
		(void) component->ExchangeSet(queue[0]);
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
			if (Cuts(mp, adj, all, grown.Value(), excluded, callback))
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
			if (!left->Get(next) && !excluded->Get(next))
			{
				(void) frontier->ExchangeSet(next);
			}
		}
	}
	CAutoRef<CBitSet> blocked(GPOS_NEW(mp) CBitSet(mp, *excluded));
	CBitSetIter candidates(*frontier.Value());
	while (candidates.Advance())
	{
		CAutoRef<CBitSet> grown(GPOS_NEW(mp) CBitSet(mp, *left));
		(void) grown->ExchangeSet(candidates.Bit());
		if (Cuts(mp, adj, all, grown.Value(), blocked.Value(), callback))
		{
			return true;
		}
		(void) blocked->ExchangeSet(candidates.Bit());
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
									 [&](ULONG id) { return assigned[id]; }),
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
	if (nodes->Size() < 2)
	{
		return false;
	}
	const ULONG n = m_graph->NodeCount();
	std::vector<std::vector<ULONG>> adj(n);
	std::map<std::pair<ULONG, ULONG>, std::vector<ULONG>> provenance;
	for (ULONG id = 0; id < m_graph->LogicalEdgeCount(); ++id)
	{
		const auto *edge = m_graph->Edge(2 * id);
		if (nodes->ContainsAll(edge->m_left) &&
			nodes->ContainsAll(edge->m_right))
		{
			const auto link = m_representatives[id];
			AddAdjacent(&adj, link.first, link.second);
			provenance[std::minmax(link.first, link.second)].push_back(id);
		}
	}

	// Sec. 4.4: a bridge representing one complex hyperedge makes each of
	// its endpoints non-separable in this partition. Union-find merges
	// overlapping compounds. Never contract a shared representative or a
	// representative also supplied by a simple edge.
	std::vector<ULONG> parent(n), discovery(n, 0), low(n, 0);
	std::iota(parent.begin(), parent.end(), 0);
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
				++m_stats.m_compound_merges;
			}
		}
	};
	ULONG clock = 0;
	std::function<void(ULONG, ULONG)> bridges = [&](ULONG v, ULONG prev)
	{
		GPOS_CHECK_ABORT;
		GPOS_CHECK_STACK_SIZE;
		discovery[v] = low[v] = ++clock;
		for (ULONG next : adj[v])
		{
			if (next == prev)
			{
				continue;
			}
			if (0 == discovery[next])
			{
				bridges(next, v);
				low[v] = std::min(low[v], low[next]);
				if (low[next] > discovery[v])
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
			else
			{
				low[v] = std::min(low[v], discovery[next]);
			}
		}
	};
	bridges(First(nodes), n);
	if (clock != nodes->Size())
	{
		return false;
	}
	std::vector<std::vector<ULONG>> contracted(n);
	CAutoRef<CBitSet> all(GPOS_NEW(m_mp) CBitSet(m_mp));
	CBitSetIter it(*nodes);
	while (it.Advance())
	{
		(void) all->ExchangeSet(root(it.Bit()));
		for (ULONG next : adj[it.Bit()])
		{
			AddAdjacent(&contracted, root(it.Bit()), root(next));
		}
	}
	CAutoRef<CBitSet> left(GPOS_NEW(m_mp) CBitSet(m_mp));
	(void) left->ExchangeSet(root(First(nodes)));
	CAutoRef<CBitSet> excluded(GPOS_NEW(m_mp) CBitSet(m_mp));
	return Cuts(
		m_mp, contracted, all.Value(), left.Value(), excluded.Value(),
		[&](const CBitSet *l, const CBitSet *)
		{
			CAutoRef<CBitSet> decoded_left(GPOS_NEW(m_mp) CBitSet(m_mp));
			CAutoRef<CBitSet> decoded_right(GPOS_NEW(m_mp) CBitSet(m_mp));
			CBitSetIter original(*nodes);
			while (original.Advance())
			{
				CBitSet *side = l->Get(root(original.Bit()))
									? decoded_left.Value()
									: decoded_right.Value();
				(void) side->ExchangeSet(original.Bit());
			}
			++m_stats.m_candidates;
			return callback(decoded_left.Value(), decoded_right.Value());
		});
}

BOOL
CTDHyperEnumerator::VisitPair(const CBitSet *left, const CBitSet *right)
{
	std::vector<ULONG> edges;
	for (ULONG id = 0; id < m_graph->LogicalEdgeCount(); ++id)
	{
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
	if (Visit(left) || Visit(right))
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
CTDHyperEnumerator::Visit(const CBitSet *nodes)
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
	if (Partition(nodes, [&](const CBitSet *l, const CBitSet *r)
				  { return VisitPair(l, r); }))
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
	if (Visit(nodes))
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
				(void) frontier->ExchangeSet(next);
			}
		}
	}
	CAutoRef<CBitSet> blocked(GPOS_NEW(m_mp) CBitSet(m_mp, *excluded));
	CBitSetIter candidates(*frontier.Value());
	while (candidates.Advance())
	{
		CAutoRef<CBitSet> grown(GPOS_NEW(m_mp) CBitSet(m_mp, *nodes));
		(void) grown->ExchangeSet(candidates.Bit());
		if (Sweep(adj, grown.Value(), blocked.Value()))
		{
			return true;
		}
		(void) blocked->ExchangeSet(candidates.Bit());
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
		(void) all->ExchangeSet(node);
	}
	if (0 == all->Size() || Visit(all.Value()))
	{
		return 0 != all->Size();
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
		(void) seed->ExchangeSet(node);
		if (Sweep(adj, seed.Value(), excluded.Value()))
		{
			return true;
		}
		(void) excluded->ExchangeSet(node);
	}
	return false;
}
