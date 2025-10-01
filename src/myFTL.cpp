#include "myFTL.h"

#include <limits>
#include <list>

#include "common.h"

namespace {

/*
Given the following maximum values of the SSD parameters, we can define the
following data types to optimize for memory usage.
- Maximum number of packages in the SSD (SSD SIZE) is 4
- Maximum number of dies per package (PACKAGE SIZE) is 8
- Maximum number of planes per die (DIE SIZE) is 2
- Maximum number of blocks per plane (PLANE SIZE) is 10
- Maximum number of pages per block (BLOCK SIZE) is 64
- Maximum number of erases per block (BLOCK ERASES) is 64
*/
using pg_size_t = uint16_t;
using blk_size_t = uint16_t;
using erase_size_t = uint8_t;
using pgcnt_size_t = uint8_t;

constexpr pg_size_t INVALID_PAGE = std::numeric_limits<pg_size_t>::max();

}  // namespace

template <typename PageType>
class MyFTL : public FTLBase<PageType> {
 public:
  /*
   * Constructor
   */
  MyFTL(const ConfBase *conf)
      : ssd_size_(conf->GetSSDSize()),
        package_size_(conf->GetPackageSize()),
        die_size_(conf->GetDieSize()),
        plane_size_(conf->GetPlaneSize()),
        block_size_(conf->GetBlockSize()),
        block_erase_count_(conf->GetBlockEraseCount()),
        largest_lba_(0),
        lba_page_map_(),
        page_lba_map_(),
        block_erase_map_(),
        free_log_blocks_(),
        used_log_blocks_(),
        log_block_(0),
        log_page_offset_(0) {
    /* Overprovioned blocks as a percentage of total number of blocks */
    size_t op = conf->GetOverprovisioning();

    printf("SSD Configuration: %zu, %zu, %zu, %zu, %zu\n", ssd_size_,
           package_size_, die_size_, plane_size_, block_size_);
    printf("Max Erase Count: %zu, Overprovisioning: %zu%%\n",
           block_erase_count_, op);

    size_t num_blocks = ssd_size_ * package_size_ * die_size_ * plane_size_;
    size_t num_op_blocks =
        (num_blocks * op + 100 / 2) / 100;  // rounds to nearest integer
    size_t num_pages = num_blocks * block_size_;

    // initialize data structures and variables based on config
    largest_lba_ = (num_blocks - num_op_blocks) * block_size_ - 1;
    lba_page_map_.assign(largest_lba_ + 1, INVALID_PAGE);
    page_lba_map_.assign(num_pages, INVALID_PAGE);
    block_erase_map_.assign(num_blocks, 0);

    for (blk_size_t i = 0; i < num_blocks; ++i) {
      free_log_blocks_.push_back(i);
    }
    used_log_blocks_.clear();

    block_livepages_map_.assign(num_blocks, 0);

    log_block_ = free_log_blocks_.front();
    free_log_blocks_.pop_front();
    log_page_offset_ = 0;
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

    pg_size_t page_idx = lba_page_map_[lba];
    if (page_idx == INVALID_PAGE) {
      return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
    }

    return std::make_pair(ExecState::SUCCESS, GetAddrFromPageIdx(page_idx));
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

    if (log_page_offset_ >= block_size_) {
      // current log block is full
      if (free_log_blocks_.empty()) {
        return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
      }

      // allocate new log block
      used_log_blocks_.push_back(log_block_);
      log_block_ = free_log_blocks_.front();
      free_log_blocks_.pop_front();
      log_page_offset_ = 0;

      // do some cleaning if we are running out of free blocks
      if (free_log_blocks_.size() < GC_THRESHOLD) {
        Clean(func);
      }
      if (log_page_offset_ >= block_size_) {
        return std::make_pair(ExecState::FAILURE, Address(0, 0, 0, 0, 0));
      }
    }

    pg_size_t page_idx = LogLba(lba);
    return std::make_pair(ExecState::SUCCESS, GetAddrFromPageIdx(page_idx));
  }

  /*
   * Optionally mark a LBA as a garbage.
   */
  ExecState Trim(size_t lba, const ExecCallBack<PageType> &func) {
    (void)func;

    if (!IsValidLba(lba)) {
      return ExecState::FAILURE;
    }

    pg_size_t page_idx = lba_page_map_[lba];
    if (page_idx == INVALID_PAGE) {
      return ExecState::SUCCESS;
    }

    UpdatePageLba(page_idx, INVALID_PAGE);
    lba_page_map_[lba] = INVALID_PAGE;

    return ExecState::SUCCESS;
  }

 private:
  // if the number of free log blocks fall below this level, we'll do
  // some GC
  static constexpr size_t GC_THRESHOLD = 1;

