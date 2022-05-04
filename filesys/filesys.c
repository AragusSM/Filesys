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

// System call filesystem equivalents similar design to
// project 2 userprog implementation with process methods.
bool fs_chdir(const char* dir);
bool fs_mkdir(const char* dir);
bool fs_readdir(int fd, char *name);
bool fs_isdir(int fd);
int fs_inumber(int fd);


//struct dir* open_dir_rte_abs (const char* rte_abs_path);
// Helper methods to save file_path and directories
// into buffers so that we can check more conditions.
void save_file (const char* name, char* file_path);
void save_dir (const char* name, char* dir_path);

// Handles absolute and relative paths.
struct dir* open_dir_rte_abs (const char* rte_abs_path) {
  int path_len = strlen (rte_abs_path);
  if(path_len == 0){
    return thread_current()->curr_dir ? dir_reopen(thread_current()->curr_dir) : dir_open_root();
  }
  char *rte_abs_copy = calloc(1, (path_len + 1));
  if (! rte_abs_copy)
    return NULL;
  struct dir *curr_dir = NULL;
  strlcpy(rte_abs_copy, rte_abs_path, path_len + 1);
  // Checking if it is an absolute or relative path
  // If it is absolute, it will have the root '/',
  if (rte_abs_copy[0] == '/' || ! thread_current()->curr_dir) {
    curr_dir = dir_open_root();
  } else {
    curr_dir = dir_reopen(thread_current()->curr_dir);
  }
  // Now we tokenize
  char* save_ptr = NULL;
  char* token = strtok_r (rte_abs_copy, "/", &save_ptr);
  while (token != NULL) {
    if(strcmp(token, ".") == 0){
      token = strtok_r (NULL, "/", &save_ptr);
      continue;
    }
    // Set the next node while we are at it.
    struct inode *i_next = NULL;
    // Lookup failed
    if (! dir_lookup (curr_dir, token, &i_next)) {
      dir_close (curr_dir);
      free (rte_abs_copy);
      return NULL;
    }
    //printf("%i\n", inode_get_inumber(i_next));
    struct dir* d_next = dir_open(i_next);
    if (! d_next) {
      // Open inode failed
      dir_close (curr_dir);
      free (rte_abs_copy);
      return NULL;
    }

    dir_close (curr_dir);
    curr_dir = d_next;
    token = strtok_r (NULL, "/", &save_ptr);
  }

  // Also, we cannot open removed directories
  if (inode_removed (dir_get_inode (curr_dir))) {
    dir_close (curr_dir);
    free (rte_abs_copy);
    return NULL;
  }

  free (rte_abs_copy);

  // Curr_dir is still open.
  return curr_dir;
}

// Saves the file into the file_path by parsing through
// the name of the file.
void save_file(const char* name, char* file_path) {
  int path_len = strlen(name);
  // Make a new copy to prevent corruption
  char* fpath_copy = calloc(1, (path_len + 1));
  if (! fpath_copy)
    return NULL;
  strlcpy(fpath_copy, file_path, path_len + 1);
  char* save_ptr = NULL;
  char* prev_token = ""; 
  char* curr_token = strtok_r(fpath_copy, "/", &save_ptr);
  int token_size;
  while (curr_token != NULL) {
    prev_token = curr_token;
    curr_token = strtok_r(NULL, "/", &save_ptr);
  }
  // Copying over the file path
  
  memcpy (file_path, prev_token, strlen(prev_token) + 1);
  
  // Freeing what you calloc
  free(fpath_copy);
}

// Helper method to save tokens in dir_path
void save_dir (const char* name, char* dir_path) {
  int path_len = strlen(name);
  char* dpath_copy = calloc(1, (path_len + 1));
  if (! dpath_copy)
    return NULL;
  strlcpy(dpath_copy, dir_path, path_len + 1);
  if (path_len > 0 && dpath_copy[0] == '/') {
    dir_path[0] = '/';  
    dir_path++;
  }
  char* save_ptr = NULL;
  char* prev_token = "";
  char* curr_token = strtok_r(dpath_copy, "/", &save_ptr);
  int token_size;
  while (curr_token != NULL) {
    if (strlen(prev_token) > 0) {
      memcpy(dir_path, prev_token, strlen(prev_token));
      dir_path[strlen(prev_token)] = '/';
      dir_path += strlen(prev_token) + 1;
    }
    prev_token = curr_token;
    curr_token = strtok_r(NULL, "/", &save_ptr);
  }
  *dir_path = '\0';
  // Free what you calloc
  free(dpath_copy);
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
bool filesys_create (const char *name, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  int name_length = strlen(name);

  char* file_path = calloc(1, name_length + 1);
  char* dir_path = calloc(1, name_length + 1);

  if (file_path == NULL || dir_path == NULL) {
    return false;
  }
  
  save_file(name, file_path);
  save_dir(name, dir_path);
  int index = strlen(name) - 1;
  while(index >= 0 && name[index] != '/'){
    index--;
  }
  index++;
  strlcpy(dir_path, name, index + 1);
  strlcpy(file_path, name + index, (name_length + 1) - index);
  struct dir *dir = open_dir_rte_abs(dir_path);
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
struct file *filesys_open (const char *name)
{
  struct dir *dir = NULL;
  dir = open_dir_rte_abs(name);
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
  strlcpy(dir_path, name, index + 1);
  strlcpy(file_path, name + index, (name_length + 1) - index);
  dir = (open_dir_rte_abs(dir_path));
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
  strlcpy(dir_path, name, index + 1);
  strlcpy(file_path, name + index, (name_length + 1) - index);
  struct dir *dir = open_dir_rte_abs(dir_path);
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


/* Helper method that gives the correct file name.
   
    Update the existing system calls so that, anywhere a 
    file name is provided by the caller, an absolute or 
    relative path name may used. 
    
    The directory separator 
    character is forward slash ("/"). 
    
    You must also support 
    special file names "." and "..", which have the same meanings as they do in Unix.
   
   Params: file name passed in. */

/* Pseudocode 
  Three cases: 
  Case 1
  Special character '.'

  Case 2
  Special characters ".."

  Case 3
  Ordinary Files
    Use strtok_r delimiter to parse through.
*/  
/* "project-4-filesystem/filesys/inode.c" */
char* parse_file(const char* filepath) {

  int path_len = strlen(filepath);
  char* fpath_copy = calloc(1, (path_len + 1));
  if (! fpath_copy)
    return NULL;
  

  strlcpy(fpath_copy, filepath, sizeof(filepath + 1));
  // Absolute path case
  // Would we need to obtain the inode for the directory that we get?
  // I'm having trouble imagining how exactly we form a directory path.
  // Is it the same as disk accessing the inodes for the directory?
  if (fpath_copy[0] == '/' && path_len > 0) {
    
  }
  // Would we need a buffer to parse the file path?
  // Is there some way to pass in a variable we can set and
  // assign values to?
  // Pass in a pointer and then change the value of that pointer, but
  // for what reason?
}

struct dir* parse_dir(const char* dirpath) {
  char* dpath_copy = calloc(1, sizeof(dirpath + 1));
  strlcpy(dpath_copy, dirpath, sizeof(dirpath + 1));
  if (dpath_copy[0] == '.' || dpath_copy[0] == '..') {
       
  }
}