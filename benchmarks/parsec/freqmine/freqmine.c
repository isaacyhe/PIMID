/*
 * Frequent Itemset Mining -- FP-growth algorithm (PARSEC freqmine)
 * Builds an FP-tree from synthetic transactions, then mines frequent
 * itemsets via recursive conditional FP-tree construction.
 * Pointer-heavy tree traversal -- irregular memory access pattern.
 *
 * Compile: g++ -std=c++11 -O2 -Wall -I<path-to-zsim/misc/hooks> -lpthread -lm freqmine.c -o freqmine
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "zsim_hooks.h"

/* ------------------------------------------------------------------ */
/*  Arg parser                                                        */
/* ------------------------------------------------------------------ */
static int parse_int_arg(int argc, char** argv, const char* flag, int def) {
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return atoi(argv[i + 1]);
    return def;
}

/* ------------------------------------------------------------------ */
/*  LCG                                                               */
/* ------------------------------------------------------------------ */
static uint32_t bench_rand(uint32_t* s) {
    *s = *s * 1103515245 + 12345;
    return (*s >> 16) & 0x7FFF;
}

/* ------------------------------------------------------------------ */
/*  FP-tree node                                                      */
/* ------------------------------------------------------------------ */
#define MAX_ITEMS    64
#define MAX_CHILDREN 64

typedef struct FPNode {
    int             item;
    int             count;
    struct FPNode*  parent;
    struct FPNode*  children[MAX_CHILDREN];
    int             num_children;
    struct FPNode*  next_link;   /* header-table link: next node with same item */
} FPNode;

/* Simple pool allocator to avoid per-node malloc overhead */
#define NODE_POOL_SIZE 262144
static FPNode  g_node_pool[NODE_POOL_SIZE];
static int     g_node_pool_idx = 0;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static FPNode* alloc_node(void) {
    pthread_mutex_lock(&g_pool_lock);
    if (g_node_pool_idx >= NODE_POOL_SIZE) {
        pthread_mutex_unlock(&g_pool_lock);
        fprintf(stderr, "FP-tree node pool exhausted\n");
        return NULL;
    }
    FPNode* n = &g_node_pool[g_node_pool_idx++];
    pthread_mutex_unlock(&g_pool_lock);
    memset(n, 0, sizeof(FPNode));
    n->item = -1;
    return n;
}

static void reset_pool(void) {
    g_node_pool_idx = 0;
}

/* ------------------------------------------------------------------ */
/*  Header table: per-item linked list of FP-tree nodes               */
/* ------------------------------------------------------------------ */
typedef struct {
    int     item;
    int     support;         /* total support count */
    FPNode* head;            /* first node in linked list */
} HeaderEntry;

/* ------------------------------------------------------------------ */
/*  Insert a sorted transaction into the FP-tree                      */
/* ------------------------------------------------------------------ */
static void insert_tree(FPNode* root, int* items, int len, int count,
                         HeaderEntry* header, int num_items) {
    FPNode* cur = root;
    for (int i = 0; i < len; i++) {
        int item = items[i];

        /* Find existing child for this item */
        FPNode* child = NULL;
        for (int c = 0; c < cur->num_children; c++) {
            if (cur->children[c]->item == item) {
                child = cur->children[c];
                break;
            }
        }

        if (child) {
            child->count += count;
        } else {
            /* Create new child */
            child = alloc_node();
            if (!child) return;
            child->item   = item;
            child->count  = count;
            child->parent = cur;
            if (cur->num_children < MAX_CHILDREN)
                cur->children[cur->num_children++] = child;

            /* Add to header-table linked list */
            for (int h = 0; h < num_items; h++) {
                if (header[h].item == item) {
                    child->next_link = header[h].head;
                    header[h].head = child;
                    break;
                }
            }
        }
        cur = child;
    }
}

/* ------------------------------------------------------------------ */
/*  Comparison for sorting items by descending support                */
/* ------------------------------------------------------------------ */
static int* g_item_support;  /* global for qsort comparison */

static int cmp_by_support(const void* a, const void* b) {
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    return g_item_support[ib] - g_item_support[ia];
}

