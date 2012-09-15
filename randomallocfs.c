// Simple FUSE filesystem that allocates blocks randomly
// Precursor to FUSE-based convenient plausible deniability FS.
// Vitaly "_Vi" Shukela; License=MIT; 2012.

#define FUSE_USE_VERSION 26

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fuse.h>
#include <ulockmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>


int block_count;
int block_size;
FILE* rnd;
FILE* data;
const char* rnd_name;
const char* data_name;

int user_first_block;

/* 0 - free, 1 - busy */
unsigned char *busy_map;
int busy_blocks_count;

int *saved_directly_blocks;
int saved_directly_blocks_size;

unsigned char* shred_buffer;

struct mydirent {
    char* full_path;
    long long int length;
    int* blocks;
    int blocks_array_size;
    
};

struct myhandle {
    struct mydirent* ent;
    char* tmpbuf;
    int current_block;    
};

struct mydirent *dirents;
int current_dirent_size;
int dirent_entries_count;


int is_directory(const struct mydirent* i) {
    return i->full_path[strlen(i->full_path)-1] == '/';
}

int is_file(const struct mydirent* i) {
    return ! is_directory(i);
}

struct mydirent* find_dirent(const char* path) {
    int i;
    int pl = strlen(path);
    if (pl==0) return NULL;
    if (path[pl-1]=='/') --pl;
    for (i=0; i<dirent_entries_count; ++i) {
        int l = strlen(dirents[i].full_path);
        if (is_directory(&dirents[i])) --l;
        
        if (pl != l) continue;
        
        if (strncmp(path, dirents[i].full_path, pl)) continue;
            
        return &dirents[i];
    }
    return NULL;
}

void copy_dirent(struct mydirent* dst, struct mydirent* src) {
    memcpy(dst, src, sizeof (*src));
}

int get_block_count_for_length(long long int size);
void mark_unused_block(int i);
int write_block(const unsigned char* buffer, int i);

void remove_dirent(struct mydirent* ent) {
    int index = ent - dirents;
    int i;
    
    free(ent->full_path);
    
    unsigned char* zeroes = (unsigned char*) malloc(block_size);
    memset(zeroes, 0, block_size);
    
    if (ent->blocks) {
        int bc = get_block_count_for_length(ent->length);
        for (i=0; i<bc; ++i) {
            write_block(zeroes, ent->blocks[i]);
            mark_unused_block(ent->blocks[i]);
        }
    }
    free(ent->blocks);
    free(zeroes);
    for(i=index; i<dirent_entries_count-1; ++i) {
        copy_dirent(&dirents[i], &dirents[i+1]);
    }
    --dirent_entries_count;
}

struct mydirent* create_dirent(const char* path) {
    struct mydirent* ent;
    if (dirent_entries_count < current_dirent_size) {
        ent = &dirents[dirent_entries_count++];
    } else {
        current_dirent_size*=2;
        dirents = realloc(dirents, current_dirent_size*sizeof(*dirents));
        ent = &dirents[dirent_entries_count++];
    }
    ent->full_path = strdup(path);
    ent->length = 0;
    ent->blocks_array_size = 0;
    ent->blocks = NULL;
    return ent;
}


/*
   returns -1 on failure 
*/
int allocate_block(int privileged_mode) {
    int i;
    int index;
    
    if (!privileged_mode && busy_blocks_count*105.0/100.0 >= block_count) {
        return -1;
    }
    
    for (i=0; i<100; ++i) {
        unsigned long long int rrr;
        fread(&rrr, sizeof(rrr), 1, rnd);
        index = rrr % block_count;
        if (busy_map[index]) continue;
        busy_map[index] = 1;
        ++busy_blocks_count;
        return index;
    }
    
    for(i=index+1; i<block_count; ++i) {
        if (busy_map[index]) continue;
        return i;
    }
    
    for(i=0; i<index; ++i) {
        if (busy_map[index]) continue;
        return i;        
    }
    
    /* No more free blocks at all */
    
    if (privileged_mode) {
        /* Emergency measures: expand the storage file to save directory in it */
        fprintf(stderr, "Expanding the data file to store the directory\n");
        ++block_count;
        return block_count-1;
    }
    
    return -1; /* out of free space */
}

