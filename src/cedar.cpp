#include <fstream>
#include <iostream>
#include <string>
#include <cstdlib> // std::abs
#include <cmath> // std::abs
#include <memory>
#include <unordered_set>
#include <list>

#include "cedar.hpp"

#include "clipp.h"
#include "EquivalenceClassBuilder.hpp"
#include "CLI/Timer.hpp"
#include "PufferFS.hpp"
#include "cereal/archives/binary.hpp"
#include "cereal/types/vector.hpp"
#include "cereal/types/string.hpp"
//#include "LinearMultiArray.h"
#include "SetCover.h"

struct set_data {
    unsigned int ** sets;
    unsigned short ** weights;
    unsigned int * set_sizes;
    unsigned int * element_size_lookup;
    unsigned int set_count;
    unsigned int uniqu_element_count;
    unsigned int all_element_count;
    unsigned int max_weight;
};
// trim from both ends (in place)
static inline std::string& trim(std::string &s) {
    std::string chars = "\t\n\v\f\r ";
    s.erase(0, s.find_first_not_of(chars));
    s.erase(s.find_last_not_of(chars) + 1);
    return s;
}

struct CedarOpts {
    std::string taxonomyTree_filename;
    std::string refId2TaxId_filename;
    std::string mapperOutput_filename;
    std::string output_filename;
    std::string level = "species";
    size_t maxIter = 1000;
    double eps = 0.001;
    double filterThreshold = 0;
    double minCnt = 0;
    bool flatAbund{false};
    bool requireConcordance{false};
    bool isPuffOut{false};
    bool isSAM{false};
    bool onlyUniq{false};
    bool onlyPerfect{false};
		uint32_t segmentSize{200};
};

template<class ReaderType>
Cedar<ReaderType>::Cedar(std::string& taxonomyTree_filename, 
                 std::string& refId2TaxId_filename, 
                 std::string pruneLevelIn,
                 double filteringThresholdIn,
                 bool flatAbundIn,
                 std::shared_ptr<spdlog::logger> loggerIn) {
    flatAbund = flatAbundIn;
    logger = loggerIn;
    logger->info("Cedar: Construct ..");

    // map rank string values to enum values
    filteringThreshold = filteringThresholdIn;
    if (!flatAbund) {
        pruningLevel = TaxaNode::str2rank(pruneLevelIn);
        std::ifstream tfile;
        uint32_t id, pid;
         std::string rank, name;
        // load the reference id (name) to its corresponding taxonomy id
        tfile.open(refId2TaxId_filename);
        while(!tfile.eof()) {
            tfile >> name >> id;
            refId2taxId[name] = id;
        }
        tfile.close();

        // load the taxonomy child-parent tree and the rank of each node
        tfile.open(taxonomyTree_filename);
        std::string tmp, line;
        uint64_t lcntr{0};
        //std::cerr << "\n\nreading taxonomy tree\n\n";
        while (!tfile.eof()) {
            try {
                lcntr++;
                std::getline(tfile, line);
                uint64_t first = line.find_first_of('|');
                std::string fstr = line.substr(0, first);
                id = (uint32_t) std::stoul(trim(fstr));
                uint64_t second = line.find_first_of('|', first + 1);
                fstr = line.substr(first + 1, second - first - 1);
                pid = (uint32_t) std::stoul(trim(fstr));
                uint64_t third = line.find_first_of('|', second + 1);
                fstr = line.substr(second + 1, third - second - 1);
                rank = trim(fstr);
                if (rank.find("HS") != std::string::npos)
                    std::cerr << "here:" << id << " " << pid << " " << tmp << "\n";
                //if (lcntr == 1369188) {
                //std::cerr << lcntr << " " /*<< line << "\n"*/ << id << " " << pid << " " << rank << "\n";
                //}

                taxaNodeMap[id] = TaxaNode(id, pid, TaxaNode::str2rank(rank));
                if (taxaNodeMap[id].isRoot()) {
                    rootId = id;
                    logger->info("Root Id : {}", id);
                }
            } catch (const std::invalid_argument& ia) {
                std::cerr << "Invalid argument: " << ia.what() << '\n';
                continue;
            }
            
        }

        tfile.close();
        //std::cerr << "\n\ndone reading taxonomy tree\n\n";
    } 
}
template<class ReaderType>
void Cedar<ReaderType>::calculateCoverage() {
	for (uint32_t i=0; i<strain_coverage_bins.size(); i++) {
		auto bins = strain_coverage_bins[i];
		double covered = 0.0;
		uint32_t expression_depth = 0;
		for (uint32_t j=0; j<bins.size(); j++) {
			if (bins[j]>0)
				covered++;
			expression_depth+=bins[j];
		}
		strain_coverage[i] = covered / bins.size();
	}
}

