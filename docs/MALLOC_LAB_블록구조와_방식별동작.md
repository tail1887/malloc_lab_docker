# Malloc Lab: 블록 구조와 방식별 동작 정리

이 문서는 `malloc-lab/mm.c`를 구현할 때 헷갈리는 **블록 구조**를 먼저 설명하고,  
그 다음에 **할당기 방식별(naive / implicit / explicit / segregated)** 핵심 코드와 동작을 비교합니다.

---

## 1) 먼저 현재 구조 확인 (이 저장소 기준)

현재 `mm.c`는 다음 특징을 가집니다.

- `mm_malloc`: `mem_sbrk()`로 힙을 그냥 늘리고, 앞에 `size_t` 1개를 기록
- `mm_free`: 아무 동작 없음
- `mm_realloc`: `malloc + memcpy + free`

즉, 지금은 **재사용 없는 naive allocator** 입니다.

---

## 2) 블록(Block) 구조란?

할당기에서 블록은 보통 아래 3개로 생각합니다.

1. **Header**: 블록 크기, 할당 여부(alloc bit) 같은 메타데이터
2. **Payload**: 사용자가 실제로 쓰는 메모리
3. **Footer(선택)**: Header 복사본(주로 coalescing 쉽게 하려고 사용)

왜 이렇게 나누냐면, `malloc`이 단순히 주소만 주는 게 아니라  
나중에 `free`/`realloc` 시점에 **블록의 경계와 상태를 역추적**해야 하기 때문입니다.

### 2-1. 시각화

`[Header | Payload | Footer]`

- 할당된 블록: Footer를 생략하는 구현도 많음
- free 블록: Footer를 둬서 이전 블록 병합을 쉽게 함

조금 더 실제적인 그림은 아래처럼 봅니다.

```text
free block
low addr                                              high addr
+----------+---------------------------+--------------+
| Header   | free payload(또는 링크)   | Footer       |
+----------+---------------------------+--------------+

allocated block (단순 구현)
low addr                                              high addr
+----------+---------------------------+--------------+
| Header   | user payload              | Footer(opt)  |
+----------+---------------------------+--------------+
```

> 포인트: `bp`는 보통 **payload 시작 주소**를 의미합니다.

### 2-1-1. Header에 실제로 들어가는 값

정렬이 8-byte이면 블록 크기는 항상 8의 배수입니다.  
즉, 크기의 하위 3비트가 0이므로 이 비트들을 상태 플래그로 재활용할 수 있습니다.

대표 매크로:

```c
#define PACK(size, alloc) ((size) | (alloc))
#define GET_SIZE(p)  (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)
```

- `PACK(size, 1)` : 할당된 블록 헤더/푸터 값 생성
- `GET_SIZE`      : 하위 상태 비트 제거 후 순수 블록 크기 추출
- `GET_ALLOC`     : 할당 비트만 추출

즉 Header는 흔히 `size | alloc` 형태의 정수 1개입니다.

### 2-1-2. Footer를 쓰는 이유(특히 coalesce)

다음 블록 상태는 현재 블록 Header에서 쉽게 접근 가능합니다.  
하지만 **이전 블록 상태**는 바로 알기 어렵습니다.

이때 Footer를 쓰면:

1. 이전 블록의 Footer에서 이전 블록 크기를 읽고
2. 그 값으로 이전 블록 Header 주소를 역산
3. 이전 블록이 free인지 즉시 판단

그래서 implicit 구현 초반에는 Footer를 두는 편이 병합 로직이 단순합니다.

### 2-2. 정렬(Alignment)

이 과제는 보통 8-byte 정렬을 요구합니다.

- 반환 포인터 `bp`(payload 시작)가 8의 배수여야 함
- 크기 계산 시 `ALIGN(size)` 같은 매크로로 맞춤

대표적으로 다음 3개를 동시에 만족해야 합니다.