void mark_unused_block(int i) {
    if (!busy_map[i]) {
        fprintf(stderr, "Freeing not occupied block %d\n", i);
    } else {
        --busy_blocks_count;
    }
    busy_map[i] = 0;
}

void shred_block(int i) {
    fread(shred_buffer, 1, block_size, rnd);
    write_block(shred_buffer, i);
}

void mark_used_block(int i) {
    if (busy_map[i]) {
        fprintf(stderr, "Marking the block %d twice\n", i);
    } else {
        ++busy_blocks_count;
    }
    busy_map[i] = 1;
}

int nearest_power_of_two(int s) {
    int r=1;
    while(r<s) r*=2;
    return r;
}

int get_block_count_for_length(long long int size) {
    int bc = (size - 1) / block_size + 1;
    if (size == 0) bc = 0;
    return bc;
}

/* returns 0 on failure, 1 on success */
int ensure_size(struct mydirent* ent, long long int size) {
    if (size == 0) return 1;
    if (size <= ent->length) return 1;
    int ent_block_count      = get_block_count_for_length(ent->length);
    int required_block_count = get_block_count_for_length(size);
    if (required_block_count > ent->blocks_array_size) {
        ent->blocks_array_size = nearest_power_of_two(required_block_count);
        int* nb = (int*)realloc(ent->blocks, ent->blocks_array_size*sizeof(int));
        if(!nb) return 0;
        ent->blocks = nb;
    }
    
    unsigned char* zeroes = (unsigned char*) malloc(block_size);
    memset(zeroes, 0, block_size);
    
    int i;
    for(i=ent_block_count; i<required_block_count; ++i) {
        ent->blocks[i] = allocate_block(0);
        if ((ent->blocks[i]) == -1) {
            free(zeroes);
            return 0;
        }
        write_block(zeroes, ent->blocks[i]);
    }
    free(zeroes);
    
    ent->length = size;
    return 1;    
}

int d_truncate(struct mydirent* ent, long long int size) {
    if (size >= ent->length) return ensure_size(ent, size);
        
    int ent_block_count      = get_block_count_for_length(ent->length);
    int required_block_count = get_block_count_for_length(size);
    int i;
    
    for (i=required_block_count; i<ent_block_count; ++i) {
        shred_block(ent->blocks[i]);
        mark_unused_block(ent->blocks[i]);
    }
    ent->length = size;
    return 1;
}

int write_block(const unsigned char* buffer, int i) {
    fseek(data, i*block_size, SEEK_SET);
    return block_size == fwrite(buffer, 1, block_size, data);
}

int read_block(unsigned char* buffer, int i) {
    fseek(data, i*block_size, SEEK_SET);
    return block_size == fread(buffer, 1, block_size, data);
}


int get_saved_entry_minimal_size(struct mydirent* ent) {
    size_t dirent_size = 0;
    dirent_size += 4; /* full_path string length */
    dirent_size += strlen(ent->full_path);
    dirent_size += 8; /* file length */
    //int bc = get_block_count_for_length(ent->length);
    //dirent_size += 4*bc; /* block list */
    dirent_size += 4; /* number of blocks in this extent */
    // If we can't save all block numbers in this block, we save further block numbers in next blocks
    
    dirent_size+=16; /* there should be room for at least 4 blocks or this is not serious */
    dirent_size += 4; /* next dirent's block number */
    dirent_size += 4; /* next dirent's offset in block */ 
    dirent_size += 8; /* padding for possible extensions */
    return dirent_size;
}

