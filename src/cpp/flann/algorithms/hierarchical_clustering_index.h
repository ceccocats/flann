/***********************************************************************
 * Software License Agreement (BSD License)
 *
 * Copyright 2008-2011  Marius Muja (mariusm@cs.ubc.ca). All rights reserved.
 * Copyright 2008-2011  David G. Lowe (lowe@cs.ubc.ca). All rights reserved.
 *
 * THE BSD LICENSE
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *************************************************************************/

#ifndef FLANN_HIERARCHICAL_CLUSTERING_INDEX_H_
#define FLANN_HIERARCHICAL_CLUSTERING_INDEX_H_

#include <algorithm>
#include <string>
#include <map>
#include <cassert>
#include <limits>
#include <cmath>

#include "flann/general.h"
#include "flann/algorithms/nn_index.h"
#include "flann/algorithms/dist.h"
#include "flann/util/matrix.h"
#include "flann/util/result_set.h"
#include "flann/util/heap.h"
#include "flann/util/allocator.h"
#include "flann/util/random.h"
#include "flann/util/saving.h"


namespace flann
{

struct HierarchicalClusteringIndexParams : public IndexParams
{
    HierarchicalClusteringIndexParams(int branching = 32,
                                      flann_centers_init_t centers_init = FLANN_CENTERS_RANDOM,
                                      int trees = 4, int leaf_size = 100)
    {
        (*this)["algorithm"] = FLANN_INDEX_HIERARCHICAL;
        // The branching factor used in the hierarchical clustering
        (*this)["branching"] = branching;
        // Algorithm used for picking the initial cluster centers
        (*this)["centers_init"] = centers_init;
        // number of parallel trees to build
        (*this)["trees"] = trees;
        // maximum leaf size
        (*this)["leaf_size"] = leaf_size;
    }
};


/**
 * Hierarchical index
 *
 * Contains a tree constructed through a hierarchical clustering
 * and other information for indexing a set of points for nearest-neighbour matching.
 */
template <typename Distance>
class HierarchicalClusteringIndex : public NNIndex<Distance>
{
public:
    typedef typename Distance::ElementType ElementType;
    typedef typename Distance::ResultType DistanceType;

    /**
     * Constructor.
     *
     * @param index_params
     * @param d
     */
    HierarchicalClusteringIndex(const IndexParams& index_params = HierarchicalClusteringIndexParams(), Distance d = Distance())
        : NNIndex<Distance>(index_params), distance_(d), size_at_build_(0)
    {
        memoryCounter_ = 0;

        branching_ = get_param(index_params_,"branching",32);
        centers_init_ = get_param(index_params_,"centers_init", FLANN_CENTERS_RANDOM);
        trees_ = get_param(index_params_,"trees",4);
        leaf_size_ = get_param(index_params_,"leaf_size",100);

        switch(centers_init_) {
        case FLANN_CENTERS_RANDOM:
        	chooseCenters_ = new RandomCenterChooser<Distance>(d);
        	break;
        case FLANN_CENTERS_GONZALES:
        	chooseCenters_ = new GonzalesCenterChooser<Distance>(d);
        	break;
        case FLANN_CENTERS_KMEANSPP:
            chooseCenters_ = new KMeansppCenterChooser<Distance>(d);
        	break;
        default:
            throw FLANNException("Unknown algorithm for choosing initial centers.");
        }
    }


    /**
     * Index constructor
     *
     * Params:
     *          inputData = dataset with the input features
     *          params = parameters passed to the hierarchical k-means algorithm
     */
    HierarchicalClusteringIndex(const Matrix<ElementType>& inputData, const IndexParams& index_params = HierarchicalClusteringIndexParams(),
                                Distance d = Distance())
        : NNIndex<Distance>(index_params), distance_(d), size_at_build_(0)
    {
        memoryCounter_ = 0;

        branching_ = get_param(index_params_,"branching",32);
        centers_init_ = get_param(index_params_,"centers_init", FLANN_CENTERS_RANDOM);
        trees_ = get_param(index_params_,"trees",4);
        leaf_size_ = get_param(index_params_,"leaf_size",100);

        switch(centers_init_) {
        case FLANN_CENTERS_RANDOM:
        	chooseCenters_ = new RandomCenterChooser<Distance>(d);
        	break;
        case FLANN_CENTERS_GONZALES:
        	chooseCenters_ = new GonzalesCenterChooser<Distance>(d);
        	break;
        case FLANN_CENTERS_KMEANSPP:
            chooseCenters_ = new KMeansppCenterChooser<Distance>(d);
        	break;
        default:
            throw FLANNException("Unknown algorithm for choosing initial centers.");
        }
        chooseCenters_->setDataset(inputData);
        
        setDataset(inputData);
    }

