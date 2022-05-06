#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h> // Filesys For strtok 
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "threads/synch.h"

#include "userprog/process.h"
#include <user/syscall.h>
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "filesys/inode.h"

#include "devices/input.h"
#include "devices/shutdown.h"


// Create a global lock that can be accessed in both
// Syscall.c and process.c
extern struct lock filesys_lock; 


static void syscall_handler (struct intr_frame *);
static void check_pointer(void * vaddr );
static bool valid_fd(int fd);
void halt(void);
bool create (const char *file, unsigned initial_size);
int open(const char *file /*, struct dir *directory*/);
pid_t exec(const char *cmd_line);
int wait(pid_t pid);
int filesize(int fd);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);
bool remove (const char *file);
struct thread* child_thr(tid_t tid_val);
void set_parent(struct thread *t, void *aux);
void get_tids(struct thread *t, void *aux);

// FILESYS method headers
bool chdir(const char* dir);
bool mkdir(const char* dir);
bool readdir(int fd, char *name);
bool isdir(int fd);
int inumber(int fd);


// Driver: Joel
void syscall_init (void)
{
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}


// Driver: Michael, Ashley, Joel
/* For each system call that passes in pointers, we check the pointer
   before running that system call, and we set f->eax to appropriate
   values */
static void syscall_handler (struct intr_frame *f UNUSED)
{
  // Retrieve system call number from the stack.
  // Driver: Ashley
  int *program = (int *) f->esp;
  check_pointer((void *)program);
  int program_name = program[0]; 
  
  // Driver Michael
  switch (program_name)
  {
  case SYS_HALT:
    halt();
    break;

  case SYS_EXIT:
    check_pointer( (void *) (program + 1));
    int *status = (int *) program[1];
    exit((int)status);
    break;

  case SYS_EXEC:
    check_pointer( (void *) (program + 1));
    const char *filename = (const char *) program[1];
    f->eax = exec(filename);
    break;

  case SYS_WAIT:
    check_pointer( (void *) program + 1);
    // First argument is pid
    int pid = program[1]; 
    f->eax = wait(pid);
    break;

  case SYS_CREATE:
    check_pointer((void *) (program[1]));
    check_pointer( (void *) (program + 2));
    const char * file1 = (const char *) program[1];
    unsigned initial_size = (unsigned) program[2];
    f->eax = create(file1,  initial_size); 
    break;

  case SYS_REMOVE:
    check_pointer( (void *) (program + 1));
    const char * file2 = (const char *) program[1];
    f->eax = remove(file2);
    break;

  case SYS_OPEN:
    // Driver: Ashley
    check_pointer( (void *) program [1]);
    const char * file3 = (const char *) program [1];
    // Pass in directories too for 5.3.3
    f->eax = open(file3 /*, thread_current()->curr_dir*/);
    break;
    
  case SYS_FILESIZE:
    check_pointer( (void *) (program + 1));
    int fd1 = program[1];
    f->eax = filesize(fd1);
    break;
  
  case SYS_READ:
    // Driver: Joel
    check_pointer( (void *) (program + 1));
    check_pointer( (void *) program[2]);
    check_pointer( (void *) (program + 3));
    int fd2 = program[1];
    void * buffer1 = (void *)program[2];
    unsigned size1 = (unsigned)program[3];   
    f->eax = read(fd2, buffer1, size1);
    break;

  case SYS_WRITE:
    check_pointer( (void *) (program + 1));
    check_pointer( (void *) program[2]);
    check_pointer( (void *) (program + 3));
    int fd3 = program[1];
    const void * buffer2 = (const void *)program[2];
    unsigned size2 = (unsigned)program[3];
    f->eax = write(fd3, buffer2, size2);
    break;

  case SYS_SEEK:
    check_pointer( (void *) (program + 1));
    check_pointer( (void *) (program + 2));
    int fd4 = program[1];
    unsigned position = program[2];
    seek(fd4, position);
    break;

  case SYS_TELL:
    check_pointer( (void *) (program + 1));
    int fd5 = program[1];
    f->eax = tell(fd5);
    break;

  case SYS_CLOSE:
    check_pointer( (void *) (program + 1));
    int fd6 = program[1];
    close(fd6);
    break;

  /* Filesys System Call Cases
     Driver: Joel and Ashley */
  case SYS_READDIR:
    check_pointer( (void *) (program + 1));
    check_pointer( (void *) (program + 2));
    const char* dir_filename = program[2];
    int fd7 = program[1];
    f->eax = readdir(fd7, dir_filename);
    break;


  case SYS_ISDIR:
    check_pointer( (void *) (program + 1));
    int fd8 = program[1];
    f->eax = isdir(fd8);
    break;
  

  case SYS_INUMBER:
    check_pointer( (void *) (program + 1));
    int fd9 = (int) program[1];
    f->eax = inumber(fd9);
    break;
  
  // /* FILESYS cases */
  case SYS_CHDIR:
    check_pointer( (void *) (program + 1));
    const char* dir_ptr1 = program[1];
    f->eax = chdir(dir_ptr1);
    break;

  case SYS_MKDIR:
    check_pointer( (void *) (program + 1));
    const char* dir_ptr2 = program[1];
    f->eax = mkdir(dir_ptr2);
    break;

  default:
    break;
  }
}

