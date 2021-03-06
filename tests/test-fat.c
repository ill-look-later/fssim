#include "fssim/common.h"
#include "fssim/fat.h"

void test1()
{
  fs_fat_t* fat = fs_fat_create(10);

  ASSERT(fat->blocks[0] == 0, "0->NIL");
  ASSERT(fat->blocks[9] == 9, "9->NIL");

  fs_fat_destroy(fat);
}

void test2()
{
  fs_fat_t* fat = fs_fat_create(10);

  uint32_t file_entry0 = fs_fat_addfile(fat);
  ASSERT(file_entry0 == 0, "");
  ASSERT(fat->blocks[file_entry0] == 0,
         "0->NIL but internally 0 is marked used (bmp should take care)");

  uint32_t file_entry1 = fs_fat_addfile(fat);
  ASSERT(file_entry1 == 1, "");
  ASSERT(fat->blocks[file_entry1] == 1, "1->NIL should be received. 0 is "
                                        "already taken (even though it points "
                                        "to NIL)");
  fs_fat_destroy(fat);
}

void test3()
{
  fs_fat_t* fat = fs_fat_create(10);

  uint32_t file_entry0 = fs_fat_addfile(fat);
  uint32_t file_entry1 = fs_fat_addfile(fat);

  // file0 :  0->2->NIL
  // file1 :  1->NIL
  fs_fat_addblock(fat, file_entry0);
  ASSERT(fat->blocks[file_entry0] == 2, "");
  ASSERT(fat->blocks[2] == 2, "");

  // file0 :  0->2->NIL
  // file1 :  1->3->NIL
  fs_fat_addblock(fat, file_entry1);
  ASSERT(fat->blocks[file_entry1] == 3, "");
  ASSERT(fat->blocks[3] == 3, "");

  // file0 :  0->2->NIL
  // file1 :  1->3->4->NIL
  fs_fat_addblock(fat, file_entry1);
  ASSERT(fat->blocks[3] == 4, "");
  ASSERT(fat->blocks[4] == 4, "");

  fs_fat_destroy(fat);
}

void test4()
{
  fs_fat_t* fat = fs_fat_create(7);

  uint32_t file_entry0 = fs_fat_addfile(fat);
  uint32_t file_entry1 = fs_fat_addfile(fat);

  fs_fat_addblock(fat, file_entry0);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  // file0 :  0->2->NIL
  // file1 :  1->3->4->5->6->NIL
  fs_fat_addblock(fat, file_entry1);

  // file0 :  -
  // file1 :  1->3->4->5->6->NIL
  fs_fat_removefile(fat, file_entry0);

  // file1 :  1->3->4->5->6->0->NIL
  //          2->NIL
  fs_fat_addblock(fat, file_entry1);

  // file1 :  1->3->4->5->6->0->2->NIL
  fs_fat_addblock(fat, file_entry1);

  ASSERT(fat->blocks[0] == 2, "");
  ASSERT(fat->blocks[1] == 3, "");
  ASSERT(fat->blocks[2] == 2, "");
  ASSERT(fat->blocks[3] == 4, "");
  ASSERT(fat->blocks[4] == 5, "");
  ASSERT(fat->blocks[5] == 6, "");
  ASSERT(fat->blocks[6] == 0, "");

  fs_fat_destroy(fat);
}

void test5()
{
  fs_fat_t* fat = fs_fat_create(7);
  unsigned char* buf = calloc(512, sizeof(*buf));

  uint32_t file_entry0 = fs_fat_addfile(fat);
  uint32_t file_entry1 = fs_fat_addfile(fat);

  fs_fat_addblock(fat, file_entry0);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_removefile(fat, file_entry0);
  fs_fat_addblock(fat, file_entry1);

  // file1 :  1->3->4->5->6->0->2->NIL
  fs_fat_addblock(fat, file_entry1);
  fs_fat_serialize(fat, buf, 512);

  ASSERT(deserialize_uint32_t(buf) == fat->blocks[0], "");
  ASSERT(deserialize_uint32_t(buf + 4) == fat->blocks[1], "");
  ASSERT(deserialize_uint32_t(buf + 8) == fat->blocks[2], "");
  ASSERT(deserialize_uint32_t(buf + 12) == fat->blocks[3], "");
  ASSERT(deserialize_uint32_t(buf + 16) == fat->blocks[4], "");
  ASSERT(deserialize_uint32_t(buf + 20) == fat->blocks[5], "");
  ASSERT(deserialize_uint32_t(buf + 24) == fat->blocks[6], "");

  ASSERT(deserialize_uint8_t(buf + 28) == fat->bmp->mapping[0], "");

  fs_fat_destroy(fat);
  free(buf);
}

void test6()
{
  fs_fat_t* fat = fs_fat_create(7);
  const size_t BUFSIZE = fat->length*4 + fat->bmp->size;
  unsigned char* buf = calloc(BUFSIZE, sizeof(*buf));
  PASSERT(buf, FS_ERR_MALLOC);
  memset(buf, '\0', BUFSIZE);

  uint32_t file_entry0 = fs_fat_addfile(fat);
  uint32_t file_entry1 = fs_fat_addfile(fat);

  fs_fat_addblock(fat, file_entry0);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_addblock(fat, file_entry1);
  fs_fat_removefile(fat, file_entry0);
  fs_fat_addblock(fat, file_entry1);

  // file1 :  1->3->4->5->6->0->2->NIL
  fs_fat_addblock(fat, file_entry1);
  fs_fat_serialize(fat, buf, BUFSIZE);

  fs_fat_t* fat2 = fs_fat_load(buf, 7);

  ASSERT(fat2->blocks[0] == fat->blocks[0], "");
  ASSERT(fat2->blocks[1] == fat->blocks[1], "");
  ASSERT(fat2->blocks[2] == fat->blocks[2], "");
  ASSERT(fat2->blocks[3] == fat->blocks[3], "");
  ASSERT(fat2->blocks[4] == fat->blocks[4], "");
  ASSERT(fat2->blocks[5] == fat->blocks[5], "");
  ASSERT(fat2->blocks[6] == fat->blocks[6], "");

  ASSERT(fat->bmp->mapping[0], "Must not be zero as we allocated stuff before");
  ASSERT(fat2->bmp->mapping[0] == fat->bmp->mapping[0], "");

  fs_fat_destroy(fat);
  fs_fat_destroy(fat2);
  free(buf);
}

int main(int argc, char* argv[])
{
  TEST(test1, "creation and deletion");
  TEST(test2, "file add");
  TEST(test3, "add block");
  TEST(test4, "remove file");

  TEST(test5, "persistence - serialize");
  TEST(test6, "persistence - load()");

  return 0;
}
