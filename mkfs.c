#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include "wfs.h"

void print_usage(const char *progname) {
    fprintf(stderr, "Usage: %s -r <raid_mode> -d <disk1> -d <disk2> ... -i <num_inodes> -b <num_blocks>\n", progname);
    exit(EXIT_FAILURE);
}

void initialize_superblock(struct wfs_sb *sb, size_t num_inodes, size_t num_blocks, int raid_mode, size_t disk_size) {
    // Round up the number of inodes to the nearest multiple of 32 for alignment
    num_inodes = (num_inodes + 31) / 32 * 32;

    sb->num_inodes = num_inodes;
    sb->num_data_blocks = num_blocks;

    size_t inode_bitmap_size = ((num_inodes + 31) / 32) * sizeof(int); // Bitmap size in bytes
    size_t data_bitmap_size = ((num_blocks + 31) / 32) * sizeof(int); // Bitmap size in bytes

    sb->i_bitmap_ptr = sizeof(struct wfs_sb);
    sb->d_bitmap_ptr = sb->i_bitmap_ptr + inode_bitmap_size;
    sb->i_blocks_ptr = ((sb->d_bitmap_ptr + data_bitmap_size + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
    sb->d_blocks_ptr = ((sb->i_blocks_ptr + num_inodes * BLOCK_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;

    // Validate that the calculated layout fits within the disk size
    if (sb->d_blocks_ptr + (num_blocks * BLOCK_SIZE) > disk_size) {
        fprintf(stderr, "Error: Disk size too small for the specified filesystem layout.\n");
        exit(-1);
    }
}

void write_superblock(int fd, struct wfs_sb *sb) {
    if (pwrite(fd, sb, sizeof(struct wfs_sb), 0) != sizeof(struct wfs_sb)) {
        perror("Failed to write superblock");
        exit(EXIT_FAILURE);
    }
}

void initialize_bitmap(int fd, size_t offset, size_t num_bits, int allocate_first) {
    size_t bitmap_size = (num_bits + 31) / 32 * sizeof(int);
    int *bitmap = calloc(1, bitmap_size);
    if (!bitmap) {
        perror("Failed to allocate memory for bitmap");
        exit(EXIT_FAILURE);
    }

    // Mark the first bit if allocate_first is true (e.g., for the root inode)
    if (allocate_first) {
        bitmap[0] = 1; // Mark the first bit as allocated
    }

    if (pwrite(fd, bitmap, bitmap_size, offset) != (ssize_t)bitmap_size) {
        perror("Failed to write bitmap");
        free(bitmap);
        exit(EXIT_FAILURE);
    }

    free(bitmap);
}

void initialize_inodes(int fd, size_t inode_offset, size_t num_inodes) {
    struct wfs_inode empty_inode = {0};

    // Write all inodes to the inode table
    for (size_t i = 0; i < num_inodes; ++i) {
        if (pwrite(fd, &empty_inode, sizeof(empty_inode), inode_offset + i * BLOCK_SIZE) != sizeof(empty_inode)) {
            perror("Failed to initialize inodes");
            exit(EXIT_FAILURE);
        }
    }
}

int initialize_root_inode(int fd[], int num_disks, struct wfs_sb *superblock) {
    struct wfs_inode root_inode = {
        .num = 0,
        .mode = S_IFDIR | 0755,
        .uid = getuid(),
        .gid = getgid(),
        .size = 0,
        .nlinks = 2, // . and ..
        .atim = time(NULL),
        .mtim = time(NULL),
        .ctim = time(NULL),
    };

    // Write the root inode to all disks
    for (int i = 0; i < num_disks; ++i) {
        if (pwrite(fd[i], &root_inode, sizeof(root_inode), superblock->i_blocks_ptr) != sizeof(root_inode)) {
            perror("Failed to write root inode");
            return -1;
        }
    }

    // Mark the root inode as allocated in the inode bitmap on all disks
    int *bitmap = malloc(sizeof(int));
    if (!bitmap) {
        perror("Failed to allocate memory for inode bitmap");
        return -1;
    }

    bitmap[0] = 1; // Mark the first inode bit as used

    for (int i = 0; i < num_disks; ++i) {
        if (pwrite(fd[i], bitmap, sizeof(int), superblock->i_bitmap_ptr) != sizeof(int)) {
            perror("Failed to update inode bitmap for root inode");
            free(bitmap);
            return -1;
        }
    }

    free(bitmap);
    return 0;
}

int main(int argc, char *argv[]) {
    int raid_mode = -1;
    size_t num_inodes = 0, num_blocks = 0;
    char *disk_files[32];
    int num_disks = 0;
    int fds[32];

    int opt;
    while ((opt = getopt(argc, argv, "r:d:i:b:")) != -1) {
        switch (opt) {
            case 'r':
                raid_mode = atoi(optarg);
                if (raid_mode != 0 && raid_mode != 1) {
                    fprintf(stderr, "Invalid RAID mode: %s\n", optarg);
                    print_usage(argv[0]);
                }
                break;
            case 'd':
                if (num_disks >= 32) {
                    fprintf(stderr, "Too many disks specified (max: 32)\n");
                    print_usage(argv[0]);
                }
                disk_files[num_disks++] = optarg;
                break;
            case 'i':
                num_inodes = strtoul(optarg, NULL, 10);
                break;
            case 'b':
                num_blocks = strtoul(optarg, NULL, 10);
                break;
            default:
                print_usage(argv[0]);
        }
    }

    if (raid_mode == -1 || num_disks < 2 || num_inodes == 0 || num_blocks == 0) {
        print_usage(argv[0]);
    }

    num_blocks = (num_blocks + 31) / 32 * 32; // Round to nearest multiple of 32

    struct wfs_sb superblock; // Declare superblock locally

    for (int i = 0; i < num_disks; ++i) {
        fds[i] = open(disk_files[i], O_RDWR | O_CREAT, 0644);
        if (fds[i] < 0) {
            perror("Failed to open disk image");
            exit(EXIT_FAILURE);
        }

        off_t disk_size = lseek(fds[i], 0, SEEK_END);
        if (disk_size < 0) {
            perror("Failed to get disk size");
            close(fds[i]);
            exit(EXIT_FAILURE);
        }

        initialize_superblock(&superblock, num_inodes, num_blocks, raid_mode, disk_size);
        write_superblock(fds[i], &superblock);

        // Initialize bitmaps
        initialize_bitmap(fds[i], superblock.i_bitmap_ptr, num_inodes, 0); // Do not allocate inode in initialize_bitmap
        initialize_bitmap(fds[i], superblock.d_bitmap_ptr, num_blocks, 0); // No data blocks allocated initially

        // Initialize the inode table for each disk
        initialize_inodes(fds[i], superblock.i_blocks_ptr, superblock.num_inodes);
    }

    // Initialize root inode and allocate it in the inode bitmap
    if (initialize_root_inode(fds, num_disks, &superblock) < 0) {
        fprintf(stderr, "Failed to initialize root inode.\n");
        for (int i = 0; i < num_disks; ++i) {
            close(fds[i]);
        }
        exit(EXIT_FAILURE);
    }

    // Close all disk files
    for (int i = 0; i < num_disks; ++i) {
        close(fds[i]);
    }

    printf("Filesystem successfully initialized with RAID mode %d on %d disks.\n", raid_mode, num_disks);
    return 0;
}
