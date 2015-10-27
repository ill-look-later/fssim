#include "fssim/bmp.h"

fs_bmp_t* fs_bmp_create(size_t size)
{
  ASSERT(size, "Size must be at least > 0");

  fs_bmp_t* bmp = malloc(sizeof(*bmp));
  PASSERT(bmp, FS_ERR_MALLOC);

  bmp->last_block = 0;
  bmp->num_blocks = size;
  bmp->size = ((size - 1) / 8 | 0) + 1;
  bmp->mapping = calloc(bmp->size, sizeof(*bmp->mapping));
  PASSERT(bmp->mapping, FS_ERR_MALLOC);

  memset(bmp->mapping, 0x00, bmp->size);

  return bmp;
}

void fs_bmp_destroy(fs_bmp_t* bmp)
{
  free(bmp->mapping);
  free(bmp);
}

void fs_bmp_free(fs_bmp_t* bmp, uint32_t block)
{
  if (FS_BMP_IS_ON_(bmp, block)) {
    FS_BMP_FLIP_(bmp, block);
  } else {
    LOGERR("already freed block %d passed to `fs_bmp_free`.", block);
  }
}

uint32_t fs_bmp_alloc(fs_bmp_t* bmp)
{
  while (1) {
    if (!FS_BMP_IS_ON_(bmp, bmp->last_block)) {
      FS_BMP_FLIP_(bmp, bmp->last_block);
      return bmp->last_block;
    }

    bmp->last_block = (bmp->last_block + 1) % bmp->num_blocks;
  }

  ASSERT(0, "fs_bmp_alloc(): No free space found");
}