/* ------------------------------------------------------------------ */
/*  Mine frequent itemsets from a given FP-tree (recursive FP-growth) */
/*  Returns count of frequent itemsets found.                         */
/* ------------------------------------------------------------------ */
static long mine_fptree(HeaderEntry* header, int num_items, int min_sup,
                         int* prefix, int prefix_len, int depth) {
    long count = 0;

    /* Limit recursion depth to prevent stack overflow */
    if (depth > 10) return 0;

    /* Process items from least frequent to most frequent */
    for (int h = num_items - 1; h >= 0; h--) {
        if (header[h].support < min_sup) continue;

        int item = header[h].item;

        /* This item + prefix is a frequent itemset */
        count++;

        /* Build conditional pattern base */
        /* Traverse header-table link for this item */
        int cond_support[MAX_ITEMS];
        memset(cond_support, 0, sizeof(cond_support));

        /* Collect conditional patterns */
        int max_patterns = 256;
        int* cond_patterns = (int*)malloc((size_t)max_patterns * (MAX_ITEMS + 1) * sizeof(int));
        int  num_patterns = 0;

        for (FPNode* node = header[h].head; node != NULL; node = node->next_link) {
            /* Path from node's parent up to root */
            int path[MAX_ITEMS];
            int path_len = 0;
            FPNode* p = node->parent;
            while (p && p->item >= 0) {
                if (path_len < MAX_ITEMS)
                    path[path_len++] = p->item;
                p = p->parent;
            }
            if (path_len > 0 && num_patterns < max_patterns) {
                /* Store reversed path + count */
                int base = num_patterns * (MAX_ITEMS + 1);
                cond_patterns[base] = node->count;
                for (int i = 0; i < path_len; i++)
                    cond_patterns[base + 1 + i] = path[path_len - 1 - i];
                for (int i = path_len; i < MAX_ITEMS; i++)
                    cond_patterns[base + 1 + i] = -1;
                num_patterns++;

                for (int i = 0; i < path_len; i++)
                    cond_support[path[i]] += node->count;
            }
        }

        /* Build conditional FP-tree if any patterns exist */
        if (num_patterns > 0) {
            /* Filter items by min support and sort */
            int cond_items[MAX_ITEMS];
            int num_cond = 0;
            for (int i = 0; i < MAX_ITEMS; i++) {
                if (cond_support[i] >= min_sup)
                    cond_items[num_cond++] = i;
            }

            if (num_cond > 0) {
                /* Sort conditional items by support descending */
                g_item_support = cond_support;
                qsort(cond_items, (size_t)num_cond, sizeof(int), cmp_by_support);

                /* Build header table for conditional tree */
                HeaderEntry cond_header[MAX_ITEMS];
                for (int i = 0; i < num_cond; i++) {
                    cond_header[i].item    = cond_items[i];
                    cond_header[i].support = cond_support[cond_items[i]];
                    cond_header[i].head    = NULL;
                }

                /* Create conditional FP-tree root */
                FPNode* cond_root = alloc_node();
                if (cond_root) {
                    /* Insert each conditional pattern */
                    for (int p = 0; p < num_patterns; p++) {
                        int base = p * (MAX_ITEMS + 1);
                        int pcount = cond_patterns[base];

                        /* Filter and re-order items in pattern */
                        int filtered[MAX_ITEMS];
                        int flen = 0;
                        for (int ci = 0; ci < num_cond; ci++) {
                            int citem = cond_items[ci];
                            for (int j = 0; j < MAX_ITEMS; j++) {
                                if (cond_patterns[base + 1 + j] == -1) break;
                                if (cond_patterns[base + 1 + j] == citem) {
                                    filtered[flen++] = citem;
                                    break;
                                }
                            }
                        }
                        if (flen > 0)
                            insert_tree(cond_root, filtered, flen, pcount,
                                        cond_header, num_cond);
                    }

                    /* Recurse */
                    prefix[prefix_len] = item;
                    count += mine_fptree(cond_header, num_cond, min_sup,
                                         prefix, prefix_len + 1, depth + 1);
                }
            }
        }

        free(cond_patterns);
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  Thread context: each thread mines a subset of single-item suffixes*/
/* ------------------------------------------------------------------ */
typedef struct {
    int            id;
    int            start_item;    /* first header index to mine */
    int            end_item;      /* one past last header index */
    HeaderEntry*   header;
    int            num_items;
    int            min_sup;
    long           result;        /* frequent itemset count */
} MineCtx;

static void* mine_worker(void* arg) {
    MineCtx* ctx = (MineCtx*)arg;
    int prefix[MAX_ITEMS];
    long count = 0;

    /* Mine only the assigned suffix items */
    /* We create a copy of the header range and mine individually */
    for (int h = ctx->end_item - 1; h >= ctx->start_item; h--) {
        if (ctx->header[h].support < ctx->min_sup) continue;
        /* Count this single-item itemset */
        count++;

        /* Build and mine conditional FP-tree for this item */
        int item = ctx->header[h].item;
        int cond_support[MAX_ITEMS];
        memset(cond_support, 0, sizeof(cond_support));

        int max_patterns = 256;
        int* cond_patterns = (int*)malloc((size_t)max_patterns * (MAX_ITEMS + 1) * sizeof(int));
        int  num_patterns = 0;

        for (FPNode* node = ctx->header[h].head; node; node = node->next_link) {
            int path[MAX_ITEMS];
            int path_len = 0;
            FPNode* p = node->parent;
            while (p && p->item >= 0) {
                if (path_len < MAX_ITEMS) path[path_len++] = p->item;
                p = p->parent;
            }
            if (path_len > 0 && num_patterns < max_patterns) {
                int base = num_patterns * (MAX_ITEMS + 1);
                cond_patterns[base] = node->count;
                for (int i = 0; i < path_len; i++)
                    cond_patterns[base + 1 + i] = path[path_len - 1 - i];
                for (int i = path_len; i < MAX_ITEMS; i++)
                    cond_patterns[base + 1 + i] = -1;
                num_patterns++;
                for (int i = 0; i < path_len; i++)
                    cond_support[path[i]] += node->count;
            }
        }

        if (num_patterns > 0) {
            int cond_items[MAX_ITEMS];
            int num_cond = 0;
            for (int i = 0; i < MAX_ITEMS; i++)
                if (cond_support[i] >= ctx->min_sup)
                    cond_items[num_cond++] = i;

            if (num_cond > 0) {
                g_item_support = cond_support;
                qsort(cond_items, (size_t)num_cond, sizeof(int), cmp_by_support);

                HeaderEntry cond_header[MAX_ITEMS];
                for (int i = 0; i < num_cond; i++) {
                    cond_header[i].item    = cond_items[i];
                    cond_header[i].support = cond_support[cond_items[i]];
                    cond_header[i].head    = NULL;
                }

                FPNode* cond_root = alloc_node();
                if (cond_root) {
                    for (int pat = 0; pat < num_patterns; pat++) {
                        int base = pat * (MAX_ITEMS + 1);
                        int pcount = cond_patterns[base];
                        int filtered[MAX_ITEMS];
                        int flen = 0;
                        for (int ci = 0; ci < num_cond; ci++) {
                            int citem = cond_items[ci];
                            for (int j = 0; j < MAX_ITEMS; j++) {
                                if (cond_patterns[base + 1 + j] == -1) break;
                                if (cond_patterns[base + 1 + j] == citem) {
                                    filtered[flen++] = citem;
                                    break;
                                }
                            }
                        }
                        if (flen > 0)
                            insert_tree(cond_root, filtered, flen, pcount,
                                        cond_header, num_cond);
                    }
                    prefix[0] = item;
                    count += mine_fptree(cond_header, num_cond, ctx->min_sup,
                                         prefix, 1, 1);
                }
            }
        }
        free(cond_patterns);
    }

    ctx->result = count;
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */
int main(int argc, char** argv) {
    int num_trans   = parse_int_arg(argc, argv, "--size", 4096);
    int num_items   = parse_int_arg(argc, argv, "--items", MAX_ITEMS);
    int num_threads = parse_int_arg(argc, argv, "--threads", 1);

    if (num_items > MAX_ITEMS) num_items = MAX_ITEMS;
    int min_sup = num_trans / 20;   /* 5% minimum support */
    if (min_sup < 2) min_sup = 2;

    printf("Frequent Itemset Mining (FP-growth) -- trans=%d items=%d min_sup=%d threads=%d\n",
           num_trans, num_items, min_sup, num_threads);

    /* Generate synthetic transactions from LCG */
    /* Each transaction has 3-12 items from [0, num_items) */
    int max_trans_len = 12;
    int* trans_data = (int*)malloc((size_t)num_trans * (size_t)max_trans_len * sizeof(int));
    int* trans_len  = (int*)malloc((size_t)num_trans * sizeof(int));
    if (!trans_data || !trans_len) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* Item frequency counting */
    int item_count[MAX_ITEMS];
    memset(item_count, 0, sizeof(item_count));

    uint32_t seed = 42;
    for (int t = 0; t < num_trans; t++) {
        int len = 3 + (int)(bench_rand(&seed) % (uint32_t)(max_trans_len - 2));
        trans_len[t] = len;
        /* Generate unique items for this transaction */
        int used[MAX_ITEMS];
        memset(used, 0, sizeof(int) * (size_t)num_items);
        int base = t * max_trans_len;
        for (int i = 0; i < len; i++) {
            int item;
            do {
                item = (int)(bench_rand(&seed) % (uint32_t)num_items);
            } while (used[item]);
            used[item] = 1;
            trans_data[base + i] = item;
            item_count[item]++;
        }
    }

    /* ---- ROI begin ---- */
    zsim_roi_begin();

    reset_pool();

    /* Sort items by support descending, filter by min_sup */
    int sorted_items[MAX_ITEMS];
    int num_freq = 0;
    for (int i = 0; i < num_items; i++) {
        if (item_count[i] >= min_sup)
            sorted_items[num_freq++] = i;
    }
    g_item_support = item_count;
    qsort(sorted_items, (size_t)num_freq, sizeof(int), cmp_by_support);

    /* Create item-to-rank mapping for sorting transactions */
    int item_rank[MAX_ITEMS];
    memset(item_rank, -1, sizeof(item_rank));
    for (int i = 0; i < num_freq; i++)
        item_rank[sorted_items[i]] = i;

    /* Build header table */
    HeaderEntry header[MAX_ITEMS];
    for (int i = 0; i < num_freq; i++) {
        header[i].item    = sorted_items[i];
        header[i].support = item_count[sorted_items[i]];
        header[i].head    = NULL;
    }

    /* Build FP-tree */
    FPNode* root = alloc_node();
    if (!root) { fprintf(stderr, "pool alloc failed\n"); return 1; }

    for (int t = 0; t < num_trans; t++) {
        /* Filter and sort transaction items by rank */
        int base = t * max_trans_len;
        int sorted_trans[MAX_ITEMS];
        int slen = 0;
        for (int i = 0; i < trans_len[t]; i++) {
            int item = trans_data[base + i];
            if (item_rank[item] >= 0)
                sorted_trans[slen++] = item;
        }
        /* Simple insertion sort by rank */
        for (int i = 1; i < slen; i++) {
            int key = sorted_trans[i];
            int kr  = item_rank[key];
            int j = i - 1;
            while (j >= 0 && item_rank[sorted_trans[j]] > kr) {
                sorted_trans[j + 1] = sorted_trans[j];
                j--;
            }
            sorted_trans[j + 1] = key;
        }
        if (slen > 0)
            insert_tree(root, sorted_trans, slen, 1, header, num_freq);
    }

    /* Mine frequent itemsets: partition header items among threads */
    pthread_t*  threads = (pthread_t*)malloc((size_t)num_threads * sizeof(pthread_t));
    MineCtx*    ctxs    = (MineCtx*)malloc((size_t)num_threads * sizeof(MineCtx));

    int chunk = num_freq / num_threads;
    if (chunk < 1) chunk = 1;
    for (int t = 0; t < num_threads; t++) {
        ctxs[t].id         = t;
        ctxs[t].start_item = t * chunk;
        ctxs[t].end_item   = (t == num_threads - 1) ? num_freq : (t + 1) * chunk;
        ctxs[t].header     = header;
        ctxs[t].num_items  = num_freq;
        ctxs[t].min_sup    = min_sup;
        ctxs[t].result     = 0;
        pthread_create(&threads[t], NULL, mine_worker, &ctxs[t]);
    }
    for (int t = 0; t < num_threads; t++)
        pthread_join(threads[t], NULL);

    zsim_roi_end();
    /* ---- ROI end ---- */

    long total_frequent = 0;
    for (int t = 0; t < num_threads; t++)
        total_frequent += ctxs[t].result;

    /* Checksum: frequent itemset count + sum of frequent item supports */
    long checksum = total_frequent;
    for (int i = 0; i < num_freq; i++)
        checksum += header[i].support;

    printf("Frequent itemsets: %ld  Frequent items: %d\n", total_frequent, num_freq);
    printf("BENCH_CHECKSUM: %ld\n", checksum);
    printf("BENCH_DONE\n");

    free(ctxs);
    free(threads);
    free(trans_len);
    free(trans_data);
    return 0;
}
