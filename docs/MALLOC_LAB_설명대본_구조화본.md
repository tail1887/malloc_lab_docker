# Malloc Lab 설명대본 (구조화본)

이 문서는 발표용으로 바로 읽을 수 있게, 각 파트를 아래 틀로 통일했습니다.

- **핵심 동작 흐름**
- **주요 함수**
- **코드**
- **한 줄 요약**

---

## 목차

1. 용어 먼저: alloc / free / realloc
2. 블록(Block) 구조와 8바이트 정렬
3. naive allocator
4. implicit free list
5. coalesce 4가지 케이스
6. split 조건과 최소 블록 크기
7. explicit free list
8. segregated free list
9. realloc 안전 패턴
10. 디버깅 체크포인트
11. 어떤 순서로 구현할지

---

## 1) 용어 먼저: alloc / free / realloc

### 핵심 동작 흐름

1. `alloc(malloc)`으로 메모리를 빌린다.  
2. 필요하면 `realloc`으로 크기를 바꾼다.  
3. 다 쓴 뒤 `free`로 반납한다.

### 주요 함수

- `malloc(size)`
- `free(ptr)`
- `realloc(ptr, new_size)`

### 코드

```c
#include <stdlib.h>

int *data = (int *)malloc(3 * sizeof(int));   // alloc
if (data == NULL) return;

data[0] = 10;
data[1] = 20;
data[2] = 30;

int *tmp = (int *)realloc(data, 6 * sizeof(int)); // 크기 변경
if (tmp != NULL) {
    data = tmp;
    data[3] = 40;
}

free(data);   // 반납
data = NULL;
```

### 한 줄 요약

메모리 사용의 기본 사이클은 **할당 -> 크기 조정 -> 해제**입니다.

---

## 2) 블록(Block) 구조와 8바이트 정렬

### 핵심 동작 흐름

1. 블록은 `Header | Payload | Footer(선택)`로 본다.  
2. Header에는 `size + alloc bit`를 저장한다.  
3. `mm_malloc`이 반환하는 payload 포인터는 8바이트 정렬이어야 한다.

### 주요 함수

- (매크로 중심) `ALIGN`, `PACK`, `GET_SIZE`, `GET_ALLOC`
- `HDRP`, `FTRP`, `NEXT_BLKP`, `PREV_BLKP`

### 코드

```c
#define WSIZE 4
#define DSIZE 8
#define ALIGN(size) (((size) + 7) & ~0x7)

#define PACK(size, alloc) ((size) | (alloc))
#define GET(p) (*(unsigned int *)(p))
#define PUT(p, val) (*(unsigned int *)(p) = (val))

#define GET_SIZE(p)  (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)))
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))
```

### 한 줄 요약

이 파트를 이해하면 이후 모든 방식의 코드가 읽히기 시작합니다.

---

## 3) naive allocator

### 핵심 동작 흐름

1. `mm_malloc` 요청이 오면 `mem_sbrk`로 힙을 그냥 확장한다.  
2. free 공간 검색/재사용을 하지 않는다.  
3. `mm_free`는 사실상 no-op이다.

### 주요 함수

- `mm_malloc`
- `mm_free`
- (보조) `mem_sbrk`

### 코드

```c
void *mm_malloc(size_t size) {
    size_t asize = ALIGN(size + SIZE_T_SIZE);
    void *p = mem_sbrk((int)asize);
    if (p == (void *)-1) return NULL;
    *(size_t *)p = size;
    return (char *)p + SIZE_T_SIZE;
}

void mm_free(void *ptr) {
    // naive: no-op
}
```

### 한 줄 요약

**가장 단순하지만 재사용이 없어 비효율적**인 출발점입니다.

---

## 4) implicit free list (힙 전체 순회)

### 핵심 동작 흐름

1. 블록에 Header/Footer를 둔다.  
2. `mm_malloc`에서 힙 전체를 선형 순회(`find_fit`)한다.  
3. 찾으면 `place`로 배치하고 필요하면 split한다.  
4. `mm_free`에서는 free 표시 후 `coalesce`로 병합한다.

