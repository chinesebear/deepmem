#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "random.h"
#include "deep_mem.h"
#include "deep_log.h"

mem_pool_t *pool;

static void *deep_malloc_fast_bins (uint32_t size);
static void *deep_malloc_sorted_bins (uint32_t size);
static void deep_free_fast_bins (void *ptr);
static void deep_free_sorted_bins (void *ptr);

/* helper functions for maintining the sorted_block skiplist */
static sorted_block_t *
_split_into_two_sorted_blocks (sorted_block_t *block,
                               uint32_t aligned_size);
static void _merge_into_single_block (sorted_block_t *curr,
                                      sorted_block_t *next);
static sorted_block_t *
_allocate_block_from_skiplist (uint32_t aligned_size);
static inline bool _sorted_block_is_in_skiplist (sorted_block_t *block);
static sorted_block_t *
_find_sorted_block_by_size_on_index (sorted_block_t *node, uint32_t size,
                                     uint32_t index_level);
static sorted_block_t *
_find_sorted_block_by_size (sorted_block_t *node, uint32_t size);
static void _insert_sorted_block_to_skiplist (sorted_block_t *block);
static void _remove_sorted_block_from_skiplist (sorted_block_t *block);

static inline bool
block_is_allocated (block_head_t const *head)
{
  return (*head) & A_FLAG_MASK;
}

static inline void
block_set_A_flag (block_head_t *head, bool allocated)
{
  *head = allocated ? (*head | A_FLAG_MASK) : (*head & (~A_FLAG_MASK));
}

static inline bool
prev_block_is_allocated (block_head_t const *head)
{
  return (*head) & P_FLAG_MASK;
}

static inline void
block_set_P_flag (block_head_t *head, bool allocated)
{
  *head = allocated ? (*head | P_FLAG_MASK) : (*head & (~P_FLAG_MASK));
}

/**
 * since the block is 8-bytes aligned, the smallest 8's multiple greater than
 * size is used instead.
 *
 * Aligned size, including the block head
 **/
static inline block_size_t
block_get_size (block_head_t const *head)
{
  return (*head) & BLOCK_SIZE_MASK;
}

/**
 * since the block is 8-bytes aligned, the smallest 8's multiple greater than
 * size is used instead.
 *
 * Aligned size, including the block head
 **/
static inline void
block_set_size (block_head_t *head, block_size_t size)
{
  *head = (*head & (~BLOCK_SIZE_MASK)) /* preserve flags */
          | ALIGN_MEM_SIZE (size);     /* ensure rounds up */
}

static inline void *
get_pointer_by_offset_in_bytes (void *p, int64_t offset)
{
  return (uint8_t *)p + offset;
}

static inline int64_t
get_offset_between_pointers_in_bytes (void *p, void *q)
{
  return (uint8_t *)p - (uint8_t *)q;
}

static inline struct sorted_block *
get_block_by_offset (struct sorted_block *node, int32_t offset)
{
  return (struct sorted_block *)(get_pointer_by_offset_in_bytes ((mem_t *)node,
                                                                 offset));
}

static inline int32_t
get_offset_between_blocks (struct sorted_block *origin,
                           struct sorted_block *target)
{
  return get_offset_between_pointers_in_bytes ((mem_t *)target,
                                               (mem_t *)origin);
}

static inline mem_size_t
get_remainder_size (struct mem_pool const *pool)
{
  return get_offset_between_pointers_in_bytes (pool->remainder_block_end.addr,
                                               pool->remainder_block.addr);
}