template<class ReaderType>
void Cedar<ReaderType>::loadMappingInfo(std::string mapperOutput_filename,
                            bool requireConcordance,
                            bool onlyUniq,
                            bool onlyPerfect,
														uint32_t segmentSize) {
    int32_t rangeFactorization{4};
    uint64_t totalReadCnt{0}, seqNotFound{0},
    totalMultiMappedReads{0}, totalUnmappedReads{0}, totalReadsNotPassingCond{0}, tid;
    logger->info("Cedar: Load Mapping File ..");
    logger->info("Mapping Output File: {}", mapperOutput_filename);
    mappings.load(mapperOutput_filename, logger);
    logger->info("is dataset paired end? {}", mappings.isMappingPaired());
    ReadInfo readInfo;
    TaxaNode* prevTaxa{nullptr};
    size_t conflicting{0};
    size_t discordantMappings{0};

		// Construct coverage bins
    for (uint64_t i=0; i<mappings.numRefs(); ++i) {
			auto refLength = mappings.refLength(i);
			uint64_t binCnt = refLength/segmentSize;
			if (binCnt == 0) binCnt = 1;
			std::vector<uint32_t> bins(binCnt, 0);
			strain_coverage_bins[i] = bins;
    }
	
    constexpr const bool getReadName = true;
    size_t numMapped{0};
    bool wasMapped = false;
    std::cerr << "\n";
    while(mappings.nextRead(readInfo, getReadName)) {
        totalReadCnt++;
        if (totalReadCnt % 1000000 == 0) {
          std::cerr << "\rProcessed " << totalReadCnt << " reads";
        }
        activeTaxa.clear();
        double readMappingsScoreSum = 0;
        std::vector<std::pair<uint64_t, double>> readPerStrainProbInst;
        readPerStrainProbInst.reserve(readInfo.cnt);
        bool isConflicting = true;
        uint64_t maxScore = readInfo.len;
        if (readInfo.cnt != 0) {
            if (readInfo.cnt > 1) {
                totalMultiMappedReads++;
            }
            if (!wasMapped) { wasMapped = true; ++numMapped; }
            std::set<uint64_t> seen;
            prevTaxa = nullptr;
            for (auto& mapping : readInfo.mappings) {
                auto &refNam = mappings.refName(mapping.getId());
                // first condition: Ignore those references that we don't have a taxaId
                // second condition: Ignore repeated exactly identical mappings (FIXME thing)
                // note: Fatemeh!! don't change the or and and again!!
                // if we are on flatAbund, we want to count for multiple occurrences of a reference
                if( (flatAbund or
                     (refId2taxId.find(refNam) != refId2taxId.end()))
                        and
                        activeTaxa.find(mapping.getId()) == activeTaxa.end()) {
                    if (prevTaxa != nullptr and
                        activeTaxa.find(mapping.getId()) == activeTaxa.end() and 
                        !prevTaxa->compareIntervals(mapping)) {
                        isConflicting = false;
                    }
                    activeTaxa.insert(mapping.getId());
                    if (requireConcordance && mappings.isMappingPaired() &&
                    (!mapping.isConcordant() || mapping.isFw(ReadEnd::LEFT) == mapping.isFw(ReadEnd::RIGHT) ) ) {
                        discordantMappings++;
                        continue;
                    }

                    tid = flatAbund ? mapping.getId() : refId2taxId[refNam];
                    seqToTaxMap[mapping.getId()] = static_cast<uint32_t >(tid);

                    if (cov.find(refId2taxId[refNam]) == cov.end()) {
                        cov[refId2taxId[refNam]] = 0;
                    }
                    cov[refId2taxId[refNam]] += mapping.getScore();
                    readPerStrainProbInst.emplace_back(mapping.getId(),
                            static_cast<double>( mapping.getScore()) / static_cast<double>(mappings.refLength(mapping.getId())));
                    readMappingsScoreSum += readPerStrainProbInst.back().second;

										uint32_t bin_number = mapping.getPos(ReadEnd::LEFT)/segmentSize > 0 ? mapping.getPos(ReadEnd::LEFT)/segmentSize - 1 : 0;
										strain_coverage_bins[mapping.getId()][bin_number]++;
                    prevTaxa = &mapping;
                }
            }
            if (activeTaxa.empty()) {
                seqNotFound++;
            } else if ( (!onlyUniq and !onlyPerfect) 
                        or (onlyUniq and activeTaxa.size() == 1)
                        or (onlyPerfect and activeTaxa.size() == 1 
                            and 
                            readInfo.mappings[0].getScore() >= maxScore) ) {
                if (!isConflicting) {conflicting++;}
                // it->first : strain id
                // it->second : prob of current read comming from this strain id (mapping_score/ref_len)
                for (auto it = readPerStrainProbInst.begin(); it != readPerStrainProbInst.end(); it++) {
                    it->second = it->second/readMappingsScoreSum; // normalize the probabilities for each read
                    strain[it->first] += 1.0/static_cast<double>(readPerStrainProbInst.size());
                }
                // SAVE MEMORY, don't push this
                //readPerStrainProb.push_back(readPerStrainProbInst);

                // construct the range factorized eq class here 
                std::sort(readPerStrainProbInst.begin(), readPerStrainProbInst.end(), 
                [](std::pair<uint64_t, double>& a, std::pair<uint64_t, double>& b) {
                    return a.first < b.first;
                });
                std::vector<uint32_t> genomeIDs; genomeIDs.reserve(2*readPerStrainProbInst.size());
                std::vector<double> probs; probs.reserve(readPerStrainProbInst.size());
                for (auto &it : readPerStrainProbInst) {
                    genomeIDs.push_back(static_cast<uint32_t>(it.first));
                    probs.push_back(it.second);
                } 
                if (rangeFactorization > 0) {
                    int genomeSize = genomeIDs.size();
                    int rangeCount = std::sqrt(genomeSize) + rangeFactorization;
                    for (int i = 0; i < genomeSize; i++) {
                        int rangeNumber = static_cast<int>(probs[i] * rangeCount);
                        genomeIDs.push_back(static_cast<uint32_t>(rangeNumber));
                    }
                }
                readCnt++;
                TargetGroup tg(genomeIDs);
                eqb.addGroup(std::move(tg), probs); //add or update eq read cnt by 1
            }
            else {
                totalReadsNotPassingCond++;
            }
        } else {
                totalUnmappedReads++;
        }
    }
		calculateCoverage();
    std::cerr << "\r";
    //logger->info("Total # of unique reads: {}", readset.size());
    //notMappedReadsFile.close();
    logger->info("# of mapped (and accepted) reads: {}", readCnt);
    if (onlyUniq or onlyPerfect)
        logger->info("# of mapped reads that were not uniq/perfect: {}", totalReadsNotPassingCond);
    logger->info("# of multi-mapped reads: {}", totalMultiMappedReads);
    logger->info("# of conflicting reads: {}", conflicting);
    logger->info("# of unmapped reads: {}", totalUnmappedReads);
    if (requireConcordance)
        logger->info("Discarded {} discordant mappings.", discordantMappings);
}