    HierarchicalClusteringIndex(const HierarchicalClusteringIndex&);
    HierarchicalClusteringIndex& operator=(const HierarchicalClusteringIndex&);

    /**
     * Index destructor.
     *
     * Release the memory used by the index.
     */
    virtual ~HierarchicalClusteringIndex()
    {
    }

    /**
     * Computes the inde memory usage
     * Returns: memory used by the index
     */
    int usedMemory() const
    {
        return pool_.usedMemory+pool_.wastedMemory+memoryCounter_;
    }

    /**
     * Builds the index
     */
    void buildIndex()
    {
        if (branching_<2) {
            throw FLANNException("Branching factor must be at least 2");
        }
        tree_roots_.resize(trees_);
        std::vector<int> indices(size_);
        for (int i=0; i<trees_; ++i) {
            for (size_t j=0; j<size_; ++j) {
                indices[j] = j;
            }
            tree_roots_[i] = new(pool_) Node();
            computeClustering(tree_roots_[i], &indices[0], size_);
        }
        size_at_build_ = size_;
    }

    
    void addPoints(const Matrix<ElementType>& points, float rebuild_threshold = 2)
    {
        assert(points.cols==veclen_);
        size_t old_size = size_;

        extendDataset(points);
        
        if (rebuild_threshold>1 && size_at_build_*rebuild_threshold<size_) {
            pool_.free();
            buildIndex();
        }
        else {
            for (size_t i=0;i<points.rows;++i) {
                for (int j = 0; j < trees_; j++) {
                    addPointToTree(tree_roots_[j], old_size + i);
                }
            }            
        }
    }


    flann_algorithm_t getType() const
    {
        return FLANN_INDEX_HIERARCHICAL;
    }


    void saveIndex(FILE* stream)
    {
        save_value(stream, branching_);
        save_value(stream, trees_);
        save_value(stream, centers_init_);
        save_value(stream, leaf_size_);
        save_value(stream, memoryCounter_);
        for (int i=0; i<trees_; ++i) {
            save_tree(stream, tree_roots_[i], i);
        }

    }


    void loadIndex(FILE* stream)
    {
        load_value(stream, branching_);
        load_value(stream, trees_);
        load_value(stream, centers_init_);
        load_value(stream, leaf_size_);
        load_value(stream, memoryCounter_);
        tree_roots_.resize(trees_);
        for (int i=0; i<trees_; ++i) {
            load_tree(stream, tree_roots_[i], i);
        }

        index_params_["algorithm"] = getType();
        index_params_["branching"] = branching_;
        index_params_["trees"] = trees_;
        index_params_["centers_init"] = centers_init_;
        index_params_["leaf_size"] = leaf_size_;
    }


    /**
     * Find set of nearest neighbors to vec. Their indices are stored inside
     * the result object.
     *
     * Params:
     *     result = the result object in which the indices of the nearest-neighbors are stored
     *     vec = the vector for which to search the nearest neighbors
     *     searchParams = parameters that influence the search algorithm (checks)
     */
    void findNeighbors(ResultSet<DistanceType>& result, const ElementType* vec, const SearchParams& searchParams)
    {

        int maxChecks = searchParams.checks;

        // Priority queue storing intermediate branches in the best-bin-first search
        Heap<BranchSt>* heap = new Heap<BranchSt>(size_);

        DynamicBitset checked(size_);
        int checks = 0;
        for (int i=0; i<trees_; ++i) {
            findNN(tree_roots_[i], result, vec, checks, maxChecks, heap, checked);
        }

        BranchSt branch;
        while (heap->popMin(branch) && (checks<maxChecks || !result.full())) {
            NodePtr node = branch.node;
            findNN(node, result, vec, checks, maxChecks, heap, checked);
        }

        delete heap;

    }


private:

