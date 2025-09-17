#include "myFTL.h"

#include <limits>
#include <list>
#include <memory>
#include <unordered_map>

#include "common.h"

/* Number of pages in a block_ */
size_t block_size;

// Abstract class for a garbage collector policy
class GCPolicy {
 public:
  // returns the corresponding datablock_idx of the logblock to clean
  virtual size_t SelectBlockToClean() = 0;
  // handler is called when a logblock has been allocated to a datablock
  virtual void LogBlockAllocatedHandler(size_t datablock_idx,
                                        size_t livepages) = 0;
  // handler is called when a datablock has been written to
  virtual void DataBlockWrittenHandler(size_t datablock_idx,
                                       bool is_newly_live) {
    (void)datablock_idx;
    (void)is_newly_live;
  }
};

class RoundRobinPolicy : public GCPolicy {
 public:
  RoundRobinPolicy() : blocks_queue() {}

  size_t SelectBlockToClean() {
    size_t datablock_idx = blocks_queue.front();
    blocks_queue.pop_front();
    return datablock_idx;
  }

  void LogBlockAllocatedHandler(size_t datablock_idx, size_t) {
    blocks_queue.push_back(datablock_idx);
  }

 private:
  std::list<size_t> blocks_queue;
};

class LRUPolicy : public GCPolicy {
 public:
  LRUPolicy() : blocks_lru(), block_node_map() {}

  size_t SelectBlockToClean() {
    size_t datablock_idx = blocks_lru.front();
    block_node_map.erase(datablock_idx);
    blocks_lru.pop_front();
    return datablock_idx;
  }

  void LogBlockAllocatedHandler(size_t datablock_idx, size_t) {
    block_node_map.insert(std::make_pair(
        datablock_idx, blocks_lru.insert(blocks_lru.end(), datablock_idx)));
  }

  void DataBlockWrittenHandler(size_t datablock_idx, bool) {
    if (block_node_map.count(datablock_idx) == 0) {
      return;
    }
    blocks_lru.erase(block_node_map[datablock_idx]);
    block_node_map[datablock_idx] =
        blocks_lru.insert(blocks_lru.end(), datablock_idx);
  }

 private:
  std::list<size_t> blocks_lru;
  std::unordered_map<size_t, std::list<size_t>::iterator> block_node_map;
};

class GreedyPolicy : public GCPolicy {
 public:
  GreedyPolicy() : block_livepages_map() {}

  size_t SelectBlockToClean() {
    size_t block_min_livepages = std::numeric_limits<size_t>::max();
    size_t min_livepages = std::numeric_limits<size_t>::max();
    for (const auto &it : block_livepages_map) {
      if (it.second < min_livepages) {
        block_min_livepages = it.first;
        min_livepages = it.second;
      }
    }
    block_livepages_map.erase(block_min_livepages);
    return block_min_livepages;
  }

  void LogBlockAllocatedHandler(size_t datablock_idx, size_t livepages) {
    block_livepages_map[datablock_idx] = livepages;
  }

  void DataBlockWrittenHandler(size_t datablock_idx, bool is_newly_live) {
    if (block_livepages_map.count(datablock_idx) == 0) {
      return;
    }
    if (is_newly_live) {
      ++block_livepages_map[datablock_idx];
    }
  }

 private:
  std::unordered_map<size_t, size_t> block_livepages_map;
};

class CostBenefitPolicy : public GCPolicy {
 public:
  CostBenefitPolicy() : curr_ts(0), block_ts_map(), block_livepages_map() {}

  size_t SelectBlockToClean() {
    size_t block_maxratio = std::numeric_limits<size_t>::max();
    double maxratio = std::numeric_limits<double>::min();
    for (const auto &it : block_livepages_map) {
      double ratio = CalcRatio(it.first);
      if (ratio > maxratio) {
        block_maxratio = it.first;
        maxratio = ratio;
      }
    }
    block_ts_map.erase(block_maxratio);
    block_livepages_map.erase(block_maxratio);
    return block_maxratio;
  }

  void LogBlockAllocatedHandler(size_t datablock_idx, size_t livepages) {
    block_ts_map[datablock_idx] = 0;
    block_livepages_map[datablock_idx] = livepages;
  }

