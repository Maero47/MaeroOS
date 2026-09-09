#include "initrd.h"
#include "vfs.h"
#include "../mm/heap.h"
#include "../lib/string.h"
#include "../kernel/printk.h"
#include "../kernel/panic.h"
#include <kernel/config.h>
#include <stdint.h>

/*
 * ustar tar header — 512 bytes per block.
 * Only the fields we actually need are named; the rest are in the pad.
 */
typedef struct {
    char name[100];      /* offset   0: file name */
    char mode[8];        /* offset 100 */
    char uid[8];         /* offset 108 */
    char gid[8];         /* offset 116 */
    char size[12];       /* offset 124: file size in octal ASCII */
    char mtime[12];      /* offset 136 */
    char checksum[8];    /* offset 148 */
    char typeflag;       /* offset 156: '0'/'\0'=file, '5'=dir */
    char linkname[100];  /* offset 157 */
    char magic[6];       /* offset 257: "ustar" */
    char pad[255];       /* remaining header bytes (not used) */
} __attribute__((packed)) ustar_hdr_t;

/* Parse a NUL-padded octal ASCII field (up to `flen` chars) */
static uint32_t octal_str(const char *s, int flen) {
    uint32_t v = 0;
    for (int i = 0; i < flen; i++) {
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (uint32_t)(s[i] - '0');
    }
    return v;
}

void initrd_init(uint32_t mod_phys_start, uint32_t mod_phys_end) {
    /*
     * boot_page_table1 maps physical 0x0-0x3FF000 at KERNEL_VMA.
     * All GRUB modules land in that range, so we can access them directly.
     */
    const uint8_t *tar  = (const uint8_t *)(mod_phys_start + KERNEL_VMA);
    uint32_t        size = mod_phys_end - mod_phys_start;

    /* Create the root directory node */
    /* FATAL by design (audit category (c)): initrd_init runs from kmain with a
     * nearly empty heap and is the only thing that creates the VFS root.
     * Returning left vfs_root NULL and let the boot carry on into vfs_mount(),
     * which faults on it a few lines later with nothing to say about why - a
     * silent half-broken kernel where a named panic belongs. */
    vfs_root = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!vfs_root)
        panic("initrd_init: no memory for the VFS root node", NULL);
    memset(vfs_root, 0, sizeof(vfs_node_t));
    vfs_root->flags = VFS_FLAG_DIR;
    vfs_root->mask  = 0755;       /* root dir: world-traversable */
    vfs_root->name[0] = '/';

    uint32_t offset    = 0;
    uint32_t nfiles    = 0;
    /* Unique inode per node: glibc's ld.so dedups shared libraries by
     * (st_dev, st_ino), so every file MUST have a distinct inode or it thinks
     * libc.so.6 is "already loaded" (as ld-linux.so.2) and never maps it. */
    uint32_t next_ino  = 2;

    while (offset + 512 <= size) {
        const ustar_hdr_t *hdr = (const ustar_hdr_t *)(tar + offset);

        /* End-of-archive: two consecutive zero blocks */
        if (hdr->name[0] == '\0')
            break;

        /* Sanity check: must be a ustar archive */
        if (memcmp(hdr->magic, "ustar", 5) != 0) {
            printk("[INITRD] Warning: non-ustar block at offset %u, stopping.\n",
                   (unsigned)offset);
            break;
        }

        uint32_t file_size = octal_str(hdr->size, 11);

        /* Process regular files ('0'/NUL) and directories ('5'). */
        if (hdr->typeflag == '0' || hdr->typeflag == '\0' ||
            hdr->typeflag == '5') {
            /* Strip leading "./" or "/" from the stored name */
            const char *name = hdr->name;
            if (name[0] == '.' && name[1] == '/') name += 2;
            else if (name[0] == '/')               name += 1;

            int is_dir = (hdr->typeflag == '5');

            if (name[0] != '\0') {
                /* Walk/create intermediate directory nodes so nested paths
                 * (e.g. usr/share/fonts/X.ttf) resolve via vfs_finddir.  GTK/
                 * fontconfig/Firefox depend on real directory structure. */
                vfs_node_t *dir = vfs_root;
                char comp[256];
                const char *s = name;
                while (*s) {
                    int len = 0;
                    while (*s && *s != '/' && len < 255) comp[len++] = *s++;
                    comp[len] = '\0';
                    int last = (*s == '\0');
                    while (*s == '/') s++;
                    if (last && *s == '\0') {
                        /* trailing slash on a dir entry → comp is the dir */
                    }
                    if (len == 0) continue;

                    int leaf = (*s == '\0');   /* this component ends the path */

                    /* Find an existing child of this name. */
                    vfs_node_t *child = NULL;
                    for (vfs_node_t *n = dir->children; n; n = n->next)
                        if (strcmp(n->name, comp) == 0) { child = n; break; }

                    if (leaf && !is_dir) {
                        if (child) break;       /* duplicate file — skip */
                        vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
                        /* FATAL by design (audit category (c)): `break` here
                         * silently truncated the initrd, and a boot missing an
                         * arbitrary subset of /init, /bin and the libraries
                         * fails later in ways that say nothing about the cause. */
                        if (!node)
                            panic("initrd_init: no memory for an initrd file node", NULL);
                        memset(node, 0, sizeof(vfs_node_t));
                        node->flags = VFS_FLAG_FILE;
                        node->size  = file_size;
                        node->data  = tar + offset + 512;
                        node->mask  = 0755;
                        node->uid = node->gid = 0;
                        node->inode = next_ino++;
                        strncpy(node->name, comp, 255);
                        node->next = dir->children;
                        dir->children = node;
                        nfiles++;
                        break;
                    }

                    /* Directory component (intermediate, or an explicit dir). */
                    if (!child) {
                        child = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
                        if (!child)     /* category (c), as above */
                            panic("initrd_init: no memory for an initrd directory node", NULL);
                        memset(child, 0, sizeof(vfs_node_t));
                        child->flags = VFS_FLAG_DIR;
                        child->mask  = 0755;
                        child->uid = child->gid = 0;
                        child->inode = next_ino++;
                        strncpy(child->name, comp, 255);
                        child->next = dir->children;
                        dir->children = child;
                    }
                    dir = child;
                }
            }
        }

        /* Advance past header + data (rounded up to 512-byte boundary) */
        uint32_t data_blocks = (file_size + 511) / 512;
        offset += (1 + data_blocks) * 512;
    }

    printk("[INITRD] Mounted initrd: %u file(s).\n", (unsigned)nfiles);
}
