/*
 * pehot: windowed hot-code profile for PowerEmu.
 *
 * Counts how often each translation block runs (inline, so cheap) and can be
 * reset and dumped while the guest runs, so a profile covers only the part
 * of a session that matters (a game, not the boot).
 *
 *   -plugin libpehot.dylib,ctl=/path/prefix
 *
 * Touch <prefix>.reset to zero the counters; touch <prefix>.dump to write
 * <prefix>.N.raw: every block that ran or was translated in the window, with
 * its run count and instruction words.  scripts/pehot-report.py ranks them.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_BLOCKS (1 << 20)
#define MAX_WORDS 64

typedef struct {
    uint64_t pc;
    uint32_t insns;
    uint32_t hash;
    uint32_t idx;
    uint32_t trans;         /* translations in this window */
    char *words;            /* first MAX_WORDS insns, hex */
} Block;

static GMutex lock;
static GHashTable *blocks;
static GPtrArray *by_idx;
static struct qemu_plugin_scoreboard *score;
static char *ctl;
static int dumps;
static uint64_t window_trans;
static gint64 window_start;
static volatile int stopping;

static guint block_hash(gconstpointer v)
{
    const Block *b = v;
    return (guint)(b->pc ^ (b->pc >> 32)) ^ b->insns * 0x9e3779b1u ^ b->hash;
}

static gboolean block_equal(gconstpointer a, gconstpointer b)
{
    const Block *x = a, *y = b;
    return x->pc == y->pc && x->insns == y->insns && x->hash == y->hash;
}

static qemu_plugin_u64 counter(uint32_t idx)
{
    return (qemu_plugin_u64){ score, idx * sizeof(uint64_t) };
}

static uint64_t count_of(const Block *b)
{
    return qemu_plugin_u64_sum(counter(b->idx));
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);
    Block key = { .pc = qemu_plugin_tb_vaddr(tb), .insns = n };
    uint32_t h = 2166136261u;

    for (size_t i = 0; i < n && i < 8; i++) {
        uint32_t w = 0;
        qemu_plugin_insn_data(qemu_plugin_tb_get_insn(tb, i), &w, sizeof(w));
        h = (h ^ w) * 16777619u;
    }
    key.hash = h;

    g_mutex_lock(&lock);
    Block *b = g_hash_table_lookup(blocks, &key);
    if (!b) {
        if (by_idx->len >= MAX_BLOCKS) {
            g_mutex_unlock(&lock);
            return;
        }
        b = g_new0(Block, 1);
        *b = key;
        b->idx = by_idx->len;
        GString *d = g_string_new(NULL);
        for (size_t i = 0; i < n && i < MAX_WORDS; i++) {
            uint32_t w = 0;
            qemu_plugin_insn_data(qemu_plugin_tb_get_insn(tb, i), &w, sizeof(w));
            g_string_append_printf(d, i ? ",%08x" : "%08x", GUINT32_FROM_BE(w));
        }
        b->words = g_string_free(d, FALSE);
        g_ptr_array_add(by_idx, b);
        g_hash_table_insert(blocks, b, b);
    }
    b->trans++;
    window_trans++;
    g_mutex_unlock(&lock);

    qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
        tb, QEMU_PLUGIN_INLINE_ADD_U64, counter(b->idx), 1);
}

static void reset(void)
{
    int ncpu = qemu_plugin_num_vcpus();
    g_mutex_lock(&lock);
    for (guint i = 0; i < by_idx->len; i++) {
        Block *b = g_ptr_array_index(by_idx, i);
        for (int c = 0; c < ncpu; c++) {
            qemu_plugin_u64_set(counter(b->idx), c, 0);
        }
        b->trans = 0;
    }
    window_trans = 0;
    window_start = g_get_monotonic_time();
    g_mutex_unlock(&lock);
}

/* Raw dump; scripts/pehot-report.py decodes and ranks it. */
static void dump(void)
{
    g_autofree char *path = g_strdup_printf("%s.%d.raw", ctl, ++dumps);
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    g_mutex_lock(&lock);
    fprintf(f, "window %.3f translations %" PRIu64 "\n",
            (g_get_monotonic_time() - window_start) / 1e6, window_trans);
    for (guint i = 0; i < by_idx->len; i++) {
        Block *b = g_ptr_array_index(by_idx, i);
        uint64_t c = count_of(b);
        if (!c && !b->trans) {
            continue;
        }
        fprintf(f, "%" PRIx64 " %" PRIu64 " %u %u %s\n",
                b->pc, c, b->insns, b->trans, b->words);
    }
    g_mutex_unlock(&lock);
    fclose(f);
}

static gpointer control_thread(gpointer p)
{
    g_autofree char *r = g_strdup_printf("%s.reset", ctl);
    g_autofree char *d = g_strdup_printf("%s.dump", ctl);
    struct stat st;

    while (!stopping) {
        g_usleep(250000);
        if (stat(r, &st) == 0) {
            unlink(r);
            reset();
        }
        if (stat(d, &st) == 0) {
            unlink(d);
            dump();
        }
    }
    return NULL;
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    stopping = 1;
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "ctl=")) {
            ctl = g_strdup(argv[i] + 4);
        }
    }
    if (!ctl) {
        fprintf(stderr, "pehot: needs ctl=/path/prefix\n");
        return -1;
    }
    blocks = g_hash_table_new(block_hash, block_equal);
    by_idx = g_ptr_array_new();
    score = qemu_plugin_scoreboard_new(MAX_BLOCKS * sizeof(uint64_t));
    window_start = g_get_monotonic_time();
    g_thread_new("pehot", control_thread, NULL);

    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
