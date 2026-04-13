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

/*
 * 이 구현은 explicit free list 방식의 메모리 할당기입니다.
 *
 * 핵심 아이디어는 간단합니다.
 * 1. free block만 따로 연결 리스트로 관리합니다.
 * 2. malloc은 힙 전체가 아니라 free list만 훑어서 블록을 찾습니다.
 * 3. free된 블록은 free list 맨 앞에 다시 넣습니다.
 * 4. 인접한 free block이 있으면 하나의 큰 block으로 합칩니다.
 */

/*
 * WSIZE는 header 또는 footer 한 칸의 크기입니다.
 * 이 과제의 기본 block 메타데이터는 4바이트 단위로 저장합니다.
 */
#define WSIZE 4

/*
 * DSIZE는 8바이트입니다.
 * 이 값은 정렬 단위이자 header+footer의 총 크기로도 쓰입니다.
 */
#define DSIZE 8

/*
 * PTRSIZE는 포인터 크기입니다.
 * 지금 환경은 64비트라 보통 8바이트입니다.
 * explicit free list의 prev/next 포인터 저장에 사용합니다.
 */
#define PTRSIZE sizeof(void *)

/*
 * CHUNKSIZE는 힙을 한 번 늘릴 때 기본으로 확보할 크기입니다.
 * 너무 조금씩 늘리면 비효율적이라 4KB 단위로 요청합니다.
 */
#define CHUNKSIZE (1 << 12)

/*
 * ALIGN은 어떤 크기를 8바이트 배수로 올림합니다.
 * 예를 들어 13이면 16으로, 24면 그대로 24로 맞춥니다.
 */
#define ALIGN(size) (((size) + (DSIZE - 1)) & ~0x7)

/*
 * MAX는 두 값 중 더 큰 값을 고릅니다.
 * 힙 확장 크기를 계산할 때 사용합니다.
 */
#define MAX(x, y) ((x) > (y) ? (x) : (y))

/*
 * explicit free list의 free block은
 * [header][prev ptr][next ptr][...][footer]
 * 형태를 가져야 합니다.
 *
 * 그래서 최소 free block 크기는
 * header 4 + prev 8 + next 8 + footer 4 = 24바이트이고,
 * 24는 이미 8의 배수라 그대로 최소 크기로 사용할 수 있습니다.
 *
 * 이 구현에서는 free된 모든 block이 free list에 들어가야 하므로,
 * allocated block도 최소한 이 크기 이상으로 잡아 두어야
 * 나중에 free될 때 prev/next 포인터를 저장할 수 있습니다.
 */
#define MINBLOCKSIZE ALIGN(2 * WSIZE + 2 * PTRSIZE)

/*
 * PACK은 block size와 alloc 비트를 하나의 값으로 합칩니다.
 * alloc이 1이면 사용 중, 0이면 free 상태입니다.
 */
#define PACK(size, alloc) ((size) | (alloc))

/*
 * GET은 주소 p에 저장된 4바이트 값을 읽습니다.
 * header/footer는 이 매크로로 읽습니다.
 */
#define GET(p) (*(unsigned int *)(p))

/*
 * PUT은 주소 p에 4바이트 값을 씁니다.
 * header/footer를 기록할 때 사용합니다.
 */
#define PUT(p, val) (*(unsigned int *)(p) = (val))

/*
 * GET_SIZE는 header/footer에서 block size 부분만 꺼냅니다.
 * 아래 3비트는 정렬과 alloc 비트 때문에 size 정보가 아니므로 제거합니다.
 */
#define GET_SIZE(p) (GET(p) & ~0x7)

/*
 * GET_ALLOC은 header/footer에서 alloc 비트만 꺼냅니다.
 * 결과가 1이면 allocated, 0이면 free입니다.
 */
#define GET_ALLOC(p) (GET(p) & 0x1)

/*
 * HDRP는 payload 포인터 bp에서 해당 block의 header 주소를 계산합니다.
 */
#define HDRP(bp) ((char *)(bp) - WSIZE)

/*
 * FTRP는 payload 포인터 bp에서 해당 block의 footer 주소를 계산합니다.
 * block 전체 크기에서 header와 footer 위치 관계를 이용합니다.
 */
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/*
 * NEXT_BLKP는 현재 block 바로 다음 block의 payload 시작 주소를 구합니다.
 */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))

/*
 * PREV_BLKP는 현재 block 바로 앞 block의 payload 시작 주소를 구합니다.
 * 이전 block의 footer에 저장된 size를 읽어서 뒤로 이동합니다.
 */
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE))

/*
 * PRED_PTR는 free block payload의 맨 앞 칸을 이전 free block 포인터로 봅니다.
 * 즉 bp 위치에는 prev 포인터가 저장됩니다.
 */
