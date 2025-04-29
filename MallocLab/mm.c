/*
 *   Student ID: 523031910521
 *   Student Name: 刘浩宇
 *   Description:
 *      Free List：使用 Free List 来管理 free block。每个空闲列表的大小在前几个列表中是线性的，在最后几个列表中是指数的。
 * 		堆结构：前几个块是空闲列表的偏移量，然后是堆的开始符号，然后是实际使用的块。
 * 		区块结构：
 *                     1.已分配的区块：
 *                          1.1.Header：4 字节，包括块的大小和分配的位。
 *                          1.2.Payload：实际数据。
 *                          1.3.Footer：4 字节，包括块的大小和分配的位。
 *                     2.空闲区块：
 *                          2.1.Prev： 4 字节，空闲列表中前一个空闲块的偏移量。
 *                          2.2.succ：4 字节，空闲列表中下一个空闲块的偏移量。
 * 		Realloc：如果 size 等于 0，则释放块。如果 ptr 等于 NULL，则 malloc 块。
 * 		如果尺寸大于原始尺寸：
 * 		情况 1：如果剩余大小大于 2 * DSIZE，则拆分块。
 * 		情况 2：如果剩余大小小于 2 * DSIZE，请将块放在左侧。
 * 		情况 3：如果剩余大小为负数且下一个块是空闲的，则扩展堆。
 * 		情况 4：如果剩余大小为负数，并且分配了下一个块，则 malloc 一个新块并复制数据。
 * 		如果 size 小于原始大小，则拆分块。
 *	特征：
 * 		当 malloc 时，找到最不合适的列表进行插入。
 * 		并将较大的块放在右侧。
 *     TODO： (on summer vocation)
 *              1. When choose a suitable free block, maybe if I choose top K blocks to find a better one to get better util.
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"

/* single word (4) or double word (8) alignment */
#define ALIGNMENT 8

/* rounds up to the nearest multiple of ALIGNMENT */
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~0x7)

/* word size and double words size */
#define WSIZE 4
#define DSIZE 8

/* heap extend size */
#define INITCHUNKSIZE (1 << 6)
#define CHUNKSIZE (1 << 12 )
#define REALLOCCHUNKSIZE (40)

/* max and min */
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

/* pack a size and allocated bit into a word */
#define PACK(size, alloc) ((size) | (alloc))

/* read and write a word */
#define GET(p) (*(unsigned int *)(p))
#define PUT(p, val) (*(unsigned int *)(p) = (val))
#define PUT_PTR(p, val) (*(void **)(p) = (val))

/* read the size and allocated fields */
#define GET_SIZE(p) (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

/* get offset and address */
#define GET_OFFSET(bp) ((char *)bp - (char *)heap_listp)
#define GET_FREELIST_OFFSET(bp) ((char *)bp - (char *)free_listps)
#define GET_ADDR(offset) (heap_listp + offset)
#define GET_FREELIST_ADDR(offset) (free_listps + offset)

/* compute address of header and footer */
#define HDRP(bp) ((char *)(bp) - WSIZE)
#define FTRP(bp) ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)
#define PRED(bp) (bp)
#define SUCC(bp) ((bp) + WSIZE)

/* compute address of previous and next block */
#define PREV_BLKP(bp) ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))
#define NEXT_BLKP(bp) ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))

/* compute address of previous and next free block */
#define PREV_FREEP(bp) ((GET(PRED(bp))) ? (GET_ADDR(GET(PRED(bp)))) : NULL)
#define NEXT_FREEP(bp) ((GET(SUCC(bp))) ? (GET_ADDR(GET(SUCC(bp)))) : NULL)

/* length of free list */
#define LISTLENGTH 16
#define LINEARLENGTH 5
#define LINEARMAX ((1 << (LINEARLENGTH - 3)) - 2)

/* if size > BIGSIZE, place at right side */
#define BIGSIZE (108)

