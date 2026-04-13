/*
 * mm.c - 간단한 implicit free list 기반 메모리 할당기
 *
 * 이 구현은 CS:APP 3판 9.9장의 핵심 아이디어를 바탕으로 만들었습니다.
 *
 * 1. 각 블록은 헤더(header)와 푸터(footer)를 가지며,
 *    여기에 블록 크기와 할당 여부를 저장합니다.
 * 2. 사용 가능한 free 블록은 힙의 앞쪽부터 차례대로 훑으면서 찾습니다.
 *    이것을 implicit free list 방식이라고 합니다.
 * 3. free를 호출하면, 주변의 free 블록과 바로 합칠 수 있는지 확인합니다.
 *    이 과정을 coalescing(병합)이라고 합니다.
 * 4. free 블록이 필요한 크기보다 충분히 크면,
 *    앞부분은 할당하고 남는 뒷부분은 다시 free 블록으로 나눕니다.
 *
 * 이 코드의 목표는 최고 성능을 내는 것이 아니라,
 * 교재의 기본 아이디어를 이해하기 쉬운 형태로 구현하는 것입니다.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

/*********************************************************
 * 학생 정보는 아래 구조체에 작성합니다.
 * 다른 작업을 하기 전에 이 부분부터 채워 넣으면 됩니다.
 ********************************************************/
team_t team = {
    /* 팀 이름 */
    "Hogwart",
    /* 첫 번째 팀원의 이름 */
    "Harry Potter",
    /* 첫 번째 팀원의 이메일 */
    "bovik@cs.cmu.edu",
    /* 두 번째 팀원의 이름(없으면 빈 문자열) */
    "",
    /* 두 번째 팀원의 이메일(없으면 빈 문자열) */
    ""};

/* 교재 allocator에서 메타데이터 한 칸(word)의 크기는 4바이트입니다. */
#define WSIZE 4

/* double word는 8바이트이며, 이 과제의 정렬 단위와도 같습니다. */
#define DSIZE 8

/* 힙을 늘릴 때 최소한 이 정도 크기만큼 한 번에 요청합니다. */
#define CHUNKSIZE (1 << 12)

/* 블록이 가질 수 있는 최소 크기: 헤더 + 푸터 + 정렬된 payload 공간 */
#define MINBLOCKSIZE (2 * DSIZE)

/* 두 값 중 더 큰 값을 고릅니다. */
#define MAX(x, y) ((x) > (y) ? (x) : (y))

/*
 * PACK은 두 정보를 하나의 word 크기 값으로 합칩니다.
 *
 * size  : 블록 전체 크기(바이트 단위)
 * alloc : 할당된 블록이면 1, free 블록이면 0
 *
 * 블록 크기는 항상 8의 배수이므로,
 * 아래쪽 몇 비트는 비어 있습니다.
 * 그중 가장 낮은 1비트를 할당 여부 저장에 사용합니다.
 */
#define PACK(size, alloc) ((size) | (alloc))

/* 주소 p에서 4바이트 값을 읽습니다. */
#define GET(p) (*(unsigned int *)(p))

/* 주소 p에 4바이트 값 val을 기록합니다. */
#define PUT(p, val) (*(unsigned int *)(p) = (val))

/* 헤더나 푸터 값에서 블록 크기만 꺼냅니다. */
#define GET_SIZE(p) (GET(p) & ~0x7)

/* 헤더나 푸터 값에서 할당 비트만 꺼냅니다. */
#define GET_ALLOC(p) (GET(p) & 0x1)

/* payload 포인터 bp로부터 해당 블록의 헤더 주소를 구합니다. */
#define HDRP(bp) ((char *)(bp) - WSIZE)

/* payload 포인터 bp로부터 해당 블록의 푸터 주소를 구합니다. */
/* GET_SIZE(HDRP(bp)) = [header + payload + footer] */
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

/* 현재 블록 다음에 있는 블록으로 이동합니다. */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))

/* 현재 블록 바로 앞에 있는 블록으로 이동합니다. */
/* 이전 블록 payload 시작점을 가리킴*/
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

/*
 * heap_listp는 prologue 블록 쪽을 가리킵니다.
 * 힙을 순차적으로 탐색할 때 시작점으로 사용합니다.그럼 주소 p
 */
static char *heap_listp = NULL;

/* 이 파일 안에서만 사용할 보조 함수들입니다. */
static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void place(void *bp, size_t asize);

