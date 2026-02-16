/**
 * @file thumbgrid_ipc.h
 * @brief Shared IPC struct for ThumbGrid grid state between game-side and shell-side PRXes
 *
 * Communication via file-backed mmap at TG_IPC_PATH.
 * Game-side writes, shell-side reads. Lock-free via sequence counter:
 *   Writer: seq++ (odd=writing), write data, seq++ (even=ready)
 *   Reader: read seq, read data, read seq again; valid if both equal and even.
 */

#ifndef THUMBGRID_IPC_H
#define THUMBGRID_IPC_H

#include <stdint.h>

#define TG_IPC_PATH       "/data/thumbgrid_ipc.bin"
#define TG_IPC_FILE_SIZE  4096   /* page-aligned, larger than struct */

#define TG_IPC_MAX_OUTPUT  256
#define TG_IPC_TITLE_MAX    48
#define TG_IPC_PAGE_NAME_MAX 8

typedef struct ThumbGridSharedState {
    uint32_t sequence;              /* lock-free: odd=writing, even=ready */
    uint32_t ime_active;            /* 0=hidden, 1=visible */
    int32_t  selected_cell;         /* 0-8 */
    int32_t  current_page;          /* 0-2 */
    uint32_t accent_mode;           /* 0 or 1 */
    uint16_t output[TG_IPC_MAX_OUTPUT]; /* UTF-16 text buffer */
    uint32_t output_length;
    uint32_t text_cursor;
    uint32_t selected_all;          /* 0 or 1 */
    uint32_t sel_start;             /* selection start index (==sel_end means no selection) */
    uint32_t sel_end;               /* selection end index */
    uint16_t title[TG_IPC_TITLE_MAX];   /* title bar text (UTF-16) */
    char     page_name[TG_IPC_PAGE_NAME_MAX]; /* "abc", "ABC", "123" */
    char     cells[9][4];           /* character labels per cell [cell][button] */
    int32_t  offset_x;              /* widget position offset */
    int32_t  offset_y;
    uint32_t shift_active;          /* L2 shift held: 0 or 1 */
} ThumbGridSharedState;

/* --- Sequence counter helpers --- */

static inline void thumbgrid_ipc_write_begin(volatile ThumbGridSharedState *s) {
    s->sequence++;   /* odd = writing in progress */
    __asm__ volatile ("mfence" ::: "memory");
}

static inline void thumbgrid_ipc_write_end(volatile ThumbGridSharedState *s) {
    __asm__ volatile ("mfence" ::: "memory");
    s->sequence++;   /* even = write complete */
}

/**
 * Read a consistent snapshot of shared state.
 * Returns 1 if snapshot is valid, 0 if writer was in progress.
 */
static inline int thumbgrid_ipc_read(volatile ThumbGridSharedState *src,
                                ThumbGridSharedState *dst)
{
    uint32_t seq1 = src->sequence;
    __asm__ volatile ("lfence" ::: "memory");
    if (seq1 & 1) return 0;  /* writer in progress */

    /* Copy the data */
    const ThumbGridSharedState *s = (const ThumbGridSharedState *)src;
    *dst = *s;

    __asm__ volatile ("lfence" ::: "memory");
    uint32_t seq2 = src->sequence;
    return (seq1 == seq2) ? 1 : 0;
}

#endif /* THUMBGRID_IPC_H */