/* Returns first entry's block. -1 on failure */
int save_entries(int starting_block) {
    int i, j;
    
    int allocated_blocks_journal_size=32;
    int *allocated_blocks_journal = (int*) malloc(allocated_blocks_journal_size*sizeof(int));
    int number_of_allocated_blocks=0;
    
    int first_block = starting_block;
    if(!first_block==-1) {
        free(allocated_blocks_journal);
        return -1;
    }
    allocated_blocks_journal[number_of_allocated_blocks++] = first_block;
    
    int current_block = first_block;
    
    /* First block is saved last to prevent entirely corrupting the filesystem in case of sudden shutdown */
    unsigned char* first_block_buffer = (unsigned char*) malloc(block_size);
    unsigned char* block_buffer = (unsigned char*) malloc(block_size);
    unsigned char* block = first_block_buffer;
    fread(block, 1, 8, rnd);
    int offset = 8; 
    
    int next_dirent_size = 0;
    next_dirent_size = get_saved_entry_minimal_size(&dirents[0]);
    
    int position_in_block_list = 0;
    int dirent_fully_saved = 0;
    
    for (i=0; i<dirent_entries_count; ++i) {
        struct mydirent* ent = &dirents[i];
        
        int current_dirent_size = next_dirent_size;
        int number_of_blocks_we_will_save = (block_size - offset - current_dirent_size) / sizeof(int);
        if (i==dirent_entries_count-1) {
            next_dirent_size = 0;
        } else {
            next_dirent_size = get_saved_entry_minimal_size(&dirents[i+1]);
        }
        
        int path_string_length = strlen(ent->full_path);
        long long int file_lenght = ent->length;
        int bc = get_block_count_for_length(ent->length);
        
        if (bc <= position_in_block_list + number_of_blocks_we_will_save) {
            number_of_blocks_we_will_save = bc - position_in_block_list;
            dirent_fully_saved = 1;
        } else {
            dirent_fully_saved = 0;
            next_dirent_size = current_dirent_size;
        }
        
        *(long int*)(block+offset) = htobe32(path_string_length); offset+=4;
        memcpy(block+offset, ent->full_path, path_string_length); offset+=path_string_length;
        *(long long int*)(block+offset) = htobe64(file_lenght); offset+=8;
        *(long int*)(block+offset) = htobe32(number_of_blocks_we_will_save); offset+=4;
        for (j=position_in_block_list; j<position_in_block_list + number_of_blocks_we_will_save; ++j) {
            *(long int*)(block+offset) = htobe32(ent->blocks[j]); offset+=4;
        }
        position_in_block_list += number_of_blocks_we_will_save;
        
        if (next_dirent_size == 0) {
            *(long int*)(block+offset) = htobe32(0); offset+=4;
            *(long int*)(block+offset) = htobe32(0); offset+=4;
            memset(block+offset, 0, 8); offset+=8;            
        } else if(offset+16+next_dirent_size < block_size) {
            *(long int*)(block+offset) = htobe32(current_block); offset+=4;
            *(long int*)(block+offset) = htobe32(offset+12); offset+=4;
            memset(block+offset, 0, 8); offset+=8;            
        } else {
            int new_block = allocate_block(1);
            if(new_block==-1) {                
                free(first_block_buffer);
                free(block_buffer);
                /* rolling back block allocations... */
                for (j=0; j<number_of_allocated_blocks; ++j) {
                    if (allocated_blocks_journal[i]!=starting_block) {
                        mark_unused_block(allocated_blocks_journal[i]);
                    }
                }
                free(allocated_blocks_journal);
                return -1;
            } else {
                if (allocated_blocks_journal_size == number_of_allocated_blocks) {
                   allocated_blocks_journal_size*=2;
                   allocated_blocks_journal = (int*)realloc(allocated_blocks_journal,
                        allocated_blocks_journal_size*sizeof(int));
                }
                allocated_blocks_journal[number_of_allocated_blocks++] = new_block;
            }
            *(long int*)(block+offset) = htobe32(new_block); offset+=4;
            *(long int*)(block+offset) = htobe32(8); offset+=4;
            memset(block+offset, 0, 8); offset+=8;            
            
            if (block == first_block_buffer) {
                block = block_buffer;
            } else {
                write_block(block, current_block);
            }
            
            current_block = new_block;
            fread(block, 1, 8, rnd);
            offset = 8; 
        }
        if (dirent_fully_saved) {
            position_in_block_list = 0;
        } else {
            --i;
        }
        next_dirent_size = current_dirent_size;
    }
    
    write_block(block, current_block);
    write_block(first_block_buffer, starting_block);
    fflush(data);
    fsync(fileno(data));
    
    for (i=0; i<saved_directly_blocks_size; ++i) {
        if (saved_directly_blocks[i]!=starting_block) {
            mark_unused_block(saved_directly_blocks[i]);
        }
    }
    saved_directly_blocks_size = allocated_blocks_journal_size;
    free(saved_directly_blocks);
    saved_directly_blocks = (int*)malloc(saved_directly_blocks_size*sizeof(int*));
    memcpy(saved_directly_blocks, allocated_blocks_journal, sizeof(int*)*allocated_blocks_journal_size);
    
    free(first_block_buffer);
    free(block_buffer);
    free(allocated_blocks_journal);
    return first_block;
}

