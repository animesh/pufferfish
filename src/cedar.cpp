#include <fstream>
#include <iostream>
#include <string>
#include <algorithm> // std::sort
#include "CLI/CLI.hpp"
#include "cedar.hpp"

#define LEFT true
#define RIGHT true

struct CedarOpts {
    std::string taxonomyTree_filename;
    std::string refId2TaxId_filename;
    std::string mapperOutput_filename;
    std::string output_filename;
    std::string level = "species";
    double filterThreshold = 0;
};

Cedar::Cedar(std::string& taxonomyTree_filename, 
                 std::string& refId2TaxId_filename, 
                 std::string pruneLevelIn,
                 double filteringThresholdIn) {

    std::cerr << "KrakMap: Construct ..\n";
    // map rank string values to enum values
    filteringThreshold = filteringThresholdIn;
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
    std::string tmp;
    while (!tfile.eof()) {
        tfile >> id >> tmp >> pid >> tmp >> rank >> tmp;
        size_t i = 0;
        while (i < tmp.size()-1 && isspace(tmp[i]))
            i++;
        if (tmp != "|") {
            rank += " " + tmp;
        }
        taxaNodeMap[id] = TaxaNode(id, pid, TaxaNode::str2rank(rank));
        if (taxaNodeMap[id].isRoot()) {
            rootId = id;
            std::cerr << "Root Id : " << id << "\n";
        }
        std::getline(tfile, tmp);
        
    }

    tfile.close();  
}

bool Cedar::readHeader(std::ifstream& mfile) {
    std::string tmp, readType;
    mfile >> tmp >> readType;
    if (tmp != "#")
        return false;
    if (readType == "LT:S") 
        isPaired = false;
    else if (readType == "LT:P")
        isPaired = true;
    else
        return false;
    std::getline(mfile, tmp);
    return true;
}

void Cedar::loadMappingInfo(std::string mapperOutput_filename) {
    std::string rid, tname, tmp;// read id, taxa name, temp
    uint64_t lcnt, rcnt, tid, puff_tid, tlen, ibeg, ilen;
    std::cerr << "Cedar: Load Mapping File ..\n";
    std::cerr << "\tMapping Output File: " << mapperOutput_filename << "\n";
    std::ifstream mfile(mapperOutput_filename);
    uint64_t rlen, mcnt; // taxa id, read mapping count, # of interals, interval start, interval length
    uint64_t totalReadCnt = 0, totalUnmappedReads = 0, seqNotFound = 0;
    if (!readHeader(mfile)) {
        std::cerr << "ERROR: Invalid header for mapping output file.\n";
        std::exit(1);
    }
    std::cout<< "is dataset paired end? " << isPaired << "\n";
    while (!mfile.eof()) {
        mfile >> rid >> mcnt;
        totalReadCnt++;
        activeTaxa.clear();
        float readMappingsScoreSum = 0;
        std::vector<std::pair<uint64_t, float>> readPerStrainProbInst;
        readPerStrainProbInst.reserve(10);
        //std::cout << "r" << rid << " " << mcnt << "\n";
        if (mcnt != 0) {
            if (isPaired) {
                uint64_t rllen, rrlen;
                mfile >> rllen >> rrlen;
                rlen = rllen + rrlen;
            }
            else {
                mfile >> rlen;
            }
            std::set<uint64_t> seen;
            for (size_t mappingCntr = 0; mappingCntr < mcnt; mappingCntr++) {
                mfile >> puff_tid >> tname >> tlen; //txp_id, txp_name, txp_len
                // first condition: Ignore those references that we don't have a taxaId for
                // secon condition: Ignore repeated exactly identical mappings (FIXME thing)
                if (refId2taxId.find(tname) != refId2taxId.end() &&
                    activeTaxa.find(refId2taxId[tname]) == activeTaxa.end()) { 
                    tid = refId2taxId[tname];
                    activeTaxa.insert(tid);
                    
                    // fetch the taxon from the map
                    TaxaNode taxaPtr;
                    mfile >> lcnt;
                    if (isPaired)
                        mfile >> rcnt;
                    for (size_t i = 0; i < lcnt; ++i) {
                        mfile >> ibeg >> ilen;
                        taxaPtr.addInterval(ibeg, ilen, LEFT);
                    }
                    if (isPaired)
                        for (size_t i = 0; i < rcnt; ++i) {
                            mfile >> ibeg >> ilen;
                            taxaPtr.addInterval(ibeg, ilen, RIGHT);
                        }
                    taxaPtr.cleanIntervals(LEFT);
                    taxaPtr.cleanIntervals(RIGHT);
                    taxaPtr.updateScore();
                    readPerStrainProbInst.emplace_back(tid, taxaPtr.getScore());
                    readMappingsScoreSum += taxaPtr.getScore();
                }
                else { // otherwise we have to read till the end of the line and throw it away
                    std::getline(mfile, tmp);
                }
            } 
            if (activeTaxa.size() == 0) {
                seqNotFound++;
            }
            else {
                readPerStrainProb.push_back(readPerStrainProbInst);
                for (auto it = readPerStrainProbInst.begin(); it != readPerStrainProbInst.end(); it++) {
                    // strain[it->first].first : read count for strainCnt
                    // strain[it->first].second : strain length
                    if (strain.find(it->first) == strain.end()) {
                        strain[it->first] = std::make_pair(it->second/readMappingsScoreSum, tlen);
                    }
                    else {
                        strain[it->first].first += it->second/readMappingsScoreSum;
                    }
                }
            }
        } else {
            totalUnmappedReads++;
            std::getline(mfile, tmp);
        }
    }  
}

bool Cedar::basicEM() {
    
    return true;
}
/**
 * "How to run" example:
 * make Pufferfish!
 * In the Pufferfish build directory run the following command:
 * /usr/bin/time src/krakmap 
 * -t /mnt/scratch2/avi/meta-map/kraken/KrakenDB/taxonomy/nodes.dmp  
 * -s /mnt/scratch2/avi/meta-map/kraken/KrakenDB/seqid2taxid.map 
 * -m /mnt/scratch2/avi/meta-map/kraken/puff/dmps/HC1.dmp 
 * -o HC1.out 
 * -l genus (optional)
 * -f 0.8 (optional)
 **/
int main(int argc, char* argv[]) {
  (void)argc;

  CedarOpts kopts;
  CLI::App app{"krakMap : Taxonomy identification based on the output of Pufferfish mapper through the same process as Kraken."};
  app.add_option("-t,--taxtree", kopts.taxonomyTree_filename,
                 "path to the taxonomy tree file")
      ->required();
  app.add_option("-s,--seq2taxa", kopts.refId2TaxId_filename, "path to the refId 2 taxaId file")
      ->required();
  app.add_option("-m,--mapperout", kopts.mapperOutput_filename, "path to the pufferfish mapper output file")
      ->required();
  app.add_option("-o,--output", kopts.output_filename, "path to the output file to write results")
      ->required();
  app.add_option("-l,--level", kopts.level, "choose between (species, genus, family, order, class, phylum). Default:species")
      ->required(false);
  app.add_option("-f,--filter", kopts.filterThreshold, "choose the threshold (0-1) to filter out mappings with a score below that. Default: no filter")
      ->required(false);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }
  Cedar cedar(kopts.taxonomyTree_filename, kopts.refId2TaxId_filename, kopts.level, kopts.filterThreshold);
  cedar.loadMappingInfo(kopts.mapperOutput_filename);
  cedar.basicEM();
  return 0;
}