  void Clean(const ExecCallBack<PageType> &func) {
    blk_size_t blk = SelectBlockToClean();

    if (block_erase_map_[blk] >= block_erase_count_) {
      // erase limit reached
      return;
    }

    // migrate live pages
    // invariant: there's enough slots in log page to hold all the live pages
    for (pg_size_t page = blk * block_size_; page < ((blk + 1) * block_size_);
         ++page) {
      pg_size_t lba = page_lba_map_[page];
      if (lba == INVALID_PAGE) {
        continue;
      }

      // this is a live page
      func(OpCode::READ, GetAddrFromPageIdx(page));
      pg_size_t new_page = LogLba(lba);
      func(OpCode::WRITE, GetAddrFromPageIdx(new_page));
    }

    func(OpCode::ERASE, GetAddrFromBlockIdx(blk));
    ++block_erase_map_[blk];

    used_log_blocks_.remove(blk);
    free_log_blocks_.push_back(blk);
  }

  blk_size_t SelectBlockToClean() {
    // select block with min score to GC
    blk_size_t blk_min_score = std::numeric_limits<blk_size_t>::max();
    size_t min_score = std::numeric_limits<size_t>::max();
    for (const auto &blk : used_log_blocks_) {
      size_t score = CalcBlockScore(blk);
      if (score < min_score) {
        blk_min_score = blk;
        min_score = score;
      }
    }
    return blk_min_score;
  }

  // We assign each block some score that'll determine whether it should be the
  // GC candidate. Lower score == more likely to be GC candidate.
  size_t CalcBlockScore(blk_size_t blk) {
    // increment score for each live page the block has
    // this makes blocks with more live pages less desirable and controls the
    // write amplification
    size_t score = block_livepages_map_[blk];

    // increment score as a block gets closer to dying to ensure write leveling
    size_t erases_left = block_erase_count_ - block_erase_map_[blk];
    if (erases_left < (block_erase_count_ / 2)) {
      score += block_size_;
    }
    if (erases_left < (block_erase_count_ / 4)) {
      score += block_size_;
    }
    if (erases_left == 1) {
      score += (block_size_ * 10);  // last breath
    }
    if (erases_left == 0) {
      score += (block_size_ * 10);  // dead
    }

    return score;
  }

  // helper function to write an LBA to the current log, and returns page index
  // of the page written to
  pg_size_t LogLba(pg_size_t lba) {
    // invalidate the previous page of this lba
    pg_size_t prev_page_idx = lba_page_map_[lba];
    if (prev_page_idx != INVALID_PAGE) {
      UpdatePageLba(prev_page_idx, INVALID_PAGE);
    }
    // write to next free page in current log block
    pg_size_t page_idx = log_block_ * block_size_ + log_page_offset_++;
    UpdatePageLba(page_idx, lba);
    lba_page_map_[lba] = page_idx;
    return page_idx;
  }

  void UpdatePageLba(pg_size_t page_idx, pg_size_t lba) {
    if (lba == INVALID_PAGE) {
      --block_livepages_map_[page_idx / block_size_];
    } else {
      ++block_livepages_map_[page_idx / block_size_];
    }
    page_lba_map_[page_idx] = lba;
  }

  bool IsValidLba(size_t lba) { return lba <= largest_lba_; }

  // We mostly use indexes to represent the 5-tuple addresses to save space.
  // These functions convert 5-tuple addresses to indexes and vice versa.
  pg_size_t GetPageIdxFromAddr(const Address &addr) {
    return addr.package * package_size_ * die_size_ * plane_size_ *
               block_size_ +
           addr.die * die_size_ * plane_size_ * block_size_ +
           addr.plane * plane_size_ * block_size_ + addr.block * block_size_ +
           addr.page;
  }
  Address GetAddrFromPageIdx(pg_size_t page_idx) {
    return Address(
        page_idx / (package_size_ * die_size_ * plane_size_ * block_size_),
        (page_idx / (die_size_ * plane_size_ * block_size_)) % package_size_,
        (page_idx / (plane_size_ * block_size_)) % die_size_,
        (page_idx / (block_size_)) % plane_size_, page_idx % block_size_);
  }
  Address GetAddrFromBlockIdx(blk_size_t blk_idx) {
    return Address(blk_idx / (package_size_ * die_size_ * plane_size_),
                   (blk_idx / (die_size_ * plane_size_)) % package_size_,
                   (blk_idx / plane_size_) % die_size_, blk_idx % plane_size_,
                   0);
  }

  // Number of packages in a ssd
  size_t ssd_size_;
  // Number of dies in a package_
  size_t package_size_;
  // Number of planes in a die_
  size_t die_size_;
  // Number of blocks in a plane_
  size_t plane_size_;
  // Number of pages in a block_
  size_t block_size_;
  // Maximum number a block_ can be erased
  size_t block_erase_count_;

  // gives the largest valid lba
  size_t largest_lba_;

  // mapping of lba to physical page index
  std::vector<pg_size_t> lba_page_map_;
  // mapping of physical page index to lba
  std::vector<pg_size_t> page_lba_map_;
  // mapping of block index to erase count
  std::vector<erase_size_t> block_erase_map_;
  // list of free log blocks (not programmed at all)
  std::list<blk_size_t> free_log_blocks_;
  // list of used log blocks (fully programmed)
  std::list<blk_size_t> used_log_blocks_;

  // track live pages per block
  std::vector<pgcnt_size_t> block_livepages_map_;

  // index of the current log block we are using and the offset of the next
  // free page
  blk_size_t log_block_;
  pg_size_t log_page_offset_;
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