/* return number of loaded entries on success, 0 on failure */
int load_entries(int starting_block, int only_mark_blocks) {
    if (!busy_map[starting_block]) {
        mark_used_block(starting_block);
    }
    int current_block = starting_block;
    
    unsigned char* block = (unsigned char*) malloc(block_size);
    
    read_block(block, starting_block);
    int offset=8;
    
    int j;
    int counter;
    
    char* previous_entry_name = strdup("///"); /* non-existing name */
    int block_position_in_previous_ent;
    
    struct mydirent *ent = NULL;
            
    for(;;) {            
        int pathlen = be32toh(*(long int*)(block+offset)); offset+=4;
        if(pathlen < 0 || pathlen >= block_size-32) { free(block); return 0; }
        if (!only_mark_blocks) {
            char* path = strndup((char*)(block+offset), pathlen);
            if (!strcmp(previous_entry_name, path)) {
                /* continued blocks for old entry, not a new one */
            } else {
                free(previous_entry_name);
                previous_entry_name = path;
                ent = create_dirent(path);
                block_position_in_previous_ent = 0;
            }
        }
        offset+=pathlen;
        long long int filelen = be64toh(*(long long int*)(block+offset)); offset+=8;
        if (ent) {
            ent->length = filelen;
        }
        
        int bc = get_block_count_for_length(filelen);
        int blocks_here = be32toh(*(long int*)(block+offset)); offset+=4;
        if (ent && !block_position_in_previous_ent) {
            ent->blocks_array_size = nearest_power_of_two(bc);
            ent->blocks = (int*)malloc(ent->blocks_array_size * sizeof(int));
            memset(ent->blocks, 0, ent->blocks_array_size);
        }
        for (j=0; j<blocks_here; ++j) {
            int idx = be32toh(*(long int*)(block+offset)); offset+=4;
            if(idx>=0 && idx<block_count) {
                mark_used_block(idx);
                if (ent) {
                    ent->blocks[j+block_position_in_previous_ent] = idx;
                }
            } else {
                free(block);
                return 0;
            }
        }
        block_position_in_previous_ent += blocks_here;
        
        ++counter;
        
        int next_block = be32toh(*(long int*)(block+offset)); offset+=4;
        int next_offset = be32toh(*(long int*)(block+offset)); offset+=4;
        
        if (next_block == 0 && next_offset == 0) break;
        
        if (next_block < 0 || next_block >= block_count) { free(block); return 0; }
        if (next_offset < 8  || next_offset >= block_size-32) { free(block); return 0; }
            
        if (next_block != current_block) {
            current_block = next_block;
            read_block(block, current_block);
            mark_used_block(current_block);
        }        
        
        offset = next_offset;
    }
    
    free(block);
    free(previous_entry_name);
    return counter;
}