// Driver: Joel
/* Memory Access 
  Pointer cannot be null, point to kernel address space, or unmapped.
  Order matters because some things break first before others.
*/
void check_pointer(void * vaddr ){
  if(vaddr == NULL){
    exit(-1);
  }
  else if(is_kernel_vaddr(vaddr)){
    exit(-1);
  }
  else if(!pagedir_get_page(thread_current()->pagedir, vaddr)){
    exit(-1);
  }
}


// Driver: Ashley
/*
  Terminates Pintos by calling shutdown_power_off() 
  (declared in devices/shutdown.h). 
  This should be seldom used, because you 
  lose some information about possible 
  deadlock situations, etc.
*/
void halt(void)
{
  shutdown_power_off();
}


// Driver: Ashley
/*
  Creates a new file called file initially 
  initial_size bytes in size. Returns true 
  if successful, false otherwise.Creating a 
  new file does not open it: opening the new 
  file is a separate operation which would 
  require a open system call.
*/
bool create (const char *file, unsigned initial_size){
  lock_acquire(&filesys_lock);
  bool success = filesys_create(file, initial_size);
  lock_release(&filesys_lock);
  return success;
}


// Driver: Ashley
// Helper to ensure that the fd is
// Not out of the bounds of 0 - 127
static bool valid_fd(int fd){
  if(!(fd < 0 || fd > 127)){
    return true;
  }
  return false;
}

// Helper to find open fd or null file
// Driver: Michael
int get_fd(){
  for(int fd_idx = MIN_FD_VAL; fd_idx < MAX_FD_VAL; fd_idx++){
    if(!thread_current()->fd_list[fd_idx]){
      return fd_idx;
    }
  }
  return -1;
}

// Driver: Joel
/* Opens the file called file. Returns a nonnegative integer handle called a 
  "file descriptor" (fd) or -1 if the file could not be opened.
  File descriptors numbered 0 and 1 are reserved for the console: 
  fd 0 (STDIN_FILENO) is standard input, fd 1 (STDOUT_FILENO) is 
  standard output. The open system call will never return either 
  of these file descriptors, which are valid as system call arguments 
  only as explicitly described below.

  Each process has an independent set of file descriptors. File descriptors 
  are not inherited by child processes.
  When a single file is opened more than once, whether by a single process 
  or different processes, each open returns a new file descriptor. 
  Different file descriptors for a single file are closed independently 
  in separate calls to close and they do not share a file position.
 */
int open(const char *file /*, struct dir* directory*/) {
  lock_acquire(&filesys_lock);
  if(file == NULL || strlen(file) == 0){
    lock_release(&filesys_lock);
    return -1;
  }
  // Changed fd from -1 to fd_val, and made the open_file check after.
  int fd = get_fd();
  struct file *open_file = filesys_open(file);
  if (open_file == NULL) {
    lock_release(&filesys_lock);
    return -1;
  }
  if(fd == -1){
    close(thread_current()->fd_val);
    thread_current()->fd_val++;
    if(thread_current()->fd_val == MAX_FD_VAL){
      // 0 and 1 are reserved for READ and WRITE
      thread_current()->fd_val = MIN_FD_VAL;
    }
  }
  thread_current()->fd_list[fd] = open_file;
  lock_release(&filesys_lock);
  return fd;
}