  void DataBlockWrittenHandler(size_t datablock_idx, bool is_newly_live) {
    ++curr_ts;
    if (block_livepages_map.count(datablock_idx) == 0) {
      return;
    }
    block_ts_map[datablock_idx] = curr_ts;
    if (is_newly_live) {
      ++block_livepages_map[datablock_idx];
    }
  }

 private:
  double CalcRatio(size_t block_idx) {
    double age = curr_ts - block_ts_map[block_idx];
    double livepages = block_livepages_map[block_idx];
    double utilization = livepages / (2 * block_size);
    return (1 - utilization) / (1 + utilization) * age;
  }

  size_t curr_ts;
  std::unordered_map<size_t, size_t> block_ts_map;
  std::unordered_map<size_t, size_t> block_livepages_map;
};

std::unique_ptr<GCPolicy> SelectGCPolicy(size_t policy_idx) {
  switch (policy_idx) {
    case 0:
      return std::unique_ptr<RoundRobinPolicy>(new RoundRobinPolicy());
    case 1:
      return std::unique_ptr<LRUPolicy>(new LRUPolicy());
    case 2:
      return std::unique_ptr<GreedyPolicy>(new GreedyPolicy());
    case 3:
      return std::unique_ptr<CostBenefitPolicy>(new CostBenefitPolicy());
    default:
      throw std::runtime_error{"invalid GC policy_idx"};
  }
}

template <typename PageType>
class MyFTL : public FTLBase<PageType> {
 public:
  /*
   * Constructor
   */
  MyFTL(const ConfBase *conf)
      : ssd_size(conf->GetSSDSize()),
        package_size(conf->GetPackageSize()),
        die_size(conf->GetDieSize()),
        plane_size(conf->GetPlaneSize()),
        block_erase_count(conf->GetBlockEraseCount()),
        free_log_blocks(),
        data_logblock_map(),
        pages_valid(),
        erase_counts(),
        logblock_lbas_map(),
        cleanblock_idxs(),
        curr_cleanblock_idx(0),
        gc_policy(SelectGCPolicy(conf->GetGCPolicy())) {
    /* Overprovioned blocks as a percentage of total number of blocks */
    size_t op = conf->GetOverprovisioning();

    block_size = conf->GetBlockSize();

    printf("SSD Configuration: %zu, %zu, %zu, %zu, %zu\n", ssd_size,
           package_size, die_size, plane_size, block_size);
    printf("Max Erase Count: %zu, Overprovisioning: %zu%%\n", block_erase_count,
           op);

    size_t num_blocks = ssd_size * package_size * die_size * plane_size;
    size_t num_op_blocks =
        (num_blocks * op + 100 / 2) / 100;  // rounds to nearest integer

    largest_lba = (num_blocks - num_op_blocks) * block_size - 1;

    // place all overprovisioned blocks at the end of the SSD, for now...
    for (size_t block_idx = num_blocks - num_op_blocks; block_idx < num_blocks;
         ++block_idx) {
      free_log_blocks.push_back(block_idx);
    }
    // allocate half of over-provisioned blocks as cleaning blocks
    for (size_t i = 0; i < num_op_blocks / 2; ++i) {
      cleanblock_idxs.push_back(free_log_blocks.front());
      free_log_blocks.pop_front();
    }

    size_t num_pages = num_blocks * block_size;
    pages_valid.resize(num_pages, false);

    erase_counts.resize(num_blocks, 0);
  }

  /*
   * Destructor - Plase keep it as virtual to allow destroying the
   *              object with base type_ pointer
   */
  virtual ~MyFTL() {}

  /*
   * ReadTranslate() - Translates read address
   *
   * This function translates a physical LBA into an Address object that will
   * be used as the target address of the read operation.
   *
   * If you need to issue extra operations, please use argument func to
   * interact with class Controller
   */
  std::pair<ExecState, Address> ReadTranslate(
      size_t lba, const ExecCallBack<PageType> &func) {
    (void)func;

    if (!IsValidLba(lba)) {
      return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
    }

    Address datapage_addr = CalcPhyAddr(lba);
    size_t datapage_idx = GetPageIdx(datapage_addr);

    if (!pages_valid[datapage_idx]) {
      return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
    }

    size_t datablock_idx = GetBlockIdx(datapage_addr);
    if (data_logblock_map.count(datablock_idx) == 0) {
      return std::make_pair(ExecState::SUCCESS, datapage_addr);
    }

    size_t logblock_idx = data_logblock_map[datablock_idx];
    for (int i = logblock_lbas_map[logblock_idx].size() - 1; i >= 0; --i) {
      if (logblock_lbas_map[logblock_idx][i] == lba) {
        return std::make_pair(ExecState::SUCCESS,
                              GetAddrFromBlockPageIdx(logblock_idx, i));
      }
    }

    return std::make_pair(ExecState::SUCCESS, datapage_addr);
  }