#define PRED_PTR(bp) (*(void **)(bp))

/*
 * SUCC_PTR는 free block payload의 그다음 칸을 다음 free block 포인터로 봅니다.
 * 즉 bp + PTRSIZE 위치에는 next 포인터가 저장됩니다.
 */
#define SUCC_PTR(bp) (*(void **)((char *)(bp) + PTRSIZE))

/*
 * heap_listp는 힙의 시작 쪽을 가리키는 기준 포인터입니다.
 * 보통 prologue block의 payload 위치를 가리키게 됩니다.
 */
static char *heap_listp = NULL;

/*
 * free_listp는 explicit free list의 첫 번째 free block을 가리킵니다.
 * free block이 하나도 없으면 NULL입니다.
 */
static void *free_listp = NULL;

/*
 * 아래 함수들은 이 파일 안에서만 쓰는 내부 helper들입니다.
 */
static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);
static void insert_free_block(void *bp);
static void remove_free_block(void *bp);

/*
 * insert_free_block은 free block을 free list 맨 앞에 넣습니다.
 * 구현이 단순하고 빠르기 때문에 LIFO 방식으로 관리합니다.
 */
static void insert_free_block(void *bp)
{
    /* 새 block의 next는 기존 free list의 맨 앞 block을 가리키게 합니다. */
    SUCC_PTR(bp) = free_listp;

    /* 새 block은 이제 맨 앞이므로 이전 block이 없습니다. */
    PRED_PTR(bp) = NULL;

    /* 기존 맨 앞 block이 있었다면, 그 block의 prev를 새 block으로 바꿉니다. */
    if (free_listp != NULL)
    {
        PRED_PTR(free_listp) = bp;
    }

    /* free list의 시작점을 새 block으로 갱신합니다. */
    free_listp = bp;
}

/*
 * remove_free_block은 free list 안에 있는 특정 block을 리스트에서 뺍니다.
 * malloc으로 사용할 block을 고를 때나, coalesce 전에 이 함수를 사용합니다.
 */
static void remove_free_block(void *bp)
{
    /* pred는 bp 앞에 있는 free block입니다. */
    void *pred = PRED_PTR(bp);

    /* succ는 bp 뒤에 있는 free block입니다. */
    void *succ = SUCC_PTR(bp);

    /* 앞 block이 있다면 그 block의 next가 bp를 건너뛰게 만듭니다. */
    if (pred != NULL)
    {
        SUCC_PTR(pred) = succ;
    }
    else
    {
        /* bp가 맨 앞 block이었다면, free list 시작점을 다음 block으로 바꿉니다. */
        free_listp = succ;
    }

    /* 뒤 block이 있다면 그 block의 prev가 bp를 건너뛰게 만듭니다. */
    if (succ != NULL)
    {
        PRED_PTR(succ) = pred;
    }
}

/*
 * mm_init은 allocator가 사용할 초기 힙 구조를 만듭니다.
 *
 * 맨 앞에는 padding, prologue, epilogue를 두어서
 * 경계 조건을 단순하게 처리합니다.
 */
int mm_init(void)
{
    /* 초기에는 free list가 비어 있으므로 NULL로 시작합니다. */
    free_listp = NULL;

    /*
     * 힙 시작 부분에 필요한 기본 구조를 만들기 위해 4워드를 확보합니다.
     * [padding][prologue header][prologue footer][epilogue header]
     */
    heap_listp = mem_sbrk(4 * WSIZE);

    /* mem_sbrk가 실패하면 초기화도 실패입니다. */
    if (heap_listp == (void *)-1)
    {
        return -1;
    }

    /* 첫 워드는 정렬을 맞추기 위한 padding입니다. */
    PUT(heap_listp, 0);

    /* prologue header는 크기 8, allocated 상태의 가짜 block입니다. */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));

    /* prologue footer도 같은 값을 기록합니다. */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));

    /* epilogue header는 크기 0, allocated 상태로 힙 끝을 표시합니다. */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));

    /* heap_listp를 prologue의 payload 위치로 이동해 두면 순회 시작점으로 쓰기 편합니다. */
    heap_listp += (2 * WSIZE);

    /* 초기 free block 하나를 만들기 위해 힙을 크게 한 번 늘립니다. */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
    {
        return -1;
    }

    return 0;
}

