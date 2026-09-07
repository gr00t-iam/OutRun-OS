/* Host tests for the disk & volume manager's core.
 *
 * Everything here is the part of the application that decides WHAT to show:
 * pool arithmetic, byte formatting, the directory grouping that turns a flat
 * VFS dirent table into browsable prefixes, and the health verdict. None of it
 * touches a syscall, which is why it can be exercised in a second under ASan
 * instead of inside a 300 s emulated boot.
 *
 * The two things this file is most careful about are both mistakes this tree
 * has made before: a percentage that silently divides by zero on a volume that
 * was never mounted, and a listing that quietly stops early when the table is
 * bigger than the buffer. Both are asserted to be REPORTED, not absorbed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define APP_HOST_TEST
#include "outrun_disks.c"

static struct disk_view v;

static void seed_info(void) {
    memset(&v.info, 0, sizeof v.info);
    v.info.version = OUTRUN_HW_ABI_VERSION;
    v.info.size = (unsigned)sizeof v.info;
    v.info.cas_mounted = 1;
    v.info.cas_version = 5;
    v.info.cas_block_size = 512;
    v.info.cas_total_blocks = 20000;
    v.info.cas_used_blocks = 5000;
    v.info.cas_put_count = 400;
    v.info.cas_dedup_hits = 100;
    v.info.vfs_files_max = 96;
    v.info.ndev = 1;
    v.info.dev[0].ready = 1;
    v.info.dev[0].capacity_sectors = 40960;
    v.info.dev[0].sector_size = 512;
    v.info.dev[0].queue_size = 128;
    v.info.dev[0].irq_line = 11;
    v.info.dev[0].irqs = 77;
    strcpy(v.info.dev[0].name, "virtio-blk0");
}

int main(void) {
    char buf[24];

    /* ---- percentages saturate and never divide by zero ------------------ */
    assert(disk_pct(0, 0) == 0);
    assert(disk_pct(5, 0) == 0);
    assert(disk_pct(1, 4) == 25);
    assert(disk_pct(4, 4) == 100);
    assert(disk_pct(9, 4) == 100);             /* saturates, does not wrap    */
    assert(disk_pct(1ull << 62, 1ull << 63) == 50);   /* no overflow at scale */

    /* ---- byte formatting ------------------------------------------------ */
    disk_bytes(buf, 0);                  assert(!strcmp(buf, "0 B"));
    disk_bytes(buf, 512);                assert(!strcmp(buf, "512 B"));
    disk_bytes(buf, 1024);               assert(!strcmp(buf, "1.0 KIB"));
    disk_bytes(buf, 1536);               assert(!strcmp(buf, "1.5 KIB"));
    disk_bytes(buf, 10ull << 20);        assert(!strcmp(buf, "10.0 MIB"));
    disk_bytes(buf, 3ull << 30);         assert(!strcmp(buf, "3.0 GIB"));
    disk_bytes(buf, 5ull << 40);         assert(!strcmp(buf, "5.0 TIB"));

    /* ---- the pool derives from the superblock, not from guesses --------- */
    seed_info();
    assert(disk_pool_pct(&v.info) == 25);
    assert(disk_pool_free(&v.info) == 15000);
    assert(disk_pool_bytes(&v.info) == 20000ull * 512);
    assert(disk_dedup_pct(&v.info) == 25);     /* 100 hits of 400 puts        */
    assert(disk_capacity_bytes(&v.info.dev[0]) == 40960ull * 512);

    /* An unmounted volume reports nothing rather than dividing by zero. */
    v.info.cas_mounted = 0;
    v.info.cas_total_blocks = 0;
    v.info.cas_put_count = 0;
    assert(disk_pool_pct(&v.info) == 0 && disk_pool_free(&v.info) == 0);
    assert(disk_dedup_pct(&v.info) == 0);
    assert(!strcmp(disk_health(&v.info), "CAS NOT MOUNTED"));

    /* ---- health verdicts name the counter that produced them ------------ */
    seed_info();
    assert(!strcmp(disk_health(&v.info), "HEALTHY"));
    v.info.cas_recover_replays = 1;
    assert(!strcmp(disk_health(&v.info), "JOURNAL REPLAYED ON MOUNT"));
    v.info.cas_ref_underflow = 1;              /* outranks the replay notice  */
    assert(!strcmp(disk_health(&v.info), "REFCOUNT UNDERFLOW: DATA AT RISK"));
    seed_info();
    v.info.cas_used_blocks = 19500;            /* 97% */
    assert(!strcmp(disk_health(&v.info), "POOL ABOVE 90% USED"));

    /* ---- the directory listing ------------------------------------------ */
    seed_info();
    disk_reset(&v);
    assert(v.nent == 0 && !v.truncated);
    assert(disk_add(&v, "readme", 100) == 0);
    assert(disk_add(&v, "/usr/include/stdio.h", 200) == 0);
    assert(disk_add(&v, "/usr/include/string.h", 300) == 0);
    assert(disk_add(&v, "/usr/lib/libc.a", 400) == 0);
    assert(disk_add(&v, "/bin/occ", 500) == 0);
    assert(v.nent == 5 && v.ent_bytes == 1500);
    /* An empty name is not an entry: SYS_READDIR reports filtered-out slots
     * with used == 0, and counting those would inflate every total. */
    assert(disk_add(&v, "", 10) < 0 && v.nent == 5);

    disk_group(&v);
    /* Root-relative names group under "/"; the rest under their first path
     * component, which is what the flat dirent table actually encodes. */
    assert(v.ngrp == 4);
    assert(!strcmp(v.grp[0].prefix, "/"));
    assert(v.grp[0].files == 1 && v.grp[0].bytes == 100);
    assert(!strcmp(v.grp[1].prefix, "/usr/"));
    assert(v.grp[1].files == 3 && v.grp[1].bytes == 900);
    assert(!strcmp(v.grp[2].prefix, "/bin/"));
    assert(v.grp[2].files == 1 && v.grp[2].bytes == 500);
    assert(!strcmp(v.grp[3].prefix, "TOTAL"));
    assert(v.grp[3].files == 5 && v.grp[3].bytes == 1500);

    /* ---- overflow is REPORTED, not silently dropped --------------------- */
    disk_reset(&v);
    for (int i = 0; i < DISK_MAXENT; i++) {
        char name[16];
        sprintf(name, "/f%d", i);
        assert(disk_add(&v, name, 1) == 0);
    }
    assert(v.nent == DISK_MAXENT && !v.truncated);
    assert(disk_add(&v, "/one-too-many", 1) < 0);
    assert(v.truncated == 1 && v.nent == DISK_MAXENT);
    assert(!strcmp(disk_listing_note(&v), "LISTING TRUNCATED: MORE FILES THAN THIS VIEW HOLDS"));
    disk_reset(&v);
    assert(!strcmp(disk_listing_note(&v), "0 FILES"));

    /* A name longer than the entry buffer is truncated with a terminator and
     * still counted — losing the file from the listing entirely would be a
     * worse lie than showing a shortened name. */
    disk_reset(&v);
    char longname[200];
    memset(longname, 'a', sizeof longname - 1);
    longname[0] = '/';
    longname[sizeof longname - 1] = 0;
    assert(disk_add(&v, longname, 7) == 0);
    assert(strlen(v.ent[0].name) == DISK_NAMELEN - 1 && v.ent_bytes == 7);

    /* ---- scrolling and tab selection stay inside the model -------------- */
    disk_reset(&v);
    for (int i = 0; i < 40; i++) { char n[16]; sprintf(n, "/f%d", i); disk_add(&v, n, 1); }
    v.rows = 10;
    assert(v.top == 0);
    disk_scroll(&v, 1);  assert(v.top == 10);
    disk_scroll(&v, 1);  assert(v.top == 20);
    disk_scroll(&v, 1);  assert(v.top == 30);
    disk_scroll(&v, 1);  assert(v.top == 30);      /* clamped at the last page */
    disk_scroll(&v, -1); assert(v.top == 20);
    disk_scroll(&v, -1); disk_scroll(&v, -1); disk_scroll(&v, -1);
    assert(v.top == 0);                            /* clamped at the first     */

    v.tab = DISK_TAB_DEVICES;
    disk_set_tab(&v, DISK_TAB_FILES);
    assert(v.tab == DISK_TAB_FILES && v.top == 0); /* a tab change rewinds     */

    printf("outrun_disks: pool arithmetic, formatting, grouping, truncation, "
           "health and scrolling PASS; state=%zu bytes\n", sizeof v);
    return 0;
}