bool deep_mem_init (void *mem, uint32_t size)
{

  if (size < sizeof (mem_pool_t))
    {
      return false; /* given buffer is too small */
    }

  memset (mem, 0, size);
  mem_size_t aligned_size = ALIGN_MEM_SIZE_TRUNC (size);

  pool = (mem_pool_t *)mem;
  pool->free_memory = aligned_size - sizeof (mem_pool_t)
                      - sizeof (sorted_block_t) - sizeof (block_head_t);
  /* the first node in the list, to simplify implementation */
  pool->sorted_block.addr
      = (sorted_block_t *)(get_pointer_by_offset_in_bytes (
          mem, sizeof (mem_pool_t)));
  /* all other fields are set as 0 */
  pool->sorted_block.addr->payload.info.level_of_indices = SORTED_BLOCK_INDICES_LEVEL;

  pool->remainder_block.addr
      = (sorted_block_t *)(get_pointer_by_offset_in_bytes (
          mem, sizeof (mem_pool_t) + sizeof (sorted_block_t)));
  pool->remainder_block_end.addr
      = (sorted_block_t *)(get_pointer_by_offset_in_bytes (
          mem, aligned_size - 8)); // -8 for safety
  for (int i = 0; i < FAST_BIN_LENGTH; ++i)
    {
      pool->fast_bins[i].addr = NULL;
    }
  // initialise remainder block's head
  block_set_A_flag (&pool->remainder_block.addr->head, false);
  block_set_P_flag (&pool->remainder_block.addr->head, true);

  return true;
}

void
deep_mem_destroy (void)
{
  pool = NULL;
}

void *
deep_malloc (uint32_t size)
{
  if (pool->free_memory < size)
    {
      return NULL;
    }
  if (size <= FAST_BIN_MAX_SIZE)
    {
      return deep_malloc_fast_bins (size);
    }
  return deep_malloc_sorted_bins (size);
}

static void *
deep_malloc_fast_bins (uint32_t size)
{
  block_size_t aligned_size = ALIGN_MEM_SIZE (size + sizeof (block_head_t));
  uint32_t offset = (aligned_size >> 3) - 1;
  bool P_flag = false;
  fast_block_t *ret = NULL;
  block_size_t payload_size;

  if (pool->fast_bins[offset].addr != NULL)
    {
      ret = pool->fast_bins[offset].addr;
      pool->fast_bins[offset].addr = ret->payload.next;
      P_flag = prev_block_is_allocated (&ret->head);
      payload_size = block_get_size (&ret->head) - sizeof (block_head_t);
    }
  else if (aligned_size <= get_remainder_size (pool))
    {
      ret = (fast_block_t *)(get_pointer_by_offset_in_bytes (
          pool->remainder_block_end.addr,
          -(int64_t)aligned_size - sizeof (block_head_t)));
      pool->remainder_block_end.addr = (sorted_block_t *)ret;

      payload_size = aligned_size - sizeof (block_head_t);
      block_set_size (&ret->head, aligned_size);
      pool->free_memory -= sizeof (block_head_t);
    }
  else
    {
      return NULL;
    }

  memset (&ret->payload, 0, payload_size);
  block_set_A_flag (&ret->head, true);
  block_set_P_flag (&ret->head, P_flag);
  pool->free_memory -= payload_size;

  return &ret->payload;
}

static void *
deep_malloc_sorted_bins (uint32_t size)
{
  block_size_t aligned_size = ALIGN_MEM_SIZE (size + sizeof (block_head_t));
  sorted_block_t *ret = NULL;
  block_size_t payload_size;

  if ((pool->sorted_block.addr != NULL)
      && ((ret = _allocate_block_from_skiplist (aligned_size)) != NULL))
    {
      /* pass */
    }
  else if (aligned_size <= get_remainder_size (pool))
    {
      /* no suitable sorted_block */
      ret = (sorted_block_t *)pool->remainder_block.addr;
      block_set_size (&ret->head, get_remainder_size (pool));
      pool->remainder_block.addr = _split_into_two_sorted_blocks (ret, aligned_size);
    }
  else
    {
      return NULL;
    }

  payload_size = aligned_size - sizeof (block_head_t);
  memset (&ret->payload, 0, payload_size);
  block_set_A_flag (&ret->head, true);
  block_set_P_flag (&get_block_by_offset (ret, aligned_size)->head, true);
  pool->free_memory -= payload_size;

  return &ret->payload;
}