    struct PointInfo
    {
    	/** Point index */
    	size_t index;
    	/** Point data */
    	ElementType* point;
    };

    /**
     * Struture representing a node in the hierarchical k-means tree.
     */
    struct Node
    {
        /**
         * The cluster center
         */
    	ElementType* pivot;
        /**
         * Child nodes (only for non-terminal nodes)
         */
        std::vector<Node*> childs;
        /**
         * Node points (only for terminal nodes)
         */
        std::vector<PointInfo> points;
    };
    typedef Node* NodePtr;



    /**
     * Alias definition for a nicer syntax.
     */
    typedef BranchStruct<NodePtr, DistanceType> BranchSt;



    void save_tree(FILE* stream, NodePtr node, int num)
    {
    	//FIXME
        save_value(stream, node->pivot);
        size_t childs_size = node->childs.size();
        save_value(stream, childs_size);

        if (childs_size==0) {
//            save_value(stream, node->indices);
        }
        else {
            for(size_t i=0; i<childs_size; ++i) {
                save_tree(stream, node->childs[i], num);
            }
        }
    }


    void load_tree(FILE* stream, NodePtr& node, int num)
    {
    	//FIXME
        node = new(pool_) Node();
        load_value(stream, node->pivot);
        size_t childs_size = 0;
        load_value(stream, childs_size);

        if (childs_size==0) {
//            load_value(stream, node->indices);
        }
        else {
            node->childs.resize(childs_size);
            for(size_t i=0; i<childs_size; ++i) {
                load_tree(stream, node->childs[i], num);
            }
        }
    }




    void computeLabels(int* indices, int indices_length,  int* centers, int centers_length, int* labels, DistanceType& cost)
    {
        cost = 0;
        for (int i=0; i<indices_length; ++i) {
            ElementType* point = points_[indices[i]];
            DistanceType dist = distance_(point, points_[centers[0]], veclen_);
            labels[i] = 0;
            for (int j=1; j<centers_length; ++j) {
                DistanceType new_dist = distance_(point, points_[centers[j]], veclen_);
                if (dist>new_dist) {
                    labels[i] = j;
                    dist = new_dist;
                }
            }
            cost += dist;
        }
    }

    /**
     * The method responsible with actually doing the recursive hierarchical
     * clustering
     *
     * Params:
     *     node = the node to cluster
     *     indices = indices of the points belonging to the current node
     *     branching = the branching factor to use in the clustering
     *
     */
    void computeClustering(NodePtr node, int* indices, int indices_length)
    {
        if (indices_length < leaf_size_) { // leaf node
            node->points.resize(indices_length);
            for (int i=0;i<indices_length;++i) {
            	node->points[i].index = indices[i];
            	node->points[i].point = points_[indices[i]];
            }
            node->childs.clear();
            return;
        }

        std::vector<int> centers(branching_);
        std::vector<int> labels(indices_length);

        int centers_length;
        (*chooseCenters_)(branching_, indices, indices_length, &centers[0], centers_length);

        if (centers_length<branching_) {
            node->points.resize(indices_length);
            for (int i=0;i<indices_length;++i) {
            	node->points[i].index = indices[i];
            	node->points[i].point = points_[indices[i]];
            }
            node->childs.clear();
            return;
        }


        //  assign points to clusters
        DistanceType cost;
        computeLabels(indices, indices_length, &centers[0], centers_length, &labels[0], cost);

        node->childs.resize(branching_);
        int start = 0;
        int end = start;
        for (int i=0; i<branching_; ++i) {
            for (int j=0; j<indices_length; ++j) {
                if (labels[j]==i) {
                    std::swap(indices[j],indices[end]);
                    std::swap(labels[j],labels[end]);
                    end++;
                }
            }

            node->childs[i] = new(pool_) Node();
            node->childs[i]->pivot = points_[centers[i]];
            node->childs[i]->points.clear();
            computeClustering(node->childs[i],indices+start, end-start);
            start=end;
        }
    }