/* return 1 on success, 0 on failure */
void traverse_entries_and_debug_print(int starting_block) {
    int current_block = starting_block;
    
    unsigned char* block = (unsigned char*) malloc(block_size);
    unsigned char* block2 = (unsigned char*) malloc(block_size);
    
    read_block(block, starting_block);
    int offset=8;
    
    int j;
    
    for(;;) {
        int pathlen = be32toh(*(long int*)(block+offset)); offset+=4;
        if(pathlen < 0 || pathlen >= block_size-32) {
            fprintf(stderr, "pathlen = %d is too big\n", pathlen);
            free(block2); free(block);
            return;
        }
        {
            unsigned char buf[256];
            memcpy(buf, block+offset, pathlen);
            buf[pathlen]=0;
            fprintf(stdout, "entry %s\n", buf); fflush(stdout);
        }
        offset+=pathlen;
        long long int filelen = be64toh(*(long long int*)(block+offset)); offset+=8;
        fprintf(stdout, "  size %lld (", filelen); fflush(stdout);
        int bc = get_block_count_for_length(filelen);
        fprintf(stdout, "block_count %d)\n", bc); fflush(stdout);
        int blocks_here = be32toh(*(long int*)(block+offset)); offset+=4;
        fprintf(stdout, "  block here %d\n", blocks_here); fflush(stdout);
        for (j=0; j<blocks_here; ++j) {
            int idx = be32toh(*(long int*)(block+offset)); offset+=4;
            fprintf(stdout, "  block %d\n", idx); fflush(stdout);
            if(idx>=0 && idx<block_count) {
                read_block(block2, idx);
                fprintf(stdout, "    %02X%02X%02X%02X\n", 
                    block2[0], block2[1], block2[2], block2[3]); 
                fflush(stdout);
            }
        }
        int next_block = be32toh(*(long int*)(block+offset)); offset+=4;
        fprintf(stdout, "  next_block %d\n", next_block); fflush(stdout);
        int next_offset = be32toh(*(long int*)(block+offset)); offset+=4;
        fprintf(stdout, "  next_offset %d\n", next_offset); fflush(stdout);
        
        if (next_block == 0 && next_offset == 0) break;
        
        if (next_block < 0 || next_block >= block_count) { free(block); free(block2); return; }
        if (next_offset < 8  || next_offset >= block_size-32) { free(block); free(block2); return; }
            
        if (next_block != current_block) {
            current_block = next_block;
            read_block(block, current_block);
        }        
        
        offset = next_offset;
    }
    
    free(block);
    free(block2);
    return;
}

void generate_test_dirents() {
    struct mydirent* ent;

    ent = create_dirent("/");
    
    ent = create_dirent("/ololo");    
    ensure_size(ent, 20);
    
    ent = find_dirent("/ololo");
    unsigned char *block = (unsigned char*) malloc(block_size);
    strcpy((char*)block, "Hello, world\n");
    write_block(block, ent->blocks[0]);
    
    ent = create_dirent("/r/");
    
    
    ent = create_dirent("/r/ke");
    
    ent = create_dirent("/r/kekeke");
    ensure_size(ent, 10000);
    int i;
    for(i=0; i<9999/block_size+1; ++i) {
        block[1]=i;
        write_block(block, ent->blocks[i]);
    }
    
    
    free(block);
    
    
    int s = save_entries(user_first_block);
    
    fprintf(stderr, "%d\n", s);
}

void debug_print_dirents(int starting_block) {
    traverse_entries_and_debug_print(starting_block);
    load_entries(starting_block, 1);
    int i;
    fprintf(stdout, "busy blocks: ");
    for(i=0; i<block_count; ++i) {
        if (busy_map[i]) {
            fprintf(stdout, "%d ", i);
        }
    }
    fprintf(stdout, "\n"); fflush(stdout);
    fprintf(stdout, "usage: %d of %d (%g%%)\n", busy_blocks_count, block_count, 100.0*busy_blocks_count/block_count);
}