// Driver: Michael
// Helper function to set the parent of a child thread in the list
// Pass in aux as the tid 
void set_parent(struct thread *t, void *aux){
  tid_t given_tid = (tid_t) aux;
  if (t->tid == given_tid){
    t->parent = thread_current();
  }
}

// Driver: Ashley
// Helper to find correct child based on a 
// Passed in tid 
struct thread* child_thr(tid_t tid_val)
{ 
  lock_acquire(&filesys_lock);
  struct list* child_list = &thread_current()->list_of_childs;
  if(!list_empty(child_list))
  {
    struct list_elem* child_ele = list_begin(child_list);
    while(child_ele != list_end(child_list))
    {
      if(tid_val == 
      list_entry(child_ele, struct thread, child_elem)->tid)
      {
        lock_release(&filesys_lock);
        return list_entry(child_ele, struct thread, child_elem); 
      }
      child_ele = list_next(child_ele);
    }
  }
  else{
    lock_release(&filesys_lock);
    return NULL;
  }
}

// Driver: Michael and Ashley
/* Runs the executable whose name is given in cmd_line, 
   passing any given arguments, and returns the new process's program 
   id (pid). Must return pid -1, which otherwise should not be a 
   valid pid, if the program cannot load or run for any reason. Thus, 
   the parent process cannot return from the exec until it knows whether 
   the child process successfully loaded its executable. You must use 
   appropriate synchronization to ensure this.*/
/* Modifications: Added error checking, and removes children if
   load was unsuccessful. */
pid_t exec(const char *cmd_line){
 
  if(cmd_line == NULL){
    return -1; // Error checking
  }
  tid_t tid = process_execute(cmd_line);
  pid_t pid = (pid_t) tid;
  if(pid == PID_ERROR){
    return PID_ERROR;
  }

  // Check for children
  struct thread * child =  child_thr(tid);
  if(child == NULL){
    return -1;
  }
  else{
    if(child->load_error == true){
      // Remove this child since process did not
      // Load successfully
      list_remove(&child->child_elem);  
      return -1;
    }
    return pid;
  }
  return pid;
}

// Driver Michael
/*
  Waits for a child process pid and retrieves the child's exit status.
  If pid is still alive, waits until it terminates. 
  Then, returns the status that pid passed to exit. 
  If pid did not call exit(), but was terminated by the kernel 
  (e.g. killed due to an exception), wait(pid) must return -1. It is 
  perfectly legal for a parent process to wait for child processes that 
  have already terminated by the time the parent calls wait, but the 
  kernel must still allow the parent to retrieve its child's exit status 
  or learn that the child was terminated by the kernel.
*/
int wait(pid_t pid){
  // Implemented in process.c
  return process_wait(pid);
}

// Driver: Michael and Joel
/*
  Terminates the current user program, returning status to the kernel. 
  If the process's parent waits for it, this is the status
  that will be returned. Conventionally, a status of 0 indicates success 
  and nonzero values indicate errors.
*/
void exit(int status){
  if(thread_current()->parent && status == 0){
    thread_current()->parent->child_exited = true;
    thread_current()->parent->child_status = status;
  }
  printf ("%s: exit(%d)\n", thread_current()->name, status);
  thread_current()->e_status = status;
  if (!lock_held_by_current_thread(&filesys_lock)) {
    lock_acquire(&filesys_lock);
  }
  if(thread_current()->curr_file){
    file_allow_write(thread_current()->curr_file);
  }
  lock_release(&filesys_lock);
  thread_exit();
}