    /**
     * Performs one descent in the hierarchical k-means tree. The branches not
     * visited are stored in a priority queue.
     *
     * Params:
     *      node = node to explore
     *      result = container for the k-nearest neighbors found
     *      vec = query points
     *      checks = how many points in the dataset have been checked so far
     *      maxChecks = maximum dataset points to checks
     */


    template<typename ResultSet>
    void findNN(NodePtr node, ResultSet& result, const ElementType* vec, int& checks, int maxChecks,
                Heap<BranchSt>* heap,  DynamicBitset& checked)
    {
        if (node->childs.empty()) {
            if (checks>=maxChecks) {
                if (result.full()) return;
            }

            for (size_t i=0; i<node->points.size(); ++i) {
            	PointInfo& pointInfo = node->points[i];

                if (checked.test(pointInfo.index) || removed_points_.test(pointInfo.index)) continue;
                DistanceType dist = distance_(pointInfo.point, vec, veclen_);
                result.addPoint(dist, pointInfo.index);
                checked.set(pointInfo.index);
                ++checks;
            }
        }
        else {
            DistanceType* domain_distances = new DistanceType[branching_];
            int best_index = 0;
            domain_distances[best_index] = distance_(vec, node->childs[best_index]->pivot, veclen_);
            for (int i=1; i<branching_; ++i) {
                domain_distances[i] = distance_(vec, node->childs[i]->pivot, veclen_);
                if (domain_distances[i]<domain_distances[best_index]) {
                    best_index = i;
                }
            }
            for (int i=0; i<branching_; ++i) {
                if (i!=best_index) {
                    heap->insert(BranchSt(node->childs[i],domain_distances[i]));
                }
            }
            delete[] domain_distances;
            findNN(node->childs[best_index],result,vec, checks, maxChecks, heap, checked);
        }
    }
    
    void addPointToTree(NodePtr node, size_t index)
    {
        ElementType* point = points_[index];
        
        if (node->childs.empty()) { // leaf node
        	PointInfo pointInfo;
        	pointInfo.point = point;
        	pointInfo.index = index;
            node->points.push_back(pointInfo);

            if (node->points.size()>=size_t(branching_)) {
                std::vector<int> indices(node->points.size());

                for (size_t i=0;i<node->points.size();++i) {
                	indices[i] = node->points[i].index;
                }
                computeClustering(node, &indices[0], indices.size());
            }
        }
        else {            
            // find the closest child
            int closest = 0;
            ElementType* center = node->childs[closest]->pivot;
            DistanceType dist = distance_(center, point, veclen_);
            for (size_t i=1;i<size_t(branching_);++i) {
                center = node->childs[i]->pivot;
                DistanceType crt_dist = distance_(center, point, veclen_);
                if (crt_dist<dist) {
                    dist = crt_dist;
                    closest = i;
                }
            }
            addPointToTree(node->childs[closest], index);
        }                
    }

private:

    /**
     * The root nodes in the tree.
     */
    std::vector<Node*> tree_roots_;

    /**
     * The distance
     */
    Distance distance_;

    /**
     * Number of features in the dataset when the index was last built.
     */
    size_t size_at_build_;

    /**
     * Pooled memory allocator.
     *
     * Using a pooled memory allocator is more efficient
     * than allocating memory directly when there is a large
     * number small of memory allocations.
     */
    PooledAllocator pool_;

    /**
     * Memory occupied by the index.
     */
    int memoryCounter_;

    /** index parameters */
    /**
     * Branching factor to use for clustering
     */
    int branching_;
    
    /**
     * How many parallel trees to build
     */
    int trees_;
    
    /**
     * Algorithm to use for choosing cluster centers
     */
    flann_centers_init_t centers_init_;
    
    /**
     * Max size of leaf nodes
     */
    int leaf_size_;
    
    /**
     * Algorithm used to choose initial centers
     */
    CenterChooser<Distance>* chooseCenters_;

    USING_BASECLASS_SYMBOLS
};

}

#endif /* FLANN_HIERARCHICAL_CLUSTERING_INDEX_H_ */
