#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0 /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1 /* Root directory file inode sector. */

/* Block device that contains the file system. */
extern struct block *fs_device;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);

// Helper Method Headers
char* parse_file (const char* filepath);
struct dir* parse_dir (const char* dirpath);
struct dir* open_dir_rte_abs (const char* rte_abs_path);
//void save_file (const char* name, char* file_path);
//void save_dir (const char* name, char* dir_path);

#endif /* filesys/filesys.h */