/* global variables */
static void *heap_listp;
static void *free_listps;

/* helper functions */
static void *extend_heap(size_t size);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void *insert_freeblk(void *bp);
static void *remove_freeblk(void *bp);
static void *place(void *bp, size_t asize);
static int get_free_list_index(size_t size);

int mm_check(void);

/*
 *  mm_check - check the consistency of the heap
 */
int mm_check(void)
{
    /* Is every block in the free list marked as free? */
    /* Do the pointers in the free list point to valid free blocks? */
    for (int i = 0; i < LISTLENGTH; ++i) {
        unsigned int offset = GET(free_listps + i * WSIZE);
        if (!offset)
            continue;
        void *bp = GET_ADDR(offset);
        while (bp) {
            if (GET_ALLOC(HDRP(bp))) {
                fprintf(stderr, "Error: Block in free list is not free\n");
                return -1;
            }
            if (GET(PRED(bp)) && !GET_ADDR(GET(PRED(bp)))) {
                fprintf(stderr, "Error: Invalid predecessor pointer\n");
                return -1;
            }
            if (GET(SUCC(bp)) && !GET_ADDR(GET(SUCC(bp)))) {
                fprintf(stderr, "Error: Invalid successor pointer\n");
                return -1;
            }
            bp = NEXT_FREEP(bp);
        }
    }

    /* Are there any contiguous free blocks that somehow escaped coalescing? */
    void *bp = heap_listp;
    while (GET_SIZE(HDRP(bp)) > 0) {
        void *freebp = NULL;
        for(int i = 0; i < LISTLENGTH; ++i) {
            NORMAAL:
            freebp = GET_ADDR(GET(free_listps + i * WSIZE));
            while (freebp) {
                if (freebp == bp) {
                    goto NORMAAL;
                }
                freebp = NEXT_FREEP(freebp);
            }
            fprintf(stderr, "Error: Contiguous free blocks escaped coalescing\n");
        }
    }

    /* Is every free block actually in the free list? */
    bp = heap_listp;
    while (GET_SIZE(HDRP(bp)) > 0) {
        void *freebp = NULL;
        for (int i = 0; i < LISTLENGTH; ++i) {
            freebp = GET_ADDR(GET(free_listps + i * WSIZE));
            while (freebp) {
                if (freebp == bp)
                    break;
                freebp = NEXT_FREEP(freebp);
            }
            if (freebp)
                break;
        }
        if (!freebp) {
            fprintf(stderr, "Error: Free block not in free list\n");
            return -1;
        }
        bp = NEXT_BLKP(bp);
    }

    /* Do any allocated blocks overlap? */
    bp = heap_listp;
    while (GET_SIZE(HDRP(bp)) > 0) {
        if (!GET_ALLOC(HDRP(bp))) {
            bp = NEXT_BLKP(bp);
            continue;
        }
        void *next = NEXT_BLKP(bp);
        if (GET_SIZE(HDRP(next)) > 0 && !GET_ALLOC(HDRP(next))) {
            fprintf(stderr, "Error: Allocated blocks overlap\n");
            return -1;
        }
        bp = next;
    }

    /* Do the pointers in a heap block point to valid heap addresses? */
    bp = heap_listp;
    while (GET_SIZE(HDRP(bp)) > 0) {
        if (GET(PRED(bp)) && !GET_ADDR(GET(PRED(bp)))) {
            fprintf(stderr, "Error: Invalid predecessor pointer\n");
            return -1;
        }
        if (GET(SUCC(bp)) && !GET_ADDR(GET(SUCC(bp)))) {
            fprintf(stderr, "Error: Invalid successor pointer\n");
            return -1;
        }
        bp = NEXT_BLKP(bp);
    }

    return 0;
}

/*
 * mm_init - initialize the malloc package.
 */