static int xmp_getattr(const char *path, struct stat *stbuf)
{
    struct mydirent* ent = find_dirent(path);
    if(!ent) return -ENOENT;
        
    
    memset(stbuf, 0, sizeof(*stbuf));
    if (is_directory(ent)) {
        stbuf->st_mode = 0750 | S_IFDIR;
    } else {
        stbuf->st_mode = 0750 | S_IFREG;
        stbuf->st_size = ent->length;
        stbuf->st_blocks = get_block_count_for_length(ent->length);
        stbuf->st_blksize = block_size;
    }
    stbuf->st_ino = ent - dirents;
    return 0;
}

static int xmp_access(const char *path, int mask)
{
        return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size) { return -ENOSYS; }
static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
{
    struct stat st;
    //struct mydirent* ent = find_dirent(path);
        
    //if(!ent) return -ENOENT;
    //if(!is_directory(ent)) return -ENOENT;
    
    memset(&st, 0, sizeof(st));
    st.st_ino = 0;
    st.st_mode = 0640 | S_IFDIR;
    filler(buf, ".", &st, 0);
    filler(buf, "..", &st, 0);
    
    int i;
    int l = strlen(path);
    if (path[l-1]=='/') --l;
    
    // suppose path is "/ololo"
    for (i=0; i<dirent_entries_count; ++i) {
        struct mydirent* ent = &dirents[i];
        if(!strncmp(path, ent->full_path, l)) {
            // /ololoWHATEVER
            if (!strcmp(ent->full_path+l, "/")) continue; //  /ololo/ itself
            if (ent->full_path[l] != '/') continue; // /ololo2
                
            char pbuf[256];
            strncpy(pbuf, ent->full_path+l+1, 256);
            pbuf[255]=0;
            
            if (strchr(ent->full_path+l+1, '/')) {
                // /ololo/*/*
                if (strcmp(strchr(ent->full_path+l+1, '/'), "/")) {
                    // /ololo/something/nested
                    continue; // in subdirectory
                } else {
                    // /ololo/something/
                    // just directory mydirent
                }
            } 
            
            if (is_directory(ent)) {                
                st.st_mode = 0750 | S_IFDIR;
                pbuf[strlen(pbuf)-1]=0; // strip trailing '/'
            } else {
                st.st_mode = 0750 | S_IFREG;
                st.st_size = ent->length;
                st.st_blocks = get_block_count_for_length(ent->length);
                st.st_blksize = block_size;
            }
            st.st_ino = i;
            if (filler(buf, pbuf, &st, 0)) {
                return 0;
            }
        }
    }
    
    return 0;
}
static int xmp_mkdir(const char *path, mode_t mode)
{
    struct mydirent* ent = find_dirent(path);
    if(ent) return -EEXIST;
    
    int l = strlen(path);
    
    if(l>PATH_MAX-2) return -ENOSYS;
    
    char buf[PATH_MAX];
    strncpy(buf, path, PATH_MAX);
    buf[PATH_MAX-1]=0;
    
    // ensure the path ends in trailing slash
    if(buf[l-1]!='/') buf[l]='/'; 
    
    ent = create_dirent(buf);
    
    return 0;
}

static int xmp_unlink(const char *path)
{
    struct mydirent* ent = find_dirent(path);
    if (!ent) return -ENOENT;
    if (is_directory(ent)) return -EISDIR;
    
    remove_dirent(ent);
    
    return 0;
}

