#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
    "Hogwart",
    "Harry Potter",
    "bovik@cs.cmu.edu",
    "",
    ""};




#define WSIZE 4


#define DSIZE 8


#define PTRSIZE sizeof(void *)


#define CHUNKSIZE (1 << 12)


#define ALIGN(size) (((size) + (DSIZE - 1)) & ~0x7)


#define MAX(x, y) ((x) > (y) ? (x) : (y))


#define MINBLOCKSIZE ALIGN(2 * WSIZE + 2 * PTRSIZE)


#define PACK(size, alloc) ((size) | (alloc))


#define GET(p) (*(unsigned int *)(p))


#define PUT(p, val) (*(unsigned int *)(p) = (val))


#define GET_SIZE(p) (GET(p) & ~0x7)


#define GET_ALLOC(p) (GET(p) & 0x1)


#define HDRP(bp) ((char *)(bp) - WSIZE)


#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)


#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))


#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE))


#define PRED_PTR(bp) (*(void **)(bp))


#define SUCC_PTR(bp) (*(void **)((char *)(bp) + PTRSIZE))


static char *heap_listp = NULL;


static void *free_listp = NULL;


static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);
static void insert_free_block(void *bp);
static void remove_free_block(void *bp);


static void insert_free_block(void *bp)
{
    
    SUCC_PTR(bp) = free_listp;

    
    PRED_PTR(bp) = NULL;

    
    if (free_listp != NULL)
    {
        PRED_PTR(free_listp) = bp;
    }

    
    free_listp = bp;
}


static void remove_free_block(void *bp)
{
    
    void *pred = PRED_PTR(bp);

    
    void *succ = SUCC_PTR(bp);

    
    if (pred != NULL)
    {
        SUCC_PTR(pred) = succ;
    }
    else
    {
        
        free_listp = succ;
    }

    
    if (succ != NULL)
    {
        PRED_PTR(succ) = pred;
    }
}


int mm_init(void)
{
    
    free_listp = NULL;

    
    heap_listp = mem_sbrk(4 * WSIZE);

    
    if (heap_listp == (void *)-1)
    {
        return -1;
    }

    
    PUT(heap_listp, 0);

    
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));

    
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));

    
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));

    
    heap_listp += (2 * WSIZE);

    
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
    {
        return -1;
    }

    return 0;
}


static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    
    bp = mem_sbrk(size);

    
    if (bp == (void *)-1)
    {
        return NULL;
    }

    
    PUT(HDRP(bp), PACK(size, 0));

    
    PUT(FTRP(bp), PACK(size, 0));

    
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    
    return coalesce(bp);
}


static void *coalesce(void *bp)
{
    
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));

    
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));

    
    size_t size = GET_SIZE(HDRP(bp));

    
    if (prev_alloc && next_alloc)
    {
        insert_free_block(bp);
        return bp;
    }

    
    if (prev_alloc && !next_alloc)
    {
        remove_free_block(NEXT_BLKP(bp));
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
        insert_free_block(bp);
        return bp;
    }

    
    if (!prev_alloc && next_alloc)
    {
        remove_free_block(PREV_BLKP(bp));
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
        insert_free_block(bp);
        return bp;
    }

    
    remove_free_block(PREV_BLKP(bp));
    remove_free_block(NEXT_BLKP(bp));
    size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
    PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
    bp = PREV_BLKP(bp);
    insert_free_block(bp);
    return bp;
}


static void *find_fit(size_t asize)
{
    
    void *cur = free_listp;

    
    while (cur != NULL)
    {
        
        if (GET_SIZE(HDRP(cur)) >= asize)
        {
            return cur;
        }

        
        cur = SUCC_PTR(cur);
    }

    
    return NULL;
}


static void place(void *bp, size_t asize)
{
    
    size_t csize = GET_SIZE(HDRP(bp));

    
    remove_free_block(bp);

    
    if ((csize - asize) >= MINBLOCKSIZE)
    {
        
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));

        
        void *next_bp = NEXT_BLKP(bp);

        
        PUT(HDRP(next_bp), PACK(csize - asize, 0));

        
        PUT(FTRP(next_bp), PACK(csize - asize, 0));

        
        insert_free_block(next_bp);
    }
    else
    {
        
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}


void *mm_malloc(size_t size)
{
    
    size_t asize;

    
    size_t extendsize;

    
    char *bp;

    
    if (size == 0)
    {
        return NULL;
    }

    
    asize = ALIGN(size + DSIZE);
    if (asize < MINBLOCKSIZE)
    {
        asize = MINBLOCKSIZE;
    }

    
    bp = find_fit(asize);
    if (bp != NULL)
    {
        place(bp, asize);
        return bp;
    }

    
    extendsize = MAX(asize, CHUNKSIZE);
    bp = extend_heap(extendsize / WSIZE);
    if (bp == NULL)
    {
        return NULL;
    }

    
    place(bp, asize);
    return bp;
}


void mm_free(void *ptr)
{
    
    size_t size;

    
    if (ptr == NULL)
    {
        return;
    }

    
    size = GET_SIZE(HDRP(ptr));

    
    PUT(HDRP(ptr), PACK(size, 0));

    
    PUT(FTRP(ptr), PACK(size, 0));

    
    coalesce(ptr);
}


void *mm_realloc(void *ptr, size_t size)
{
    
    void *newptr;

    
    size_t copySize;

    
    if (ptr == NULL)
    {
        return mm_malloc(size);
    }

    
    if (size == 0)
    {
        mm_free(ptr);
        return NULL;
    }

    
    newptr = mm_malloc(size);
    if (newptr == NULL)
    {
        return NULL;
    }

    
    copySize = GET_SIZE(HDRP(ptr)) - DSIZE;

    
    if (size < copySize)
    {
        copySize = size;
    }

    
    memcpy(newptr, ptr, copySize);

    
    mm_free(ptr);

    return newptr;
}
