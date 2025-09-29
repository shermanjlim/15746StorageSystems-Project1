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

      used_log_blocks_.push_back(log_block_);
      log_block_ = free_log_blocks_.front();
      free_log_blocks_.pop_front();
      log_page_offset_ = 0;
    }

    // invalidate the previous page of this lba
    pg_size_t prev_page_idx = lba_page_map_[lba];
    if (prev_page_idx != INVALID_PAGE) {
      page_lba_map_[prev_page_idx] = INVALID_PAGE;
    }
    // write to next free page in current log block
    pg_size_t page_idx = log_block_ * block_size_ + log_page_offset_++;
    page_lba_map_[page_idx] = lba;
    lba_page_map_[lba] = page_idx;

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

    return ExecState::SUCCESS;
  }

 private:
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

  // index of the current log block we are using and the offset of the next free
  // page
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