int mm_init(void)
{
    /* create the initial empty heap */
    heap_listp = mem_sbrk(ALIGN(LISTLENGTH * WSIZE) + 4 * WSIZE);
    if (heap_listp == (void *)-1)
        return -1;

    /* initialize the free list */
    free_listps = heap_listp;
    memset(heap_listp, 0, LISTLENGTH * WSIZE);

    /* initialize the heap */
    heap_listp += ALIGN(LISTLENGTH * WSIZE) + 2 * WSIZE;
    PUT(HDRP(heap_listp), PACK(DSIZE, 1));
    PUT(FTRP(heap_listp), PACK(DSIZE, 1));
    PUT(HDRP(NEXT_BLKP(heap_listp)), PACK(0, 1));

    /* extend the empty heap with a free block */
    /* TODO: inline the extend_heap function */
    if (extend_heap(INITCHUNKSIZE) == NULL)
        return -1;

    return 0;
}

/*
 * mm_malloc - Allocate a block by incrementing the brk pointer.
 *     Always allocate a block whose size is a multiple of the alignment.
 */
void *mm_malloc(size_t size)
{
    if (size <= 0)
        return NULL;

    size_t asize;
    size_t extendsize;
    char *bp;
    /* find a block of asize */
    asize = ALIGN(size) + DSIZE;

    /* place asize in the block found */
    if ((bp = find_fit(asize)) != NULL) {
        bp = place(bp, asize);

        return bp;
    }

    /* if not found, extend */
    extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize)) == NULL)
        return NULL;

    /* place asize in the block found */
    bp = place(bp, asize);

    return bp;
}

/*
 * mm_free - Freeing a block does nothing.
 */
void mm_free(void *ptr)
{

    /* set the allocated bit to 0 */
    size_t size = GET_SIZE(HDRP(ptr));
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));

    /* coalesce and insert to free list */
    coalesce(ptr);
}

/*
 * mm_realloc - Better realloc than just malloc and free
 */
void *mm_realloc(void *ptr, size_t size)
{
    /* If size <= 0*/
    if (size <= 0) {
        mm_free(ptr);
        return NULL;
    }
    /* If ptr == NULL */
    if (ptr == NULL) {
        return mm_malloc(size);
    }
    /* If size > 0 and ptr != NULL */
    size_t oldSize = GET_SIZE(HDRP(ptr));
    size_t newSize = ALIGN(size + DSIZE);
    /* Case 0: The oldSize equals the newSize */
    if (oldSize == newSize) {
        return ptr;
    }
    /* Case 1: The oldSize is larger than the newSize */
    long remainSize = oldSize - newSize +  (GET_ALLOC(HDRP(NEXT_BLKP(ptr))) ? 0 : GET_SIZE(HDRP(NEXT_BLKP(ptr))));
    if (oldSize > newSize) {
        if (remainSize >= 2 * DSIZE) {
            PUT(HDRP(ptr), PACK(newSize, 1));
            PUT(FTRP(ptr), PACK(newSize, 1));
            PUT(HDRP(NEXT_BLKP(ptr)), PACK(remainSize, 0));
            PUT(FTRP(NEXT_BLKP(ptr)), PACK(remainSize, 0));
            coalesce(NEXT_BLKP(ptr));
            return ptr;
        } else {
            return ptr;
        }
    }
    /* Case 2: The oldSize is smaller than the newSize */
    /*      case 2.1: The remaining size plus oldSize is larger than the newSize or it can be extended */
    int isNextFree =
        GET_ALLOC(HDRP(NEXT_BLKP(ptr))) ? 0 : GET_SIZE(HDRP(NEXT_BLKP(ptr)));

    /* If the block is the last one, it can extend the heap to satisfy it */
    int isExtendable = (isNextFree && !GET_SIZE(HDRP(NEXT_BLKP(NEXT_BLKP(ptr))))) || !GET_SIZE(HDRP(NEXT_BLKP(ptr)));

    /* If extended is needed */
    if (remainSize < 0 && isExtendable) {
        extend_heap(MAX(-remainSize, REALLOCCHUNKSIZE));
        remainSize = GET_SIZE(HDRP(NEXT_BLKP(ptr))) + oldSize - newSize;
    }

    /* If remaining size enough */
    if (remainSize >= 0) {
        /* If the remaining size is too small */
        if (remainSize <  2* DSIZE) {
            remove_freeblk(NEXT_BLKP(ptr));
            PUT(HDRP(ptr), PACK(newSize + remainSize, 1));
            PUT(FTRP(ptr), PACK(newSize + remainSize, 1));
            return ptr;
        }

        /* else place at left side */
        remove_freeblk(NEXT_BLKP(ptr));
        PUT(HDRP(ptr), PACK(newSize, 1));
        PUT(FTRP(ptr), PACK(newSize, 1));
        PUT(HDRP(NEXT_BLKP(ptr)), PACK(remainSize, 0));
        PUT(FTRP(NEXT_BLKP(ptr)), PACK(remainSize, 0));
        insert_freeblk(NEXT_BLKP(ptr));
        return ptr;
    }

    /*        case 2.2 The remaining size plus oldSize is smaller than the newSize */
    void *newptr = mm_malloc(size);
    if (!newptr)
        return NULL;
    memcpy(newptr, ptr, oldSize - DSIZE);
    mm_free(ptr);

    return newptr;
}