### 주요 함수

- `mm_init`
- `find_fit`
- `place`
- `coalesce`
- `mm_malloc`, `mm_free`

### 코드

```c
static void *find_fit(size_t asize) {
    char *bp;
    for (bp = heap_listp; GET_SIZE(HDRP(bp)) > 0; bp = NEXT_BLKP(bp)) {
        if (!GET_ALLOC(HDRP(bp)) && asize <= GET_SIZE(HDRP(bp)))
            return bp; // first-fit
    }
    return NULL;
}

static void place(void *bp, size_t asize) {
    size_t csize = GET_SIZE(HDRP(bp));
    if ((csize - asize) >= 2 * DSIZE) { // split
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
    } else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}

void mm_free(void *bp) {
    size_t size = GET_SIZE(HDRP(bp));
    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    coalesce(bp);
}
```

### 한 줄 요약

**구현 난이도와 이해도가 좋은 기본형**이며, 학습 첫 타깃으로 적합합니다.

---

## 5) coalesce 4가지 케이스

### 핵심 동작 흐름

1. `free`된 현재 블록 기준으로 이전/다음 블록의 할당 상태를 본다.  
2. `(prev alloc/free) x (next alloc/free)` 조합으로 4가지 경우를 처리한다.

### 주요 함수

- `coalesce`
- `GET_ALLOC`, `GET_SIZE`, `HDRP`, `FTRP`, `NEXT_BLKP`, `PREV_BLKP`

### 코드

```c
static void *coalesce(void *bp) {
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    if (prev_alloc && next_alloc) {                  // case 1
        return bp;
    } else if (prev_alloc && !next_alloc) {          // case 2
        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    } else if (!prev_alloc && next_alloc) {          // case 3
        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    } else {                                         // case 4
        size += GET_SIZE(HDRP(PREV_BLKP(bp))) + GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }
    return bp;
}
```

### 한 줄 요약

coalesce는 단편화 완화의 핵심이며, 4케이스를 정확히 처리해야 합니다.

---

## 6) split 조건과 최소 블록 크기

### 핵심 동작 흐름

1. 큰 free 블록에서 요청 크기만 할당한다.  
2. 남는 공간이 최소 블록 크기 이상이면 split한다.  
3. 너무 작게 남으면 split하지 않고 통째로 할당한다.

### 주요 함수

- `place`
- `GET_SIZE`, `PUT`, `HDRP`, `FTRP`

### 코드

```c
static void place(void *bp, size_t asize) {
    size_t csize = GET_SIZE(HDRP(bp));
    if ((csize - asize) >= 2 * DSIZE) { // 최소 블록 크기 조건
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        bp = NEXT_BLKP(bp);
        PUT(HDRP(bp), PACK(csize - asize, 0));
        PUT(FTRP(bp), PACK(csize - asize, 0));
    } else {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
}
```

### 한 줄 요약

split 조건이 느슨하면 블록이 깨지고, 너무 보수적이면 이용률이 떨어집니다.

---

## 7) explicit free list (free 블록만 순회)

### 핵심 동작 흐름

1. free 블록 payload에 `prev/next` 포인터를 저장한다.  
2. `mm_free` 시 free 리스트에 삽입한다.  
3. `mm_malloc`은 free 리스트만 탐색한다.  
4. 할당되면 리스트에서 제거하고, 병합 후 재삽입한다.

### 주요 함수

- `insert_free`
- `remove_free`
- `find_fit`(free list 버전)
- `coalesce`

### 코드

```c
#define PRED(bp) (*(void **)(bp))
#define SUCC(bp) (*(void **)((char *)(bp) + sizeof(void *)))

static void insert_free(void *bp) {       // LIFO 삽입 예시
    SUCC(bp) = free_list_head;
    PRED(bp) = NULL;
    if (free_list_head) PRED(free_list_head) = bp;
    free_list_head = bp;
}

static void remove_free(void *bp) {
    if (PRED(bp)) SUCC(PRED(bp)) = SUCC(bp);
    else free_list_head = SUCC(bp);
    if (SUCC(bp)) PRED(SUCC(bp)) = PRED(bp);
}
```