template<class ReaderType>
bool Cedar<ReaderType>::basicEM(size_t maxIter, double eps, double minCnt) {
    eqb.finish();
    auto& eqvec = eqb.eqVec();
    int64_t maxSeqID{-1};
    for (auto& kv : strain) { 
        maxSeqID = (static_cast<int64_t>(kv.first) > maxSeqID) ? static_cast<int64_t>(kv.first) : maxSeqID;
    }

    std::vector<double> newStrainCnt(maxSeqID+1,0.0); 
    std::vector<double> strainCnt(maxSeqID+1);
    std::vector<bool> strainValid(maxSeqID+1);
    std::vector<bool> potentiallyRemoveStrain(maxSeqID+1);

    for (uint64_t i = 0; i < strainValid.size(); i++) {
        strainValid[i] = true;
        potentiallyRemoveStrain[i] = false;
    }
    for (auto& kv : strain) { 
      strainCnt[kv.first] = kv.second;
    }

    logger->info("maxSeqID : {}", maxSeqID);
    logger->info("found : {} equivalence classes",eqvec.size()); 
    size_t totCount{0};
    for (auto& eqc : eqvec) {
        totCount += eqc.second.count;
    }
    logger->info("Total starting count {}", totCount);
    logger->info("Total mapped reads cnt {}", readCnt);

    size_t cntr = 0;
    bool converged = false;
    uint64_t thresholdingIterStep = 10;
    bool canHelp = true;
    while (cntr++ < maxIter && !converged) {
        if (cntr % thresholdingIterStep == 0 && canHelp) {
            for (size_t i = 0; i < strainCnt.size(); ++i) {
                potentiallyRemoveStrain[i] = strainValid[i]? strainCnt[i] <= minCnt: false;
            }
            std::unordered_map <uint32_t, std::unordered_set<uint64_t>> ref2eqset;
            uint64_t removedImmediatelyCnt{0}, wasAlreadyRemovedCnt{0};
            for (auto &eqc : eqvec) {
                auto &tg = eqc.first;
                auto &v = eqc.second;
                auto csize = v.weights.size();
                uint64_t potentialRemovedCntr = 0;
                //uint64_t maxCnt = 0, maxTgt = 0;
                uint64_t totalValidEqs{0};
                for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                    auto &tgt = tg.tgts[readMappingCntr];
                    if (strainValid[tgt]) {
                        totalValidEqs++;
                    } else {
                        potentiallyRemoveStrain[tgt] = false;
                    }
                    if (potentiallyRemoveStrain[tgt]) {
                        potentialRemovedCntr++;
                    }
                    /*if (strainCnt[tgt] > maxCnt) {
                        maxCnt = strainCnt[tgt];
                        maxTgt = tgt;
                    }*/
                }
                /*if (potentialRemovedCntr < csize) {
                    maxTgt = -1;
                }
                for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                    auto &tgt = tg.tgts[readMappingCntr];
                    if (potentiallyRemoveStrain[tgt] and tgt != maxTgt)
                        strainValid[tgt] = false;
                        if (tgt == maxTgt) {
                                strainValid[tgt] = true;
                                potentiallyRemoveStrain[tgt] = false;
                        }
                }*/
                for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                    auto &tgt = tg.tgts[readMappingCntr];
                    if (potentiallyRemoveStrain[tgt]) {
                        if (potentialRemovedCntr < totalValidEqs) {
                            if (strainValid[tgt]) {
                                removedImmediatelyCnt++;
                            }
                            else
                                wasAlreadyRemovedCnt++;
                            strainValid[tgt] = false;
                        } else {
                            ref2eqset[tgt].insert(tg.hash);
                        }
                    }
                }
            }
            std::cerr << "\nRemoved immediately: " << removedImmediatelyCnt <<
                      " was already removed: " << wasAlreadyRemovedCnt <<
                      " ref2eqset size: " << ref2eqset.size() << "\n";
            std::unordered_map <uint64_t, uint32_t > eq2id;
            if (ref2eqset.size() > 0) {
                uint32_t id = 1;
                for (auto &kv : ref2eqset) {
                    for (auto v : kv.second)
                        if (eq2id.find(v) == eq2id.end()) {
                            eq2id[v] = id;
                            id++;
                        }
                }
                auto elementCnt = eq2id.size(); // n
                auto setCnt = ref2eqset.size(); // m
                //uniquEqs.clear();
                //std::cerr << elementCnt << " " << setCnt << "\n";
                set_data ret_struct;
                ret_struct.set_count = setCnt;
                ret_struct.uniqu_element_count = elementCnt;

                unsigned int *element_size = new unsigned int[elementCnt + 2];
                memset(&element_size[0], 0, sizeof(unsigned int) * (elementCnt + 2));
                ret_struct.element_size_lookup = element_size;
                unsigned int *set_size = new unsigned int[setCnt];

                ret_struct.set_sizes = set_size;
                ret_struct.sets = new unsigned int *[setCnt];
                ret_struct.weights = new unsigned short *[setCnt];
                ret_struct.max_weight = 0;
                ret_struct.all_element_count = 0;

                uint32_t i = 0;
                for (auto &kv : ref2eqset) {
                    auto setSize = kv.second.size();
                    ret_struct.set_sizes[i] = setSize;
                    if (ret_struct.max_weight < setSize) {
                        ret_struct.max_weight = setSize;
                    }
                    ret_struct.sets[i] = new unsigned int[setSize];
                    uint32_t element_counter = 0;
                    for (auto &eq : kv.second) {
                        ret_struct.sets[i][element_counter++] = eq2id[eq];
                        ret_struct.element_size_lookup[eq2id[eq]]++;
                        ret_struct.all_element_count++;
                    }
                    ret_struct.weights[i] = new unsigned short[setSize];
                    std::fill_n(ret_struct.weights[i], setSize, 1);
                    i++;
                }
                std::cerr << "# of refs (set cnt): " << ret_struct.set_count
                << " # of uniq eqs (uniq elem cnt):" << ret_struct.uniqu_element_count << "\n";
                std::cerr << "max set size: " << ret_struct.max_weight << "\n";
                /*for (uint32_t j = 0; j < elementCnt; j++) {
                    std::cerr << ret_struct.element_size_lookup[j] << " ";
                }
                std::cerr << "\n";*/
                set_cover setcover(ret_struct.set_count,
                                   ret_struct.uniqu_element_count,
                                   ret_struct.max_weight,
                                   ret_struct.all_element_count,
                                   ret_struct.element_size_lookup
                );
                for(int j = 0; j < ret_struct.set_count; j++){
                    setcover.add_set(j+1, ret_struct.set_sizes[j]
                            ,(const unsigned int*)ret_struct.sets[j],
                                     (const unsigned short*)ret_struct.weights[j],
                                     ret_struct.set_sizes[j]);
                    free(ret_struct.sets[j]);
                    free(ret_struct.weights[j]);

                }
                std::list < set * > ret = setcover.execute_set_cover();
                std::unordered_set <uint32_t> remainingRefs;
/*
                std::vector<bool> forValidation;
                forValidation.resize(ret_struct.uniqu_element_count);
                for (auto kk = 0; kk < ret_struct.uniqu_element_count; kk++)
                    forValidation[kk] = false;
*/
                for (auto iterator = ret.begin(); iterator != ret.end(); ++iterator) {
                    //std::cerr << "set id " << (*iterator)->set_id << " Element: ";
                    remainingRefs.insert((*iterator)->set_id);
                    /*set::element *element = (*iterator)->elements;
                    do {
                        forValidation[element->element_id-1] = true;
                    } while ((element = element->next) != NULL);*/
                    //std::cerr << std::endl;
                }
               /* // To validate:
                uint32_t missedEqCnt = 0;
                for (auto v : forValidation) {
                    if (!v) {
                        missedEqCnt++;
                    }
                }
                if (missedEqCnt > 0) {
                    std::cerr << "AAAAA missed validation: " << missedEqCnt <<
                    " out of " << forValidation.size() << "\n";
                    //std::exit(1);
                }*/
                uint32_t alreadySet{0}, canRemove{0}, reverted{0};
                i = 1;
                for (auto &kv : ref2eqset) { // the order is the same as the order of input sets to setCover
                    potentiallyRemoveStrain[kv.first] = false;
                    if (remainingRefs.find(i) == remainingRefs.end()) {
                        if (!strainValid[kv.first])
                            alreadySet++;
                        else
                            canRemove++;
                        strainValid[kv.first] = false;
                    } else {
                        if (!strainValid[kv.first]) {
                            reverted++;
                        }
                        strainValid[kv.first] = true;
                    }
                    i++;
                }

                std::cerr << "total suspicious: " << ref2eqset.size() <<
                " cannot remove: " << remainingRefs.size() <<
                ", can remove: " << canRemove <<
                ", has been removed: " << alreadySet <<
                ", has been reverted: " << reverted <<
                ", should be equal: " << alreadySet + canRemove + remainingRefs.size() <<
                " " << ref2eqset.size() << "\n";
                uint32_t totalvalid{0};
                for (auto s : strainValid) {
                    totalvalid += s;
                }
                std::cerr << "valid: " << totalvalid
                            << ", invalid: " << strainValid.size() - totalvalid << "\n";
/*
            std::vector<std::pair<uint32_t, std::unordered_set<uint64_t>>> refAndeqset;
            std::copy(ref2eqset.begin(), ref2eqset.end(),
                    std::back_inserter(refAndeqset));
            std::sort(refAndeqset.begin(), refAndeqset.end(),
                    [](const std::pair<uint32_t, std::unordered_set<uint64_t>>& v1,
                            const std::pair<uint32_t, std::unordered_set<uint64_t>>& v2) {
                return v1.second.size() > v2.second.size();
            });
            std::unordered_set<uint64_t> covered;
            for (auto &kv : refAndeqset) {
                for (auto &eq : kv.second) {
                    if (covered.find(eq) == covered.end()) {
                        strainValid[kv.first] = false;
                        covered.insert(eq);
                    }
                }
            }*/
                canHelp = removedImmediatelyCnt != reverted || canRemove;
            }

        }
        // M step
        // Find the best (most likely) count assignment
        for (auto& eqc : eqvec) {
            auto& tg = eqc.first;
            auto& v = eqc.second;
            auto csize = v.weights.size();
            std::vector<double> tmpReadProb(csize, 0.0);
            double denom{0.0};
            for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                auto& tgt = tg.tgts[readMappingCntr];
                if (strainValid[tgt]) {

                    tmpReadProb[readMappingCntr] =
                            v.weights[readMappingCntr] * strainCnt[tgt];// * strain_coverage[tgt];// * (1.0/refLengths[tgt]);
                    //std::cerr << tmpReadProb[readMappingCntr] << ",";
                    denom += tmpReadProb[readMappingCntr];
                    //std::cerr << denom << " ";
                }
            }
            /*if (denom == 0) {
                std::cerr << "denom is 0: " << tg.hash << " " << csize << " " << v.count <<  "\n";
                for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                    auto &tgt = tg.tgts[readMappingCntr];
                    if (strainValid[tgt]) {
                        std::cerr << " weird!\n";
                    }
                }
            }*/
            for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
                auto& tgt = tg.tgts[readMappingCntr];
                if (strainValid[tgt])
                    newStrainCnt[tgt] += v.count * (tmpReadProb[readMappingCntr] / denom);
                else {
                    newStrainCnt[tgt] = 0;
                }
            }
        }
        //std::cerr << "\n";
    
        // E step
        // normalize strain probabilities using the denum : p(s) = (count(s)/total_read_cnt) 
        double readCntValidator = 0;
        converged = true;   
        double maxDiff={0.0};
        for (size_t i = 0; i < strainCnt.size(); ++i) {
            readCntValidator += newStrainCnt[i];
            auto adiff = std::abs(newStrainCnt[i] - strainCnt[i]);
            if ( adiff > eps) {
                converged = false;
            }
            maxDiff = (adiff > maxDiff) ? adiff : maxDiff;
            strainCnt[i] = newStrainCnt[i];
            newStrainCnt[i] = 0.0;
        }

        /*if (std::abs(readCntValidator - readCnt) > 10) {
            //logger->error("Total read count changed during the EM process");
            logger->error("original: {}, current : {}, diff : {}", readCnt, 
                           readCntValidator, std::abs(readCntValidator - readCnt));
            //std::exit(1);
        }*/
        
        if (cntr > 0 and cntr % 100 == 0) {
            logger->info("max diff : {}", maxDiff);
        }
    }
    logger->info( "iterator cnt: {}", cntr);
    /*auto &eqvec1 = eqb.eqVec();
    std::vector<bool> eqRemained;
    eqRemained.resize(eqvec1.size());
    for (auto kk=0; kk<eqRemained.size(); kk++)
        eqRemained[kk] = false;
    uint32_t kk{0};
    for (auto & eqc: eqvec1) {
        auto &tg = eqc.first;
        auto &v = eqc.second;
        auto csize = v.weights.size();
        for (size_t readMappingCntr = 0; readMappingCntr < csize; ++readMappingCntr) {
            auto &tgt = tg.tgts[readMappingCntr];
            if (strainValid[tgt]) {
                eqRemained[kk] = true;
            }
        }
        kk++;
    }
    kk=0;
    for (auto tf: eqRemained) {
        if (!tf) {
            std::cerr << "so bad eq is removed: " << kk << "\n";
        }
        kk++;
    }*/
    // We have done the EM in the space of sequence / reference IDs
    // but we need to output results in terms of taxa IDs.  Here, we 
    // will map our reference IDs back to taxa IDs, and put the resulting
    // computed abundances in the "strain" member variable.
    decltype(strain) outputMap;
    outputMap.reserve(strain.size());
    for (auto& kv : strain) {
        std::cerr << kv.first << ":" << strainCnt[kv.first] << "," << kv.second << "\t";
        outputMap[seqToTaxMap[kv.first]] += strainValid[kv.first]? strainCnt[kv.first]: 0;
    }
    std::cerr << "\n";
    // Until here, strain map was actually holding refids as key, but after swap it'll be holding strain taxids
    std::swap(strain, outputMap);
    
    return cntr < maxIter;
}