/*
 * extend_heap은 memlib에서 새 힙 공간을 받아 free block 하나를 만듭니다.
 * 그리고 그 뒤에는 새 epilogue header를 다시 붙입니다.
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /*
     * block 크기는 8바이트 정렬이 맞아야 하므로,
     * word 개수가 홀수면 하나 더 늘려 짝수 워드로 맞춥니다.
     */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    /* 필요한 만큼 힙을 늘리고, 새 영역 시작 주소를 받습니다. */
    bp = mem_sbrk(size);

    /* 힙을 더 늘릴 수 없으면 실패입니다. */
    if (bp == (void *)-1)
    {
        return NULL;
    }

    /* 새 free block의 header를 기록합니다. */
    PUT(HDRP(bp), PACK(size, 0));

    /* 새 free block의 footer도 같은 값으로 기록합니다. */
    PUT(FTRP(bp), PACK(size, 0));

    /* 새 free block 뒤에는 새 epilogue header를 붙입니다. */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    /* 주변 free block과 합칠 수 있으면 합치고, free list에도 넣습니다. */
    return coalesce(bp);
}

/*
 * coalesce는 방금 free된 block 주변에 free block이 있는지 확인해서 병합합니다.
 * explicit free list에서는 병합되기 전에 기존 free block들을 리스트에서 빼고,
 * 병합이 끝난 새 block을 다시 리스트에 넣어야 합니다.
 */
static void *coalesce(void *bp)
{
    /* 이전 block이 allocated인지 free인지 확인합니다. */
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));

    /* 다음 block이 allocated인지 free인지 확인합니다. */
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));

    /* 현재 block의 크기를 읽어 둡니다. */
    size_t size = GET_SIZE(HDRP(bp));

    /* 앞뒤 모두 사용 중이면 병합할 대상이 없으니 현재 block만 free list에 넣습니다. */
    if (prev_alloc && next_alloc)
    {
        insert_free_block(bp);
        return bp;
    }

    /* 뒤 block만 free면, 뒤 block을 free list에서 빼고 현재 block과 합칩니다. */
    if (prev_alloc && !next_alloc)
    {
        remove_free_block(NEXT_BLKP(bp));
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
        insert_free_block(bp);
        return bp;
    }

    /* 앞 block만 free면, 앞 block을 free list에서 빼고 앞쪽으로 합칩니다. */
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

    /* 앞뒤 둘 다 free면 둘 다 free list에서 빼고 세 block을 한 번에 합칩니다. */
    remove_free_block(PREV_BLKP(bp));
    remove_free_block(NEXT_BLKP(bp));
    size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
    PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
    bp = PREV_BLKP(bp);
    insert_free_block(bp);
    return bp;
}

/*
 * find_fit은 explicit free list 안에서 요청 크기에 가장 잘 맞는 free block을 찾습니다.
 * 이 구현은 best fit이므로, 끝까지 순회하면서 남는 공간이 가장 적은 block을 고릅니다.
 */
static void *find_fit(size_t asize)
{
    /* cur은 현재 보고 있는 free block입니다. */
    void *cur = free_listp;

    /* best_bp는 지금까지 찾은 후보 중 가장 잘 맞는 block입니다. */
    void *best_bp = NULL;

    /* best_size는 지금까지 찾은 후보의 크기입니다. */
    size_t best_size = 0;

    /* free list 끝에 도달할 때까지 순회합니다. */
    while (cur != NULL)
    {
        /* csize는 현재 free block의 전체 크기입니다. */
        size_t csize = GET_SIZE(HDRP(cur));

        /* 현재 block이 요청 크기를 담을 수 있으면 best 후보와 비교합니다. */
        if (csize >= asize)
        {
            /*
             * 아직 후보가 없거나,
             * 현재 block이 이전 후보보다 더 작아서 더 딱 맞으면 후보를 갱신합니다.
             */
            if (best_bp == NULL || csize < best_size)
            {
                best_bp = cur;
                best_size = csize;
            }

            /* 요청 크기와 정확히 같은 block을 찾았으면 더 볼 필요가 없습니다. */
            if (csize == asize)
            {
                break;
            }
        }

        /* 아니면 다음 free block으로 이동합니다. */
        cur = SUCC_PTR(cur);
    }

    /* 끝까지 검사한 뒤 가장 잘 맞는 block을 반환합니다. 못 찾았으면 NULL입니다. */
    return best_bp;
}

/*
 * place는 찾은 free block 안에 allocated block을 실제로 배치합니다.
 * 남는 조각이 너무 작지 않으면 split해서 뒤쪽을 다시 free block으로 남깁니다.
 */
