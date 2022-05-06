#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

// Constants
/* 512 bytes per sector / 4 bytes per pointer */
#define BLOCKS_PER_INDIRECT 128 
#define NUM_DIRECT 123
#define NUM_TOTAL 16635
#define NUM_SINGLE 1
#define NUM_DOUBLE 1
/* NUM( DIRECT + SINGLE + DOUBLE ) pointers */
#define TOTAL_POINTERS 125
#define OPEN_CNT_CONSTANT 4

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */  
  block_sector_t direct[NUM_DIRECT]; // Array of 123 direct block pointers
  block_sector_t sgl_indirect;     // 1 single indirect block pointer
  block_sector_t dbl_indirect;    // 1 doubly indirect block pointer
  int is_sub_directory; // A modified bool for alignment that checks sub_dir
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size)
{
  return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int write_counter;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
  struct lock filegrow_lock; /* lock per inode */
};

/* Helper Method
   Returns true if inode_is_open, 
           false if not
*/
// Driver: Michael
bool inode_is_open(const struct inode *inode){
  return inode->open_cnt > OPEN_CNT_CONSTANT;
}

/* Helper method for system calls
   Parameters: an inode
   Returns true if the inode is a subdirectory 
           false if not
*/
// Driver: Ashley
bool inode_is_subdir (const struct inode *inode)
{  
  bool is_sub_d = (bool) inode->data.is_sub_directory;
  return is_sub_d;
}

bool map_inode_to_sect(block_sector_t sector, struct inode_disk *in_disk);

/* 
   Parameters: const struct inode *inode, off_t pos

   Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS.
   
   Modified to fit our fast file system implementation

   Three Cases:
   // Case 1: Direct - Returns index of direct block
   // Case 2: Single indirect - Returns index of correct singly indirect 
        block, not including direct block
   // Case 3: Double indirect - Returns index of correct doubly indirect
        block, not including direct block
*/
// Driver: Ashley
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos)
{
  ASSERT(inode != NULL);

  if (pos < 0 || inode->data.length < pos)
  {
    return -1;
  }

  off_t get_ptr_index = pos / BLOCK_SECTOR_SIZE;
  // Used for both singly indirect and doubly indirect      
  block_sector_t indirect[BLOCKS_PER_INDIRECT]; 
  
  if (get_ptr_index < NUM_DIRECT)
  {
    return inode->data.direct[get_ptr_index];
  }
  else if (get_ptr_index < NUM_DIRECT + BLOCKS_PER_INDIRECT)
  {
    // Read indirect data and put into indirect node array
    block_read(fs_device, inode->data.sgl_indirect, indirect);
    // Returns correct array index not including direct blocks
    return indirect[get_ptr_index - NUM_DIRECT];
  }
  else if (get_ptr_index < NUM_TOTAL)
  { 
    block_sector_t double_indr[BLOCKS_PER_INDIRECT];
    block_read(fs_device, inode->data.dbl_indirect, double_indr);
    
    off_t sngl_indir_index = (get_ptr_index
     - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) / BLOCKS_PER_INDIRECT; 
    off_t sngl_sect_index = (get_ptr_index
     - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) % BLOCKS_PER_INDIRECT;
    
    block_read(fs_device, double_indr[sngl_indir_index], indirect);
    return indirect[sngl_sect_index];
  }
  return -1;
}


/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
// Driver: Michael
static struct list open_inodes;


/* 
   Initializes the inode module. */
