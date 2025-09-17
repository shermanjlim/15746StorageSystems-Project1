#include "myFTL.h"

#include <list>
#include <unordered_map>

#include "common.h"

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
        block_size(conf->GetBlockSize()),
        free_log_blocks(),
        data_logblock_map(),
        pages_valid(),
        logblock_lbas_map() {
    /* Maximum number a block_ can be erased */
    size_t block_erase_count = conf->GetBlockEraseCount();
    /* Overprovioned blocks as a percentage of total number of blocks */
    size_t op = conf->GetOverprovisioning();

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
    // allocate just one cleaning block for now...
    cleanblock_idx = free_log_blocks.front();
    free_log_blocks.pop_front();

    size_t num_pages = num_blocks * block_size;
    pages_valid.resize(num_pages, false);
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
    (void)func;

    if (!IsValidLba(lba)) {
      return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
    }

    Address datapage_addr = CalcPhyAddr(lba);
    size_t datapage_idx = GetPageIdx(datapage_addr);

    if (!pages_valid[datapage_idx]) {
      // page is empty
      pages_valid[datapage_idx] = true;
      return std::make_pair(ExecState::SUCCESS, datapage_addr);
    }

    // page is not empty
    size_t datablock_idx = GetBlockIdx(datapage_addr);
    if (data_logblock_map.count(datablock_idx) == 0) {
      if (free_log_blocks.empty()) {
        return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
      }
      // allocate logblock
      data_logblock_map[datablock_idx] = free_log_blocks.front();
      free_log_blocks.pop_front();
    }
    size_t logblock_idx = data_logblock_map[datablock_idx];

    if (logblock_lbas_map[logblock_idx].size() == block_size) {
      // logblock is full
      return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
    }

    logblock_lbas_map[logblock_idx].push_back(lba);
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
    func(OpCode::ERASE, GetAddrFromBlockIdx(datablock_idx));
    func(OpCode::ERASE, GetAddrFromBlockIdx(logblock_idx));
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
    func(OpCode::ERASE, GetAddrFromBlockIdx(cleanblock_idx));

    return true;
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
  /* Number of pages in a block_ */
  size_t block_size;

  // gives the largest valid lba
  size_t largest_lba;

  // list of free block indexes
  std::list<size_t> free_log_blocks;
  // mapping of data blocks to log reservation blocks
  std::unordered_map<size_t, size_t> data_logblock_map;
  // bitset of whether pages are valid
  std::vector<bool> pages_valid;
  // mapping of log reservation blocks to LBAs written
  std::unordered_map<size_t, std::vector<size_t>> logblock_lbas_map;
  size_t cleanblock_idx;
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