static int xmp_rmdir(const char *path)
{
    struct mydirent* ent = find_dirent(path);
    if (!ent) return -ENOENT;
    if (!is_directory(ent)) return -ENOTDIR;
    
    int l = strlen(path);
    if (path[l-1]=='/') --l;
    int i;
    for (i=0; i<dirent_entries_count; ++i) {
        struct mydirent* ent2 = &dirents[i];
        if(!strncmp(path, ent2->full_path, l)) {
            // /ololoWHATEVER
            if (ent2 == ent) continue; //  /ololo/ itself
            if (ent->full_path[l] != '/') continue; // /ololo2
                
            return -ENOTEMPTY;
        }   
    }
    
    remove_dirent(ent);
    
    return 0;
}
static int xmp_rename(const char *from, const char *to)
{
    struct mydirent* ent = find_dirent(from);
    struct mydirent* ent2 = find_dirent(to);
        
    if(!ent) return -ENOENT;
    if(ent2) return -ENOTEMPTY;
    
    
    int l = strlen(to);
    
    if(l>PATH_MAX-2) return -ENOSYS;
    
    char buf[PATH_MAX];
    strncpy(buf, to, PATH_MAX);
    buf[PATH_MAX-1]=0;
    
    if(is_directory(ent)) {
        // ensure the path ends in trailing slash
        if(buf[l-1]!='/') buf[l]='/'; 
    } else {
        // ensure that file path has not trailing slash
        if(buf[l-1]=='/') buf[l-1]=0;
    }
    
    free(ent->full_path);
    ent->full_path = strdup(buf);
    
    
    return 0;
}
static int xmp_chmod(const char *path, mode_t mode) { return -ENOSYS; }
static int xmp_chown(const char *path, uid_t uid, gid_t gid) { return -ENOSYS; }

static int xmp_truncate(const char *path, off_t size)
{
    struct mydirent* ent = find_dirent(path);
    if (!ent) return -ENOENT;   
        
    int ret = d_truncate(ent, size);
    if(ret) return 0;
        
	return -ENOSPC;
}

static int xmp_utimens(const char *path, const struct timespec ts[2]) { return -ENOSYS; }


static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	struct mydirent* ent = find_dirent(path);
    
    if (ent && is_directory(ent)) return -EISDIR;
        
    int flags = fi->flags;
    
    if (flags&O_CREAT) {
        if (ent) {
            if (flags&O_EXCL) return -EEXIST;
            if (flags&O_TRUNC) {
                d_truncate(ent, 0);
            }
        } else {
            ent = create_dirent(path);
        }
    } else {
        if (!ent) return -ENOENT;
    }
    
    struct myhandle *h = (struct myhandle*)malloc(sizeof(*h));
    
    h->tmpbuf = (char*)malloc(block_size);
    h->current_block = -1;
    h->ent = ent;
    fi->fh = (intptr_t)h;
    
    return 0;
}

static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	return xmp_open(path, fi);
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
    struct myhandle* h = (struct myhandle*)(intptr_t)fi->fh;
    struct mydirent* ent = h->ent;
    
    if(offset > ent->length) return 0;
    
    if (size + offset > ent->length) size=ent->length - offset;
        
    if (size<=0) return 0;
        
    int buf_offset = 0;
        
    int saved_size = size;
    
    while (size>0) {
        int block_number = (offset / block_size);
        
        if(h->current_block != block_number) {
            read_block((unsigned char*)h->tmpbuf, ent->blocks[block_number]);
            h->current_block = block_number;
        }
        
        int minioffset = offset - block_size*block_number;
        int minilen = block_size-minioffset;
        if (size < minilen) minilen = size;
            
        memcpy(buf+buf_offset, h->tmpbuf + minioffset, minilen);
        
        buf_offset += minilen;
        size-=minilen;
        offset+=minilen;
    }
    
	return saved_size;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
    struct myhandle* h = (struct myhandle*)(intptr_t)fi->fh;
    struct mydirent* ent = h->ent;
    
    int ret = ensure_size(ent, offset+size);
    
    if(!ret) return -ENOSPC;
        
    int buf_offset = 0;
    
    size_t saved_size = size;
    
    while (size>0) {
        int block_number = (offset / block_size);
        
        if(h->current_block != block_number) {
            if (block_number >= ent->blocks_array_size) {
                return -EINVAL;
            }
            read_block((unsigned char*)h->tmpbuf, ent->blocks[block_number]);
            h->current_block = block_number;
        }
        
        int minioffset = offset - block_size*block_number;
        int minilen = block_size-minioffset;
        if (size < minilen) minilen = size;
            
        memcpy(h->tmpbuf + minioffset, buf+buf_offset, minilen);
        
        write_block((unsigned char*)h->tmpbuf, ent->blocks[block_number]);
        
        buf_offset += minilen;
        size-=minilen;
        offset+=minilen;
    }
    
	return saved_size;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	return -ENOENT;
}