1. `mm_malloc` 반환값 주소 `% 8 == 0`
2. 블록 전체 크기(`block size`)가 8의 배수
3. split 후 남는 free 블록도 최소/정렬 조건 유지

예시:

```c
#define ALIGN(size) (((size) + 7) & ~0x7)

// footer를 쓰는 단순 implicit 예시
if (size <= DSIZE)
    asize = 2 * DSIZE;          // 최소 블록 크기
else
    asize = ALIGN(size + DSIZE); // header+footer 포함
```

### 2-2-1. 포인터 관점에서 자주 헷갈리는 점

`bp`를 payload 시작으로 두면 보통 아래처럼 역참조합니다.

```c
#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)
```

- `bp` 자체는 사용자에게 반환한 포인터
- Header/Footer는 매크로로 계산해서 접근

이 기준이 머릿속에 잡히면 `free`, `coalesce`, `realloc` 디버깅이 훨씬 쉬워집니다.

---

## 3) 방식별 핵심 차이 한눈에

| 방식 | free 블록 탐색 | 장점 | 단점 |
|------|----------------|------|------|
| naive | 탐색 자체 없음 (`mem_sbrk`만) | 코드 가장 단순 | 재사용 불가, util 나쁨 |
| implicit list | 힙 전체 선형 순회 | 구현 쉬움 | 큰 트레이스에서 느림 |
| explicit list | free 블록만 연결 리스트 순회 | 속도 개선 | 리스트 관리 복잡 |
| segregated list | 크기 구간별 free 리스트 | 더 빠른 탐색 가능 | 구현/디버깅 난이도 높음 |

---

## 4) Naive 방식 (현재 코드와 유사)

### 4-1. 블록 구조

`[size_t requested_size | payload]`

- alloc bit, footer, free list 없음
- `free`를 해도 다시 쓰지 않음

### 4-2. 핵심 코드 패턴

```c
int newsize = ALIGN(size + SIZE_T_SIZE);
void *p = mem_sbrk(newsize);
if (p == (void *)-1) return NULL;
*(size_t *)p = size;
return (char *)p + SIZE_T_SIZE;
```

### 4-3. 동작 요약

- 요청이 오면 항상 힙 끝에 새 블록 추가
- 과거 free 블록을 절대 재사용하지 않음

---

## 5) Implicit Free List (전체 순회)

네가 지금 하려는 방식이 이 섹션입니다.

### 5-1. 블록 구조(대표 예시)

`[Header(size|alloc) | Payload ... | Footer(size|alloc)]`

Header/Footer는 보통 4바이트(WSIZE), 정렬 단위는 8바이트(DSIZE)로 둡니다.

### 5-2. 자주 쓰는 매크로

```c
#define WSIZE 4
#define DSIZE 8
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

### 5-3. `mm_init` 핵심 동작

```c
// prologue + epilogue 생성 후 초기 free block 확장
heap_listp = mem_sbrk(4 * WSIZE);
PUT(heap_listp, 0);
PUT(heap_listp + WSIZE, PACK(DSIZE, 1));      // prologue header
PUT(heap_listp + 2*WSIZE, PACK(DSIZE, 1));    // prologue footer
PUT(heap_listp + 3*WSIZE, PACK(0, 1));        // epilogue header
heap_listp += 2 * WSIZE;
extend_heap(CHUNKSIZE / WSIZE);
```

### 5-4. `mm_malloc` 동작

1. 요청 크기 정렬 + 최소 블록 크기로 보정
2. `find_fit`으로 힙을 처음부터 끝까지 순회
3. 맞는 free 블록 찾으면 `place`로 배치(필요 시 split)
4. 없으면 `extend_heap` 후 배치

### 5-5. `mm_free` + `coalesce`

```c
PUT(HDRP(bp), PACK(size, 0));
PUT(FTRP(bp), PACK(size, 0));
bp = coalesce(bp);
```

- 앞/뒤 이웃 블록의 alloc bit를 보고 4가지 케이스 병합

### 5-6. 장단점

- 장점: 디버깅하기 쉽고 학습용으로 좋음
- 단점: `find_fit`이 힙 전체 순회라 큰 트레이스에서 느림

---

## 6) Explicit Free List

Implicit에서 한 단계 발전한 방식입니다.

### 6-1. 블록 구조(대표 예시)

할당 블록:
`[Header | Payload | (optional Footer)]`

free 블록:
`[Header | prev ptr | next ptr | ... | Footer]`

즉, free 블록 payload 일부를 링크드리스트 포인터로 사용합니다.

### 6-2. 핵심 변화

- `find_fit`: 힙 전체가 아니라 **free 리스트만 순회**
- `mm_free`: free 리스트에 삽입
- `place`: free 리스트에서 제거 후 할당
- `coalesce`: 병합된 블록 기준으로 리스트 재삽입

### 6-3. 핵심 코드 스케치

```c
// free 블록에서 링크 포인터 접근
#define PRED(bp) (*(void **)(bp))
#define SUCC(bp) (*(void **)((char *)(bp) + sizeof(void *)))

