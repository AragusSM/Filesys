#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

//Constants
#define BLOCKS_PER_INDIRECT 128 //512/4
#define NUM_DIRECT 123
#define NUM_TOTAL 16632
#define NUM_SINGLE 1
#define NUM_DOUBLE 1
#define TOTAL_POINTERS 125

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  //block_sector_t pointers[3]; /* stores first direct block, sgl_indirect, and dbl_indirect */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  //uint32_t unused[125];  
  block_sector_t direct[NUM_DIRECT]; //array of 120 pointers to sectors aka direct blocks
  block_sector_t sgl_indirect;     //1 single indirect
  block_sector_t dbl_indirect;    //1 doubly indirect
  int is_directory; //should be a bool but modified for alignment purposes
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
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

// /* Returns the block device sector that contains byte offset POS
//    within INODE.
//    Returns -1 if INODE does not contain data for a byte at offset
//    POS. */
// static block_sector_t byte_to_sector (const struct inode *inode, off_t pos)
// {
//   ASSERT (inode != NULL);
//   if (pos < inode->data.length)
//     return inode->data.start + pos / BLOCK_SECTOR_SIZE;
//   else
//     return -1;
// }

bool map_inode_to_sect(block_sector_t sector, struct inode_disk *in_disk);

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos)
{
  ASSERT(inode != NULL);
  if (pos < 0 || inode->data.length < pos)
  {
    return -1;
  }
  off_t index = pos / BLOCK_SECTOR_SIZE;        // get pointer index # in array to check what kind of pointer it is
  block_sector_t indirect[BLOCKS_PER_INDIRECT]; // used for both singly indirect and doubly direct
  // case 1: direct EASIEST CASE
  if (index < NUM_DIRECT)
  {
    // can directly return block since it is direct
    return inode->data.direct[index];
  }
  // case 2: singly indirect
  else if (index < NUM_DIRECT + BLOCKS_PER_INDIRECT)
  {
    // read indirect data and put into indirect node array
    block_read(fs_device, inode->data.sgl_indirect, indirect);
    return indirect[index - NUM_DIRECT]; // need to return correct array index not including direct blocks
  }
  // case 3: doubly indirect
  else if (index < NUM_TOTAL)
  { // max index for all possible pointers
    // not sure yet how to read this in...
    // need to read and put in buffer double indirect reads
    // need to parse out indirect blocks
    // and return based on indirect block direct pointer
    // create a buffer of indirect pointers since we have 128 indirect pointers
    block_sector_t double_indr[BLOCKS_PER_INDIRECT];
    block_read(fs_device, inode->data.dbl_indirect, double_indr);
    // filled up with singly indirect pointers now
    off_t sngl_indir_index = (index - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) / BLOCKS_PER_INDIRECT; // indirect block num
    // find singl indirect sector number by using %
    off_t sngl_sect_index = (index - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) % BLOCKS_PER_INDIRECT;
    // now need to read the data in the singular indirect and put in buffer
    // similar to case 2
    block_read(fs_device, double_indr[sngl_indir_index], indirect);
    return indirect[sngl_sect_index];
  }
  return -1;
}


/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
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
    disk_inode->is_directory = dir; // added
    //pass in an pointers into free_map_allocate and assign them to blocks
    if (map_inode_to_sect(sectors, disk_inode))
    {
      //write the inode to block
      block_write(fs_device, sector, disk_inode);
      success = true;
    }
      free (disk_inode);
  }
  
  return success;
}