/*
 * extend_heap - extend heap to contain larger size
 *          The implentation is based on the book, but since the size has been aligned, the size is always even.
 *          So the alignment is not necessary.
 */
static void *extend_heap(size_t size)
{
    /* allocate an even number of words to maintain alignment */
    /* Actually it is not indeed needed, but if any other program call this func, so i reserve it. */
    size = ALIGN(size);
    char *bp;
    if ((bp = mem_sbrk(size)) == (void *)-1)
        return NULL;

    /* Initialize free bock header/footer and the epilogue header */
    PUT(HDRP(bp), PACK(size, 0));         /* free block header */
    PUT(FTRP(bp), PACK(size, 0));         /* free block footer */
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1)); /* new epilogue header */

    /* Coalesce if the previous block was free */
    return coalesce(bp);
}

/*
 * coalesce - insert current block to free list and coalesce adjecent free blocks
 */
static void *coalesce(void *bp)
{
    void *prev = PREV_BLKP(bp);
    void *next = NEXT_BLKP(bp);
    size_t prev_alloc = GET_ALLOC(HDRP(prev));
    size_t next_alloc = GET_ALLOC(HDRP(next));
    size_t size = GET_SIZE(HDRP(bp));

    if (prev_alloc && next_alloc) { /* case 1: no coalesce */
        insert_freeblk(bp);
        return bp;
    }

    else if (prev_alloc && !next_alloc) { /* case 2: coalesce with next block */
        remove_freeblk(next);
        size += GET_SIZE(HDRP(next));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
        insert_freeblk(bp);
        return bp;
    }

    else if (!prev_alloc && next_alloc) { /* case 3: coalesce with previous block */
        remove_freeblk(prev);
        size += GET_SIZE(HDRP(prev));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(prev), PACK(size, 0));
        insert_freeblk(prev);
        bp = prev;
        return bp;
    }

    else { /* case 4: coalesce with both next block and previous block*/
        remove_freeblk(next);
        remove_freeblk(prev);
        size += GET_SIZE(HDRP(next)) + GET_SIZE(HDRP(prev));
        PUT(HDRP(prev), PACK(size, 0));
        PUT(FTRP(next), PACK(size, 0));
        insert_freeblk(prev);
        bp = prev;
    }

    return bp;
}

/*
 * find_fit - find a free block that can fit the given size
 */
