
#include "sdsl/int_vector.hpp"
#include "sdsl/rank_support.hpp"
#include "sdsl/select_support.hpp"

#include "PufferfishIndex.hpp"

struct Bucket {
  uint64_t numOfKmers = 0;
  uint64_t numOfUnitigs = 0;
  uint64_t seqLength = 0;
};
void findMinimizer(uint64_t kmer, uint8_t k, uint8_t m, uint8_t* minimizer, uint64_t* minimizerPos) {
  uint8_t mask = (0x01 << 2*m)-1;
  for (uint64_t cntr = 0; cntr < k; cntr++) {
    //std::cout << "c" << cntr << " " << ((kmer >> 2*cntr) & mask) << " ";
    if ((*minimizer) > ((kmer >> 2*cntr) & mask)) {
      (*minimizer) = (kmer >> 2*cntr) & mask;
      (*minimizerPos) = cntr;
    }
  }
}

int main(int argc, char* argv[]) {

  PufferfishIndex pfi(argv[1]);
  std::cout << "Successfully read.\n";
  uint8_t k = pfi.k();
  sdsl::int_vector<2>& seq = pfi.getSeq();
  sdsl::bit_vector::select_1_type& unitigS = pfi.getBoundarySelect();

  for (uint8_t m = 2; m < 6; m++) {
    uint64_t contigCntr = 1;
    uint64_t start = 0;
    uint64_t cur = 0;
    uint64_t next = unitigS(contigCntr);
    uint8_t minimizer = 0x01 << 2*m;
    uint64_t minimizerPos = 0;
    uint64_t totalNumOfKmers = (next - cur) -k+1;

    // findMinimizer test:
    /*
    uint8_t tmpMinimizer = 0x01 << 2*m;
    uint64_t relMinimizerPos = 0;
    findMinimizer(0xFFFFFFFEFFFFFFFF, k, m, &tmpMinimizer, &relMinimizerPos);
    std::cout << "IT SHOULD BE 11 AND IS " << (size_t)tmpMinimizer << "\n";
    */
    std::vector<Bucket> buckets(0x01 << 2*m);
    // initial all pairs to zero
    uint64_t unitigSplitCnt = 0; // total number of splits happening across all unitigs
    uint64_t splittedUnitigCnt = 0; // total number of unitigs that we had a split in
    uint64_t tmpUnitigSplitCnt = 0; // number of splits in current unitig
    uint64_t prevPos = 0;
    while (cur < seq.size()-(k-1)) {
      // if we've already passed the last kmer in current unitig, go to next unitig
      if (cur >= next - (k-1)) {
        // got to the end of the unitig. Add the new contig to the right minimizer bucket
        buckets[minimizer].seqLength += (cur+(k-1)-prevPos);
        buckets[minimizer].numOfKmers += (cur+(k-1)-prevPos) - k+1;
        buckets[minimizer].numOfUnitigs++;
        minimizer = 0x01 << 2*m; // reset minimizer to max for next contig
        start = next;
        cur = next;
        prevPos = cur;
        next = unitigS(contigCntr++);
        if (next == 0) break;
        totalNumOfKmers += next-cur-k+1;
        if (tmpUnitigSplitCnt > 0) {
          splittedUnitigCnt++;
          unitigSplitCnt += tmpUnitigSplitCnt;
          // reset current unitig split cnt to zero
          tmpUnitigSplitCnt = 0;
        }
      }

      // if current kmer is the first kmer in unitig or we've surpassed current minimizer
      if (cur == start or cur > minimizerPos) {
        uint64_t kmer = seq.get_int(2*cur, 2*k);
        uint8_t tmpMinimizer = 0x01 << 2*m;
        uint64_t relMinimizerPos = 0;
        findMinimizer(kmer, k, m, &tmpMinimizer, &relMinimizerPos);
        minimizerPos = cur + relMinimizerPos;
        // If we are not at a new unitig and the minimizer is different from previous one, this means a split in unitig
        if (cur != start and minimizer != tmpMinimizer) {
          // store the info about the new unitig up until this minimizer
          buckets[minimizer].seqLength += (cur+(k-1)-prevPos);
          buckets[minimizer].numOfKmers += (cur+(k-1)-prevPos) - k+1;
          buckets[minimizer].numOfUnitigs++;
          tmpUnitigSplitCnt++; // increase unitig split count
          prevPos = cur;
        }
        minimizer = tmpMinimizer;
        //std::cout << (size_t)minimizer << "\n";
      }
      else {
        uint8_t nextPotentialMinimizer = seq.get_int(2*(cur+(k-m)), 2*m);
        if (nextPotentialMinimizer < minimizer) {
          tmpUnitigSplitCnt++;
          minimizer = nextPotentialMinimizer;
          minimizerPos = cur+(k-m);
          buckets[minimizer].seqLength += (cur+(k-1)-prevPos);
          buckets[minimizer].numOfKmers += (cur+(k-1)-prevPos) - k+1;
          buckets[minimizer].numOfUnitigs++;
          prevPos = cur;
        }
      }
      cur++;
    }
    uint64_t totalBits = totalNumOfKmers*ceil(log2(totalNumOfKmers)) + (seq.size()*2);
    std::cout << "\nMINIMIZER LENGTH : "<< static_cast<size_t>(m) << "\n\n";
    std::cout << "# of unitigs before splitting: " << contigCntr << "\n"
              << "# of unitigs after splitting: " << unitigSplitCnt << "\n"
              << "# of unitigs that were splitted: " << splittedUnitigCnt << "\n";
    std::cout << "\nBefore applying minimizers:\n"
              << "# of kmers: " << totalNumOfKmers << " , "
              << "bits per kmer pos "<< ceil(log2(totalNumOfKmers)) << "\n"
              <<"total bits: " << totalBits << " or " << totalBits/(1024*1024*8) << "MB\n";


    totalBits = 0;
    std::cout << "\nAfter applying minimizers:\n";
    uint8_t bCntr = 0;
    for (auto bIt = buckets.begin(); bIt != buckets.end(); bIt++) {
      if (bIt->numOfKmers != 0) {
        totalBits += bIt->seqLength*2 + ceil(log2(bIt->numOfKmers));
        std::cout << "bucket" << static_cast<size_t>(bCntr++) << " : "
                << "# of unitigs: " << bIt->numOfUnitigs << " , "
                << "# of kmers: " << bIt->numOfKmers << " , "
                << "bits per kmer pos "<< ceil(log2(bIt->numOfKmers)) << "\n";
      }
    }
    std::cout <<"total bits: " << totalBits << " or " << totalBits/(1024*1024*8) << "MB\n";
  }

}