// Driver Joel
/* 
  Returns the size, in bytes, of the file open as fd.
*/
int filesize(int fd) {
  lock_acquire(&filesys_lock);
  int size = 0;
  if(!valid_fd(fd)){
    lock_release(&filesys_lock);
    return size;
  }
  struct file * curr_file = thread_current()->fd_list[fd];
  if(curr_file == NULL){
    lock_release(&filesys_lock);
    return size;
  }
  // Have a file!
  size =  (int) file_length(curr_file);
  lock_release(&filesys_lock);
  return size;
}

// Driver Ashley
/* 
  Reads size bytes from the file open as fd into buffer.
  Returns the number of bytes actually read (0 at end of file),
  or -1 if the file could not be read (due to a condition other
  than end of file). fd 0 reads from the keyboard using input_getc(). 
*/
int read(int fd, void *buffer, unsigned size) { 
  lock_acquire(&filesys_lock);
  int bytes_read = 0;
  if(!valid_fd(fd)){ 
    lock_release(&filesys_lock); 
    return bytes_read; 
  }

  // Read from keyboard and save into buffer 
  if(fd == 0){
     uint8_t *buffer_casted = (uint8_t *) buffer;
     uint8_t val = input_getc();
     // Save key into first
     // Index of the buffer
     // Since it returns 1 key at a time
     buffer_casted[0] = val;
     // Size of the buffer
     bytes_read = size;
     lock_release(&filesys_lock);
    return bytes_read; 
  }
  // Use filesys command instead of keyboard
  struct file * curr_file = thread_current()->fd_list[fd];
  if(curr_file == NULL){
    lock_release(&filesys_lock); 
    return bytes_read;
  } 
  bytes_read = (int)file_read (curr_file, buffer, size);
  lock_release(&filesys_lock);
  return bytes_read;
}

// Driver Joel
/* Writes size bytes from buffer to the open file fd.

  Returns the number of bytes actually written, which may 
  be less than size if some bytes could not be written.

  Writing past end-of-file would normally extend the file, 
  but file growth is not implemented by the basic file 
  system. 

  The expected behavior is to write as many bytes 
  as possible up to end-of-file and return the actual number
  written, or 0 if no bytes could be written at all.

  fd 1 writes to the console. Your code to write to the 
  console should write all of buffer in one call to 
  putbuf(), at least as long as size is not bigger than 
  a few hundred bytes. 
  (It is reasonable to break up larger 
  buffers.) 

  Otherwise, lines of text output by different 
  processes may end up interleaved on the console, 
  confusing both human readers and our grading scripts.
*/
int write(int fd, const void *buffer, unsigned size) {
  lock_acquire(&filesys_lock);
  if(!valid_fd(fd)){
    lock_release(&filesys_lock);
    return 0;
  }
  int bytes_written = 0;
  struct file * curr_file = thread_current()->fd_list[fd];
  // Set file 
  // Write to console
  if(fd == 1){
    putbuf(buffer, (size_t) size);
    lock_release(&filesys_lock);
    return size;
  }

  if(curr_file != NULL){
      if(inode_is_subdir(file_get_inode(curr_file))){
        lock_release(&filesys_lock);
        return -1;
      }
      bytes_written = (int) file_write(curr_file, buffer, size);
      lock_release(&filesys_lock);
      return bytes_written;
  }
  else{
    lock_release(&filesys_lock);
    return 0;
  }
}

// Driver Joel
/* 
  Changes the next byte to be read or written in open file fd to position,
  expressed in bytes from the beginning of the file. 
  
  If Position == 0 -> file start
*/
void seek(int fd, unsigned position) {
  lock_acquire(&filesys_lock);

  if(!valid_fd(fd)){
    lock_release(&filesys_lock);
    return;
  }
  
  struct file * curr_file = thread_current()->fd_list[fd];
  // Have a file!

  if(curr_file != NULL){
      file_seek(curr_file, position);
      lock_release(&filesys_lock);
  }
  else{
    lock_release(&filesys_lock);
  }

}

