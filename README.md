# fssim

> A Filesystem simulator in C

<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->


- [Running](#running)
- [Cli](#cli)
- [Under the Hood](#under-the-hood)
  - [FS](#fs)
  - [Flow](#flow)
  - [File Attributes](#file-attributes)
  - [Block device Structure](#block-device-structure)
    - [Overview](#overview)
  - [Files](#files)
  - [Directories](#directories)
- [Utilities](#utilities)
  - [LS](#ls)
- [LICENSE](#license)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

## Running

This project depends on `cmake` for generating Make files and asserting the required libs (currently depends on [gnu readline](https://cnswww.cns.cwru.edu/php/chet/readline/rltop.html) only).

```sh
$ mkdir build && cd $_
$ cmake ..

$ make -j5      # concurrent build - 4core machine
$ ./src/fssim
```

If you wish to test the project as well you might be interested on having the verbose output when a test fails. To enable such feature, add the following variable to your environment and run `cmake` w/ `Debug` set up:

```
$ export CTEST_OUTPUT_ON_FAILURE=1
$ cmake -DCMAKE_BUILD_TYPE=Debug ..

$ make -j5    # concurrent build - 4core machine
$ ctest -j4   # concurrent test  - 4core machine
```

### Example

```sh
# generate a ~42MB file
$ dd if=/dev/urandom of=/tmp/sample.txt bs=4M count=10
10+0 records in
10+0 records out
41943040 bytes (42 MB) copied, 4,33719 s, 9,7 MB/s

$ du -h /tmp/sample.txt 
40M /tmp/sample.txt

# go to the fs
$ basename $(pwd)
build
$ ./src/fssim
[ep3] mount /tmp/fssim
Filesystem successfully mounted at `/tmp/fssim`

[ep3] ls /
d   4.0KB 1969-12-31 21:00 .         
d   4.0KB 1969-12-31 21:00 .. 

[ep3] df
Files:              0
Directories:        0
Free Space:     100.0MB
Wasted Space:     0.0 B

[ep3] cp /tmp/sample.txt /sample.txt
[ep3] ls /
d   4.0KB 1969-12-31 21:00 .         
d   4.0KB 1969-12-31 21:00 ..        
f  40.0MB 2015-11-15 14:24 sample.txt

[ep3] df
Files:              1
Directories:        0
Free Space:      60.0MB
Wasted Space:     0.0 B
```

## Cli

```sh

$ ./fssim -h

USAGE: `$ ./fssim`
  Starts a prompt which accepts the following commands:

COMMANDS:
  mount <fname>         mounts the fs in the given <fname>. In
                        case <fname> already exists, countinues
                        from where it stopped.

  cp <src> <dest>       copies a file from the real system to the
                        simulated filesystem (dest).

  mkdir <dir>           creates a directory named <dir>

  rmdir <dir>           removes the directory <dir> along with the
                        files that were there (ir any)

  cat <fname>           shows (stdout) the contents of <fname>

  touch <fname>         creates a file <fname>. If it already exists,
                        updates the file's last access time.

  rm <fname>            removes a given <fname>

  ls <dir>              lists the files and directories 'below'
                        <dir>, showing $name, $size, $last_mod for
                        files and an indicator for directories.
  
  find <dir> <fname>    taking <dir> as root, recursively searches
                        for a file named <fname>

  df                    shows filesystem info, including number of
                        diferectories, files, freespace and wasted
                        space

  unmount               unmounts the current filesystem

  help                  shows this message

  sai                   exits the simulator


NOTES:
  in the whole simulation all filenames must be specified as 
  absolute paths.

AUTHOR:
  Written by Ciro S. Costa <ciro dot costa at usp dot br>.
  See `https://github.com/cirocosta/fssim`. Licensed with MPLv2.
```

## Under the Hood

### FS

The filesystem represents a unique partition of a disk without the booting related stuff that a normal partition (in a normal disk) would have. It supports up to 100MB of both regular files and metadata. Its blocks are of 4KB each (being the block the granularity).

It's storage is done using FAT (File Allocation Table), having the root at `/` and using `/` as the hierarchy separator character. Free storage management is implemented using bitmapping. 

Each directory is a list with an entry for each file inside the directory.

### Flow

```
filepath
  |
  |      (dir struct)                     (f_index struct)
  |     .-----------.                    .----------------.
  '---> |           |                    |                |
        |           |                    +----------------+
        |           |              .---> |  &data_blocks  |
        +-----------+              |     +----------------+
        |    FILE   |---(file_no)--'     |                |
        +-----------+                    |                |
        |___________|                    |                |
                                         |________________|


    |  path resolution  |             |   blocks reading    |
    '-------------------'             '---------------------'
      happens at open()                   
           time
  
``` 

- `touch`: creates a file with no data, setting some attributes and adding a reference to it in the containing directory.
- `rm`: frees space, removes entry in the directory as well as fat
- `open`: 

### File Attributes

Simulated files or directories (which are files) contain:
-   name
-   size in bytes (except for directories)
-   creation time
-   modif time
-   last access time
-   data

These attributes are stuck with file entries in directory files.



### Block device Structure

#### Overview

```
  (size in B)           
------------------                         --
4                |  block size             |
------------------                         | Super Block
4                |  n = number of blocks   |
------------------                         --
                 |                         |
n*4              |  FAT                    |  File Alloc
                 |                         |  Table and
------------------                         |  Free Space
                 |                         |  Management
((n-1)/8|0)+1)   |  BMP                    |
                 |                         |
------------------                         --
                 |                         |
4KB              |  Block 0                | 
                 |                         | 
------------------                         | 
(...)                                      | Files and 
(...)                                      | its 
------------------                         | payload
                 |                         |
4KB              |  Block N                |
                 |                         |
------------------                         --


              Stays in memory:
/-----------------------------------------/
SuperBlock + FAT + BMP = 8 + 4n + ceil(n/8)
/-----------------------------------------/
```

As we're dealing with >1 byte numbers we have to also care about endianess (as computer  do not agree on MSB). Don't forget to use `htonl` and `ntohl` when (de)serializing numbers from the block char (we're always going with uint32_t, which is fine).

### Files

Files are implemented as an unstructured sequence of bytes. Each file is indexed by the location of its first block in the FAT and might have any size up to disk limit and with a 'floor' of 4KB (given that we're using a granularity of `1 Block = 4096 Bytes`).

### Directories

Directory blocks are well structured. Each directory block starts with 32B reserved for metadata and 127 other 32B reserved for directory entries (thus, limiting a directory to contain 127 entries - files or other directories).

```
Directory:

(size in B)           
+----------+                
| 32       |  directory metadata
+----------+                
| 4064     |  directory entries
+----------+                

Directory Metadata:

        1B              31B   // reserved for future use
+-------------------------------+
| children_count |   padding     |
+-------------------------------+
```

Each directory entry (32B) has well defined structure:

```c
struct directory_entry {
  uint8_t is_dir;   // bool: directory or not
  char fname[11];   // filename
  uint32_t fblock;  // first block in FAT
  uint32_t ctime;   // creation time
  uint32_t mtime;   // last modified
  uint32_t atime;   // last access
  uint32_t size;    // file size
};

```

Being serialized into and deserialized from:
```
/----------------------- 32Bytes ------------------------/     

                                                        
   1B       11B      4B       4B      4B      4B     4B
+-------------------------------------------------+------+  --
| is_dir | fname | fblock | ctime | mtime | atime | size |  |    1
+-------------------------------------------------+------+  --
                                                            |  
                      (...)                                 |   (..)
                                                            |
+-------------------------------------------------+------+  --
| is_dir | fname | fblock | ctime | mtime | atime | size |  |   (127)
+-------------------------------------------------+------+  --
``` 

As a design decision, directories must fit in a single block (4KB) only. This limits each layer in the hierarchy tree to support a maximum of 127 entries (files or directories) as mentioned but directory nesting is only limited by disk limit.


### Utilities

Each one of those commands in `cli` are separated utilities. They operate on top of the filesystem mutating it or not conforming to the state of the system. In this section we despict some of them.

#### LS

Each line if formatted according to:

```
           36 characters
  |-----------------------------|
    
     %c    %7s    %16s    %10s
  +-----+ +---+ +-------+ +----+
  |     | |   | |       | |    |
  is_dir  size  last_mod  fname
```


## LICENSE

MPLv2. See `LICENSE`.