void *
deep_realloc (void *ptr, uint32_t size)
{
  return NULL;
}

void
deep_free (void *ptr)
{

  void *head
      = get_pointer_by_offset_in_bytes (ptr, -(int64_t)sizeof (block_head_t));

  if (ptr == NULL || !block_is_allocated ((block_head_t *)head))
    {
      return;
    }
  block_size_t size = block_get_size ((block_head_t *)head);

  if (size <= FAST_BIN_MAX_SIZE)
    {
      deep_free_fast_bins (head);
    }
  else
    {
      deep_free_sorted_bins (head);
    }
}

static void
deep_free_fast_bins (void *ptr)
{
  fast_block_t *block = ptr;
  block_size_t payload_size
      = block_get_size (&block->head) - sizeof (block_head_t);
  uint32_t offset = ((payload_size + sizeof (block_head_t)) >> 3) - 1;

  memset (&block->payload, 0, payload_size);

  block_set_A_flag (&block->head, false);
  block->payload.next = pool->fast_bins[offset].addr->payload.next;
  pool->fast_bins[offset].addr = block;

  pool->free_memory += payload_size;
}

static void
deep_free_sorted_bins (void *ptr)
{
  sorted_block_t *block = ptr;
  sorted_block_t *the_other = NULL;
  block_size_t payload_size
      = block_get_size (&block->head) - sizeof (block_head_t);

  memset (&block->payload, 0, payload_size);

  block_set_A_flag (&block->head, false);

  /* try to merge */
  /* merge above */
  if (!prev_block_is_allocated (&block->head))
    {
      block_size_t prev_size
          = block_get_size ((block_head_t *)(block - sizeof (block_head_t)));
      the_other = get_block_by_offset (block, -((int32_t)prev_size));
      _merge_into_single_block (the_other, block);

      block = the_other;
    }

  /* merge below */
  the_other = get_block_by_offset (block, block_get_size (&block->head));
  if (!block_is_allocated (&the_other->head))
    {
      _merge_into_single_block (block, the_other);
    }
  /* update remainder_block if it is involved */
  if (the_other == pool->remainder_block.addr)
    {
      pool->remainder_block.addr = block;
    }

  if (!_sorted_block_is_in_skiplist (block))
    {
      _insert_sorted_block_to_skiplist (block);
    }

  pool->free_memory += payload_size;
}

bool
deep_mem_migrate (void *new_mem, uint32_t size)
{
  return false;
}

/* helper functions for maintining the sorted_block skiplist */
static sorted_block_t *
_split_into_two_sorted_blocks (sorted_block_t *block,
                               uint32_t aligned_size)
{
  sorted_block_t *new_block = get_block_by_offset (block, aligned_size);
  block_size_t new_block_size = block_get_size (&block->head) - aligned_size;

  memset (new_block, 0, new_block_size);
  block_set_size (&new_block->head, new_block_size);
  block_set_A_flag (&new_block->head, false);
  block_set_P_flag (&new_block->head, false); /* by default */

  block_set_size (&block->head, aligned_size);
  pool->free_memory -= sizeof (block_head_t);

  return new_block;
}

/**
 * Assuming `curr` and `next` are contiguous in memory address,
 * where curr < next
 * Attempt to remove both nodes from list, merge them, and insert the merged.
 * NOTE:
 *   - will update `free_memory` of releasing `sizeof (block_head_t)` to pool
 **/
static void
_merge_into_single_block (sorted_block_t *curr, sorted_block_t *next)
{
  _remove_sorted_block_from_skiplist (curr);
  _remove_sorted_block_from_skiplist (next);

  block_size_t new_size
      = block_get_size (&curr->head) + block_get_size (&next->head);

  block_set_size (&curr->head, new_size);
  memset (&curr->payload, 0, new_size - sizeof (curr->head));
  // copy over new head info to footer
  *(block_head_t *)get_pointer_by_offset_in_bytes (
      &curr->head, new_size - sizeof (block_head_t))
      = curr->head;

  _insert_sorted_block_to_skiplist (curr);

  pool->free_memory += sizeof (block_head_t);
}