template<class ReaderType>
void Cedar<ReaderType>::serialize(std::string& output_filename) {
    logger->info("Write results into the file: {}", output_filename);
    logger->info("# of strains: {}", strain.size());
    std::ofstream ofile(output_filename);
    ofile << "taxaId\ttaxaRank\tcount\n";
    spp::sparse_hash_map<uint64_t, double> validTaxa;
    std::cerr << "strain size: " << strain.size() << "\n";
    for (auto& kv : strain) {
        if (taxaNodeMap.find(kv.first) != taxaNodeMap.end()) {
            TaxaNode *walker = &taxaNodeMap[kv.first];
            //std::cerr << "s" << walker->getId() << " ";
            while (!walker->isRoot() && walker->getRank() != pruningLevel) {
                //std::cerr << "p" << walker->getParentId() << " ";
                walker = &taxaNodeMap[walker->getParentId()];
                //std::cerr << walker->getId() << " ";
                if (walker->getId() > 18000000000000) std::exit(1);
            }
            //std::cerr << "\n";
            if (!walker->isRoot()) {
                if (validTaxa.find(walker->getId()) == validTaxa.end()) {
                    validTaxa[walker->getId()] = kv.second;
                } else {
                    validTaxa[walker->getId()] += kv.second;
                }
            }
        } else {
            std::cerr << "taxa not found: " << kv.first << "\n";
        }
    }
    for (auto& kv : validTaxa) { 
        ofile << kv.first << "\t" 
              << TaxaNode::rank2str(taxaNodeMap[kv.first].getRank()) 
              << "\t" << kv.second << "\n";
    }
    ofile.close();

    std::ofstream covOfile(output_filename + ".coverage");
    for (auto& kv: cov) {
        covOfile << kv.first << "\t" << kv.second << "\n";
    }
    covOfile.close();
    
}

