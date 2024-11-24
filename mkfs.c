#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#define BLOCK_SIZE 512
#define MAX_NAME 28
#define D_BLOCK 6
#define IND_BLOCK (D_BLOCK + 1)
#define N_BLOCKS (IND_BLOCK + 1)
#define ALIGNMENT 32

// Superblock structure
struct wfs_sb {
    size_t num_inodes;
    size_t num_data_blocks;
    off_t i_bitmap_ptr;
    off_t d_bitmap_ptr;
    off_t i_blocks_ptr;
    off_t d_blocks_ptr;
    uint32_t magic;
    size_t block_size;
    time_t creation_time;
};

// Inode structure
struct wfs_inode {
    int num;                       /* Inode number */
    mode_t mode;                   /* File type and mode */
    uid_t uid;                     /* User ID of owner */
    gid_t gid;                     /* Group ID of owner */
    off_t size;                    /* Total size, in bytes */
    int nlinks;                    /* Number of links */
    time_t atim, mtim, ctim;       /* Timestamps */
    off_t blocks[N_BLOCKS];        /* Data block pointers */
};

// Directory entry structure
struct wfs_dentry {
    char name[MAX_NAME];
    int num;
};

// Function prototypes
void usage();
int round_up(int num, int multiple);
void init_superblock(struct wfs_sb *sb, size_t num_inodes, size_t num_data_blocks);
void write_root_inode(int fd, struct wfs_sb *sb);

int main(int argc, char *argv[]) {
    int raid_mode = 0, inode_count = 0, block_count = 0;
    char *disk_file = NULL;
    int opt;

    // Parse command-line arguments
    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        switch (opt) {
            case 'r':
                raid_mode = atoi(optarg);
                break;
            case 'd':
                disk_file = optarg;
                break;
            case 'i':
                inode_count = atoi(optarg);
                break;
            case 'b':
                block_count = atoi(optarg);
                break;
            default:
                usage();
                return -1;
        }
    }

    if (!disk_file || inode_count <= 0 || block_count <= 0) {
        usage();
        return -1;
    }

    // Align block count
    block_count = round_up(block_count, ALIGNMENT);

    // Calculate metadata sizes
    size_t inode_bitmap_blocks = (inode_count + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    size_t data_bitmap_blocks = (block_count + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    size_t inode_table_blocks = (inode_count * sizeof(struct wfs_inode) + BLOCK_SIZE - 1) / BLOCK_SIZE;
    size_t total_blocks = 1 + inode_bitmap_blocks + data_bitmap_blocks + inode_table_blocks + block_count;

    // Open disk file
    int fd = open(disk_file, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("Failed to open disk file");
        return -1;
    }

    // Check disk size
    off_t disk_size = lseek(fd, 0, SEEK_END);
    if (disk_size < total_blocks * BLOCK_SIZE) {
        fprintf(stderr, "Disk file is too small. Required: %ld bytes.\n", total_blocks * BLOCK_SIZE);
        close(fd);
        return -1;
    }

    // Initialize and write superblock
    struct wfs_sb sb;
    init_superblock(&sb, inode_count, block_count);
    lseek(fd, 0, SEEK_SET);
    write(fd, &sb, sizeof(struct wfs_sb));

    // Zero out inode and data bitmaps, inode table, and data blocks
    char zero_block[BLOCK_SIZE] = {0};
    for (size_t i = 0; i < total_blocks; i++) {
        write(fd, zero_block, BLOCK_SIZE);
    }

    // Write root inode
    write_root_inode(fd, &sb);

    close(fd);
    printf("Filesystem initialized successfully.\n");
    return 0;
}

// Print usage instructions
void usage() {
    fprintf(stderr, "Usage: ./mkfs -r <raid_mode> -d <disk_file> -i <inodes> -b <blocks>\n");
}

// Round up a number to the nearest multiple
int round_up(int num, int multiple) {
    return ((num + multiple - 1) / multiple) * multiple;
}

// Initialize the superblock
void init_superblock(struct wfs_sb *sb, size_t num_inodes, size_t num_data_blocks) {
    size_t inode_bitmap_blocks = (num_inodes + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    size_t data_bitmap_blocks = (num_data_blocks + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    size_t inode_table_blocks = (num_inodes * sizeof(struct wfs_inode) + BLOCK_SIZE - 1) / BLOCK_SIZE;

    sb->num_inodes = num_inodes;
    sb->num_data_blocks = num_data_blocks;
    sb->i_bitmap_ptr = BLOCK_SIZE; // First block after superblock
    sb->d_bitmap_ptr = sb->i_bitmap_ptr + inode_bitmap_blocks * BLOCK_SIZE;
    sb->i_blocks_ptr = sb->d_bitmap_ptr + data_bitmap_blocks * BLOCK_SIZE;
    sb->d_blocks_ptr = sb->i_blocks_ptr + inode_table_blocks * BLOCK_SIZE;
    sb->magic = 0xABCD1234;
    sb->block_size = BLOCK_SIZE;
    sb->creation_time = time(NULL);
}

// Write the root inode
void write_root_inode(int fd, struct wfs_sb *sb) {
    struct wfs_inode root_inode = {
        .num = 0,
        .mode = S_IFDIR | 0755, // Directory with rwxr-xr-x permissions
        .uid = 0,
        .gid = 0,
        .size = 0,
        .nlinks = 2, // "." and ".."
        .atim = time(NULL),
        .mtim = time(NULL),
        .ctim = time(NULL),
        .blocks = {0}
    };

    // Write the root inode to the first inode block
    lseek(fd, sb->i_blocks_ptr, SEEK_SET);
    write(fd, &root_inode, sizeof(struct wfs_inode));
}
