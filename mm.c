/*
 * mm.c - A simple implicit free list allocator.
 *
 * This version follows the main ideas from CS:APP 3e Section 9.9:
 *
 * 1. Every block has a header and footer that store the block size
 *    and whether the block is allocated.
 * 2. Free blocks are found by scanning the heap from the front
 *    (implicit free list).
 * 3. When we free a block, we immediately try to merge it with
 *    neighboring free blocks (coalescing).
 * 4. When a free block is larger than needed, we split it into
 *    an allocated piece and a smaller free piece.
 *
 * The goal here is not to be the fastest possible allocator.
 * The goal is to implement the textbook design in a clean way that
 * is easy to study and explain.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/*********************************************************
 * NOTE TO STUDENTS: Before you do anything else, please
 * provide your team information in the following struct.
 ********************************************************/
team_t team = {
    /* Team name */
    "Hogwart",
    /* First member's full name */
    "Harry Potter",
    /* First member's email address */
    "bovik@cs.cmu.edu",
    /* Second member's full name (leave blank if none) */
    "",
    /* Second member's email address (leave blank if none) */
    ""};

/* A word is 4 bytes in the textbook allocator metadata. */
#define WSIZE 4

/* A double word is 8 bytes, which also matches the required alignment. */
#define DSIZE 8

/* When we need more heap space, ask for at least this many bytes. */
#define CHUNKSIZE (1 << 12)

/* Smallest block size: header + footer + minimum payload alignment. */
#define MINBLOCKSIZE (2 * DSIZE)

/* Pick the larger of two values. */
#define MAX(x, y) ((x) > (y) ? (x) : (y))

/*
 * PACK combines two pieces of information into one 4-byte value.
 *
 * size  : block size in bytes
 * alloc : 1 if allocated, 0 if free
 *
 * Block sizes are multiples of 8, so the lower bits are free.
 * We store the allocation flag in the lowest bit.
 */
#define PACK(size, alloc) ((size) | (alloc))

/* Read a 4-byte value from address p. */
#define GET(p) (*(unsigned int *)(p))

/* Write a 4-byte value val to address p. */
#define PUT(p, val) (*(unsigned int *)(p) = (val))

/* Read just the block size from a header or footer word. */
#define GET_SIZE(p) (GET(p) & ~0x7)

/* Read just the allocation bit from a header or footer word. */
#define GET_ALLOC(p) (GET(p) & 0x1)

/* Find the header address from a payload pointer. */
#define HDRP(bp) ((char *)(bp) - WSIZE)

/* Find the footer address from a payload pointer. */
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* Move from the current block to the next block. */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))

/* Move from the current block to the previous block. */
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

/*
 * heap_listp points into the prologue block.
 * We use it as the starting point when scanning the heap.
 */
static char *heap_listp = NULL;

/* Helper functions used only inside this file. */
static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);

/*
 * mm_init - Build the initial empty heap.
 *
 * After this function runs, the heap starts with:
 * padding -> prologue block -> epilogue header
 *
 * The prologue is a fake allocated block that makes corner cases simpler.
 * The epilogue is a size-0 allocated header that marks the end of the heap.
 */
int mm_init(void)
{
    /* Reserve 4 words for the initial heap structure. */
    heap_listp = mem_sbrk(4 * WSIZE);

    /* If mem_sbrk fails, signal failure to the driver. */
    if (heap_listp == (void *)-1)
    {
        return -1;
    }

    /* Alignment padding: keeps payloads aligned to 8 bytes. */
    PUT(heap_listp, 0);

    /* Prologue header: size 8, allocated. */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));

    /* Prologue footer: same info as the header. */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));

    /* Epilogue header: size 0, allocated. */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));

    /* Move pointer to the prologue payload position. */
    heap_listp += (2 * WSIZE);

    /* Create the first real free block by extending the heap. */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
    {
        return -1;
    }

    return 0;
}

/*
 * extend_heap - Ask memlib for more heap space.
 *
 * The new area becomes one free block plus a new epilogue header.
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* Keep the heap aligned to 8 bytes by using an even number of words. */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    /* mem_sbrk returns the start of the new space. */
    bp = mem_sbrk(size);

    /* If the heap cannot grow, return failure. */
    if (bp == (void *)-1)
    {
        return NULL;
    }

    /* Write header for the new free block. */
    PUT(HDRP(bp), PACK(size, 0));

    /* Write footer for the new free block. */
    PUT(FTRP(bp), PACK(size, 0));

    /* Write the new epilogue header right after the free block. */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    /* Merge with the previous block if that block was also free. */
    return coalesce(bp);
}

