#include "pool.h"

#include "logging.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

const size_t DEFAULT_NUM_BLOCKS = 64;

// ==== DATA TYPES ====

struct meta_block;

struct control_block
{
    unsigned char* block;
    // Pointer back to my parent meta_block
    struct meta_block* parent;
    bool          is_free;
};

struct meta_block
{
    size_t        blocks_used;
    size_t        num_blocks;
    struct meta_block* next;
    unsigned char blocks[];
};

struct pool
{
    size_t block_size;
    size_t next_free_block;
    size_t total_num_blocks;
    // Head of the linked list of meta-blocks
    struct meta_block* blocks;
    // Map of ptrs to blocks and whether or not they're free
    struct control_block* control;
};

// ==== STATIC FUNCTION PROTOTYPES

static int pool_add_meta_block(struct pool* pool);

// ==== PUBLIC FUNCTIONS IMPLEMENTATION ====

struct pool* pool_init(size_t block_size, size_t init_num_blocks)
{
    struct pool* pool = calloc(1, sizeof(struct pool));
    if (!pool)
    {
        MINIWEB_LOG_ERROR(
            "Failed to allocate space for new pool control structure!");
        return NULL;
    }

    int rc = pool_create(pool, block_size, init_num_blocks);
    if (rc != 0)
    {
        MINIWEB_LOG_ERROR("Failed to initialise pool control structure: %d", rc);
        free(pool);
        return NULL;
    }

    return pool;
}

int pool_create(struct pool* restrict pool,
                size_t                block_size,
                size_t                init_num_blocks)
{
    assert(pool);

    // Create our first meta block
    struct meta_block* meta =
        calloc(1, sizeof(struct meta_block) + (init_num_blocks * block_size));
    if (!meta)
    {
        MINIWEB_LOG_ERROR("Failed to initialise meta_block of size: %zu",
                          sizeof(struct meta_block) +
                              (init_num_blocks * block_size));
        return -1;
    }

    // Create the initial control block
    struct control_block* control =
        calloc(1, sizeof(struct control_block) * init_num_blocks);
    if (!control)
    {
        MINIWEB_LOG_ERROR("Failed to initialise control block for %zu elements",
                          init_num_blocks);
        free(meta);
        return -2;
    }

    // Initialise our members
    meta->blocks_used = 0;
    meta->num_blocks  = init_num_blocks;
    meta->next        = NULL;

    for (size_t i = 0; i < init_num_blocks; ++i)
    {
        control[i].block   = meta->blocks + (block_size * i);
        control[i].is_free = true;
        control[i].parent  = meta;
    }

    pool->block_size      = block_size;
    pool->next_free_block = 0;
    pool->total_num_blocks = init_num_blocks;
    pool->blocks          = meta;
    pool->control         = control;

    return 0;
}

struct pool_handle pool_alloc(pool_t* pool)
{
    if (pool->next_free_block == SIZE_MAX)
    {
        int rc = pool_add_meta_block(pool);
        if (rc != 0) { return (struct pool_handle) {.id = SIZE_MAX, .data = NULL}; }
    }

    // Let's look in the block at next_free_block to see if it's really free
    size_t control_index = pool->next_free_block;
    for (; control_index <= pool->total_num_blocks; ++control_index)
    {
        if (control_index == pool->total_num_blocks)
        {
            int rc = pool_add_meta_block(pool);
            if (rc != 0)
            { return (struct pool_handle) {.id = SIZE_MAX, .data = NULL}; }
        }
        if (pool->control[control_index].is_free) { break; }
    }

    struct control_block* control = &pool->control[control_index];

    control->is_free = false;
    ++control->parent->blocks_used;
    return (struct pool_handle) {.id = control_index, .data = control->block};
}

struct pool_handle pool_calloc(struct pool* pool)
{
    assert(pool);

    struct pool_handle handle = pool_alloc(pool);
    memset(handle.data, 0, pool->block_size);

    return handle;
}

void pool_free(pool_t* pool, struct pool_handle handle)
{
    assert(handle.id < pool->total_num_blocks);

    struct control_block* control = &pool->control[handle.id];
    control->is_free              = true;
    control->parent->blocks_used -= 1;

    if (pool->next_free_block > handle.id) { pool->next_free_block = handle.id; }
}

void pool_clear(struct pool* restrict pool)
{
    assert(pool);
    // Need to free all the meta blocks
    // Then free the control block
    struct meta_block* curr = pool->blocks;
    while (curr)
    {
        struct meta_block* old = curr;
        curr                   = old->next;
        free(old);
    }

    free(pool->control);
}

void pool_destroy(struct pool* restrict pool)
{
    assert(pool);
    pool_clear(pool);
    free(pool);
}

// ==== STATIC FUNCTIONS IMPLEMENTATION

static int pool_add_meta_block(struct pool* pool)
{
    // Let's make the new meta block twice as big
    struct meta_block* last_meta = pool->control[pool->total_num_blocks - 1].parent;
    size_t             new_meta_size = last_meta->num_blocks * 2;

    struct meta_block* new_meta =
        calloc(1, sizeof(struct meta_block) + (new_meta_size * pool->block_size));
    if (!new_meta)
    {
        MINIWEB_LOG_ERROR("Failed to alloc new meta_block of size %zu",
                          sizeof(struct meta_block) +
                              (new_meta_size * pool->block_size));
        return -1;
    }

    // Now we need to grow the control array...
    struct control_block* new_control =
        realloc(pool->control, pool->total_num_blocks + new_meta_size);
    if (!new_control)
    {
        MINIWEB_LOG_ERROR("Failed to grow the control array to size %zu",
                          pool->total_num_blocks + new_meta_size);
        return -2;
    }

    // Fill out our new data...
    new_meta->num_blocks  = new_meta_size;
    new_meta->blocks_used = 0;
    new_meta->next        = NULL;
    last_meta->next       = new_meta;

    for (size_t i = pool->total_num_blocks;
         i < pool->total_num_blocks + new_meta_size; ++i)
    {
        new_control[i].parent  = new_meta;
        new_control[i].is_free = true;
    }

    pool->next_free_block  = pool->total_num_blocks;
    pool->total_num_blocks = pool->total_num_blocks + new_meta_size;
    pool->control          = new_control;

    return 0;
}