// // allocate an inode to a given sector depending on size of sector
bool map_inode_to_sect(block_sector_t sector, struct inode_disk *in_disk)
{

  // have to check if we have enought to fill up all direct blocks
  // or if we need more indirect blocks as well...
  bool need_sng_indir = false;
  int num_direct_needed = 0;
  if (sector <= NUM_DIRECT)
  {
    num_direct_needed = sector;
  }
  if (sector > NUM_DIRECT)
  {
    need_sng_indir = true;
  }

 if (need_sng_indir == false)
  {
    // directly allocate
    for (int i = 0; i < num_direct_needed; i++)
    {
      block_sector_t *curr_sect = &in_disk->direct[i];
      // allocate one sector
      bool allocat = free_map_allocate(1, curr_sect);
      if (allocat == false)
      {
        return false;
      }
      else
      {
        char zero[BLOCK_SECTOR_SIZE];
        block_write(fs_device, *curr_sect, zero);
      }
    }
    return true;
  }
  else
  {
    // need one singly indirect
    // Check how many direct blocks are needed from singly indirect
    int num_indir_dir = sector - NUM_DIRECT;
    // need to check if we need a doubly indirect same as before
    bool need_double = false;
    if (num_indir_dir > 128)
    {
      need_double = true;
    }
    if (need_double == false)
    {
      // directly allocate first
      for (int i = 0; i < NUM_DIRECT; i++)
      {
        block_sector_t *curr_sect = &in_disk->direct[i];
        // allocate one sector
        bool allocat = free_map_allocate(1, curr_sect);
        if (allocat == false)
        {
          return false;
        }
        else
        {
          char zero[BLOCK_SECTOR_SIZE];
          block_write(fs_device, *curr_sect, zero);
        }
      }
      // allocate for singly indirect now
      // save direct blocks into an array
      block_sector_t direct[128];
      // allocate index block
      bool allocated = free_map_allocate(1, &in_disk->sgl_indirect);
      if (allocated)
      {
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, in_disk->sgl_indirect, zeros);
      }
      else
      {
        return false;
      }
      for (int i = 0; i < num_indir_dir; i++)
      {
        // now allocate the direct blocks in the singly indirect
        bool allocated = free_map_allocate(1, &direct[i]);
        if (allocated == false)
        {
          return false;
        }
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, direct[i], zeros);
      }
      // write the actual singly indirect block now
      block_write(fs_device, in_disk->sgl_indirect, direct);

    }
    else
    {
      // directly allocate first
      for (int i = 0; i < NUM_DIRECT; i++)
      {
        block_sector_t *curr_sect = &in_disk->direct[i];
        // allocate one sector
        bool allocat = free_map_allocate(1, curr_sect);
        
        if (allocat == false)
        {
          return false;
        }
        else
        {
          char zero[BLOCK_SECTOR_SIZE];
          block_write(fs_device, *curr_sect, zero);
        }
      }
      // allocate for singly indirect now
      // save direct blocks into an array
      block_sector_t direct[128];
      // allocate index block
      bool allocated = free_map_allocate(1, &in_disk->sgl_indirect);
      if (allocated)
      {
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, in_disk->sgl_indirect, zeros);
      }
      else
      {
        return false;
      }
      for (int i = 0; i < num_indir_dir; i++)
      {
        // now allocate the direct blocks in the singly indirect
        bool allocated = free_map_allocate(1, &direct[i]);
        if (allocated == false)
        {
          return false;
        }
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, direct[i], zeros);
      }
      // write the actual singly indirect block now
      block_write(fs_device, in_disk->sgl_indirect, direct);

      // handle doubly indirect case need to allocate blocks to
      // hold the doubly indirect indirect sectors
      // then also allocate blocks to hold the direct from the indirect sectors
      // TODO
      // first try to allocate the double indirect block
      bool allocated2 = free_map_allocate(1, &in_disk->dbl_indirect);
      if (allocated2)
      {
        char zeros2[BLOCK_SECTOR_SIZE];
        block_write(fs_device, in_disk->dbl_indirect, zeros2);
      }
      else
      {
        return false;
      }
      int num_dir_allocated = sector - (NUM_DIRECT + BLOCKS_PER_INDIRECT);
      int num_dir_total = BLOCKS_PER_INDIRECT * BLOCKS_PER_INDIRECT;
      if (sector > num_dir_total)
      {
        num_dir_allocated = num_dir_total;
      }

      int num_dir_fr_dbl = num_dir_allocated;

      
      block_sector_t indir_blocks[BLOCKS_PER_INDIRECT];
      for (int i = 0; i < BLOCKS_PER_INDIRECT; i++)
      {
        if (num_dir_allocated <= 0)
        {
          i = BLOCKS_PER_INDIRECT;
          // early exit
        }
        int num_indir_dir2 = num_dir_allocated < BLOCKS_PER_INDIRECT ? num_dir_allocated : BLOCKS_PER_INDIRECT;

        // allocate singly indir
        block_sector_t direct_dbl[128];
        // allocate index block
        bool allocated = free_map_allocate(1, &indir_blocks[i]);
        if (allocated)
        {
          char zeros_dbl[BLOCK_SECTOR_SIZE];
          block_write(fs_device, indir_blocks[i], zeros_dbl);
        }
        else
        {
          return false;
        }


        for (int j = 0; j < num_indir_dir2; j++)
        {
          // now allocate the direct blocks in the singly indirect
          bool allocated_dbl = free_map_allocate(1, &direct_dbl[j]);
          if (allocated_dbl == false)
          {
            return false;
         }
          char zeros_dbl2[BLOCK_SECTOR_SIZE];
          block_write(fs_device, direct_dbl[j], zeros_dbl2);
        }
         block_write (fs_device, indir_blocks[i], direct_dbl);

        num_dir_allocated -= num_indir_dir2;


      }

       block_write (fs_device, in_disk->dbl_indirect, indir_blocks);

       if(sector - num_dir_fr_dbl == 0){
         return true;
       }
       else{
         return false;
       }
    }
  }
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open(block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes);
       e = list_next(e))
  {
    inode = list_entry(e, struct inode, elem);
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

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);  
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
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

