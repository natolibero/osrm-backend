#include "engine/routing_algorithms/many_to_many.hpp"
#include "engine/routing_algorithms/routing_base_ch.hpp"

#include <boost/assert.hpp>
#include <boost/range/iterator_range_core.hpp>

#include <limits>
#include <memory>
#include <vector>

namespace osrm
{
namespace engine
{
namespace routing_algorithms
{

namespace ch
{

inline bool addLoopWeight(const DataFacade<ch::Algorithm> &facade,
                          const NodeID node,
                          EdgeWeight &weight,
                          EdgeDuration &duration)
{ // Special case for CH when contractor creates a loop edge node->node
    BOOST_ASSERT(weight < 0);

    const auto loop_weight = ch::getLoopWeight<false>(facade, node);
    if (loop_weight != INVALID_EDGE_WEIGHT)
    {
        const auto new_weight_with_loop = weight + loop_weight;
        if (new_weight_with_loop >= 0)
        {
            weight = new_weight_with_loop;
            duration += ch::getLoopWeight<true>(facade, node);
            return true;
        }
    }

    // No loop found or adjusted weight is negative
    return false;
}

template <bool DIRECTION>
void relaxOutgoingEdges(const DataFacade<Algorithm> &facade,
                        const NodeID node,
                        const EdgeWeight weight,
                        const EdgeDuration duration,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                        const PhantomNode &)
{
    if (stallAtNode<DIRECTION>(facade, node, weight, query_heap))
    {
        return;
    }

    for (auto edge : facade.GetAdjacentEdgeRange(node))
    {
        const auto &data = facade.GetEdgeData(edge);
        if (DIRECTION == FORWARD_DIRECTION ? data.forward : data.backward)
        {
            const NodeID to = facade.GetTarget(edge);

            const auto edge_weight = data.weight;
            const auto edge_duration = data.duration;

            BOOST_ASSERT_MSG(edge_weight > 0, "edge_weight invalid");
            const auto to_weight = weight + edge_weight;
            const auto to_duration = duration + edge_duration;

            // New Node discovered -> Add to Heap + Node Info Storage
            if (!query_heap.WasInserted(to))
            {
                query_heap.Insert(to, to_weight, {node, to_duration});
            }
            // Found a shorter Path -> Update weight and set new parent
            else if (std::tie(to_weight, to_duration) <
                     std::tie(query_heap.GetKey(to), query_heap.GetData(to).duration))
            {
                query_heap.GetData(to) = {node, to_duration};
                query_heap.DecreaseKey(to, to_weight);
            }
        }
    }
}

void forwardRoutingStep(const DataFacade<Algorithm> &facade,
                        const unsigned row_idx,
                        const unsigned number_of_targets,
                        typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                        const std::vector<NodeBucket> &search_space_with_buckets,
                        std::vector<EdgeWeight> &weights_table,
                        std::vector<EdgeDuration> &durations_table,
                        std::vector<NodeID> &middle_nodes_table,
                        const PhantomNode &phantom_node)
{
    const auto node = query_heap.DeleteMin();
    const auto source_weight = query_heap.GetKey(node);
    const auto source_duration = query_heap.GetData(node).duration;

    // Check if each encountered node has an entry
    const auto &bucket_list = std::equal_range(search_space_with_buckets.begin(),
                                               search_space_with_buckets.end(),
                                               node,
                                               NodeBucket::Compare());
    for (const auto &current_bucket : boost::make_iterator_range(bucket_list))
    {
        // Get target id from bucket entry
        const auto column_idx = current_bucket.column_index;
        const auto target_weight = current_bucket.weight;
        const auto target_duration = current_bucket.duration;

        auto &current_weight = weights_table[row_idx * number_of_targets + column_idx];
        auto &current_duration = durations_table[row_idx * number_of_targets + column_idx];

        // Check if new weight is better
        auto new_weight = source_weight + target_weight;
        auto new_duration = source_duration + target_duration;

        if (new_weight < 0)
        {
            if (addLoopWeight(facade, node, new_weight, new_duration))
            {
                current_weight = std::min(current_weight, new_weight);
                current_duration = std::min(current_duration, new_duration);
                middle_nodes_table[row_idx * number_of_targets + column_idx] = node;
            }
        }
        else if (std::tie(new_weight, new_duration) < std::tie(current_weight, current_duration))
        {
            current_weight = new_weight;
            current_duration = new_duration;
            middle_nodes_table[row_idx * number_of_targets + column_idx] = node;
        }
    }

    relaxOutgoingEdges<FORWARD_DIRECTION>(
        facade, node, source_weight, source_duration, query_heap, phantom_node);
}

void backwardRoutingStep(const DataFacade<Algorithm> &facade,
                         const unsigned column_idx,
                         typename SearchEngineData<Algorithm>::ManyToManyQueryHeap &query_heap,
                         std::vector<NodeBucket> &search_space_with_buckets,
                         const PhantomNode &phantom_node)
{
    const auto node = query_heap.DeleteMin();
    const auto target_weight = query_heap.GetKey(node);
    const auto target_duration = query_heap.GetData(node).duration;
    const auto parent = query_heap.GetData(node).parent;

    // Store settled nodes in search space bucket
    search_space_with_buckets.emplace_back(
        node, parent, column_idx, target_weight, target_duration);

    relaxOutgoingEdges<REVERSE_DIRECTION>(
        facade, node, target_weight, target_duration, query_heap, phantom_node);
}

} // namespace ch

std::vector<NodeID>
retrievePackedPathFromSearchSpace(NodeID middle_node,
                                  const unsigned column_idx,
                                  std::vector<NodeBucket> &search_space_with_buckets)
{

    //     [  0           1          2         3    ]
    //     [ [m0,p=m3],[m1,p=m2],[m2,p=m1], [m3,p=2]]

    //           targets (columns) target_id = column_idx
    //              a   b   c
    //          a  [0,  1,  2],
    // sources  b  [3,  4,  5],
    //  (rows)  c  [6,  7,  8],
    //          d  [9, 10, 11]
    // row_idx * number_of_targets + column_idx
    // a -> c 0 * 3 + 2 = 2
    // c -> c 2 * 3 + 2 = 8
    // d -> c 3 * 3 + 2 = 11

    //   middle_nodes_table = [0 , 1, 2, .........]

    auto bucket_list = std::equal_range(search_space_with_buckets.begin(),
                                        search_space_with_buckets.end(),
                                        middle_node,
                                        NodeBucket::ColumnCompare(column_idx));

    std::vector<NodeID> packed_path = {bucket_list.first->middle_node};
    while (bucket_list.first->parent_node != bucket_list.first->middle_node &&
           bucket_list.first != search_space_with_buckets.end())
    {

        packed_path.emplace_back(bucket_list.first->parent_node);
        bucket_list = std::equal_range(search_space_with_buckets.begin(),
                                       search_space_with_buckets.end(),
                                       bucket_list.first->parent_node,
                                       NodeBucket::ColumnCompare(column_idx));
    }

    return packed_path;
}

template <>
std::vector<EdgeDuration> manyToManySearch(SearchEngineData<ch::Algorithm> &engine_working_data,
                                           const DataFacade<ch::Algorithm> &facade,
                                           const std::vector<PhantomNode> &phantom_nodes,
                                           const std::vector<std::size_t> &source_indices,
                                           const std::vector<std::size_t> &target_indices)
{
    const auto number_of_sources = source_indices.size();
    const auto number_of_targets = target_indices.size();
    const auto number_of_entries = number_of_sources * number_of_targets;

    std::vector<EdgeWeight> weights_table(number_of_entries, INVALID_EDGE_WEIGHT);
    std::vector<EdgeDuration> durations_table(number_of_entries, MAXIMAL_EDGE_DURATION);
    std::vector<NodeID> middle_nodes_table(number_of_entries, SPECIAL_NODEID);

    std::vector<NodeBucket> search_space_with_buckets;

    engine_working_data.InitializeOrClearUnpackingCacheThreadLocalStorage();

    // Populate buckets with paths from all accessible nodes to destinations via backward searches
    for (std::uint32_t column_idx = 0; column_idx < target_indices.size(); ++column_idx)
    {
        const auto index = target_indices[column_idx];
        const auto &phantom = phantom_nodes[index];

        engine_working_data.InitializeOrClearManyToManyThreadLocalStorage(
            facade.GetNumberOfNodes());
        auto &query_heap = *(engine_working_data.many_to_many_heap);
        insertTargetInHeap(query_heap, phantom);

        // Explore search space
        while (!query_heap.Empty())
        {
            backwardRoutingStep(facade, column_idx, query_heap, search_space_with_buckets, phantom);
        }
    }

    // Order lookup buckets
    std::sort(search_space_with_buckets.begin(), search_space_with_buckets.end());

    std::cout << "search_space_with_buckets:" << std::endl;
    for (std::vector<NodeBucket>::iterator bucket = search_space_with_buckets.begin();
         bucket != search_space_with_buckets.end();
         bucket++)
    {
        std::cout << "NodeBucket { middle_node: " << bucket->middle_node << " "
                  << " parent_node: " << bucket->parent_node << " "
                  << " column_index: " << bucket->column_index << " "
                  << " weight: " << bucket->weight << " "
                  << " duration: " << bucket->duration << " }\n";
    }

    // Find shortest paths from sources to all accessible nodes
    for (std::uint32_t row_idx = 0; row_idx < source_indices.size(); ++row_idx)
    {
        const auto index = source_indices[row_idx];
        const auto &phantom = phantom_nodes[index];

        // Clear heap and insert source nodes
        engine_working_data.InitializeOrClearManyToManyThreadLocalStorage(
            facade.GetNumberOfNodes());
        auto &query_heap = *(engine_working_data.many_to_many_heap);
        insertSourceInHeap(query_heap, phantom);

        // Explore search space
        while (!query_heap.Empty())
        {
            forwardRoutingStep(facade,
                               row_idx,
                               number_of_targets,
                               query_heap,
                               search_space_with_buckets,
                               weights_table,
                               durations_table,
                               middle_nodes_table,
                               phantom);
        }
        // row_idx == one source
        // target == search_space_with_buckets.column_idx

        for (unsigned column_idx = 0; column_idx < number_of_targets; ++column_idx)
        {
            NodeID middle_node_id = middle_nodes_table[row_idx * number_of_targets + column_idx];
            std::vector<NodeID> packed_path_from_middle_to_target = retrievePackedPathFromSearchSpace(middle_node_id, column_idx, search_space_with_buckets); // packed_path_from_middle_to_target
            std::cout << "packed_path_from_middle_to_target: ";
            for (unsigned idx = 0; idx < packed_path_from_middle_to_target.size(); ++idx) std::cout << packed_path_from_middle_to_target[idx] << ", ";
            std::cout << std::endl;

            std::vector<NodeID> packed_path_from_source_to_middle;
            ch::retrievePackedPathFromSingleManyToManyHeap(query_heap, middle_node_id, packed_path_from_source_to_middle); // packed_path_from_source_to_middle
            std::cout << "packed_path_from_source_to_middle: ";
            for (unsigned idx = 0; idx < packed_path_from_source_to_middle.size(); ++idx) std::cout << packed_path_from_source_to_middle[idx] << ", ";
            std::cout << std::endl;

            // FUNCTION BODY FOR REFERENCE:
            // void retrievePackedPathFromSingleManyToManyHeap(const SearchEngineData<Algorithm>::ManyToManyQueryHeap &search_heap,
            //                           const NodeID middle_node_id,
            //                           std::vector<NodeID> &packed_path)
            // {
            //     NodeID current_node_id = middle_node_id;
            //     // all initial nodes will have itself as parent, or a node not in the heap
            //     // in case of a core search heap. We need a distinction between core entry nodes
            //     // and start nodes since otherwise start node specific code that assumes
            //     // node == node.parent (e.g. the loop code) might get actived.

            //     while (current_node_id != search_heap.GetData(current_node_id).parent &&
            //            search_heap.WasInserted(search_heap.GetData(current_node_id).parent))
            //     {
            //         current_node_id = search_heap.GetData(current_node_id).parent;
            //         std::cout << "Im in here! current_node_id is " << current_node_id << std::endl;
            //         packed_path.emplace_back(current_node_id);
            //     }
            // }

            // ^ returns far fewer results than retrievePackedPathFromSearchSpace returns. for example the couts above look like this:
            // packed_path_from_middle_to_target: 1, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 0, 2, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 0, 2, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 4, 
            // Im in here! current_node_id is 0
            // packed_path_from_source_to_middle: 0, 
            // packed_path_from_middle_to_target: 3, 0, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 3, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 2, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 4, 
            // Im in here! current_node_id is 2
            // packed_path_from_source_to_middle: 2, 
            // packed_path_from_middle_to_target: 3, 0, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 3, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 2, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 4, 
            // Im in here! current_node_id is 2
            // packed_path_from_source_to_middle: 2, 
            // packed_path_from_middle_to_target: 4, 3, 0, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 4, 3, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 4, 3, 
            // packed_path_from_source_to_middle: 
            // packed_path_from_middle_to_target: 4, 
            // packed_path_from_source_to_middle:  
            //
            // why? because some middle nodes have the same id as their parents (the loop thing) and we don't want to bother with those, and otherwise, they weren't inserted into the heap. 
            // what does insertion into the heap mean? it means that that node was part of a path. so not being isnerted into the heap means we probably don't care about it since the
            // packed_path_from_middle_to_target won't have a matching packed_path_from_source_to_middle 
            // ```
            // while (current_node_id != search_heap.GetData(current_node_id).parent &&
            //      search_heap.WasInserted(search_heap.GetData(current_node_id).parent))
            // {
            // ```
            // I could use the same while conditions to only pick out the shortcuts that matter i.e. the ones that have a middle node that's relevant? But this should already be the
            // case from the use of the column_index and row_index. argh. I dont understand why ^ is happening.

            // also, does a packed path or packed leg only ever have two nodeIDs in it? the start and the end? so, is `packed_path_from_middle_to_target: 4, 3, 0, ` invalid? 
            
            // Let's try to move on to unpackPath in the meantime.

            // std::vector<NodeID> unpacked_nodes;
            // std::vector<EdgeID> unpacked_edges;
            // if (!packed_leg.empty())
            // {
            //     unpacked_nodes.reserve(packed_leg.size());
            //     unpacked_edges.reserve(packed_leg.size());
            //     unpacked_nodes.push_back(packed_leg.front());
            //     ch::unpackPath(facade,
            //                     packed_leg.begin(),
            //                     packed_leg.end(),
            //                     *engine_working_data.unpacking_cache.get(),
            //                     [&unpacked_nodes, &unpacked_edges](std::pair<NodeID, NodeID> &edge,
            //                                                       const auto &edge_id) {
            //                        BOOST_ASSERT(edge.first == unpacked_nodes.back());
            //                        unpacked_nodes.push_back(edge.second);
            //                        unpacked_edges.push_back(edge_id);
            //     });
            // }

            // Nooooooo. It compiles and then crashes, citing a socket hanging up error! :sadpanda: I'm going to sleep.

            // next steps:
            // to get the path into vector just use the same vector and pass it in to both fns above
            // unpack packed_path (ch::unpackPath())
            // calculate the duration and fill in the durations table
        }

        //           targets (columns) target_id = column_idx
        //              a   b   c
        //          a  [0,  1,  2],
        // sources  b  [3,  4,  5],
        //  (rows)  c  [6,  7,  8],
        //          d  [9, 10, 11]
        // row_idx * number_of_targets + column_idx
        // a -> c 0 * 3 + 2 = 2
        // c -> c 2 * 3 + 2 = 8
        // d -> c 3 * 3 + 2 = 11
    }

    return durations_table;
}

} // namespace routing_algorithms
} // namespace engine
} // namespace osrm