// Driver: Michael
void inode_init(void) { list_init(&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.

   Parameters: block_sector_t sector, off_t length, bool dir

   Return: true if successful.
           false if memory or disk allocation fails. */
// Driver: Ashley
bool inode_create(block_sector_t sector, off_t length, bool dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL)
  {
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->is_sub_directory = dir; 
    // Pass in an pointers into free_map_allocate and assign them to blocks
    if (map_inode_to_sect(sectors, disk_inode))
    {
      // Write the inode to block
      block_write(fs_device, sector, disk_inode);
      success = true;
    }
      free (disk_inode);
  }
  
  return success;
}


/* Allocate an inode to a given sector depending on size of sector
   Returns early once finished, otherwise allocate direct blocks,
   singly indirect blocks and doubly indirect blocks as needed.
   
   Parameters: block_sector_t sector, struct inode_disk pointer

   Returns: true if mapping succeeded,
            false if mapping failed
*/
// Driver: All of Us
bool map_inode_to_sect(block_sector_t sector, struct inode_disk *in_disk)
{

  /* Check if we have enough inodes to fill up all direct block
     allocations, or if we also need more indirect blocks */
  int num_direct_needed = NUM_DIRECT;  
  if(sector < NUM_DIRECT){
    num_direct_needed = sector;
  }

  // Allocate direct blocks
  for (int direct_idx = 0; direct_idx < num_direct_needed; direct_idx++)
  {
    block_sector_t *curr_sect = &in_disk->direct[direct_idx];
    // Allocate one sector
    if (free_map_allocate(1, curr_sect)) {
      char* zero = calloc(1, BLOCK_SECTOR_SIZE);
      block_write(fs_device, *curr_sect, zero);
      free(zero);
    }
    else {
      return false;
    }

  }
  int num_left = sector - num_direct_needed;
  if (num_left == 0) {
    // Early Return! Everything allocated using only direct blocks 
    return true;
  }
  // Allocate singly indirect blocks
  else {
    sector = num_left;
  }
  // Need one singly indirect
  // Check how many direct blocks are needed from singly indirect
    int num_indir_dir = sector;
    if (BLOCKS_PER_INDIRECT < sector) {
      num_indir_dir = BLOCKS_PER_INDIRECT;
    }
    
      // Allocate for singly indirect now
      // Save direct blocks into an array
      block_sector_t direct[BLOCKS_PER_INDIRECT];
      if (free_map_allocate(1, &in_disk->sgl_indirect)) {
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, in_disk->sgl_indirect, zeros);
        free(zeros);
      }
      else {
        return false;
      }
      // Allocate direct in indirect
      for (int direct_idx = 0; direct_idx < num_indir_dir; direct_idx++)
      {
        if (! free_map_allocate(1, &direct[direct_idx]))
        {
          return false;
        }
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, direct[direct_idx], zeros);
        free(zeros);
      }
      // Write the actual singly indirect block now
      block_write(fs_device, in_disk->sgl_indirect, direct);

      int num_left_over = sector - num_indir_dir;
      if(num_left_over == 0){
        return true;
      }
      else{
        sector = num_left_over;
      }

      // Need doubly indirect
      int num_dir_double = sector;
      if(BLOCKS_PER_INDIRECT * BLOCKS_PER_INDIRECT < sector){
        num_dir_double = (BLOCKS_PER_INDIRECT * BLOCKS_PER_INDIRECT);
      }
       
      int num_allocated_dbl = num_dir_double;
      if (free_map_allocate(1, &in_disk->dbl_indirect))
      {
        char* zeros2 = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, in_disk->dbl_indirect, zeros2);
        free(zeros2);
      }
      else
      {
        return false;
      }

      // Allocate doubly indirect blocks, first index into singly block.
      block_sector_t indir_blocks[BLOCKS_PER_INDIRECT];

      for (int indirect_idx = 0; indirect_idx < BLOCKS_PER_INDIRECT;
       indirect_idx++) \
       {
        if(num_dir_double <= 0){
          // Early exit
          indirect_idx = BLOCKS_PER_INDIRECT; 
        }
        int num_alloc = BLOCKS_PER_INDIRECT;
        if(num_dir_double < BLOCKS_PER_INDIRECT){
          num_alloc = num_dir_double;
        }
        block_sector_t *curr_block = &indir_blocks[indirect_idx];
        // Allocate single dir
        if (free_map_allocate (1, curr_block))
        {
          static char clear_zero[BLOCK_SECTOR_SIZE];
          block_write (fs_device, curr_block, clear_zero);
        }
        else{
          return false;
        }

        block_sector_t blocks[BLOCKS_PER_INDIRECT]; 
        for ( int indirect_idx = 0; indirect_idx < num_alloc; indirect_idx++)
        {
          if (free_map_allocate (1, &blocks[indirect_idx]))
          {
            static char clear_zero2[BLOCK_SECTOR_SIZE];
            block_write (fs_device, &blocks[indirect_idx], clear_zero2);
          }
          else {
            return false;            
          }
        }
        block_write (fs_device, &indir_blocks[indirect_idx], blocks);
        num_dir_double -= num_alloc;
      }
      // Write doubly indirect
      block_write (fs_device, &in_disk->dbl_indirect, indir_blocks);

      int num_left_double  = sector - num_allocated_dbl;
      if(num_left_double == 0){
        return true;
      }
      else{
        // File too big to handle
        // Should never happen in pintos, as max file size 8 MB met.
        return false;
      }  
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. 

  Parameter: block_sector_t sector

  Returns: struct inode pointer
