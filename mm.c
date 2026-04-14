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
 * explicit free list 기반의 메모리 할당기입니다.
 *
 * 1. 사용 중인 블록과 비어 있는 블록을 모두 힙에 배치합니다.
 * 2. 비어 있는 블록(free block)만 따로 연결 리스트로 관리합니다.
 * 3. malloc은 전체 힙을 보지 않고 이 free list에서만 블록을 찾습니다.
 * 4. free된 블록은 리스트로 되돌리고, 주변 free 블록이 있으면 합칩니다.
 * 5. 이 구현은 free list에서 가장 잘 맞는 블록을 찾아내는 best-fit 전략을 사용합니다.
 */

/*
 * WSIZE는 header 또는 footer에 들어가는 메타데이터 크기입니다.
 * 이 구현에서는 header/footer가 4바이트로 저장됩니다.
 */
#define WSIZE 4

/*
 * DSIZE는 블록 정렬 기준과 prologue 블록 크기에 사용합니다.
 * 대부분 환경에서 8바이트 정렬은 효율적인 메모리 접근을 보장합니다.
 */
#define DSIZE 8

/*
 * PTRSIZE는 포인터 크기입니다.
 * 이 시스템은 64비트이므로 보통 8바이트입니다.
 * explicit free list의 prev/next 포인터를 저장할 때 사용합니다.
 */
#define PTRSIZE sizeof(void *)

/*
 * CHUNKSIZE는 힙을 한 번에 늘릴 기본 단위입니다.
 * 너무 작은 단위로 자주 요청하면 성능이 떨어집니다.
 */
#define CHUNKSIZE (1 << 12)

/*
 * ALIGN은 입력 크기를 8바이트 경계로 맞춥니다.
 * 8바이트 정렬은 대부분 시스템에서 안정적인 메모리 접근을 돕습니다.
 */
#define ALIGN(size) (((size) + (DSIZE - 1)) & ~0x7)

/*
 * MAX는 두 값 중 큰 값을 반환합니다.
 * 힙을 확장할 때 요청 크기와 기본 확장 크기 중 큰 쪽을 사용합니다.
 */
#define MAX(x, y) ((x) > (y) ? (x) : (y))

/*
 * explicit free list의 free block은
 * [header][prev ptr][next ptr][...][footer]
 * 구조를 가집니다.
 *
 * header/footer는 블록 크기와 사용 여부를 저장합니다.
 * prev/next 포인터는 free list에서 앞뒤 블록을 연결합니다.
 *
 * 따라서 최소 free block 크기는 다음과 같습니다.
 *   header 4 + prev 8 + next 8 + footer 4 = 24바이트
 * 이는 8바이트 정렬에도 맞기 때문에 MINBLOCKSIZE로 사용합니다.
 *
 * 이 구현에서는 free된 모든 블록이 free list에 들어가야 합니다.
 * 그래서 alloc된 블록도 최소 크기를 이 값 이상으로 만들어야
 * 나중에 free될 때 prev/next 포인터를 저장할 수 있습니다.
 */
#define MINBLOCKSIZE ALIGN(2 * WSIZE + 2 * PTRSIZE)

/*
 * PACK은 블록 크기와 사용 여부 비트를 하나의 값으로 결합합니다.
 * header/footer에 저장할 때 사용합니다.
 */
#define PACK(size, alloc) ((size) | (alloc))

/*
 * GET은 주소 p에 저장된 4바이트 값을 읽습니다.
 */
#define GET(p) (*(unsigned int *)(p))

/*
 * PUT은 주소 p에 4바이트 값을 씁니다.
 */
#define PUT(p, val) (*(unsigned int *)(p) = (val))

/*
 * GET_SIZE는 header/footer 값에서 크기만 골라냅니다.
 * 마지막 3비트는 정렬과 alloc 정보이므로 없앱니다.
 */
#define GET_SIZE(p) (GET(p) & ~0x7)

/*
 * GET_ALLOC은 header/footer에서 사용 여부 비트만 추출합니다.
 * 1이면 사용 중, 0이면 자유 상태입니다.
 */
#define GET_ALLOC(p) (GET(p) & 0x1)

/*
 * HDRP는 블록 payload 포인터 bp에서 header 위치를 계산합니다.
 */
#define HDRP(bp) ((char *)(bp) - WSIZE)

/*
 * FTRP는 블록 payload 포인터 bp에서 footer 위치를 계산합니다.
 */
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/*
 * NEXT_BLKP는 현재 블록 다음에 있는 블록의 payload 시작 주소를 구합니다.
 */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))