template<class ReaderType>
void Cedar<ReaderType>::serializeFlat(std::string& output_filename) {
    logger->info("[FlatAbund]");
    // validate final count:
    uint64_t finalMappedReadCnt = 0;
    for (auto &kv : strain) {
        finalMappedReadCnt += kv.second;
    }
    logger->info("Before writing results in the file, total # of mapped reads is {}", finalMappedReadCnt);
    logger->info("Write results in the file: {}", output_filename);
    std::ofstream ofile(output_filename);
    ofile << "taxaId\ttaxaRank\tcount\tcoverage\n";
    std::cerr << "NUMREFS: " << mappings.numRefs() << "\n";
    for (uint32_t i = 0; i < mappings.numRefs(); ++i) { 
        //for (auto& kv : strain) {
        auto it = strain.find(i);
        double abund = 0.0;
        if (it != strain.end()){
        abund = it->second;
        }
        ofile << mappings.refName(i) << "\t" 
            << "flat" 
            << "\t" << abund <<"\t" << strain_coverage[i] << "\n";
    }
    ofile.close();
}

template<class ReaderType>
void Cedar<ReaderType>::run(std::string mapperOutput_filename,
         bool requireConcordance,
         size_t maxIter,
         double eps,
         double minCnt,
         std::string& output_filename,
         bool onlyUniq,
         bool onlyPerf,
				 uint32_t segmentSize) {
    loadMappingInfo(mapperOutput_filename, requireConcordance, onlyUniq, onlyPerf, segmentSize);
    basicEM(maxIter, eps, minCnt);
    logger->info("serialize to ", output_filename);
    if (!flatAbund) {
        serialize(output_filename);
    } else {
        serializeFlat(output_filename);
    }
    //std::cout << "I guess that's it\n";
}