static void place(void *bp, size_t asize)
{
    /* 현재 free block의 전체 크기를 읽습니다. */
    size_t csize = GET_SIZE(HDRP(bp));

    /* 이제 이 block은 free가 아니므로 먼저 free list에서 제거합니다. */
    remove_free_block(bp);

    /*
     * 남는 공간이 최소 free block 크기 이상이면 쪼개는 것이 이득입니다.
     * 그래야 남은 조각도 나중에 정상적인 free block으로 재사용할 수 있습니다.
     */
    if ((csize - asize) >= MINBLOCKSIZE)
    {
        /* 앞부분은 요청한 크기만큼 allocated block으로 표시합니다. */
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));

        /* 다음 block 위치가 곧 남은 조각의 시작점입니다. */
        void *next_bp = NEXT_BLKP(bp);

        /* 남은 조각의 header를 free 상태로 기록합니다. */
        PUT(HDRP(next_bp), PACK(csize - asize, 0));

        /* 남은 조각의 footer도 free 상태로 기록합니다. */
        PUT(FTRP(next_bp), PACK(csize - asize, 0));

        /* 남은 조각은 새 free block이므로 free list에 넣습니다. */
        insert_free_block(next_bp);
    }
    else
    {
        /* 남는 공간이 너무 작으면 쪼개지 않고 block 전체를 그냥 할당합니다. */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/*
 * mm_malloc은 사용자가 요청한 크기 이상의 payload를 담을 block을 반환합니다.
 */
void *mm_malloc(size_t size)
{
    /* asize는 정렬과 메타데이터를 반영한 실제 block 크기입니다. */
    size_t asize;

    /* extendsize는 힙을 늘릴 때 실제로 얼마나 늘릴지 결정하는 값입니다. */
    size_t extendsize;

    /* bp는 최종적으로 사용할 block 포인터입니다. */
    char *bp;

    /* malloc(0)은 할당할 의미가 없으므로 NULL을 반환합니다. */
    if (size == 0)
    {
        return NULL;
    }

    /*
     * 실제 block 크기를 계산합니다.
     * 요청한 payload 크기에 header와 footer를 더하고,
     * 최소 block 크기보다 작으면 최소 block 크기로 올립니다.
     */
    asize = ALIGN(size + DSIZE);
    if (asize < MINBLOCKSIZE)
    {
        asize = MINBLOCKSIZE;
    }

    /* free list에서 best fit으로 가장 잘 맞는 block을 찾아봅니다. */
    bp = find_fit(asize);
    if (bp != NULL)
    {
        place(bp, asize);
        return bp;
    }

    /* 맞는 block이 없으면 힙을 늘립니다. */
    extendsize = MAX(asize, CHUNKSIZE);
    bp = extend_heap(extendsize / WSIZE);
    if (bp == NULL)
    {
        return NULL;
    }

    /* 새로 확보한 free block 안에 요청 block을 배치합니다. */
    place(bp, asize);
    return bp;
}

/*
 * mm_free는 allocated block을 free 상태로 바꾸고, 가능하면 주변과 합칩니다.
 */
void mm_free(void *ptr)
{
    /* free할 block의 크기를 읽어 둡니다. */
    size_t size;

    /* free(NULL)은 아무 동작도 하지 않는 것이 표준 동작입니다. */
    if (ptr == NULL)
    {
        return;
    }

    /* 현재 block의 전체 크기를 읽습니다. */
    size = GET_SIZE(HDRP(ptr));

    /* header를 free 상태로 바꿉니다. */
    PUT(HDRP(ptr), PACK(size, 0));

    /* footer도 free 상태로 바꿉니다. */
    PUT(FTRP(ptr), PACK(size, 0));

    /* 주변 free block이 있으면 합치고, free list에도 넣습니다. */
    coalesce(ptr);
}

/*
 * mm_realloc은 가장 단순한 방식으로 구현합니다.
 * 새 block을 malloc으로 받은 뒤, 기존 데이터를 복사하고, 이전 block을 free합니다.
 */
void *mm_realloc(void *ptr, size_t size)
{
    /* newptr은 새로 받을 block입니다. */
    void *newptr;

    /* copySize는 실제로 복사할 바이트 수입니다. */
    size_t copySize;

    /* realloc(NULL, size)는 malloc(size)와 같습니다. */
    if (ptr == NULL)
    {
        return mm_malloc(size);
    }

    /* realloc(ptr, 0)은 free(ptr) 후 NULL을 반환하는 것과 같습니다. */
    if (size == 0)
    {
        mm_free(ptr);
        return NULL;
    }

    /* 새 크기에 맞는 block을 먼저 하나 할당합니다. */
    newptr = mm_malloc(size);
    if (newptr == NULL)
    {
        return NULL;
    }

    /*
     * 기존 payload에서 실제로 복사 가능한 최대 크기는
     * old block size - header/footer 크기 입니다.
     */
    copySize = GET_SIZE(HDRP(ptr)) - DSIZE;

    /* 새 요청 크기가 더 작으면 그 크기까지만 복사합니다. */
    if (size < copySize)
    {
        copySize = size;
    }

    /* 기존 데이터를 새 block으로 복사합니다. */
    memcpy(newptr, ptr, copySize);

    /* 예전 block은 이제 필요 없으므로 free합니다. */
    mm_free(ptr);

    return newptr;
}