static int xmp_flush(const char *path, struct fuse_file_info *fi)
{
	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
    struct myhandle* h = (struct myhandle*)(intptr_t)fi->fh;
        
    free(h->tmpbuf);
    free(h);
	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	return 0;
}

static struct fuse_operations xmp_oper = {
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir,
	.mkdir		= xmp_mkdir,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
	.utimens	= xmp_utimens,
	.create		= xmp_create,
	.open		= xmp_open,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.flush		= xmp_flush,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
};


int main(int argc, char* argv[]) {
    block_size = 4096;
    rnd_name = "/dev/urandom";
    
    if (getenv("BLOCK_SIZE"))  block_size = atoi(getenv("BLOCK_SIZE"));
    if (getenv("RANDOM_FILE")) rnd_name = getenv("RANDOM_FILE");
        
    if (argc < 3) {
        fprintf(stderr, "Usage: [BLOCK_SIZE=1024] [RANDOM_FILE=/dev/urandom] randomallocfs data_file mountpoint [FUSE options]\n");
        return 1;
    }
    
    data_name = argv[1];
    user_first_block = 2;
    
    rnd= fopen(rnd_name, "rb");
    if(!rnd) { perror("fopen random"); return 2; }
    data = fopen(data_name, "rb+");
    if(!data) { perror("fopen data"); return 3; }
    
    {
        fseek(data, 0, SEEK_END);
        long long int len = ftell(data);
        block_count = (len / block_size);
        if (block_count<1) {
            fprintf(stderr, "Data file is empty. It should be pre-initialized with random data\n");
            return 4;
        }
    }
    
    shred_buffer = (unsigned char*) malloc(block_size);
    busy_map = (unsigned char*) malloc(block_count);
    busy_blocks_count = 0;
    memset(busy_map, 0, block_count);
    saved_directly_blocks_size = 0;
    saved_directly_blocks = NULL;
    
    mark_used_block(user_first_block);
    
    current_dirent_size = 128;
    dirents = (struct mydirent*) malloc(current_dirent_size * sizeof(*dirents));
    dirent_entries_count=0;
    
    int ret;
    
    if (!strcmp(argv[2], "--debug-generate")) {
        generate_test_dirents();
    }
    else if (!strcmp(argv[2], "--debug-print")) {
        debug_print_dirents(atoi(argv[3]));        
    } else {
        int r = load_entries(user_first_block, 0);
        
        if (!r) {
            fprintf(stderr, "No entries loaded, creating default entry\n");
            create_dirent("/");
        }
        
        #define MY 2
        char** new_argv = (char**)malloc( (argc-1+MY+1) * sizeof(char*));
        new_argv[0]="randomallocfs";
        // "My" args
        new_argv[1]="-s"; // single threaded
        new_argv[2]="-osubtype=randomallocfs";
        int i;
        for(i=2; i<argc; ++i) {
            new_argv[i-1+MY] = argv[i];
        }
        new_argv[i-1+MY]=NULL;
        ret = fuse_main(i-1+MY, new_argv, &xmp_oper, NULL);
        free(new_argv);
        
        save_entries(user_first_block);
    }
    
    
    free(dirents);
    free(busy_map);
    fclose(data);
    fclose(rnd);
    return ret;
}