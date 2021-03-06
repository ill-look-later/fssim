#include "fssim/filesystem.h"

fs_filesystem_t* fs_filesystem_create(size_t blocks)
{
  fs_filesystem_t* fs = malloc(sizeof(*fs));
  PASSERT(fs, FS_ERR_MALLOC);

  *fs = fs_zeroed_filesystem;

  fs->blocks_num = blocks;
  fs->block_size = FS_BLOCK_SIZE;

  return fs;
}

void fs_filesystem_destroy(fs_filesystem_t* fs)
{
  if (fs->fat) {
    fs_fat_destroy(fs->fat);
    fs->fat = NULL;
  }

  if (fs->root) {
    fs_file_destroy(fs->root);
    fs->root = NULL;
  }

  if (fs->file) {
    PASSERT(fclose(fs->file) == 0, "fclose error:");
    fs->file = NULL;
  }

  if (fs->buf) {
    free(fs->buf);
    fs->buf = NULL;
  }

  free(fs);
}

static inline void fs_filesystem_mount_new(fs_filesystem_t* fs,
                                           const char* fname)
{
  fs_file_t* parent = NULL;
  size_t n = 0;

  // bcount | bsize | fat | bmp
  fs->file = fs_utils_mkfile(fname, fs->blocks_num * fs->block_size);
  fs->fat = fs_fat_create(fs->blocks_num);
  fs->root = fs_file_create("/", FS_FILE_DIRECTORY, parent);
  fs->cwd = fs->root;
  fs->root->fblock = fs_fat_addfile(fs->fat);
  fs->blocks_offset = 8 + 4 * fs->blocks_num + fs->fat->bmp->size;

  fs->buf = calloc(fs->blocks_offset, sizeof(*fs->buf));
  PASSERT(fs->buf, FS_ERR_MALLOC);

  fs_filesystem_persist_sbfatbmp(fs);
  fs_filesystem_persist_cwd(fs);

  return;
}

static inline void fs_filesystem_mount_existing(fs_filesystem_t* fs,
                                                const char* fname)
{
  fs_file_t* parent = NULL;
  size_t n = 0;
  uint8_t tmp_buf[8] = { 0 };

  PASSERT((fs->file = fopen(fname, "r+b")), "fopen (r+b): ");

  while (n < 8)
    n += fread(tmp_buf, sizeof(uint8_t), 8, fs->file);
  PASSERT(~n, "fread error: ");

  fs->block_size = deserialize_uint32_t(tmp_buf);     // 4B
  fs->blocks_num = deserialize_uint32_t(tmp_buf + 4); // 4B
  fs->blocks_offset =
      8 + 4 * fs->blocks_num + ((fs->blocks_num - 1) / 8 | 0) + 1;

  n = 0;

  fs->buf = calloc(fs->blocks_offset, sizeof(*fs->buf));
  PASSERT(fs->buf, FS_ERR_MALLOC);

  fseek(fs->file, 0, SEEK_SET);
  while (n < fs->blocks_offset)
    n += fread(fs->buf + n, sizeof(uint8_t), fs->blocks_offset, fs->file);
  PASSERT(~n && n == fs->blocks_offset, "fread error: ");

  fs_filesystem_load(fs);
}

void fs_filesystem_mount(fs_filesystem_t* fs, const char* fname)
{

  if (!fs_utils_fexists(fname))
    fs_filesystem_mount_new(fs, fname);
  else {
    fprintf(stderr, "File `%s` already exists.\n"
                    "Mounting on top of it!\n",
            fname);
    fs_filesystem_mount_existing(fs, fname);
  }
}

int fs_filesystem_serialize_superblock(fs_filesystem_t* fs, unsigned char* buf,
                                       int n)
{
  ASSERT(n >= 8, "`buf` must have at least 8 bytes remaining");
  serialize_uint32_t(buf, fs->block_size);
  serialize_uint32_t(buf + 4, fs->blocks_num);

  return 8;
}

int fs_filesystem_serialize(fs_filesystem_t* fs, unsigned char* buf, int n)
{
  int written = 0;

  written += fs_filesystem_serialize_superblock(fs, buf, n);
  written += fs_fat_serialize(fs->fat, buf + written, n - written);

  return written;
}