/* Closes INODE and writes it to disk. (Does it?  Check code.)
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
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
      free_map_release(inode->data.direct[0],
                       bytes_to_sectors(inode->data.length));
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

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size,
                    off_t offset)
{
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

  return bytes_read;
}

//return index of direct in doubly indirect pointer
int get_dir_index(off_t pos){
  int index = pos - (NUM_DIRECT + BLOCKS_PER_INDIRECT);
  int dir_index= (index / BLOCKS_PER_INDIRECT) % BLOCKS_PER_INDIRECT;
  return dir_index;
}

//grow file 
static bool grow_file(int original, off_t pos, struct inode *inode){
  //first find the last block
  block_sector_t num_dir_needed = pos / BLOCK_SECTOR_SIZE;
  //for each condition, write num_dir_needed blocks starting from the last_block + 1
  if (original + num_dir_needed > NUM_TOTAL){
    return false;
  }
  if (original < NUM_DIRECT){
    //both single and double indirect not allocated
    int num_left = NUM_DIRECT - (original + 1);
    int num_ind = num_dir_needed - num_left;
    int num_dub = 0;
    if(num_ind > BLOCKS_PER_INDIRECT){
      num_dub = num_ind - BLOCKS_PER_INDIRECT;
    }
    if (num_dir_needed <= num_left){
      
      for (int i = original + 1; i <= original + num_dir_needed; i++)
      {
        block_sector_t *curr_sect = &inode->data.direct[i];
        bool allocated = free_map_allocate (1, curr_sect);
        if (allocated == false)
        {
          return false;
        }
        char zero[BLOCK_SECTOR_SIZE];
        block_write(fs_device, *curr_sect, zero);
      }
    }else if (num_dub == 0){
      //first write num left
      for (int i = original + 1; i <= original + num_left; i++)
      {
        block_sector_t *curr_sect = &inode->data.direct[i];
        bool allocated = free_map_allocate (1, curr_sect);
        if (allocated == false)
        {
          return false;
        }
        char zero[BLOCK_SECTOR_SIZE];
        block_write(fs_device, *curr_sect, zero);
      }
      //now write the rest into indirect block
      block_sector_t direct[128];
      // allocate index block
      bool allocated = free_map_allocate(1, &inode->data.sgl_indirect);
      if (allocated)
      {
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, inode->data.sgl_indirect, zeros);
      }
      else
      {
        return false;
      }
      for (int i = 0; i < num_ind; i++)
      {
        // now allocate the direct blocks in the singly indirect
        bool allocated = free_map_allocate(1, &direct[i]);
        if (allocated == false)
        {
          return false;
        }
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, direct[i], zeros);
      }
      // write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);
    }else{
      //we need a double indirect as well
      //first write num left
      for (int i = original + 1; i <= original + num_left; i++)
      {
        block_sector_t *curr_sect = &inode->data.direct[i];
        bool allocated = free_map_allocate (1, curr_sect);
        if (allocated == false)
        {
          return false;
        }
        char zero[BLOCK_SECTOR_SIZE];
        block_write(fs_device, *curr_sect, zero);
      }
      //now write the rest into indirect block
      block_sector_t direct[128];
      // allocate index block
      bool allocated = free_map_allocate(1, &inode->data.sgl_indirect);
      if (allocated)
      {
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, inode->data.sgl_indirect, zeros);
      }
      else
      {
        return false;
      }
      for (int i = 0; i < num_ind; i++)
      {
        // now allocate the direct blocks in the singly indirect
        bool allocated = free_map_allocate(1, &direct[i]);
        if (allocated == false)
        {
          return false;
        }
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, direct[i], zeros);
      }
      // write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);
      //write the double indirect blocks
      bool allocated2 = free_map_allocate(1, &inode->data.dbl_indirect);
      if (allocated2)
      {
        char zeros2[BLOCK_SECTOR_SIZE];
        block_write(fs_device, inode->data.dbl_indirect, zeros2);
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
      for (int i = 0; i < BLOCKS_PER_INDIRECT; i++)
      {
        if (num_dir_allocated <= 0)
        {
          i = BLOCKS_PER_INDIRECT;
          // early exit
        }
        int num_indir_dir2 = num_dir_allocated < BLOCKS_PER_INDIRECT ? num_dir_allocated : BLOCKS_PER_INDIRECT;

        // allocate singly indir
        block_sector_t direct_dbl[128];
        // allocate index block
        bool allocated = free_map_allocate(1, &indir_blocks[i]);
        if (allocated)
        {
          char zeros_dbl[BLOCK_SECTOR_SIZE];
          block_write(fs_device, indir_blocks[i], zeros_dbl);
        }
        else
        {
          return false;
        }


        for (int j = 0; j < num_indir_dir2; j++)
        {
          // now allocate the direct blocks in the singly indirect
          bool allocated_dbl = free_map_allocate(1, &direct_dbl[j]);
          if (allocated_dbl == false)
          {
            return false;
         }
          char zeros_dbl2[BLOCK_SECTOR_SIZE];
          block_write(fs_device, direct_dbl[j], zeros_dbl2);
        }
         block_write (fs_device, indir_blocks[i], direct_dbl);

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
  }else if (original < NUM_DIRECT + BLOCKS_PER_INDIRECT){
    //single indirect allocated but double indirect not
    //both single and double indirect not allocated
    int num_left = (NUM_DIRECT + BLOCKS_PER_INDIRECT) - (original + 1);
    int num_ind = num_dir_needed - num_left;
    int num_dub = 0;
    if(num_ind > num_left){
      num_dub = num_ind - num_left;
    }
    if(num_dir_needed <= num_left){
      block_sector_t direct[128];
      block_read(fs_device, inode->data.sgl_indirect, direct);
      // indirect block should already be allocated
      for (int i = original + 1; i < num_left; i++)
      {
        // now allocate the direct blocks in the singly indirect
        bool allocated = free_map_allocate(1, &direct[i]);
        if (allocated == false)
        {
          return false;
        }
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, direct[i], zeros);
      }
      // write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);
    }else{
      //first allocate direct
      block_sector_t direct[128];
      block_read(fs_device, inode->data.sgl_indirect, direct);
      // indirect block should already be allocated
      for (int i = original + 1; i < num_left; i++)
      {
        // now allocate the direct blocks in the singly indirect
        bool allocated = free_map_allocate(1, &direct[i]);
        if (allocated == false)
        {
          return false;
        }
        char zeros[BLOCK_SECTOR_SIZE];
        block_write(fs_device, direct[i], zeros);
      }
      // write the actual singly indirect block now
      block_write(fs_device, inode->data.sgl_indirect, direct);

      //now allocate double indirect
      bool allocated2 = free_map_allocate(1, &inode->data.dbl_indirect);
      if (allocated2)
      {
        char zeros2[BLOCK_SECTOR_SIZE];
        block_write(fs_device, inode->data.dbl_indirect, zeros2);
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
      for (int i = 0; i < BLOCKS_PER_INDIRECT; i++)
      {
        if (num_dir_allocated <= 0)
        {
          i = BLOCKS_PER_INDIRECT;
          // early exit
        }
        int num_indir_dir2 = num_dir_allocated < BLOCKS_PER_INDIRECT ? num_dir_allocated : BLOCKS_PER_INDIRECT;

        // allocate singly indir
        block_sector_t direct_dbl[128];
        // allocate index block
        bool allocated = free_map_allocate(1, &indir_blocks[i]);
        if (allocated)
        {
          char zeros_dbl[BLOCK_SECTOR_SIZE];
          block_write(fs_device, indir_blocks[i], zeros_dbl);
        }
        else
        {
          return false;
        }


        for (int j = 0; j < num_indir_dir2; j++)
        {
          // now allocate the direct blocks in the singly indirect
          bool allocated_dbl = free_map_allocate(1, &direct_dbl[j]);
          if (allocated_dbl == false)
          {
            return false;
         }
          char zeros_dbl2[BLOCK_SECTOR_SIZE];
          block_write(fs_device, direct_dbl[j], zeros_dbl2);
        }
         block_write (fs_device, indir_blocks[i], direct_dbl);

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
    //double indirect allocated already
    int num_left = NUM_TOTAL - (original + 1);
    int num_dub = num_dir_needed - num_left;
    int start = ((original) - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) / BLOCKS_PER_INDIRECT;

      block_sector_t indir_blocks[BLOCKS_PER_INDIRECT];
      block_read (fs_device, inode->data.dbl_indirect, indir_blocks);
      for (int i = start; i < BLOCKS_PER_INDIRECT; i++)
      {
        if (num_dub <= 0)
        {
          i = BLOCKS_PER_INDIRECT;
          // early exit
        }
        int num_indir_dir2 = num_dub < BLOCKS_PER_INDIRECT ? num_dub : BLOCKS_PER_INDIRECT;

        // allocate singly indir
        block_sector_t direct_dbl[128];
        if(i == start){
          block_read(fs_device, indir_blocks[i], direct_dbl);
        }else{
          // allocate index block
          bool allocated = free_map_allocate(1, &indir_blocks[i]);
          if (allocated)
          {
            char zeros_dbl[BLOCK_SECTOR_SIZE];
            block_write(fs_device, indir_blocks[i], zeros_dbl);
          }
          else
          {
            return false;
          }
        }

        for (int j = 0; j < num_indir_dir2; j++)
        {
          if(i == start){
            j = ((original + 1) - (NUM_DIRECT + BLOCKS_PER_INDIRECT)) % BLOCKS_PER_INDIRECT;
          }
          // now allocate the direct blocks in the singly indirect
          bool allocated_dbl = free_map_allocate(1, &direct_dbl[j]);
          if (allocated_dbl == false)
          {
            return false;
         }
          char zeros_dbl2[BLOCK_SECTOR_SIZE];
          block_write(fs_device, direct_dbl[j], zeros_dbl2);
        }
         block_write (fs_device, indir_blocks[i], direct_dbl);

        num_dub -= num_indir_dir2;


      }

       block_write (fs_device, inode->data.dbl_indirect, indir_blocks);
  }
  return true;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs. */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size,
                     off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;
  
  //grow file
  off_t eof = offset + size;
  off_t old_length = inode_length (inode);
  int old_aligned =  old_length - (old_length % BLOCK_SECTOR_SIZE);
  int new_start = old_length == 0 ? 0 : old_aligned + BLOCK_SECTOR_SIZE;
  if(eof > old_length){
    //need to calculate the original last sector.
    int last_sector = old_length == 0 ? -1 : old_aligned/BLOCK_SECTOR_SIZE;
    //subtract eof from new start
    off_t bytes_to_add = eof - new_start;
    //allocate new space and grow
    if(bytes_to_add >= 0){
      grow_file(last_sector, bytes_to_add + BLOCK_SECTOR_SIZE, inode);
    }
    inode->data.length = eof;
    block_write(fs_device, inode->sector, &inode->data);
  }

  //printf("%i\n", offset);
  while (size > 0)
  {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    //printf("%i\n", size);
    //printf("%i\n", sector_idx);
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
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode)
{
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) { return inode->data.length; }