/** Obtain a most apporiate block from sorted_list if possible.
 *
 * - Obtain one with exact same size.
 * - Obtain one with bigger size, but split into two sorted blocks
 *   - returns the part with exactly same size
 *   - insert the rest into sorted_block skiplist
 *   - NOTE: this requires the block found be at least
 *           (`aligned_size + SORTED_BIN_MIN_SIZE`) big
 * - NULL
 *
 * NOTE: The obtained block will be **removed** from the skiplist.
 **/
static sorted_block_t *
_allocate_block_from_skiplist (uint32_t aligned_size)
{
  sorted_block_t *ret = NULL;

  if ((pool->sorted_block.addr == NULL)
      || ((ret = _find_sorted_block_by_size (pool->sorted_block.addr,
                                             aligned_size))
          == NULL))
    {
      return NULL;
    }
  if (block_get_size (&ret->head) != aligned_size)
    {
      if ((block_get_size (&ret->head) < aligned_size + SORTED_BIN_MIN_SIZE)
          && (ret = _find_sorted_block_by_size (
                  ret, aligned_size + SORTED_BIN_MIN_SIZE))
                 == NULL)
        {
          return NULL;
        }
      sorted_block_t *remainder = _split_into_two_sorted_blocks (ret, aligned_size);
      _insert_sorted_block_to_skiplist (remainder);
    }
  _remove_sorted_block_from_skiplist (ret);

  return ret;
}

static inline bool
_sorted_block_is_in_skiplist (sorted_block_t *block)
{
  return (block->payload.info.pred_offset != 0 || block->payload.info.level_of_indices != 0);
}

/**
 *  returns a block with desired size on the list of given index level;
 * if not possible, the greatest one that is smaller than desired.
 *
 * NOTE:
 *   - there will always be an infimum due to the existence of head
 *   - this function will not check nodes on other index level
 *   - this function will not check if there are any predecessor in the chain
 *     with same key. It assumes the `node` given has embedded all indices.
 **/
static sorted_block_t *
_find_sorted_block_by_size_on_index (sorted_block_t *node, uint32_t size,
                                     uint32_t index_level)
{

  sorted_block_t *curr = node;
  sorted_block_t *prev = curr;

  while (block_get_size (&curr->head) < size)
    {
      prev = curr; /* curr is the candidate of infimum. */
      /* reached the end of the skiplist or the biggest smaller sorted block */
      if (index_level >= SORTED_BLOCK_INDICES_LEVEL
          || curr->payload.info.offsets[index_level] == 0)
        {
          break;
        }
      curr = get_block_by_offset (curr, curr->payload.info.offsets[index_level]);
    }

  /* return a node with no indices to avoid copying indices. */
  if (block_get_size (&curr->head) == size && curr->payload.info.succ_offset != 0)
    {
      return get_block_by_offset (curr, curr->payload.info.succ_offset);
    }

  return prev;
}

/**
 *  returns a block with desired size; if not possible, the least greater one
 *
 * NOTE:
 *   - returns NULL when supremum is not in the list
 **/
static sorted_block_t *
_find_sorted_block_by_size (sorted_block_t *node, uint32_t size)
{

  sorted_block_t *curr = node;

  /* indices should only exists on first node in each sub-list. */
  while (curr->payload.info.pred_offset != 0)
    {
      curr = get_block_by_offset (curr, curr->payload.info.pred_offset);
    }

  while (block_get_size (&curr->head) < size)
    {
      uint32_t index_level
          = SORTED_BLOCK_INDICES_LEVEL - curr->payload.info.level_of_indices;

      /* skip non-existing indices, to node with size <= than desired */
      while (index_level < SORTED_BLOCK_INDICES_LEVEL
             || curr->payload.info.offsets[index_level] == 0
             || (block_get_size (&get_block_by_offset (curr, curr->payload.info.offsets[index_level])->head) > size))
        {
          index_level++;
        }

      /* reached the end of the skiplist or the biggest smaller sorted block */
      if (index_level >= SORTED_BLOCK_INDICES_LEVEL
          || curr->payload.info.offsets[index_level] == 0)
        {
          break;
        }

      /* will not be NULL as curr's size is smaller than size */
      curr = _find_sorted_block_by_size_on_index (curr, size, index_level);
    }

  /* all nodes are smaller than required. */
  if (block_get_size (&curr->head) < size)
    {
      return NULL;
    }

  /* return a node with no indices to avoid copying indices. */
  if (curr->payload.info.succ_offset != 0)
    {
      curr = get_block_by_offset (curr, curr->payload.info.succ_offset);
    }

  return curr;
}