  /*
   * WriteTranslate() - Translates write address
   *
   * Please refer to ReadTranslate()
   */
  std::pair<ExecState, Address> WriteTranslate(
      size_t lba, const ExecCallBack<PageType> &func) {
    if (!IsValidLba(lba)) {
      return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
    }

    Address datapage_addr = CalcPhyAddr(lba);
    size_t datapage_idx = GetPageIdx(datapage_addr);
    size_t datablock_idx = GetBlockIdx(datapage_addr);

    if (!pages_valid[datapage_idx]) {
      // page is empty
      pages_valid[datapage_idx] = true;
      gc_policy->DataBlockWrittenHandler(datablock_idx, true);
      return std::make_pair(ExecState::SUCCESS, datapage_addr);
    }

    // page is not empty
    if (data_logblock_map.count(datablock_idx) == 0) {
      if (free_log_blocks.empty()) {
        if (Clean(gc_policy->SelectBlockToClean(), func)) {
          return WriteTranslate(lba, func);
        } else {
          return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
        }
      }
      // allocate logblock
      data_logblock_map[datablock_idx] = free_log_blocks.front();
      free_log_blocks.pop_front();
      gc_policy->LogBlockAllocatedHandler(datablock_idx,
                                          ComputeLivePages(datablock_idx));
    }
    size_t logblock_idx = data_logblock_map[datablock_idx];

    if (logblock_lbas_map[logblock_idx].size() == block_size) {
      if (Clean(datablock_idx, func)) {
        return WriteTranslate(lba, func);
      } else {
        return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
      }
    }

    logblock_lbas_map[logblock_idx].push_back(lba);
    gc_policy->DataBlockWrittenHandler(datablock_idx, false);
    return std::make_pair(
        ExecState::SUCCESS,
        GetAddrFromBlockPageIdx(logblock_idx,
                                logblock_lbas_map[logblock_idx].size() - 1));
  }

  /*
   * Optionally mark a LBA as a garbage.
   */
  ExecState Trim(size_t lba, const ExecCallBack<PageType> &func) {
    (void)lba;
    (void)func;
    return ExecState::SUCCESS;
  }

 private:
  bool Clean(size_t datablock_idx, const ExecCallBack<PageType> &func) {
    size_t logblock_idx = data_logblock_map[datablock_idx];
    size_t cleanblock_idx = cleanblock_idxs[curr_cleanblock_idx];
    curr_cleanblock_idx = (curr_cleanblock_idx + 1) % cleanblock_idxs.size();

    if (erase_counts[datablock_idx] >= block_erase_count ||
        erase_counts[logblock_idx] >= block_erase_count ||
        erase_counts[cleanblock_idx] >= block_erase_count) {
      return false;
    }

    std::vector<bool> live_copied(block_size, false);

    // start copying live pages from log block
    auto &logblock_lbas = logblock_lbas_map[logblock_idx];
    for (int i = logblock_lbas.size() - 1; i >= 0; --i) {
      size_t lba = logblock_lbas[i];
      Address datapage_addr = CalcPhyAddr(lba);
      if (live_copied[datapage_addr.page]) {
        continue;
      }
      // this is a live page, copy it over to the cleaning block
      func(OpCode::READ, GetAddrFromBlockPageIdx(logblock_idx, i));
      func(OpCode::WRITE,
           GetAddrFromBlockPageIdx(cleanblock_idx, datapage_addr.page));
      live_copied[datapage_addr.page] = true;
    }

    // copy remaining live pages from data block
    for (size_t i = 0; i < block_size; ++i) {
      if (live_copied[i]) {
        continue;
      }
      if (!pages_valid[i]) {
        continue;
      }
      // this is a live page, copy it over to the cleaning block
      func(OpCode::READ, GetAddrFromBlockPageIdx(datablock_idx, i));
      func(OpCode::WRITE, GetAddrFromBlockPageIdx(cleanblock_idx, i));
      live_copied[i] = true;
    }

    // erase data and log block and reset data structures
    Erase(datablock_idx, func);
    Erase(logblock_idx, func);
    free_log_blocks.push_back(logblock_idx);
    data_logblock_map.erase(datablock_idx);
    logblock_lbas.clear();

    // copy from clean block to data block
    for (size_t i = 0; i < block_size; ++i) {
      if (!live_copied[i]) {
        continue;
      }
      func(OpCode::READ, GetAddrFromBlockPageIdx(cleanblock_idx, i));
      func(OpCode::WRITE, GetAddrFromBlockPageIdx(datablock_idx, i));
    }
    Erase(cleanblock_idx, func);

    return true;
  }