### 한 줄 요약

탐색은 빨라지지만, **리스트 일관성 관리가 핵심 난관**입니다.

---

## 8) segregated free list (크기별 리스트)

### 핵심 동작 흐름

1. free 리스트를 크기 구간별로 여러 개 둔다.  
2. 요청 크기에 맞는 클래스부터 탐색한다.  
3. 해당 클래스에 없으면 더 큰 클래스로 넘어간다.

### 주요 함수

- `class_index`
- `insert_free`
- `remove_free`
- `find_fit_from_class`

### 코드

```c
#define LISTS 16
static void *seg_heads[LISTS];

static int class_index(size_t size) {
    int idx = 0;
    while (idx < LISTS - 1 && size > 1) {
        size >>= 1;
        idx++;
    }
    return idx;
}

static void *find_fit_from_class(size_t asize) {
    int i = class_index(asize);
    for (; i < LISTS; i++) {
        // seg_heads[i] 순회해서 fit 찾기
    }
    return NULL;
}
```

### 한 줄 요약

처리량 개선에 유리하지만, **구현량과 튜닝 포인트가 가장 많습니다**.

---

## 9) realloc 안전 패턴

### 핵심 동작 흐름

1. 새 크기로 `realloc` 요청 시 바로 원본 포인터를 덮어쓰지 않는다.  
2. 임시 포인터(`tmp`)에 결과를 받는다.  
3. 성공 시에만 원본 포인터를 갱신한다.

### 주요 함수

- `realloc`
- `malloc`, `free` (실패 시 대응)

### 코드

```c
int *arr = (int *)malloc(4 * sizeof(int));
if (arr == NULL) return;

int *tmp = (int *)realloc(arr, 8 * sizeof(int));
if (tmp == NULL) {
    // 실패 시 기존 arr는 아직 유효
    free(arr);
    return;
}
arr = tmp;
```

### 한 줄 요약

`arr = realloc(arr, ...)`를 바로 쓰면 실패 시 원본 포인터를 잃을 수 있습니다.

---

## 10) 디버깅 체크포인트

### 핵심 동작 흐름

1. 정렬/헤더값/블록 연결 상태를 매 단계 점검한다.  
2. short trace로 재현한 뒤 full trace로 확대한다.

### 주요 함수

- `mm_malloc`, `mm_free`, `coalesce`, `place`
- (보조) `mdriver -V -f short1-bal.rep`

### 코드

```c
// 디버깅 체크 항목 예시
// [ ] 반환 포인터 8-byte 정렬
// [ ] header/footer size·alloc 일관성
// [ ] coalesce 4케이스 정상
// [ ] split 후 잔여 블록 최소 크기 만족
// [ ] free(NULL) 안전 처리
```

### 한 줄 요약

할당기는 “큰 버그 하나”보다 “작은 불일치 누적”으로 무너지는 경우가 많습니다.

---

## 11) 구현 순서 추천 (발표 마무리)

### 핵심 동작 흐름

1. naive 동작 이해  
2. implicit 완성 (`init/malloc/free/realloc + coalesce + split`)  
3. 전체 trace 통과  
4. 점수 필요 시 explicit/segregated 확장

### 주요 함수

- 1단계: `mm_init`, `mm_malloc`
- 2단계: `find_fit`, `place`, `mm_free`, `coalesce`
- 3단계: `mm_realloc`

### 코드 (체크리스트 느낌)

```c
// 구현 체크 순서
// [ ] mm_init
// [ ] mm_malloc + find_fit + place
// [ ] mm_free + coalesce
// [ ] mm_realloc
// [ ] short trace -> full trace
```

### 한 줄 요약

**정확성 먼저, 성능은 나중** 순서가 가장 안전합니다.

