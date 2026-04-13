/*
 * mm.c - explicit free list allocator.
 * free 블록만 연결 리스트로 관리(first-fit), immediate coalesce.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>

#include "mm.h"
#include "memlib.h"

team_t team = {
    "ateam",
    "Harry Bovik",
    "bovik@cs.cmu.edu",
    "",
    ""};

/* 기본 상수 */
#define ALIGNMENT 8 /* 정렬 단위(8바이트) */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1)) /* size를 8의 배수로 올림 */

#define WSIZE 4 /* 워드 크기(헤더/푸터 1칸) */
#define DSIZE 8 /* 더블워드 크기 */
#define CHUNKSIZE (1 << 12) /* 기본 힙 확장 크기(4096바이트) */

#define MAX(x, y) ((x) > (y) ? (x) : (y)) /* 두 값 중 큰 값 반환 */
#define MINBLOCKSIZE 24 /* 최소 블록: hdr4 + ftr4 + prev8 + next8 */

#define PACK(size, alloc) ((size_t)(size) | (size_t)(alloc)) /* 크기와 할당 비트를 한 값으로 결합 */

#define GET(p) (*(unsigned int *)(p)) /* p 주소의 4바이트 값 읽기 */
#define PUT(p, val) (*(unsigned int *)(p) = (val)) /* p 주소에 4바이트 값 쓰기 */

#define GET_SIZE(p) (GET(p) & ~0x7) /* 하위 3비트 제거해서 size 추출 */
#define GET_ALLOC(p) (GET(p) & 0x1) /* 최하위 비트로 alloc 상태 추출 */

#define HDRP(bp) ((char *)(bp)-WSIZE) /* payload 포인터에서 헤더 주소 계산 */
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE) /* payload 포인터에서 푸터 주소 계산 */
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp))) /* 다음 블록 payload 주소 계산 */
#define PREV_BLKP(bp) ((char *)(bp)-GET_SIZE((char *)(bp)-DSIZE)) /* 이전 블록 payload 주소 계산 */

/* free 블록 payload 영역에 prev/next 포인터를 저장 */
#define PRED_PTR(bp) ((char *)(bp)) /* prev 포인터 저장 위치 */
#define SUCC_PTR(bp) ((char *)(bp) + sizeof(void *)) /* next 포인터 저장 위치 */
#define GET_PTR(p) (*(void **)(p)) /* 포인터 값 읽기 */
#define SET_PTR(p, val) (*(void **)(p) = (val)) /* 포인터 값 쓰기 */
#define PRED(bp) (GET_PTR(PRED_PTR(bp))) /* bp의 prev free 블록 포인터 읽기 */
#define SUCC(bp) (GET_PTR(SUCC_PTR(bp))) /* bp의 next free 블록 포인터 읽기 */

static char *heap_listp; /* 프롤로그 payload 위치를 가리키는 포인터 */
static void *free_listp; /* explicit free list의 head 포인터 */

static void *coalesce(void *bp); /* 인접 free 블록 병합 */
static void *extend_heap(size_t words); /* 힙 확장 */
static void *find_fit(size_t asize); /* free list에서 first-fit 탐색 */
static void place(void *bp, size_t asize); /* 선택된 free 블록에 배치/분할 */
static void insert_free_block(void *bp); /* free list 앞에 삽입 */
static void remove_free_block(void *bp); /* free list에서 제거 */

/*
 * mm_init - prologue / epilogue 세팅 후 힙 준비
 */
int mm_init(void) /* 할당기 초기화 함수 */
{
    if ((heap_listp = mem_sbrk(4 * WSIZE)) == (void *)-1) /* 초기 힙 공간 확보 실패 */
        return -1; /* 실패 반환 */
    PUT(heap_listp, 0); /* 정렬 패딩 워드 기록 */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1)); /* 프롤로그 헤더 기록 */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1)); /* 프롤로그 푸터 기록 */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1)); /* 에필로그 헤더 기록 */
    heap_listp += (2 * WSIZE); /* 프롤로그 payload 지점으로 이동 */
    free_listp = NULL; /* free list는 빈 상태로 시작 */

    if (extend_heap(CHUNKSIZE / WSIZE) == NULL) /* 초기 청크 확장 실패 */
        return -1; /* 실패 반환 */

    return 0; /* 성공 반환 */
}

/*
 * extend_heap - epilogue 자리부터 free 블록 + 새 epilogue
 */