// Driver Ashley
/* Returns the position of the next byte to be 
read or written in open file fd, expressed in 
bytes from the beginning of the file. */
unsigned tell(int fd) {
  lock_acquire(&filesys_lock);
  int error = -1;
  if(!valid_fd(fd)){
    lock_release(&filesys_lock);
    return error;
  }
  unsigned pos = 0;
  struct file * curr_file = thread_current()->fd_list[fd];
  // Have a file!
  if(curr_file != NULL){
      pos = (unsigned)file_tell(curr_file);
      lock_release(&filesys_lock);
      return pos;
  }
  else{
    lock_release(&filesys_lock);
    return error;
  }
}

/* Closes file descriptor fd. Exiting or terminating 
  a process implicitly
  closes all its open file descriptors, as if by calling 
  this function for each one. */
 // Driver Ashley
void close(int fd) {
  lock_acquire(&filesys_lock);
  if(!valid_fd(fd)){
    lock_release(&filesys_lock);
    return;
  }

  struct file * curr_file = thread_current()->fd_list[fd];
  // Have a file!
  if(curr_file != NULL){
      file_close(curr_file);
      // We null out the spot so it 
      // Can be filled by another file descriptor
      // Since NULL signifies the spot is empty in the array
      thread_current()->fd_list[fd] = NULL; 
      lock_release(&filesys_lock);
  }
  else{
    lock_release(&filesys_lock);
  }
}

// Driver: Joel
/*
  Deletes the file called file. Returns true 
  if successful, false otherwise. 
  A file may be removed regardless of whether
  it is open or closed, and removing an open file 
  does not close it. 
*/
bool remove (const char *file){
  lock_acquire(&filesys_lock);
  bool removed = filesys_remove (file);
  lock_release(&filesys_lock);
  return removed;
}

/* Comment headers from Pintos Guide 5.3.3 */
/*
  Driver: All of Us 
  Changes the current working directory of the process to dir, which may be 
  relative or absolute. Returns true if successful, false on failure. 
*/
bool chdir(const char* dir) { 
  if (!dir || strlen(dir) == 0)
  {
    return false;
  }
  if(!thread_current()->curr_dir
   || (dir_get_inode(thread_current()->curr_dir)) == ROOT_DIR_SECTOR){
    if(!strcmp(dir, "/") || !strcmp(dir, "..") || !strcmp(dir, ".")){
      return false;
    }
  }
  struct dir *directory = handle_rel_abs_dir(dir);
  if(!directory){
    return false;
  }
  dir_close(thread_current()->curr_dir);
  thread_current()->curr_dir = directory;
  return true;
}