/*
 * PREV_BLKP는 이전 블록의 payload 시작 주소를 구합니다.
 * 이전 블록 footer에 있는 크기 정보를 사용합니다.
 */
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE((char *)(bp) - DSIZE))

/*
 * PRED_PTR는 free 블록에서 이전 free 블록 포인터를 가리킵니다.
 */
#define PRED_PTR(bp) (*(void **)(bp))

/*
 * SUCC_PTR는 free 블록에서 다음 free 블록 포인터를 가리킵니다.
 */
#define SUCC_PTR(bp) (*(void **)((char *)(bp) + PTRSIZE))

/*
 * heap_listp는 힙 시작부분의 기준점입니다.
 * prologue 블록의 payload 위치를 가리킵니다.
 */
static char *heap_listp = NULL;

/*
 * free_listp는 free 블록들을 연결한 explicit free list의 첫 블록입니다.
 * 리스트가 비어 있으면 NULL입니다.
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
 * mm_init은 allocator가 처음 시작할 때 호출됩니다.
 *
 * 이 함수는 힙의 맨 앞에 '약속된 공간'을 만들어 둡니다.
 * 이 약속 공간은 경계 처리(힙의 처음과 끝)를 단순하게 하기 위해서입니다.
 *
 * 구조는 다음과 같습니다.
 *   [패딩][prologue header][prologue footer][epilogue header]
 *
 * prologue와 epilogue는 실제로 사용되는 데이터가 아니라,
 * 블록 검색과 병합을 쉽게 해주는 가짜 블록입니다.
 */
int mm_init(void)
{
    /* 초기에는 free list가 비어 있으므로 NULL로 시작합니다. */
    free_listp = NULL;

    /*
     * 힙의 맨 앞에 4워드를 확보하여 prologue/epilogue를 만듭니다.
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
 * extend_heap은 힙을 더 사용할 수 있도록 시스템에서 메모리를 추가로 받습니다.
 *
 * 받은 메모리 영역은 처음에는 하나의 큰 free block으로 생각합니다.
 * 그리고 그 뒤에 다시 epilogue header를 붙여 힙 끝을 표시합니다.
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /*
     * 힙 영역은 8바이트 정렬이 필요하므로,
     * 단어 개수가 홀수이면 하나를 더 늘려서 짝수로 맞춥니다.
     */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    /* 필요한 만큼 힙을 늘리고, 새 영역의 시작 주소를 받습니다. */
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
 * coalesce는 방금 free된 블록 주변에 또 다른 free 블록이 있는지 확인합니다.
 *
 * 만약 앞쪽이나 뒤쪽 블록이 비어 있다면 하나의 더 큰 블록으로 합칩니다.
 * 이 과정에서 free list에 있던 작은 블록들은 먼저 목록에서 제거하고,
 * 병합된 뒤에 다시 목록에 넣습니다.
 */
static void *coalesce(void *bp)
{
    /* 이전 블록이 비어 있는지 확인합니다. */
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));

    /* 다음 블록이 비어 있는지 확인합니다. */
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
 * find_fit은 free 목록에서 요청한 크기에 가장 잘 맞는 블록을 찾습니다.
 *
 * best-fit 전략이므로, 충분히 크면서도 남는 공간이 가장 작은 블록을 선택합니다.
 * 이 블록이 가장 알맞다고 판단되면 그 블록의 위치를 반환합니다.
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
 * place는 find_fit으로 찾은 free 블록에 실제 데이터를 넣을 준비를 합니다.
 *
 * 블록이 요청 크기보다 크면, 앞쪽은 할당된 공간으로 쓰고
 * 뒤쪽 남는 부분은 새로운 free 블록으로 남깁니다.
 * 남는 부분이 너무 작으면 분할하지 않고 전체 블록을 할당합니다.
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
 * mm_malloc은 사용자의 요청 크기를 만족하는 메모리 블록을 찾아 반환합니다.
 *
 * 먼저 필요한 크기를 계산하고 free list에서 찾습니다.
 * 없으면 힙을 더 늘려서 새 블록을 만든 뒤 사용합니다.
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
 * mm_free는 사용한 블록을 다시 비어 있는 상태로 돌립니다.
 *
 * 이때 주변 블록도 같이 비어 있으면 하나로 합쳐서
 * 다음에 더 큰 블록을 만들 수 있게 합니다.
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
 * mm_realloc은 이미 할당된 블록의 크기를 바꿉니다.
 *
 * 이 구현은 간단한 방법으로, 새 블록을 만들고 데이터를 복사한 뒤
 * 기존 블록을 해제합니다.
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