static void *extend_heap(size_t words) /* words 단위로 힙 확장 */
{
    char *bp; /* 새 free 블록 payload 포인터 */
    size_t size; /* 바이트 단위 확장 크기 */

    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE; /* 8바이트 정렬 맞춤 */
    if (size < MINBLOCKSIZE) /* 너무 작으면 */
        size = MINBLOCKSIZE; /* 최소 블록 크기로 보정 */
    if ((bp = mem_sbrk((int)size)) == (void *)-1) /* sbrk 실패 */
        return NULL; /* 실패 반환 */

    PUT(HDRP(bp), PACK(size, 0)); /* 새 free 블록 헤더 설정 */
    PUT(FTRP(bp), PACK(size, 0)); /* 새 free 블록 푸터 설정 */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* 새 에필로그 헤더 설정 */

    return coalesce(bp); /* 인접 free와 병합 후 반환 */
}

static void insert_free_block(void *bp) /* free list 앞(head)에 bp 삽입 */
{
    SET_PTR(PRED_PTR(bp), NULL); /* 새 head이므로 prev는 NULL */
    SET_PTR(SUCC_PTR(bp), free_listp); /* 기존 head를 next로 연결 */

    if (free_listp != NULL) /* 기존 head가 있었다면 */
        SET_PTR(PRED_PTR(free_listp), bp); /* 기존 head의 prev를 bp로 변경 */
    free_listp = bp; /* bp를 새 head로 지정 */
}

static void remove_free_block(void *bp) /* free list에서 bp 제거 */
{
    void *pred = PRED(bp); /* bp의 prev 노드 */
    void *succ = SUCC(bp); /* bp의 next 노드 */

    if (pred != NULL) /* prev가 있으면 */
        SET_PTR(SUCC_PTR(pred), succ); /* prev의 next를 succ로 연결 */
    else /* bp가 head였다면 */
        free_listp = succ; /* head를 succ로 갱신 */

    if (succ != NULL) /* next가 있으면 */
        SET_PTR(PRED_PTR(succ), pred); /* next의 prev를 pred로 연결 */
}

static void *coalesce(void *bp) /* 인접 free 블록 병합 */
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp))); /* 이전 블록 할당 여부 */
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp))); /* 다음 블록 할당 여부 */
    size_t size = GET_SIZE(HDRP(bp)); /* 현재 블록 크기 */

    if (prev_alloc && next_alloc) { /* 양옆 모두 할당된 경우 */
        insert_free_block(bp); /* 현재 블록만 free list에 넣음 */
        return bp; /* 현재 블록 반환 */
    }

    if (prev_alloc && !next_alloc) { /* 다음 블록만 free인 경우 */
        remove_free_block(NEXT_BLKP(bp)); /* 다음 블록을 list에서 제거 */
        size += GET_SIZE(HDRP(NEXT_BLKP(bp))); /* 현재+다음 크기로 합침 */
        PUT(HDRP(bp), PACK(size, 0)); /* 병합된 헤더 기록 */
        PUT(FTRP(bp), PACK(size, 0)); /* 병합된 푸터 기록 */
        insert_free_block(bp); /* 병합 블록 삽입 */
        return bp; /* 병합 블록 반환 */
    }

    if (!prev_alloc && next_alloc) { /* 이전 블록만 free인 경우 */
        remove_free_block(PREV_BLKP(bp)); /* 이전 블록을 list에서 제거 */
        size += GET_SIZE(HDRP(PREV_BLKP(bp))); /* 이전+현재 크기로 합침 */
        PUT(FTRP(bp), PACK(size, 0)); /* 병합된 푸터 기록 */
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); /* 병합된 헤더 기록 */
        bp = PREV_BLKP(bp); /* bp를 병합 시작 위치로 이동 */
        insert_free_block(bp); /* 병합 블록 삽입 */
        return bp; /* 병합 블록 반환 */
    }

    remove_free_block(PREV_BLKP(bp)); /* 이전 free 블록 제거 */
    remove_free_block(NEXT_BLKP(bp)); /* 다음 free 블록 제거 */
    size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp))); /* 이전+현재+다음 크기 합산 */
    PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0)); /* 병합 블록 헤더 기록 */
    PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0)); /* 병합 블록 푸터 기록 */
    bp = PREV_BLKP(bp); /* 병합 시작 블록으로 bp 이동 */
    insert_free_block(bp); /* 병합 블록 삽입 */
    return bp; /* 병합 블록 반환 */
}

/*
 * find_fit - explicit free list first-fit
 */
static void *find_fit(size_t asize) /* 요청 크기에 맞는 free 블록 탐색 */
{
    void *bp; /* 순회 포인터 */

    for (bp = free_listp; bp != NULL; bp = SUCC(bp)) { /* free list만 순회 */
        if (asize <= GET_SIZE(HDRP(bp))) /* 충분히 큰 블록이면 */
            return bp; /* 해당 블록 반환 */
    }

    return NULL; /* 못 찾으면 NULL */
}

