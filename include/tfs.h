/* tiny-filesystem: a tiny power-loss-safe flash file system. See DESIGN.md. */
#ifndef TFS_H
#define TFS_H

#include <stdint.h>

#define TFS_ERR_IO          (-1)   /* port read/prog/erase failed */
#define TFS_ERR_CORRUPT     (-2)   /* no valid file system, or geometry mismatch */
#define TFS_ERR_NOENT       (-3)   /* no such file */
#define TFS_ERR_NOSPC       (-4)   /* not enough free space */
#define TFS_ERR_FBIG        (-5)   /* file would exceed tfs_max_file_size() */
#define TFS_ERR_INVAL       (-6)   /* bad argument or configuration */
#define TFS_ERR_NAMETOOLONG (-7)   /* name longer than TFS_NAME_MAX */
#define TFS_ERR_NOFILES     (-8)   /* all TFS_MAX_FILES ids in use */

#define TFS_NAME_MAX  15
#define TFS_MAX_FILES 64

struct tfs_config {
    void *ctx;
    int  (*read) (void *ctx, uint32_t addr, void *buf, uint32_t len);
    int  (*prog) (void *ctx, uint32_t addr, const void *buf, uint32_t len);
    int  (*erase)(void *ctx, uint32_t addr);      /* erases the sector at addr */
    void (*lock)  (void *ctx);                    /* optional */
    void (*unlock)(void *ctx);                    /* optional */
    uint32_t base;          /* address of the first sector */
    uint32_t sector_size;   /* erase unit, power of two */
    uint32_t sector_count;  /* >= 2 */
    uint32_t prog_size;     /* program unit: 1, 2, 4, 8 or 16 */
};

typedef struct tfs {
    const struct tfs_config *cfg;
    uint32_t head_off;      /* next free byte in the head sector */
    uint32_t next_seq;      /* next record sequence number */
    uint32_t sector_seq;    /* highest sector sequence number in use */
    uint32_t live;          /* footprint of live records, bytes */
    uint16_t oldest, head;  /* sector indices; the log runs oldest..head circularly */
    uint8_t retired;        /* head sector takes no more records */
    uint8_t gc_done;        /* head sector already holds its GC_DONE record */
} tfs_t;

typedef struct { uint8_t next_id; } tfs_dir_t;

struct tfs_info {
    char name[TFS_NAME_MAX + 1];
    uint32_t size;
};

int tfs_format(tfs_t *fs, const struct tfs_config *cfg);
int tfs_mount(tfs_t *fs, const struct tfs_config *cfg);
int tfs_size(tfs_t *fs, const char *name);
int tfs_read(tfs_t *fs, const char *name, uint32_t off, void *buf, uint32_t len);
int tfs_write(tfs_t *fs, const char *name, const void *buf, uint32_t len);
int tfs_append(tfs_t *fs, const char *name, const void *buf, uint32_t len);
int tfs_remove(tfs_t *fs, const char *name);
int tfs_dir_open(tfs_t *fs, tfs_dir_t *it);
int tfs_dir_next(tfs_t *fs, tfs_dir_t *it, struct tfs_info *info);
int tfs_gc(tfs_t *fs);
int tfs_max_file_size(const struct tfs_config *cfg);

#endif
