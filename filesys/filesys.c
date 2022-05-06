#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

// Handles absolute and relative paths.
// Driver: Joel
struct dir* handle_rel_abs_dir (const char* rel_abs_path) {
  int path_len = strlen (rel_abs_path);
  if(path_len == 0){
    return thread_current()->curr_dir
     ? dir_reopen(thread_current()->curr_dir) : dir_open_root();
  }
  char *rel_abs_copy = calloc(1, (path_len + 1));
  if (! rel_abs_copy)
    return NULL;
  struct dir *curr_dir = NULL;
  strlcpy(rel_abs_copy, rel_abs_path, path_len + 1);
  // Checking if it is an absolute or relative path
  // If it is absolute, it will have the root '/',
  if (rel_abs_copy[0] == '/' || ! thread_current()->curr_dir) {
    curr_dir = dir_open_root();
  } else {
    curr_dir = dir_reopen(thread_current()->curr_dir);
  }
  // Now we tokenize
  char* save_ptr = NULL;
  char* token = strtok_r (rel_abs_copy, "/", &save_ptr);
  while (token != NULL) {
    if(strcmp(token, ".") == 0){
      token = strtok_r (NULL, "/", &save_ptr);
      continue;
    }
    // Set the next node 
    struct inode *inode_next = NULL;
    // Lookup failed
    if (! dir_lookup (curr_dir, token, &inode_next)) {
      dir_close (curr_dir);
      free (rel_abs_copy);
      return NULL;
    }
    struct dir* dir_next = dir_open(inode_next);
    if (! dir_next) {
      // Open inode failed
      dir_close (curr_dir);
      free (rel_abs_copy);
      return NULL;
    }

    dir_close (curr_dir);
    curr_dir = dir_next;
    token = strtok_r (NULL, "/", &save_ptr);
  }

  // We cannot open removed directories
  if (inode_removed (dir_get_inode (curr_dir))) {
    dir_close (curr_dir);
    free (rel_abs_copy);
    return NULL;
  }

  free (rel_abs_copy);
  // Curr_dir is still open
  return curr_dir;
}

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done (void) { free_map_close (); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
// Driver: Michael and Joel
bool filesys_create (const char *name, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  int name_length = strlen(name);

  int index = strlen(name) - 1;
  while(index >= 0 && name[index] != '/'){
    index--;
  }
  index++;

  char* file_path = calloc(1, name_length + 1);
  char* dir_path = calloc(1, name_length + 1);

  if (file_path == NULL || dir_path == NULL) {
    return false;
  }

  strlcpy(dir_path, name, index + 1);
  strlcpy(file_path, name + index, (name_length + 1) - index);

  struct dir *dir = handle_rel_abs_dir(dir_path);
  bool success = (dir != NULL && free_map_allocate (1, &inode_sector) &&
                  inode_create (inode_sector, initial_size, false) &&
                  dir_add (dir, file_path, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close(dir);
  // Freeing dynamically allocated memory.
  free(file_path);
  free(dir_path);
  return success;
}


/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
// Driver: Michael and Joel
struct file *filesys_open (const char *name)
{
  struct dir *dir = NULL;
  dir = handle_rel_abs_dir(name);
  if(dir && inode_is_subdir(dir_get_inode(dir))){
    struct file* file = file_open(dir_get_inode(dir));
    return file;
  }
  dir_close(dir);
  struct inode *inode = NULL;

  int name_length = strlen(name);
  int index = strlen(name) - 1;
  while(index >= 0 && name[index] != '/'){
    index--;
  }
  index++;

  char* dir_path = calloc(1, name_length + 1);
  char* file_path = calloc(1, name_length + 1);

  if (file_path == NULL || dir_path == NULL) {
    return false;
  }

  strlcpy(dir_path, name, index + 1);
  strlcpy(file_path, name + index, (name_length + 1) - index);

  dir = (handle_rel_abs_dir(dir_path));
  free(dir_path);
  
  if (dir != NULL)
    dir_lookup (dir, file_path, &inode);
  dir_close (dir);
  free(file_path);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
// Driver: Michael and Joel
bool filesys_remove (const char *name)
{
  int name_length = strlen(name);
  int index = strlen(name) - 1;
  while(index >= 0 && name[index] != '/'){
    index--;
  }
  index++;

  char* dir_path = calloc(1, name_length + 1);
  char* file_path = calloc(1, name_length + 1);

  if (dir_path == NULL || file_path == NULL) {
    return false;
  }

  strlcpy(dir_path, name, index + 1);
  strlcpy(file_path, name + index, (name_length + 1) - index);
  struct dir *dir = handle_rel_abs_dir(dir_path);
  bool success = dir != NULL && dir_remove (dir, file_path);
  dir_close (dir);
  return success;
}

/* Formats the file system. */
static void do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}