static void
_insert_sorted_block_to_skiplist (sorted_block_t *block)
{
  block_size_t size = block_get_size (&block->head);
  sorted_block_t *pos
      = _find_sorted_block_by_size (pool->sorted_block.addr, size);

  /* insert into the chain with same size. */
  if (pos != NULL && block_get_size (&pos->head) == size)
    {
      block->payload.info.pred_offset = get_offset_between_blocks (block, pos);
      if (pos->payload.info.succ_offset != 0)
        {
          block->payload.info.succ_offset
              = pos->payload.info.succ_offset - get_offset_between_blocks (pos, block);
        }
      else
        {
          block->payload.info.succ_offset = 0; /* end of chain */
        }
      pos->payload.info.succ_offset = get_offset_between_blocks (pos, block);

      return;
    }

  block->payload.info.level_of_indices
      = ((uint32_t) (next () >> 32)) % SORTED_BLOCK_INDICES_LEVEL + 1;

  for (uint32_t index_level
       = SORTED_BLOCK_INDICES_LEVEL - block->payload.info.level_of_indices;
       index_level < SORTED_BLOCK_INDICES_LEVEL; ++index_level)
    {
      pos = _find_sorted_block_by_size_on_index (pool->sorted_block.addr, size,
                                                 index_level);
      if (pos->payload.info.offsets[index_level] != 0)
        {
          block->payload.info.offsets[index_level]
              = pos->payload.info.offsets[index_level]
                - get_offset_between_blocks (pos, block);
        }
      else
        {
          block->payload.info.offsets[index_level] = 0;
        }
      pos->payload.info.offsets[index_level] = get_offset_between_blocks (pos, block);
    }
}

/**
 * Remove the node and update all indices / offsets.
 * May traverse the list multiple times
 *
 * NOTE: never removes a node with offsets when it has children in the chain
 **/
static void _remove_sorted_block_from_skiplist (sorted_block_t *block)
{

  /* considering a node which is not in the skiplist */
  sorted_block_t *prev = NULL;
  block_size_t size = block_get_size (&block->head);

  for (uint32_t index_level
       = SORTED_BLOCK_INDICES_LEVEL - block->payload.info.level_of_indices;
       index_level < SORTED_BLOCK_INDICES_LEVEL; ++index_level)
    {
      /* -1 to find the strictly smaller node. */
      prev = _find_sorted_block_by_size_on_index (pool->sorted_block.addr,
                                                  size - 1, index_level);
      if (block->payload.info.offsets[index_level] != 0)
        {
          prev->payload.info.offsets[index_level] += block->payload.info.offsets[index_level];
        }
      else
        {
          prev->payload.info.offsets[index_level] = 0;
        }
    }

  if (block->payload.info.pred_offset != 0)
    {
      if (block->payload.info.succ_offset != 0)
        {
          get_block_by_offset (block, block->payload.info.pred_offset)->payload.info.succ_offset
              += block->payload.info.succ_offset;
          get_block_by_offset (block, block->payload.info.succ_offset)->payload.info.pred_offset
              += block->payload.info.pred_offset;
        }
      else
        {
          get_block_by_offset (block, block->payload.info.pred_offset)->payload.info.succ_offset = 0;
        }
    }
  /* no other cases, as if it is the first node, it should be the only node. */
}