static void _load_fs_files(fs_filesystem_t* fs, fs_file_t* file)
{
  fs_llist_t* child = NULL;
  fs_file_t* f = NULL;
  int n = 0;

  fseek(fs->file, fs->blocks_offset + (file->fblock * FS_BLOCK_SIZE), SEEK_SET);
  while (n < FS_BLOCK_SIZE)
    n += fread(fs->block_buf, sizeof(uint8_t), FS_BLOCK_SIZE - n, fs->file);
  fs_file_load_dir(file, fs->block_buf);

  n = file->children_count;
  child = file->children;

  while (child) {
    f = (fs_file_t*)child->data;

    if (f->attrs.is_directory)
      _load_fs_files(fs, f);

    child = child->next;
  }
}

void fs_filesystem_load(fs_filesystem_t* fs)
{
  fs->block_size = deserialize_uint32_t(fs->buf);
  fs->blocks_num = deserialize_uint32_t(fs->buf + 4);
  fs->fat = fs_fat_load(fs->buf + 8, fs->blocks_num);
  fs->root = fs_file_create("/", FS_FILE_DIRECTORY, NULL);
  fs->cwd = fs->root;

  _load_fs_files(fs, fs->root);
}

static fs_llist_t* _find_file_in_layer(fs_llist_t* child, const char* fname)
{
  ASSERT(child != NULL, "");

  while (child) {
    if (!strcmp(((fs_file_t*)child->data)->attrs.fname, fname))
      return child;

    child = child->next;
  }

  return NULL;
}

static fs_llist_t* _traverse_to_dir(fs_filesystem_t* fs, char** argv,
                                    unsigned argc)
{
  fs_llist_t* f = fs->root->children;

  if (!argc) { // '/'
    fs->cwd = fs->root;
    return fs->root->children;
  }

  for (int i = 0; i < argc; i++) { // '/something[/others ...]'
    if (!(f = _find_file_in_layer(f, argv[i]))) {
      fs->cwd = NULL;
      return NULL;
    }
  }

  fs->cwd = (fs_file_t*)f->data;

  return f;
}

//  - if a path to the last component exist:
//    - set fs->cwd to the location
//    - return the file (llist) if found
//    - return the directory where the file does not exist
static fs_llist_t* _traverse_to_file(fs_filesystem_t* fs, char** argv,
                                     unsigned argc)
{
  fs_llist_t root_f = {.next = NULL, .data = fs->root };
  fs_llist_t* f = &root_f;
  int i = 0;

  fs->cwd = fs->root;

  for (; i < argc - 1; i++) { // '[/others ...]/last'
    f = ((fs_file_t*)f->data)->children;

    if (!f)
      return NULL;

    if (!(f = _find_file_in_layer(f, argv[i])))
      return NULL;
  }

  // if we got somewhere
  if (argc > 1) {
    fs->cwd = (fs_file_t*)f->data;
  }

  // last component - check if we can find it
  if (f && argc > 1) {
    f = ((fs_file_t*)f->data)->children;
    if (!f)
      return NULL;

    return _find_file_in_layer(f, argv[i]);
  }

  if (fs->root->children)
    return _find_file_in_layer(fs->root->children, argv[i]);
  return NULL;
}

// DFS
fs_file_t* fs_filesystem_find(fs_filesystem_t* fs, const char* root,
                              const char* fname)
{
  if (!fs->root->children)
    return NULL;

  unsigned argc = 0;
  char** argv = fs_utils_splitpath(root, &argc);

  fs_llist_t* dir = _traverse_to_dir(fs, argv, argc);
  fs_llist_t* file = _find_file_in_layer(fs->cwd->children, fname);

  FREE_ARR(argv, argc);

  return file ? (fs_file_t*)file->data : NULL;
}

static fs_file_t* _filesystem_mkfile(fs_filesystem_t* fs, const char* fname,
                                     fs_file_type type)
{
  unsigned n = 0;
  unsigned argc = 0;
  char** argv = fs_utils_splitpath(fname, &argc);

  _traverse_to_file(fs, argv, argc);

  fs_file_t* f = fs_file_create(argv[argc - 1], type, fs->cwd);
  fs_file_addchild(fs->cwd, f);
  f->parent = fs->cwd;
  f->fblock = fs_fat_addfile(fs->fat);

  fs_filesystem_persist_cwd(fs);
  FREE_ARR(argv, argc);

  return f;
}