/*
  Creates the directory named dir, which may be relative or absolute. 
  Returns true if successful, false on failure. Fails if dir already exists 
  or if any directory name in dir, besides the last, does not already exist. 
  That is, mkdir("/a/b/c") succeeds only if "/a/b" already exists and 
  "/a/b/c" does not. 
*/
// Driver: Michael and Joel
bool mkdir(const char* dir) {
  if(strlen(dir) == 0){
    return false;
  }
  struct dir *target = handle_rel_abs_dir(dir);
  if(target){
    // Directory exists
    dir_close(target);
    return false;
  }
  dir_close(target);
  // Check that the previous directory exists
  int index = strlen(dir) - 1;
  while(index >= 0 && dir[index] != '/'){
    index--;
  }
  index++;
  // Create the directory in root or thread's current directory
  if(index == 1 || index == 0){
    block_sector_t new_dir = 0;
    if(!free_map_allocate (1, &new_dir)
     || !dir_create(new_dir, DIR_CREATE_CNST)){
      return false;
    }
    char *name = index == 1 ? dir + 1 : dir;
    if(index == 0 && thread_current()->curr_dir != NULL){
      if(!dir_add(thread_current()->curr_dir, name, new_dir)){
        return false;
      }
      struct inode *inode = NULL;
      if(!dir_lookup(thread_current()->curr_dir, name, &inode)){
        inode_close(inode);
        return false;
      }
      struct dir *child = dir_open(inode);
      if(!dir_add(child, ".", new_dir) || !dir_add(child, "..", 
        inode_get_inumber(dir_get_inode(thread_current()->curr_dir)))){
           dir_close(child);
           return false;
      }
      dir_close(child);
    }else{
      if(!dir_add(dir_open_root(), name, new_dir)){
        return false;
      }
      struct inode *inode = NULL;
      if(!dir_lookup(dir_open_root(), name, &inode)){
        inode_close(inode);
        return false;
      }
      struct dir *child = dir_open(inode);
      if(!dir_add(child, ".", new_dir)
       || !dir_add(child, "..", ROOT_DIR_SECTOR)){
           dir_close(child);
           return false;
      }
      dir_close(child);
    }
    return true;
  }
  
  // Otherwise check that the target directory exists
  char *prev_name = calloc(1, strlen(dir) + 1);
  strlcpy(prev_name, dir, index);
  target = handle_rel_abs_dir(prev_name);
  free(prev_name);
  if(!target){
    // Directory does not exist
    return false;
  }
  // Allocate into target
  block_sector_t new_dir = 0;
  if(!free_map_allocate (1, &new_dir)
   || !dir_create(new_dir, DIR_CREATE_CNST)){
    return false;
  }
  if(!dir_add(target, dir + index, new_dir)){
    return false;
  }
      struct inode *inode = NULL;
      if(!dir_lookup(target, dir + index, &inode)){
        inode_close(inode);
        return false;
      }
      struct dir *child = dir_open(inode);  
      if(!dir_add(child, ".", new_dir)
       || !dir_add(child, "..", inode_get_inumber(dir_get_inode(target)))){
           dir_close(child);
           return false;
      }
  dir_close(child);
  return true;
}
/*
  Reads a directory entry from file descriptor fd, which must represent 
  a directory. If successful, stores the null-terminated file name in name, 
  which must have room for READDIR_MAX_LEN + 1 bytes, and returns true. 
  If no entries are left in the directory, returns false.

  "." and ".." should not be returned by readdir.

  If the directory changes while it is open, then it is acceptable for some 
  entries not to be read at all or to be read multiple times. Otherwise, each 
  directory entry should be read once, in any order.

  READDIR_MAX_LEN is defined in "lib/user/syscall.h". If your file system 
  supports longer file names than the basic file system, you should increase 
  this value from the default of 14.
*/
// Driver: Ashley
bool readdir(int fd, char *name) {
  struct file * curr_file = thread_current()->fd_list[fd];
   // Error checking
  if(curr_file == NULL){
    return false;
  }

  struct dir * curr_dir = (struct dir *) curr_file;
  struct inode *curr_inode = dir_get_inode(curr_dir);
  // Error checking
  if(curr_inode == NULL){
    return false;
  }
  // Need to check if is subdir
  bool sub_d = inode_is_subdir(curr_inode);
  if(!sub_d){
    return false;
  }

  bool continue_readdir = dir_readdir(curr_dir, name);
  // Special cases "." directory, and ".." directory.
  while(continue_readdir == true){
    if((strcmp(name, ".") != 0) && strcmp(name, "..") != 0){
      break;
    }
    else{
      continue_readdir =  dir_readdir(curr_dir, name);
    }
  }
  return continue_readdir;
}

/*  
  Returns true if fd represents a directory, false if 
  it represents an ordinary file. 
*/
// Driver: Ashley
bool isdir(int fd) {
  struct file * curr_file = thread_current()->fd_list[fd];
  // Error checking
  if(curr_file == NULL){
    return -1;
  }
  struct inode * inode = file_get_inode(curr_file);
  bool is_dir = inode_is_subdir(inode);
  return is_dir;
}

/*
  Returns the inode number of the inode associated with fd, which may 
  represent an ordinary file or a directory.

  An inode number persistently identifies a file or directory. It is 
  unique during the file's existence. In Pintos, the sector number of 
  the inode is suitable for use as an inode number.
*/
// Driver: Joel
int inumber(int fd) {
  struct file * curr_file = thread_current()->fd_list[fd];
  // Error checking
  if(curr_file == NULL){
    return -1;
  }
  struct inode * inode = file_get_inode(curr_file);
  // Error checking
  int inum = -1;
  if(inode == NULL){
    return inum;
  }
  else{
    inum =  inode_get_inumber(inode);
    return inum;
  }
}