void *find_fit(size_t asize)
{

    if (!asize)
        return NULL;

    void *bp;

    /* find a least suitable list to insert */
    int index = get_free_list_index(asize);

    /* search each free list after */
    for (int i = index; i < LISTLENGTH; ++i) {
        unsigned int offset = GET(free_listps + i * WSIZE); /* get the offset */
        if (!offset)
            continue;
        bp = GET_ADDR(offset); /* convert the offset to an address */
        while (bp) {
            if (GET_SIZE(HDRP(bp)) >= asize)
                return bp;

            bp = NEXT_FREEP(bp); /* convert the offset to an address */
        }
    }

    return NULL;
}

/*
 * place - place the given size to the given block
 */
void *place(void *bp, size_t asize)
{
    size_t csize = GET_SIZE(HDRP(bp));
    size_t remain = csize - asize;

    /* remove given free block */
    remove_freeblk(bp);

    /* if remain size < smallest size, no division */
    if (remain < 2 * DSIZE) {
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
        return bp;
    }

    /* if big size, place at right side */
    else if (asize > BIGSIZE) {
        PUT(HDRP(bp), PACK(remain, 0));
        PUT(FTRP(bp), PACK(remain, 0));
        void *next = NEXT_BLKP(bp);
        PUT(HDRP(next), PACK(asize, 1));
        PUT(FTRP(next), PACK(asize, 1));
        insert_freeblk(bp);
        return next;
    }

    /* if small size, place at left size */
    else {
        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));
        void *next = NEXT_BLKP(bp);
        PUT(HDRP(next), PACK(remain, 0));
        PUT(FTRP(next), PACK(remain, 0));
        insert_freeblk(next);
        return bp;
    }
}

/*
 * insert_freeblk - insert given block to suitable free block list
 */
static void *insert_freeblk(void *bp)
{
    size_t size = GET_SIZE(HDRP(bp));

    /* get the free list to insert */
    int index = get_free_list_index(size);

    /* whether the first block */
    if (GET(free_listps + index * WSIZE)) {
        PUT(PRED(GET_ADDR(GET(free_listps + index * WSIZE))), GET_OFFSET(bp));
        PUT(SUCC(bp), GET((char *)free_listps + index * WSIZE));
        PUT(PRED(bp), 0);
    } else {
        PUT(SUCC(bp), 0);
        PUT(PRED(bp), 0);
    }

    /* set the list pointer */
    PUT(free_listps + index * WSIZE, GET_OFFSET(bp));

    return bp;
}

/*
 * remove_freeblk - remove given block from suitable free block list
 */
static void *remove_freeblk(void *bp)
{
    size_t size = GET_SIZE(HDRP(bp));

    int index = get_free_list_index(size);

    void *prev = PREV_FREEP(bp);
    void *next = NEXT_FREEP(bp);

    /* 4 cases accroding to the existence of previous and next block */
    if (prev && next) { /* Case 1: Prev and next block exist */
        PUT(SUCC(prev), GET(SUCC(bp)));
        PUT(PRED(next), GET(PRED(bp)));
    } else if (prev && !next) { /* Case 2: Prev block exist while next donnot */
        PUT(SUCC(prev), 0);
    } else if (!prev && next) { /* Case 3: Next block exist while previous donnot */
        PUT(PRED(next), 0);
        PUT(free_listps + index * WSIZE, GET(SUCC(bp)));
    } else { /* Both are not exist */
        PUT(free_listps + index * WSIZE, 0);
    }

    return bp;
}

/*
 * get_free_list_index - get the index of free list to insert
 */
static int get_free_list_index(size_t size)
{
    /* linear part */
    size_t linear_index = size / DSIZE - 2;
    if (linear_index < LINEARMAX) {
        return linear_index;
    }

    /* power of 2 part */
    size_t search = (size - 1) >> LINEARLENGTH;
    for (int i = LINEARMAX; i < LISTLENGTH - 1; ++i) {
        if (!search)
            return i;

        search >>= 1;
    }

    /* largest ones */
    return LISTLENGTH - 1;
}