template class Cedar<PuffMappingReader>;
template class Cedar<SAMReader>;

/**
 * "How to run" example:
 * make Pufferfish!
 * In the Pufferfish build directory run the following command:
 * /usr/bin/time src/cedar 
 * -t /mnt/scratch2/avi/meta-map/kraken/KrakenDB/taxonomy/nodes.dmp  
 * -s /mnt/scratch2/avi/meta-map/kraken/KrakenDB/seqid2taxid.map 
 * -m /mnt/scratch2/avi/meta-map/kraken/puff/dmps/HC1.dmp 
 * -o HC1.out 
 * -l genus (optional)
 * -f 0.8 (optional)
 **/
int main(int argc, char* argv[]) {
  (void)argc;
  using namespace clipp;
  CedarOpts kopts;
  bool showHelp{false};

  auto checkLevel = [](const char* lin) -> void {
    std::string l(lin);
    std::unordered_set<std::string> valid{"species", "genus", "family", "order", "class", "phylum"};
    if (valid.find(l) == valid.end()) {
      std::string s = "The level " + l + " is not valid.";
      throw std::range_error(s);
    }
  };

  auto cli = (
            (required("--flat").set(kopts.flatAbund, true) % "estimate flat abundance (i.e. there is no taxonomy given)"
            | (
              required("--taxtree", "-t") & value("taxtree", kopts.taxonomyTree_filename) % "path to the taxonomy tree file",
              required("--seq2taxa", "-s") & value("seq2taxa", kopts.refId2TaxId_filename) % "path to the refId 2 taxId file "
            )),
              ( (required("--puffMapperOut", "-p").set(kopts.isPuffOut, true) & value("mapout", kopts.mapperOutput_filename) % "path to the pufferfish mapper output file")
              |
              (required("--sam").set(kopts.isSAM, true) & value("mapout", kopts.mapperOutput_filename) % "path to the SAM file")
              ),
              required("--output", "-o") & value("output", kopts.output_filename) % "path to the output file to write results",
              option("--maxIter", "-i") & value("iter", kopts.maxIter) % "maximum number of EM iteratons (default : 1000)",
              option("--eps", "-e") & value("eps", kopts.eps) % "epsilon for EM convergence (default : 0.001)",
              option("--minCnt", "-c") & value("minCnt", kopts.minCnt) % "minimum count for keeping a reference with count greater than that (default : 0)",
              option("--level", "-l") & value("level", kopts.level).call(checkLevel) % "choose between (species, genus, family, order, class, phylum). (default : species)",
              option("--filter", "-f") & value("filter", kopts.filterThreshold) % "choose the threshold [0,1] below which to filter out mappings (default : no filter)",
              option("--noDiscordant").set(kopts.requireConcordance, true) % "ignore orphans for paired end reads",
              option("--unique").set(kopts.onlyUniq, true) % "report abundance based on unique reads",
              option("--perfect").set(kopts.onlyPerfect, true) % "report abundance based on perfect reads (unique and with complete coverage)",
              option("--help", "-h").set(showHelp, true) % "show help",
              option("-v", "--version").call([]{std::cout << "version 0.1.0\n\n";}).doc("show version")
              );
  //Multithreaded console logger(with color support)
  auto console = spdlog::stderr_color_mt("console");

  decltype(parse(argc, argv, cli)) res;
  try {
    res = parse(argc, argv, cli);
    if (showHelp){
      std::cout << make_man_page(cli, "cedar"); 
      return 0;
    }
  } catch (std::exception& e) {
    std::cout << "\n\nparsing command line failed with exception: " << e.what() << "\n";
    std::cout << "\n\n";
    std::cout << make_man_page(cli, "cedar");
    return 1;
  }

  if(res) {
    if (kopts.isSAM) {
        Cedar<SAMReader> cedar(kopts.taxonomyTree_filename, kopts.refId2TaxId_filename, kopts.level, kopts.filterThreshold, kopts.flatAbund, console);
        cedar.run(kopts.mapperOutput_filename, 
                  kopts.requireConcordance,
                  kopts.maxIter, 
                  kopts.eps,
                  kopts.minCnt,
                  kopts.output_filename,
                  kopts.onlyUniq,
                  kopts.onlyPerfect,
									kopts.segmentSize);
    }
    else {
        Cedar<PuffMappingReader> cedar(kopts.taxonomyTree_filename, kopts.refId2TaxId_filename, kopts.level, kopts.filterThreshold, kopts.flatAbund, console);
        cedar.run(kopts.mapperOutput_filename, 
            kopts.requireConcordance,
            kopts.maxIter, 
            kopts.eps,
            kopts.minCnt,
            kopts.output_filename,
            kopts.onlyUniq,
            kopts.onlyPerfect,
						kopts.segmentSize);
    }
    return 0;
  } else {
    std::cout << usage_lines(cli, "cedar") << '\n';
    return 1;
  }
}