*/
// Driver: Joel
struct inode *inode_open(block_sector_t sector)
{
  struct list_elem *inode_elem;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (inode_elem = list_begin(&open_inodes);
       inode_elem != list_end(&open_inodes);
       inode_elem = list_next(inode_elem))
  {
    inode = list_entry(inode_elem, struct inode, elem);
    if (inode->sector == sector)
    {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  // Not null so we can initalize a lock for inode
  lock_init(&inode->filegrow_lock);

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);  
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->write_counter = 0;
  inode->removed = false;
  block_read(fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen(struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode *inode)
{
  return inode->sector;
}

/* 
   Closes INODE and writes it to disk. 
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. 
   
   Parameters: struct inode pointer

   Return: nothing 
*/
// Driver: Joel
void inode_close(struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;
  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);
    
    /* Deallocate blocks if removed. */
    if (inode->removed)
    {
      free_map_release(inode->sector, 1);
      for(int i = 0; i < inode->data.length; i += BLOCK_SECTOR_SIZE){
        free_map_release(byte_to_sector(inode, i), 1);
      }
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode)
{
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Helper Method
   Parameter: struct inode pointer
   Returns true if passed in inode has been removed */
// Driver: Joel
bool inode_removed(struct inode *inode){
  return inode->removed;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   
   Parameters: struct inode pointer, a void pointer buffer, offset size,
   standard offset
   
   Returns: the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
// Driver: Joel
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size,
                    off_t offset)
{
  if(offset + size > inode->data.length){
    if(!lock_held_by_current_thread(&inode->filegrow_lock)){
      lock_acquire(&inode->filegrow_lock);
    }
  }
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;
  while (size > 0)
  {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
    {
      /* Read full sector directly into caller's buffer. */
      block_read(fs_device, sector_idx, buffer + bytes_read);
    }
    else
    {
      /* Read sector into bounce buffer, then partially copy
         into caller's buffer. */
      if (bounce == NULL)
      {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      block_read(fs_device, sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);
  if(lock_held_by_current_thread(&inode->filegrow_lock)){
      lock_release(&inode->filegrow_lock);
  }
  return bytes_read;
}

/* Helper function for dir_index

   Parameters: offset pos

   Returns: index of direct in doubly indirect pointer
*/
// Driver: Ashley
int get_dir_index(off_t pos){
  int index = pos - (NUM_DIRECT + BLOCKS_PER_INDIRECT);
  int dir_index = (index / BLOCKS_PER_INDIRECT) % BLOCKS_PER_INDIRECT;
  return dir_index;
}

/* Helper function to calculate how many of each block type needed for file
   growth. 
   
   Parameters: an int: original last sector, 
   offset to find file growth length, and the inode to grow.

   Returns: True if grow_file succeeded
            False if grow_file failed
*/
// Driver: All of Us 
static bool grow_file(int original, off_t pos, struct inode *inode){
  // First find the last block
  block_sector_t num_dir_needed = pos / BLOCK_SECTOR_SIZE;
  /* For each condition, write num_dir_needed blocks
    starting from last_block + 1 */
  if (original + num_dir_needed > NUM_TOTAL){
    return false;
  }
  if (original + 1 < NUM_DIRECT){
    // Both single and double indirect not allocated
    int num_left = NUM_DIRECT - (original + 1);
    int num_ind = num_dir_needed - num_left;
    int num_dub = 0;
    if(num_ind > BLOCKS_PER_INDIRECT){
      num_dub = num_ind - BLOCKS_PER_INDIRECT;
    }
    if (num_dir_needed <= num_left){
      
      for (int direct_index = original + 1;
       direct_index <= original + num_dir_needed; direct_index++)
      {
        block_sector_t *curr_sect = &inode->data.direct[direct_index];
        if (! free_map_allocate (1, curr_sect))
        {
          return false;
        }
        char* zero = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, *curr_sect, zero);
        free(zero);
      }
    }else if (num_dub == 0){
      // First write num left
      for (int direct_index = original + 1;
       direct_index <= original + num_left; direct_index++)
      {
        block_sector_t *curr_sect = &inode->data.direct[direct_index]; 
        if (! free_map_allocate (1, curr_sect))
        {
          return false;
        }
        char* zero = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, *curr_sect, zero);
        free(zero);
      }
      // Now write the rest into indirect block
      block_sector_t direct[BLOCKS_PER_INDIRECT];
      // Allocate index block
      if (free_map_allocate(1, &inode->data.sgl_indirect))
      {
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, inode->data.sgl_indirect, zeros);
        free(zeros);
      }
      else
      {
        return false;
      }
      for (int direct_index = 0; direct_index < num_ind; direct_index++)
      {
        // Now allocate the direct blocks in the singly indirect
        if (! free_map_allocate(1, &direct[direct_index]))
        {
          return false;
        }
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, direct[direct_index], zeros);
        free(zeros);
      }
      // Write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);
    }else{
      /* Now allocate into doubly indirect blocks,
         must index through direct and singly indirect blocks */
      for (int direct_index = original + 1;
       direct_index <= original + num_left;
       direct_index++)
      {
        block_sector_t *curr_sect = &inode->data.direct[direct_index];
        if (! free_map_allocate (1, curr_sect))
        {
          return false;
        }
        char* zero = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, *curr_sect, zero);
        free(zero);
      }
      // Now write the rest into indirect block
      block_sector_t direct[BLOCKS_PER_INDIRECT];
      // Allocate index block
      if (free_map_allocate(1, &inode->data.sgl_indirect))
      {
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, inode->data.sgl_indirect, zeros);
        free(zeros);
      }
      else
      {
        return false;
      }
      for (int direct_index = 0; direct_index < num_ind; direct_index++)      
      {
        // Now allocate the direct blocks in the singly indirect 
        if (! free_map_allocate(1, &direct[direct_index]))
        {
          return false;
        }
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, direct[direct_index], zeros);
        free(zeros);
      }
      // Write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);
      // Write the double indirect blocks
      if (free_map_allocate(1, &inode->data.dbl_indirect))
      {
        char* zeros2 = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, inode->data.dbl_indirect, zeros2);
        free(zeros2);
      }
      else
      {
        return false;
      }
      int num_dir_allocated = num_dub;
      int num_dir_total = BLOCKS_PER_INDIRECT * BLOCKS_PER_INDIRECT;
      if (num_dir_needed > num_dir_total)
      {
        num_dir_allocated = num_dir_total;
      }

      int num_dir_fr_dbl = num_dir_allocated;
      
      block_sector_t indir_blocks[BLOCKS_PER_INDIRECT];
      for (int indirect_idx = 0; indirect_idx < BLOCKS_PER_INDIRECT;
       indirect_idx++)
      {
        if (num_dir_allocated <= 0)
        {
          // Early exit
          indirect_idx = BLOCKS_PER_INDIRECT;
        }
        int num_indir_dir2 = num_dir_allocated < BLOCKS_PER_INDIRECT
         ? num_dir_allocated : BLOCKS_PER_INDIRECT;

        // Initialize singly indirect block array
        block_sector_t direct_dbl[BLOCKS_PER_INDIRECT];
        // Allocate index block
        if (free_map_allocate(1, &indir_blocks[indirect_idx]))
        {
          char* zeros_dbl = calloc(1, BLOCK_SECTOR_SIZE);
          block_write(fs_device, indir_blocks[indirect_idx], zeros_dbl);
          free(zeros_dbl);
        }
        else
        {
          return false;
        }


        for (int dbl_indirect_idx = 0; dbl_indirect_idx < num_indir_dir2;
         dbl_indirect_idx++)
        {
          // Now allocate the direct blocks in the singly indirect
          if (free_map_allocate(1, &direct_dbl[dbl_indirect_idx]))
          {
            return false;
         }
          char* zeros_dbl2 = calloc(1, BLOCK_SECTOR_SIZE);
          block_write(fs_device, direct_dbl[dbl_indirect_idx], zeros_dbl2);
          free(zeros_dbl2);
        }
         block_write (fs_device, indir_blocks[indirect_idx], direct_dbl);

        num_dir_allocated -= num_indir_dir2;


      }

       block_write (fs_device, inode->data.dbl_indirect, indir_blocks);

        if(num_dir_needed - num_dir_fr_dbl == 0){
          return true;
        }
        else{
          return false;
        }
    }
  }else if (original + 1 < NUM_DIRECT + BLOCKS_PER_INDIRECT){
    /* If the original block is one less than first indirect,
     need to map indirect */
    if(original + 1 == NUM_DIRECT){
      if (free_map_allocate(1, &inode->data.sgl_indirect)) {
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, inode->data.sgl_indirect, zeros);
        free(zeros);
      }
      else {
        return false;
      }
    }
    // Single indirect allocated but double indirect not
    // Both single and double indirect not allocated
    int num_left = (NUM_DIRECT + BLOCKS_PER_INDIRECT) - (original + 1);
    int num_ind = num_dir_needed - num_left;
    int num_dub = 0;
    if(num_ind > num_left){
      num_dub = num_ind - num_left;
    }
    if(num_dir_needed <= num_left){
      block_sector_t direct[BLOCKS_PER_INDIRECT];
      block_read(fs_device, inode->data.sgl_indirect, direct);
      // Indirect block should already be allocated
      int max = num_dir_needed < num_left ? num_dir_needed : num_left;
      int start = (original + 1) - NUM_DIRECT;
      for (int direct_idx = start; direct_idx <= start + max; direct_idx++)
      {
        // Now allocate the direct blocks in the singly indirect
        if (! free_map_allocate(1, &direct[direct_idx]))
        {
          return false;
        }
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, direct[direct_idx], zeros);
        free(zeros);
      }
      // Write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);
    }else{
      // First allocate single direct
      block_sector_t direct[BLOCKS_PER_INDIRECT];
      block_read(fs_device, inode->data.sgl_indirect, direct);
      // Indirect block should already be allocated
      for (int direct_idx = original + 1; direct_idx < num_left; direct_idx++)
      {
        // Now allocate the direct blocks in the singly indirect
        if (! free_map_allocate(1, &direct[direct_idx]))
        {
          return false;
        }
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, direct[direct_idx], zeros);
        free(zeros);
      }
      // Write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);

      // Now allocate double indirect
      if (free_map_allocate(1, &inode->data.dbl_indirect))
      {
        char* zeros2 = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, inode->data.dbl_indirect, zeros2);
        free(zeros2);
      }
      else
      {
        return false;
      }
      int num_dir_allocated = num_dub;
      int num_dir_total = BLOCKS_PER_INDIRECT * BLOCKS_PER_INDIRECT;
      if (num_dir_needed > num_dir_total)
      {
        num_dir_allocated = num_dir_total;
      }

      int num_dir_fr_dbl = num_dir_allocated;

      
      block_sector_t indir_blocks[BLOCKS_PER_INDIRECT];
      for (int indirect_idx = 0; indirect_idx < BLOCKS_PER_INDIRECT;
       indirect_idx++)
      {
        if (num_dir_allocated <= 0)
        {
          // Early exit
          indirect_idx = BLOCKS_PER_INDIRECT;
        }
        int num_indir_dir2 = num_dir_allocated < BLOCKS_PER_INDIRECT
         ? num_dir_allocated : BLOCKS_PER_INDIRECT;

        // Allocate singly indir
        block_sector_t direct_dbl[BLOCKS_PER_INDIRECT];
        // Allocate index block
        if (free_map_allocate(1, &indir_blocks[indirect_idx]))
        {
          char* zeros_dbl = calloc(1, BLOCK_SECTOR_SIZE);
          block_write(fs_device, indir_blocks[indirect_idx], zeros_dbl);
          free(zeros_dbl);
        }
        else
        {
          return false;
        }

        for (int dbl_indirect_idx = 0; dbl_indirect_idx < num_indir_dir2;
         dbl_indirect_idx++)
        {
          // Now allocate the direct blocks in the singly indirect
          if (! free_map_allocate(1, &direct_dbl[dbl_indirect_idx]))
          {
            return false;
         }
          char* zeros_dbl2 = calloc(1, BLOCK_SECTOR_SIZE);
          block_write(fs_device, direct_dbl[dbl_indirect_idx], zeros_dbl2);
          free(zeros_dbl2);
        }
         block_write (fs_device, indir_blocks[indirect_idx], direct_dbl);

        num_dir_allocated -= num_indir_dir2;


      }

       block_write (fs_device, inode->data.dbl_indirect, indir_blocks);

        if(num_dir_needed - num_dir_fr_dbl == 0){
          return true;
        }
        else{
          return false;
        }
    }
  }else{
    /* If the next block we need to allocate is the first dbl-indirect block 
       allocate the pointer */
    if(original + 1 == NUM_DIRECT + BLOCKS_PER_INDIRECT){
      if (free_map_allocate(1, &inode->data.dbl_indirect)) {
        char* zeros = calloc(1, BLOCK_SECTOR_SIZE);
        block_write(fs_device, inode->data.dbl_indirect, zeros);
        free(zeros);
      }
      else {
        return false;
      }
    }
    // Double indirect allocated already
    int num_left = NUM_TOTAL - (original + 1);
    int num_dub = num_dir_needed;
    int start = ((original + 1) - (NUM_DIRECT + BLOCKS_PER_INDIRECT))
     / BLOCKS_PER_INDIRECT;

      block_sector_t indir_blocks[BLOCKS_PER_INDIRECT];
      block_read (fs_device, inode->data.dbl_indirect, indir_blocks);
      for (int indirect_idx = start; indirect_idx < BLOCKS_PER_INDIRECT;
       indirect_idx++)
      {
        if (num_dub <= 0)
        {
          indirect_idx = BLOCKS_PER_INDIRECT;
        }
        int num_indir_dir2 = num_dub < BLOCKS_PER_INDIRECT ? num_dub :
         BLOCKS_PER_INDIRECT;

        // Allocate singly indir
        block_sector_t direct_dbl[BLOCKS_PER_INDIRECT];
        if(indirect_idx == start && ((original + 1)
         - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) % BLOCKS_PER_INDIRECT != 0){
          // Indirect block exists
          block_read(fs_device, indir_blocks[indirect_idx], direct_dbl);
        }else{
          // Allocate index block
          if (free_map_allocate(1, &indir_blocks[indirect_idx]))
          {
            char* zeros_dbl = calloc(1, BLOCK_SECTOR_SIZE);
            block_write(fs_device, indir_blocks[indirect_idx], zeros_dbl);
            free(zeros_dbl);
          }
          else
          {
            return false;
          }
        }

        for (int dbl_indirect_idx = 0; dbl_indirect_idx < num_indir_dir2;
         dbl_indirect_idx++)
        {
          if(indirect_idx == start){
            dbl_indirect_idx = ((original + 1)
             - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) % BLOCKS_PER_INDIRECT;
          }
          // Now allocate the direct blocks in the singly indirect
          if (! free_map_allocate(1, &direct_dbl[dbl_indirect_idx]))
          {
            return false;
          }
          char* zeros_dbl2 = calloc(1, BLOCK_SECTOR_SIZE);
          block_write(fs_device, direct_dbl[dbl_indirect_idx], zeros_dbl2);
          free(zeros_dbl2);
        }
         block_write (fs_device, indir_blocks[indirect_idx], direct_dbl);

        num_dub -= num_indir_dir2;

      }

       block_write (fs_device, inode->data.dbl_indirect, indir_blocks);
  }
  return true;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   
   Returns: the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs. */