static void insert_free(void *bp) {
    // LIFO 예시
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

### 6-4. 주의점

- 병합 전/후 remove/insert 순서가 꼬이면 버그가 자주 납니다.
- 최소 블록 크기를 포인터 2개 + header/footer를 담을 수 있게 크게 잡아야 합니다.

---

## 7) Segregated Free Lists

explicit list를 여러 개 두는 방식입니다.

### 7-1. 개념

- 예: `[16], [32], [64], [128], ...` 처럼 크기 구간별 리스트 운영
- 요청 크기에 맞는 클래스부터 탐색 시작

### 7-2. 핵심 코드 스케치

```c
static void *seg_heads[LISTS];

static int class_index(size_t size) {
    int idx = 0;
    while (idx < LISTS - 1 && size > 1) {
        size >>= 1;
        idx++;
    }
    return idx;
}
```

### 7-3. 장단점

- 장점: 탐색 길이 감소, throughput 개선 여지 큼
- 단점: 구현량/버그 포인트 증가

---

## 8) `mm_realloc` 구현 전략 비교

### 8-1. 기본(안전) 구현

```c
newptr = mm_malloc(size);
memcpy(newptr, oldptr, min(old_payload, size));
mm_free(oldptr);
```

장점: 단순, 정확성 확보 쉬움  
단점: 복사 비용 큼

### 8-2. 최적화 포인트

- 축소 시 split
- 다음 블록이 free면 합쳐서 in-place 확장
- 마지막 블록이면 `extend_heap`으로 in-place 확장 시도

---

## 9) 지금 네 상황에서 추천 순서

1. **Implicit list 완성** (`init/malloc/free/realloc + coalesce + split`)
2. `short1/short2` 통과
3. 전체 trace 통과
4. 점수 부족하면 explicit로 확장

---

## 10) 구현 점검 체크리스트

- [ ] 반환 포인터 8-byte 정렬
- [ ] Header/Footer size·alloc 일관성
- [ ] `free(NULL)` 안전 처리
- [ ] `mm_init` 재호출 시 초기화 정상
- [ ] `coalesce` 4케이스 정확
- [ ] split 후 잔여 블록 최소 크기 보장
- [ ] `realloc(ptr, 0)` 정책 일관

---

## 11) 용어 빠른 정리

- **Internal Fragmentation**: 블록 내부 남는 공간 낭비
- **External Fragmentation**: 총 free는 충분하지만 연속 공간이 부족
- **Coalescing**: 인접 free 블록 합치기
- **Splitting**: 큰 free 블록을 잘라 일부 할당

---

필요하면 다음 단계로 이 문서에 맞춰 `mm.c`를
1) implicit 완성본, 2) explicit 전환본
두 버전으로 나눠서 추가해 줄 수 있습니다.