void fs_filesystem_ls(fs_filesystem_t* fs, const char* abspath, char* buf,
                      size_t n)
{
  int written = 0;
  char mtime_buf[FS_DATE_FORMAT_SIZE] = { 0 };
  char fsize_buf[FS_FSIZE_FORMAT_SIZE] = { 0 };
  unsigned argc = 0;
  fs_llist_t* child = NULL;
  char** argv = fs_utils_splitpath(abspath, &argc);
  _traverse_to_dir(fs, argv, argc);

  if (!fs->cwd) {
    fprintf(stderr, "\nDirectory `%s` not found.\n", abspath);
    FREE_ARR(argv, argc);
    fs->cwd = fs->root;

    return;
  }

  fs_utils_fsize2str(fs->cwd->attrs.size, fsize_buf, FS_FSIZE_FORMAT_SIZE);
  fs_utils_secs2str(fs->cwd->attrs.mtime, mtime_buf, FS_DATE_FORMAT_SIZE);

  written += snprintf(buf + written, n - written, FS_LS_FORMAT,
                      fs->cwd->attrs.is_directory == 1 ? 'd' : 'f', fsize_buf,
                      mtime_buf, ".");

  written += snprintf(buf + written, n - written, FS_LS_FORMAT,
                      fs->cwd->attrs.is_directory == 1 ? 'd' : 'f', fsize_buf,
                      mtime_buf, "..");

  child = fs->cwd->children;

  while (child) {
    fs_file_t* file = (fs_file_t*)child->data;

    fs_utils_fsize2str(file->attrs.size, fsize_buf, FS_FSIZE_FORMAT_SIZE);
    fs_utils_secs2str(file->attrs.mtime, mtime_buf, FS_DATE_FORMAT_SIZE);

    written += snprintf(buf + written, n, FS_LS_FORMAT,
                        file->attrs.is_directory == 1 ? 'd' : 'f', fsize_buf,
                        mtime_buf, file->attrs.fname);

    child = child->next;
  }

  FREE_ARR(argv, argc);
}

fs_file_t* fs_filesystem_touch(fs_filesystem_t* fs, const char* fname)
{
  return _filesystem_mkfile(fs, fname, FS_FILE_REGULAR);
}

fs_file_t* fs_filesystem_mkdir(fs_filesystem_t* fs, const char* fname)
{
  return _filesystem_mkfile(fs, fname, FS_FILE_DIRECTORY);
}

fs_file_t* fs_filesystem_cp(fs_filesystem_t* fs, const char* src,
                            const char* dest)
{
  off_t offset = 0;
  int32_t remaining = 0;
  int32_t written = 0;
  int32_t size = 0;
  int32_t blocks_needed = 0;
  FILE* f = NULL;
  fs_file_t* file = NULL;
  uint32_t block = UINT32_MAX;

  ASSERT(!fs_filesystem_find(fs, fs->cwd->attrs.fname, dest),
         "File already exists");
  PASSERT(f = fopen(src, "r"), "fopen");

  size = fs_utils_fsize(f);
  blocks_needed = ((size - 1) / FS_BLOCK_SIZE | 0) + 1;
  remaining = size;

  // TODO assert that we have space [issue 14]
  // TODO how to properly notify the error? [ issue 13 ]

  file = fs_filesystem_touch(fs, dest);
  file->attrs.size = size;
  file->attrs.ctime = fs_utils_gettime();
  file->attrs.mtime = file->attrs.ctime;
  file->attrs.atime = file->attrs.ctime;
  block = file->fblock;

  fflush(fs->file);
  fflush(f);

  for (int i = 0; i < blocks_needed; i++) {
    uint32_t to_write = remaining >= FS_BLOCK_SIZE ? FS_BLOCK_SIZE : remaining;

    remaining -= to_write;
    offset = lseek(fileno(fs->file),
                   fs->blocks_offset + (FS_BLOCK_SIZE * block), SEEK_SET);

    while (to_write)
      to_write -= sendfile(fileno(fs->file), fileno(f), NULL, to_write);

    // we might endup w/ a surplus of 1 block
    // FIXME there's a linear scan happening every time.
    //       change the start to the last block.
    if (i + 1 < blocks_needed)
      block = fs_fat_addblock(fs->fat, file->fblock);
  }

  fseek(fs->file, offset, SEEK_SET);

  ASSERT(remaining == 0, "Didn't copy everything. Remaining = %d", remaining);
  PASSERT(fclose(f) == 0, "fclose");

  // persist FAT and BMP
  fs_filesystem_persist_sbfatbmp(fs);
  fs_filesystem_persist_cwd(fs);

  return file;
}