  void Erase(size_t block_idx, const ExecCallBack<PageType> &func) {
    func(OpCode::ERASE, GetAddrFromBlockIdx(block_idx));
    ++erase_counts[block_idx];
  }

  size_t ComputeLivePages(size_t datablock_idx) {
    size_t livepages = 0;
    Address addr = GetAddrFromBlockIdx(datablock_idx);

    for (size_t i = 0; i < block_size; ++i) {
      addr.page = i;
      size_t datapage_idx = GetPageIdx(addr);
      if (pages_valid[datapage_idx]) {
        ++livepages;
      }
    }

    return livepages;
  }

  bool IsValidLba(size_t lba) { return lba <= largest_lba; }

  Address CalcPhyAddr(size_t lba) {
    return Address(lba / (package_size * die_size * plane_size * block_size),
                   (lba / (die_size * plane_size * block_size)) % package_size,
                   (lba / (plane_size * block_size)) % die_size,
                   (lba / (block_size)) % plane_size, lba % block_size);
  }

  size_t GetBlockIdx(const Address &block_addr) {
    return block_addr.package * package_size * die_size * plane_size +
           block_addr.die * die_size * plane_size +
           block_addr.plane * plane_size + block_addr.block;
  }
  Address GetAddrFromBlockIdx(size_t block_idx) {
    return Address(block_idx / (package_size * die_size * plane_size),
                   (block_idx / (die_size * plane_size)) % package_size,
                   (block_idx / plane_size) % die_size, block_idx % plane_size,
                   0);
  }
  Address GetAddrFromBlockPageIdx(size_t block_idx, size_t page_idx) {
    Address addr = GetAddrFromBlockIdx(block_idx);
    addr.page = page_idx;
    return addr;
  }

  size_t GetPageIdx(const Address &addr) {
    return addr.package * package_size * die_size * plane_size * block_size +
           addr.die * die_size * plane_size * block_size +
           addr.plane * plane_size * block_size + addr.block * block_size +
           addr.page;
  }

  /* Number of packages in a ssd */
  size_t ssd_size;
  /* Number of dies in a package_ */
  size_t package_size;
  /* Number of planes in a die_ */
  size_t die_size;
  /* Number of blocks in a plane_ */
  size_t plane_size;
  /* Maximum number a block_ can be erased */
  size_t block_erase_count;

  // gives the largest valid lba
  size_t largest_lba;

  // list of free block indexes
  std::list<size_t> free_log_blocks;
  // mapping of data blocks to log reservation blocks
  std::unordered_map<size_t, size_t> data_logblock_map;
  // bitset of whether pages are valid
  std::vector<bool> pages_valid;
  // the erase_count of each block
  std::vector<size_t> erase_counts;
  // mapping of log reservation blocks to LBAs written
  std::unordered_map<size_t, std::vector<size_t>> logblock_lbas_map;

  std::vector<size_t> cleanblock_idxs;
  size_t curr_cleanblock_idx;

  std::unique_ptr<GCPolicy> gc_policy;
};

/*
 * CreateMyFTL() - Creates class MyFTL object
 *
 * You do not need to modify this
 */
FTLBase<TEST_PAGE_TYPE> *CreateMyFTL(const ConfBase *conf) {
  MyFTL<TEST_PAGE_TYPE> *ftl = new MyFTL<TEST_PAGE_TYPE>(conf);
  return static_cast<FTLBase<TEST_PAGE_TYPE> *>(ftl);
}
