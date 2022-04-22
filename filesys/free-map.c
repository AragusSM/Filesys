#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

#define BLOCKS_PER_INDIRECT 128 //512/4
#define NUM_DIRECT 120
#define NUM_TOTAL 16632

static struct file *free_map_file; /* Free map file. */
static struct bitmap *free_map;    /* Free map, one bit per sector. */

/* Initializes the free map. */
void free_map_init (void)
{
  free_map = bitmap_create (block_size (fs_device));
  if (free_map == NULL)
    PANIC ("bitmap creation failed--file system device is too large");
  bitmap_mark (free_map, FREE_MAP_SECTOR);
  bitmap_mark (free_map, ROOT_DIR_SECTOR);
}

//helper function to create a allocate a header block
static block_sector_t create_header(block_sector_t *sectorp, int val){
  block_sector_t* indirect_block = sectorp[val];
      *indirect_block = bitmap_scan_and_flip (free_map, 0, 1, false); 
      if( *indirect_block != BITMAP_ERROR && free_map_file != NULL &&
        !bitmap_write (free_map, free_map_file)){
          *indirect_block = BITMAP_ERROR;
      }
      if(*indirect_block = BITMAP_ERROR){
        bitmap_reset(free_map, *indirect_block);
        return BITMAP_ERROR;
      }
    return *indirect_block;
}

//helper to allocate direct blocks. Returns array of 512 bytes to buffer if needed
static bool allocate_direct(size_t cnt, block_sector_t *sectorp, block_sector_t *buffer, bool is_root){
    //stores allocated sectors to sector nums in order to unset bits if bitmap error occurs.
    block_sector_t sector_nums[cnt];
    for(int i = 0; i < cnt; i++){
      block_sector_t sector = bitmap_scan_and_flip (free_map, 0, 1, false);
      sector_nums[i] = sector;
      if(buffer){
        buffer[i] = sector;
      }
      if (sector != BITMAP_ERROR && free_map_file != NULL &&
          !bitmap_write (free_map, free_map_file))
        {
          //bitmap_set_multiple (free_map, sector, 1, false);
          sector = BITMAP_ERROR;
        }
      if (sector == BITMAP_ERROR);
      {
        //reset sectors 0 to i in case of error
        for(int j = 0; j <= i; j++){
          bitmap_reset(free_map, sector_nums[j]);
        }
        return false;
      }
      //if less than NUM_DIRECT, set the direct pointers to the sectors
      if(is_root){
         block_sector_t* direct_block = sectorp[0] + i;
        *direct_block = sector;
      }
    }
    return true;
}

/* Allocates CNT (CHANGED) sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough
   sectors were available or if the free_map file could not be
   written. */
bool free_map_allocate (size_t cnt, block_sector_t *sectorp)
{
  
  if(cnt > NUM_DIRECT){
    //setup bitmap for single indirect block
    if(cnt <= NUM_DIRECT + BLOCKS_PER_INDIRECT){
      block_sector_t indirect_block = create_header(sectorp, 1);
      if(indirect_block == BITMAP_ERROR){
        return false;
      }
     
     //first allocate direct
     if(!allocate_direct(NUM_DIRECT, sectorp, NULL, true)){
       return false;
     }

    //add blocks to indirect
    block_sector_t sector_nums[BLOCKS_PER_INDIRECT];
    if(!allocate_direct(cnt - NUM_DIRECT, sectorp, sector_nums, false)){
      return false;
    }

    //write the buffer filled with blocks to the indirect header
    block_write(fs_device, indirect_block, sector_nums);
    sectorp[1] = indirect_block;
    //setup bitmap for double indirect
    }else{
      if(cnt > NUM_TOTAL){
        return false;
      }
      block_sector_t dbl_indirect_block = create_header(sectorp, 2);
      if(dbl_indirect_block == BITMAP_ERROR){
        return false;
      }

      block_sector_t indirect_block = create_header(sectorp, 1);
      if(indirect_block == BITMAP_ERROR){
        return false;
      }
      //first allocate direct
     if(!allocate_direct(NUM_DIRECT, sectorp, NULL, true)){
       return false;
     }

      //add blocks to indirect
      block_sector_t sector_nums[BLOCKS_PER_INDIRECT];
      if(!allocate_direct(cnt - NUM_DIRECT, sectorp, sector_nums, false)){
        return false;
      }

      //write the buffer filled with blocks to the indirect header
      block_write(fs_device, indirect_block, sector_nums);
      sectorp[1] = indirect_block;

      //allocate double indirect blocks
      off_t total = (cnt - (NUM_DIRECT + BLOCKS_PER_INDIRECT));
      off_t num_sgl_blocks = total / BLOCKS_PER_INDIRECT;
      if(total % BLOCKS_PER_INDIRECT != 0){
        num_sgl_blocks++;
      }
      //stores allocated sectors to sector nums in order to unset bits if bitmap error occurs.
      block_sector_t sector_nums2[BLOCKS_PER_INDIRECT];
      for(int i = 0; i < num_sgl_blocks; i++){
        block_sector_t ind_block = bitmap_scan_and_flip (free_map, 0, 1, false);
        sector_nums2[i] = ind_block;
        if( ind_block != BITMAP_ERROR && free_map_file != NULL &&
          !bitmap_write (free_map, free_map_file)){
          ind_block = BITMAP_ERROR;
        }
        if(ind_block = BITMAP_ERROR){
          for(int j = 0; j <= i; j++){
            bitmap_reset(free_map, sector_nums2[j]);
          }
          return false;
        }
        //for each indirect allocate direct blocks
        block_sector_t num_left = cnt - (i * BLOCKS_PER_INDIRECT) >= BLOCKS_PER_INDIRECT ? 
        BLOCKS_PER_INDIRECT : cnt - (i * BLOCKS_PER_INDIRECT);
        //add blocks to indirect
        block_sector_t sector_nums3[BLOCKS_PER_INDIRECT];
        if(!allocate_direct(num_left, sectorp, sector_nums3, false)){
          return false;
        }

        //write the buffer filled with blocks to the indirect header
        block_write(fs_device, ind_block, sector_nums3);
      }
      block_write(fs_device, dbl_indirect_block, sector_nums2);
      sectorp[2] = indirect_block;
    }
  }else{
    //only direct allocations needed
    if(!allocate_direct(cnt, sectorp, NULL, true)){
       return false;
    }
  }
  return true;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void free_map_release (block_sector_t sector, size_t cnt)
{
  ASSERT (bitmap_all (free_map, sector, cnt));
  bitmap_set_multiple (free_map, sector, cnt, false);
  bitmap_write (free_map, free_map_file);
}

/* Opens the free map file and reads it from disk. */
void free_map_open (void)
{
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_read (free_map, free_map_file))
    PANIC ("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
void free_map_close (void) { file_close (free_map_file); }

/* Creates a new free map file on disk and writes the free map to
   it. */
void free_map_create (void)
{
  /* Create inode. */
  if (!inode_create (FREE_MAP_SECTOR, bitmap_file_size (free_map), false))
    PANIC ("free map creation failed");

  /* Write bitmap to file. */
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_write (free_map, free_map_file))
    PANIC ("can't write free map");
}