void fs_filesystem_cat(fs_filesystem_t* fs, const char* src, int fd)
{
  fs_file_t* file = NULL;
  int remaining = 0;
  int n = 0;
  off_t offset = 0;
  uint32_t to_write = 0;
  int32_t written = 0;
  unsigned argc = 0;
  char** argv = fs_utils_splitpath(src, &argc);

  ASSERT((file = fs_filesystem_find(fs, fs->root->attrs.fname, argv[0])),
         "File not found");

  if (file->attrs.is_directory) {
    fprintf(stderr, "Can't `cat` a directory.\n"
                    "Enter `help` if you need help\n");
    return;
  }

  remaining = file->attrs.size;

  fflush(fs->file);

  for (int block = file->fblock;; n++) {
    to_write = remaining >= FS_BLOCK_SIZE ? FS_BLOCK_SIZE : remaining;
    offset = lseek(fileno(fs->file),
                   fs->blocks_offset + (FS_BLOCK_SIZE * block), SEEK_SET);
    PASSERT(written += sendfile(fd, fileno(fs->file), NULL, to_write),
            "sendfile: ");

    if (fs->fat->blocks[block] == block)
      break;

    block = fs->fat->blocks[block];
    remaining -= to_write;
  }

  PASSERT(~fseek(fs->file, offset, SEEK_SET), "lseek: ");
  PASSERT(written == file->attrs.size, "Should've written %d. Wrote %d ",
          file->attrs.size, written);

  FREE_ARR(argv, argc);
}

static void _filesystem_rmfile(fs_filesystem_t* fs, fs_llist_t* file)
{
  fs_file_t* cwd = fs->cwd;
  fs_file_t* f = (fs_file_t*)file->data;

  while (f->children) {
    fs->cwd = f;
    _filesystem_rmfile(fs, f->children);
  }

  fs->cwd = cwd;

  fs_fat_removefile(fs->fat, f->fblock);
  fs->cwd->children = fs_llist_remove(fs->cwd->children, file);
  fs_llist_destroy(file, fs_file_destructor);

  fs->cwd->children_count--;
  if (!fs->cwd->children_count)
    fs->cwd->children = NULL;
}

int fs_filesystem_rm(fs_filesystem_t* fs, const char* path)
{
  int n = 0;
  unsigned argc = 0;
  char** argv = fs_utils_splitpath(path, &argc);
  fs_llist_t* file = _traverse_to_file(fs, argv, argc);

  if (!file) {
    FREE_ARR(argv, argc);
    return 0;
  }

  _filesystem_rmfile(fs, file);
  fs_filesystem_persist_cwd(fs);
  FREE_ARR(argv, argc);

  return 1;
}

int fs_filesystem_rmdir(fs_filesystem_t* fs, const char* path)
{
  int n = 0;
  unsigned argc = 0;
  char** argv = fs_utils_splitpath(path, &argc);
  fs_llist_t* dir = _traverse_to_file(fs, argv, argc);
  fs_file_t* file = NULL;

  if (!dir) {
    FREE_ARR(argv, argc);
    return 0;
  }

  file = (fs_file_t*)dir->data;

  if (!file->attrs.is_directory) {
    fprintf(stderr, "File `%s` is not a directory.\n"
                    "Enter `help` if you need help.\n",
            file->attrs.fname);
    return 0;
  }

  _filesystem_rmfile(fs, dir);
  fs_filesystem_persist_cwd(fs);
  FREE_ARR(argv, argc);

  return 1;
}

int fs_filesystem_df(fs_filesystem_t* fs, char* buf, size_t n)
{
  fs_fsinfo_t info = { 0 };
  char freespace_buf[FS_FSIZE_FORMAT_SIZE] = { 0 };
  char wastedspace_buf[FS_FSIZE_FORMAT_SIZE] = { 0 };
  int written = 0;

  fs_fsinfo_calculate(&info, fs->root);

  fs_utils_fsize2str(fs->blocks_num * fs->block_size - info.usedspace,
                     freespace_buf, FS_FSIZE_FORMAT_SIZE);
  fs_utils_fsize2str(info.wastedspace, wastedspace_buf, FS_FSIZE_FORMAT_SIZE);

  written += snprintf(buf + written, n - written, FS_DF_FORMAT, info.files,
                      info.directories, freespace_buf, wastedspace_buf);

  return written;
}