/*
 * coalesce - Merge adjacent free blocks.
 *
 * There are four cases:
 * 1. Both neighbors are allocated.
 * 2. Next block is free.
 * 3. Previous block is free.
 * 4. Both neighbors are free.
 */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    /* Case 1: nothing to merge. */
    if (prev_alloc && next_alloc)
    {
        return bp;
    }

    /* Case 2: merge current block with the next free block. */
    if (prev_alloc && !next_alloc)
    {
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
        return bp;
    }

    /* Case 3: merge current block into the previous free block. */
    if (!prev_alloc && next_alloc)
    {
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        return PREV_BLKP(bp);
    }

    /* Case 4: merge previous, current, and next blocks together. */
    size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
    PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
    return PREV_BLKP(bp);
}

/*
 * find_fit - First-fit search.
 *
 * Walk the heap from front to back and stop at the first free block
 * that is large enough.
 */
static void *find_fit(size_t asize)
{
    void *bp;

    /* Keep scanning until we reach the epilogue block. */
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        /* A fit means the block is free and large enough. */
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp))))
        {
            return bp;
        }
    }

    /* No suitable block was found. */
    return NULL;
}

/*
 * place - Put an allocated block inside a free block.
 *
 * If the free block is larger than needed, split it.
 * Otherwise, use the whole block.
 */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));

    /*
     * Only split if the leftover space is big enough to be a valid block.
     * Tiny leftovers are useless, so in that case we keep the whole block.
     */
    if ((csize - asize) >= MINBLOCKSIZE)
    {
        /* Mark the front part as allocated. */
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));

        /* Move to the remainder and mark that remainder as free. */
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
    }
    else
    {
        /* Leftover space is too small, so take the whole block. */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/*
 * mm_malloc - Return a block with at least size bytes of payload.
 */
void *mm_malloc(size_t size)
{
    size_t asize;
    size_t extendsize;
    char *bp;

    /* malloc(0) is allowed to return NULL. */
    if (size == 0)
    {
        return NULL;
    }

    /*
     * Compute the adjusted block size.
     *
     * We must include:
     * - header
     * - footer
     * - aligned payload
     */
    if (size <= DSIZE)
    {
        asize = MINBLOCKSIZE;
    }
    else
    {
        asize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
    }

    /* Try to reuse a free block already in the heap. */
    bp = find_fit(asize);
    if (bp != NULL)
    {
        place(bp, asize);
        return bp;
    }

    /* No fit found, so grow the heap. */
    extendsize = MAX(asize, CHUNKSIZE);
    bp = extend_heap(extendsize / WSIZE);
    if (bp == NULL)
    {
        return NULL;
    }

    /* Put the requested block into the newly created free block. */
    place(bp, asize);
    return bp;
}

/*
 * mm_free - Mark a block as free, then merge if possible.
 */
void mm_free(void *ptr)
{
    size_t size;

    /* free(NULL) should do nothing. */
    if (ptr == NULL)
    {
        return;
    }

    /* Read the block size before rewriting the metadata. */
    size = GET_SIZE(HDRP(ptr));

    /* Mark both header and footer as free. */
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));

    /* Merge with neighbors if they are also free. */
    coalesce(ptr);
}

/*
 * mm_realloc - Resize a block by allocate-copy-free.
 *
 * This simple version is easy to understand:
 * 1. Handle corner cases.
 * 2. Allocate a new block.
 * 3. Copy old payload bytes.
 * 4. Free the old block.
 */
void *mm_realloc(void *ptr, size_t size)
{
    void *newptr;
    size_t copySize;

    /* realloc(NULL, size) is the same as malloc(size). */
    if (ptr == NULL)
    {
        return mm_malloc(size);
    }

    /* realloc(ptr, 0) is the same as free(ptr) and return NULL. */
    if (size == 0)
    {
        mm_free(ptr);
        return NULL;
    }

    /* Allocate a new block with the requested size. */
    newptr = mm_malloc(size);
    if (newptr == NULL)
    {
        return NULL;
    }

    /*
     * Old payload size equals total block size minus header and footer.
     * Copy only as many bytes as both old and new blocks can support.
     */
    copySize = GET_SIZE(HDRP(ptr)) - DSIZE;
    if (size < copySize)
    {
        copySize = size;
    }

    /* Copy the old data into the new block. */
    memcpy(newptr, ptr, copySize);

    /* Free the old block now that the data has moved. */
    mm_free(ptr);

    return newptr;
}