static void place(void *bp, size_t asize) /* bp에 asize만큼 할당 배치 */
{
    size_t csize = GET_SIZE(HDRP(bp)); /* 현재 free 블록 전체 크기 */

    remove_free_block(bp); /* 할당 대상 블록을 free list에서 제거 */

    if ((csize - asize) >= MINBLOCKSIZE) { /* 나머지가 최소 블록 이상이면 분할 */
        PUT(HDRP(bp), PACK(asize, 1)); /* 앞부분을 할당 블록 헤더로 기록 */
        PUT(FTRP(bp), PACK(asize, 1)); /* 앞부분을 할당 블록 푸터로 기록 */

        bp = NEXT_BLKP(bp); /* bp를 뒤쪽 나머지 블록으로 이동 */
        PUT(HDRP(bp), PACK(csize - asize, 0)); /* 나머지 free 헤더 기록 */
        PUT(FTRP(bp), PACK(csize - asize, 0)); /* 나머지 free 푸터 기록 */
        insert_free_block(bp); /* 나머지 블록을 free list에 삽입 */
    } else { /* 나머지가 너무 작으면 전체 할당 */
        PUT(HDRP(bp), PACK(csize, 1)); /* 전체 블록 할당 헤더 기록 */
        PUT(FTRP(bp), PACK(csize, 1)); /* 전체 블록 할당 푸터 기록 */
    }
}


/*
 * mm_malloc
 */
void *mm_malloc(size_t size) /* size 바이트 malloc 요청 처리 */
{
    size_t asize; /* 정렬/오버헤드 반영한 실제 할당 크기 */
    size_t extendsize; /* fit 실패 시 확장할 크기 */
    char *bp; /* 할당 블록 포인터 */

    if (size == 0) /* 0바이트 요청이면 */
        return NULL; /* NULL 반환 */

    asize = ALIGN(size + DSIZE); /* payload + header/footer 크기를 정렬 */
    if (asize < MINBLOCKSIZE) /* 최소 블록보다 작으면 */
        asize = MINBLOCKSIZE; /* 최소 블록 크기로 보정 */

    if ((bp = find_fit(asize)) != NULL) { /* free list에서 적합 블록 찾으면 */
        place(bp, asize); /* 블록 배치 */
        return bp; /* payload 포인터 반환 */
    }

    extendsize = MAX(asize, CHUNKSIZE); /* 요청 크기와 기본 청크 중 큰 값 선택 */
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL) /* 힙 확장 실패 */
        return NULL; /* NULL 반환 */
    place(bp, asize); /* 확장으로 얻은 블록에 배치 */
    return bp; /* payload 포인터 반환 */
}

/*
 * mm_free
 */
void mm_free(void *ptr) /* ptr 블록 해제 */
{
    if (ptr == NULL) /* NULL 해제 요청은 무시 */
        return; /* 즉시 종료 */

    size_t size = GET_SIZE(HDRP(ptr)); /* 블록 크기 읽기 */

    PUT(HDRP(ptr), PACK(size, 0)); /* 헤더를 free 상태로 표시 */
    PUT(FTRP(ptr), PACK(size, 0)); /* 푸터를 free 상태로 표시 */
    coalesce(ptr); /* 인접 free 블록과 병합 */
}

/*
 * mm_realloc - 새 블록 할당 후 복사 (단순 패턴)
 */
void *mm_realloc(void *ptr, size_t size) /* ptr을 size로 재할당 */
{
    void *oldptr = ptr; /* 기존 블록 포인터 저장 */
    void *newptr; /* 새 블록 포인터 */
    size_t copySize; /* 복사할 바이트 수 */

    if (ptr == NULL) /* realloc(NULL, size)는 malloc(size)와 동일 */
        return mm_malloc(size); /* malloc 결과 반환 */
    if (size == 0) /* realloc(ptr, 0)은 free(ptr)와 동일 */
    {
        mm_free(ptr); /* 기존 블록 해제 */
        return NULL; /* NULL 반환 */
    }

    newptr = mm_malloc(size); /* 새 블록 할당 */
    if (newptr == NULL) /* 할당 실패 시 */
        return NULL; /* 실패 반환 */

    copySize = GET_SIZE(HDRP(oldptr)) - DSIZE; /* 기존 payload 크기 계산 */
    if (size < copySize) /* 새 요청이 더 작으면 */
        copySize = size; /* 요청 크기까지만 복사 */
    memcpy(newptr, oldptr, copySize); /* 기존 데이터 복사 */
    mm_free(oldptr); /* 기존 블록 해제 */
    return newptr; /* 새 블록 반환 */
}