/*
 * mm_init - 초기 빈 힙 구조를 만듭니다.
 *
 * 이 함수가 끝난 뒤 힙의 시작 부분은 다음과 같은 모양이 됩니다.
 * padding -> prologue block -> epilogue header
 *
 * prologue는 실제 데이터용 블록이 아니라,
 * 경계 조건 처리를 쉽게 하려고 넣는 가짜 allocated 블록입니다.
 * epilogue는 크기가 0인 allocated 헤더로,
 * 힙의 끝을 표시하는 역할을 합니다.
 */
int mm_init(void)
{
    /* 힙의 기본 뼈대를 만들기 위해 word 4개 크기만큼 먼저 확보합니다. */
    heap_listp = mem_sbrk(4 * WSIZE);

    /* mem_sbrk가 실패하면 초기화도 실패한 것으로 처리합니다. */
    if (heap_listp == (void *)-1)
    {
        return -1;
    }

    /* 정렬을 맞추기 위한 padding입니다. */
    PUT(heap_listp, 0);

    /* prologue 헤더: 크기 8, 할당됨 */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));

    /* prologue 푸터: 헤더와 같은 내용을 기록합니다. */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));

    /* epilogue 헤더: 크기 0, 할당됨 */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));

    /* heap_listp를 prologue의 payload 위치로 옮겨 둡니다. */
    heap_listp += (2 * WSIZE);

    /* 첫 번째 실제 free 블록을 만들기 위해 힙을 더 늘립니다. */
    if (extend_heap(CHUNKSIZE / WSIZE) == NULL)
    {
        return -1;
    }

    return 0;
}

/*
 * extend_heap - memlib에 요청해서 힙 공간을 더 늘립니다.
 *
 * 새로 확보한 공간은 하나의 free 블록이 되고,
 * 그 뒤에는 새로운 epilogue 헤더가 붙습니다.
 */
static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    /* 힙 정렬을 유지하려고 word 개수를 짝수로 맞춥니다. */
    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    /* mem_sbrk는 새로 늘어난 공간의 시작 주소를 돌려줍니다. */
    bp = mem_sbrk(size);

    /* 힙을 더 늘릴 수 없으면 실패를 반환합니다. */
    if (bp == (void *)-1)
    {
        return NULL;
    }

    /* 새 free 블록의 헤더를 기록합니다. */
    PUT(HDRP(bp), PACK(size, 0));

    /* 새 free 블록의 푸터를 기록합니다. */
    PUT(FTRP(bp), PACK(size, 0));

    /* free 블록 바로 뒤에 새 epilogue 헤더를 기록합니다. */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    /* 앞 블록도 free라면 합쳐서 더 큰 free 블록으로 만듭니다. */
    return coalesce(bp);
}

/*
 * coalesce - 인접한 free 블록들을 하나로 합칩니다.
 *
 * 경우는 총 4가지입니다.
 * 1. 앞뒤 블록이 모두 할당된 경우
 * 2. 다음 블록만 free인 경우))
 * 3. 이전 블록만 free인 경우
 * 4. 앞뒤 블록이 모두 free인 경우
 */
static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    /* 경우 1: 양옆이 모두 사용 중이면 합칠 것이 없습니다. */
    if (prev_alloc && next_alloc)
    {
        return bp;
    }

    /* 경우 2: 다음 블록이 free이면 현재 블록과 합칩니다. */
    if (prev_alloc && !next_alloc)
    {
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
        return bp;
    }

    /* 경우 3: 이전 블록이 free이면 이전 블록 쪽으로 합칩니다. */
    if (!prev_alloc && next_alloc)
    {
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        return PREV_BLKP(bp);
    }

    /* 경우 4: 앞뒤가 모두 free이면 세 블록을 한 번에 합칩니다. */
    size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
    PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
    return PREV_BLKP(bp);
}

/*
 * find_fit - first-fit 방식으로 맞는 free 블록을 찾습니다.
 *
 * 힙의 앞쪽부터 차례대로 보다가,
 * 요청한 크기를 담을 수 있는 첫 번째 free 블록을 반환합니다.
 */
static void *find_fit(size_t asize)
{
    void *bp;

    /* epilogue를 만날 때까지 블록을 하나씩 검사합니다. */
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp))
    {
        /* free 상태이면서 크기도 충분하면 사용할 수 있습니다. */
        if (!GET_ALLOC(HDRP(bp)) && (asize <= GET_SIZE(HDRP(bp))))
        {
            return bp;
        }
    }

    /* 조건에 맞는 free 블록을 찾지 못했습니다. */
    return NULL;
}

