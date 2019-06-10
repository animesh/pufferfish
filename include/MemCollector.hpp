#ifndef HIT_COLLECTOR_HPP
#define HIT_COLLECTOR_HPP

#include "CanonicalKmer.hpp"
#include "CanonicalKmerIterator.hpp"
#include "PufferfishIndex.hpp"
#include "PufferfishSparseIndex.hpp"
#include "PufferfishLossyIndex.hpp"
#include "Util.hpp"
#include "edlib.h"
#include "jellyfish/mer_dna.hpp"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <sparsepp/spp.h>

#include "MemChainer.hpp"


template <typename PufferfishIndexT> class MemCollector {
enum class ExpansionTerminationType : uint8_t { MISMATCH = 0, CONTIG_END, READ_END };  

public:
    explicit MemCollector(PufferfishIndexT* pfi) : pfi_(pfi) { k = pfi_->k(); }

  size_t expandHitEfficient(util::ProjectedHits& hit,
                          pufferfish::CanonicalKmerIterator& kit,
                          ExpansionTerminationType& et);
    
  bool operator()(std::string &read,
                  spp::sparse_hash_map<size_t, std::vector<util::MemCluster>>& memClusters,
                  uint32_t maxSpliceGap,
                  util::MateStatus mateStatus,
                  util::QueryCache& qc,
                  bool hChain=false,
                  bool mergeMems=false,
                  bool verbose=false);

  void clear();

private:
  PufferfishIndexT* pfi_;
  size_t k;
  //AlignerEngine ae_;
  std::vector<util::UniMemInfo> memCollectionLeft;
  std::vector<util::UniMemInfo> memCollectionRight;
  bool isSingleEnd = false;
  MemClusterer mc;
  std::map<std::pair<pufferfish::common_types::ReferenceID, bool>, std::vector<util::MemInfo>> trMemMap;
};
#endif