// Driver: Michael and Ashley
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
                     off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;
  if (inode->write_counter)
    return 0;

  // Driver: Michael
  // Grow file
  off_t eof = offset + size;
  off_t old_length = inode_length (inode);
  int old_aligned =  old_length - (old_length % BLOCK_SECTOR_SIZE);
  int new_start = old_length == 0 ? 0 : old_aligned + BLOCK_SECTOR_SIZE;

  // File growth necessary
  if(eof > old_length){

    // Need to calculate the original last sector.
    int last_sector = old_length == 0 ? -1 : old_aligned/BLOCK_SECTOR_SIZE;
    off_t bytes_to_add = eof - new_start;
    // Allocate new space and grow
    if(bytes_to_add >= 0){
      // Must grow
      // Driver: Ashley
      if(!lock_held_by_current_thread (&inode->filegrow_lock)){
        lock_acquire(&inode->filegrow_lock);
      } 
      grow_file(last_sector, bytes_to_add + BLOCK_SECTOR_SIZE, inode);
    }
      inode->data.length = eof;
      block_write(fs_device, inode->sector, &inode->data);
     if(lock_held_by_current_thread (&inode->filegrow_lock)){
       lock_release(&inode->filegrow_lock);
     } 
  }

  while (size > 0)
  {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
    {
      /* Write full sector directly to disk. */
      block_write(fs_device, sector_idx, buffer + bytes_written);
    }
    else
    {
      /* We need a bounce buffer. */
      if (bounce == NULL)
      {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }

      /* If the sector contains data before or after the chunk
         we're writing, then we need to read in the sector
         first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left)
        block_read(fs_device, sector_idx, bounce);
      else
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      block_write(fs_device, sector_idx, bounce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);

  return bytes_written;
}


/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode *inode)
{
  inode->write_counter++;
  ASSERT(inode->write_counter <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode)
{
  ASSERT(inode->write_counter > 0);
  ASSERT(inode->write_counter <= inode->open_cnt);
  inode->write_counter--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) { return inode->data.length; }