/*
 * place - 찾은 free 블록 안에 allocated 블록을 배치합니다.
 *
 * free 블록이 너무 크면 둘로 나누고,
 * 남는 공간이 애매하게 작으면 통째로 사용합니다.
 */
static void place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));

    /*
     * 남는 공간이 최소 블록 크기 이상일 때만 분할합니다.
     * 너무 작은 조각은 나중에 쓸모가 없어서 그냥 전체를 씁니다.
     */
    if ((csize - asize) >= MINBLOCKSIZE)
    {
        /* 앞부분을 allocated 블록으로 표시합니다. */
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));

        /* 남은 뒷부분은 새로운 free 블록으로 표시합니다. */
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
    }
    else
    {
        /* 남는 공간이 너무 작으면 현재 블록 전체를 할당합니다. */
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

/*
 * mm_malloc - payload로 size 바이트 이상 쓸 수 있는 블록을 반환합니다.
 */
void *mm_malloc(size_t size)
{
    size_t asize;
    size_t extendsize;
    char *bp;

    /* malloc(0)은 NULL을 반환해도 됩니다. */
    if (size == 0)
    {
        return NULL;
    }

    /*
     * 실제로 필요한 블록 크기를 계산합니다.
     *
     * 여기에는 다음이 모두 포함되어야 합니다.
     * - header
     * - footer
     * - 정렬이 맞는 payload 공간
     */
    if (size <= DSIZE)
    {
        asize = MINBLOCKSIZE;
    }
    else
    {
        asize = DSIZE * ((size + DSIZE + (DSIZE - 1)) / DSIZE);
    }

    /* 기존 힙 안에 있는 free 블록을 재사용할 수 있는지 먼저 봅니다. */
    bp = find_fit(asize);
    if (bp != NULL)
    {
        place(bp, asize);
        return bp;
    }

    /* 맞는 블록이 없으면 힙을 더 늘립니다. */
    extendsize = MAX(asize, CHUNKSIZE);
    bp = extend_heap(extendsize / WSIZE);
    if (bp == NULL)
    {
        return NULL;
    }

    /* 새로 확보한 free 블록 안에 요청한 크기의 블록을 배치합니다. */
    place(bp, asize);
    return bp;
}

/*
 * mm_free - 블록을 free 상태로 바꾸고, 가능하면 주변 free 블록과 합칩니다.
 */
void mm_free(void *ptr)
{
    size_t size;

    /* free(NULL)은 아무 일도 하지 않아야 합니다. */
    if (ptr == NULL)
    {
        return;
    }

    /* 메타데이터를 바꾸기 전에 현재 블록 크기를 읽어 둡니다. */
    size = GET_SIZE(HDRP(ptr));

    /* 헤더와 푸터를 모두 free 상태로 표시합니다. */
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));

    /* 양옆에 free 블록이 있으면 합칩니다. */
    coalesce(ptr);
}

/*
 * mm_realloc - allocate -> copy -> free 방식으로 블록 크기를 조정합니다.
 *
 * 이 구현은 이해하기 쉽게 가장 단순한 흐름으로 작성했습니다.
 * 1. 예외 상황을 먼저 처리합니다.
 * 2. 새 블록을 할당합니다.
 * 3. 기존 데이터를 복사합니다.
 * 4. 이전 블록을 free 합니다.
 */
void *mm_realloc(void *ptr, size_t size)
{
    void *newptr;
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

    /* 요청한 크기만큼 새 블록을 하나 할당합니다. */
    newptr = mm_malloc(size);
    if (newptr == NULL)
    {
        return NULL;
    }

    /*
     * 기존 payload 크기는 전체 블록 크기에서 header와 footer를 뺀 값입니다.
     * 복사는 이전 블록과 새 블록이 모두 감당할 수 있는 범위까지만 합니다.
     * min(기존 payload 크기, 새 요청 크기) 만 복사
     */
    copySize = GET_SIZE(HDRP(ptr)) - DSIZE;
    if (size < copySize)
    {
        copySize = size;
    }

    /* 기존 데이터를 새 블록으로 복사합니다. */
    memcpy(newptr, ptr, copySize);

    /* 데이터 이동이 끝났으니 이전 블록은 free 합니다. */
    mm_free(ptr);

    return newptr;
}
