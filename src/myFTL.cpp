#include "myFTL.h"

#include "common.h"

template <typename PageType>
class MyFTL : public FTLBase<PageType> {
 public:
  /*
   * Constructor
   */
  MyFTL(const ConfBase *conf) {
    /* Number of packages in a ssd */
    size_t ssd_size = conf->GetSSDSize();
    /* Number of dies in a package_ */
    size_t package_size = conf->GetPackageSize();
    /* Number of planes in a die_ */
    size_t die_size = conf->GetDieSize();
    /* Number of blocks in a plane_ */
    size_t plane_size = conf->GetPlaneSize();
    /* Number of pages in a block_ */
    block_size = conf->GetBlockSize();
    /* Maximum number a block_ can be erased */
    size_t block_erase_count = conf->GetBlockEraseCount();
    /* Overprovioned blocks as a percentage of total number of blocks */
    size_t op = conf->GetOverprovisioning();

    printf("SSD Configuration: %zu, %zu, %zu, %zu, %zu\n", ssd_size,
           package_size, die_size, plane_size, block_size);
    printf("Max Erase Count: %zu, Overprovisioning: %zu%%\n", block_erase_count,
           op);

    num_planes = ssd_size * package_size * die_size;
    size_t num_blocks = num_planes * plane_size;
    size_t num_op_blocks =
        (num_blocks * op + 100 / 2) / 100;  // rounds off to nearest integer

    // a helper map of plane index to its address
    plane_to_address.resize(num_planes);
    for (size_t package_idx = 0; package_idx < ssd_size; ++package_idx) {
      for (size_t die_idx = 0; die_idx < package_size; ++die_idx) {
        for (size_t plane_idx = 0; plane_idx < die_size; ++plane_idx) {
          size_t agg_plane_idx = plane_idx + die_idx * die_size +
                                 package_idx * die_size * package_size;
          plane_to_address[agg_plane_idx] =
              Address(package_idx, die_idx, plane_idx, 0, 0);
        }
      }
    }

    // we evenly distribute overprovisioned blocks among the planes, with
    // smaller plane indexes getting the extras.
    // we construct the map of planes to their free log blocks here.
    // and the map of planes to the no. of data blocks.
    plane_to_logblocks.resize(num_planes, std::vector<Address>());
    plane_to_num_datablocks.resize(num_planes);
    for (size_t plane_idx = 0; plane_idx < num_planes; ++plane_idx) {
      bool has_extra = plane_idx < (num_op_blocks % num_planes);
      size_t plane_num_op_blocks = (num_op_blocks / num_planes) + has_extra;

      for (size_t op_block_idx = plane_size - plane_num_op_blocks;
           op_block_idx < plane_size; ++op_block_idx) {
        Address addr = plane_to_address[plane_idx];
        addr.block = op_block_idx;
        plane_to_logblocks[plane_idx].push_back(addr);
      }

      plane_to_num_datablocks[plane_idx] = plane_size - plane_num_op_blocks;
    }
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
    (void)lba;
    (void)func;
    return std::make_pair(ExecState::SUCCESS, Address(0, 0, 0, 0, 0));
  }

  /*
   * WriteTranslate() - Translates write address
   *
   * Please refer to ReadTranslate()
   */
  std::pair<ExecState, Address> WriteTranslate(
      size_t lba, const ExecCallBack<PageType> &func) {
    (void)lba;
    (void)func;
    Address phy_addr = CalcPhyAddr(lba);

    return std::make_pair(ExecState::SUCCESS, CalcPhyAddr(lba));
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
  Address CalcPhyAddr(size_t lba) {
    size_t plane_idx = GetLbaPlane(lba);

    size_t pages_seen = 0;
    for (size_t i = 0; i < plane_idx; ++i) {
      pages_seen += (plane_to_num_datablocks[i] * block_size);
    }
    size_t page_offset = lba - pages_seen;

    Address addr = plane_to_address[plane_idx];
    addr.block = page_offset % plane_to_num_datablocks[plane_idx];
    addr.page = page_offset / plane_to_num_datablocks[plane_idx];
    return addr;
  }

  size_t GetLbaPlane(size_t lba) {
    size_t num_pages_seen = 0;
    for (size_t plane_idx = 0; plane_idx < num_planes; ++plane_idx) {
      num_pages_seen += (plane_to_num_datablocks[plane_idx] * block_size);
      if (lba < num_pages_seen) {
        return plane_idx;
      }
    }
    throw std::runtime_error{"cannot find plane of LBA"};
  }

  size_t block_size;
  size_t num_planes;

  std::vector<Address> plane_to_address;
  std::vector<std::vector<Address>> plane_to_logblocks;
  std::vector<size_t> plane_to_num_datablocks;
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
