#define _POSIX_C_SOURCE 200809L
#include "knurl_check.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Scalar, dependency-chain xorshift/multiply workload; not a crypto hash. */
static uint64_t cpu_hash(uint64_t x) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * UINT64_C(2685821657736338717);
}

struct cpu_gate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned ready, finished;
    int start, verify;
    double begin, deadline;
};
struct cpu_worker {
    struct cpu_gate *gate;
    uint64_t seed, iterations, checksum;
    unsigned index;
    int floating, verified;
    double finish;
};

/* Inputs and every intermediate dot-product result are exactly representable
 * binary fractions. The integer reference is independent of FP rounding/FMA. */
static void cpu_inputs(uint64_t seed, uint16_t a[256], uint16_t b[256],
                       double af[256], double bf[256]) {
    uint64_t x = seed ? seed : UINT64_C(1);
    for (unsigned i = 0; i < 256; i++) {
        x = cpu_hash(x);
        a[i] = (uint16_t)x;
        b[i] = (uint16_t)(x >> 32);
        af[i] = (double)a[i] / 65536.0;
        bf[i] = (double)b[i] / 65536.0;
    }
}

static void *cpu_worker_main(void *argument) {
    struct cpu_worker *w = argument;
    struct cpu_gate *g = w->gate;
    uint16_t ai[256], bi[256];
    double af[256], bf[256];
    uint64_t initial = cpu_hash(w->seed ^ (UINT64_C(0x9e3779b97f4a7c15) + w->index));
    if (!initial) initial = 1;
    if (w->floating) cpu_inputs(initial, ai, bi, af, bf);
    pthread_mutex_lock(&g->mutex);
    g->ready++;
    pthread_cond_broadcast(&g->condition);
    while (!g->start) pthread_cond_wait(&g->condition, &g->mutex);
    pthread_mutex_unlock(&g->mutex);

    uint64_t checksum = initial, count = 0;
    unsigned offset = 0;
    while (knurl_check_now() < g->deadline) {
        if (w->floating) {
            for (unsigned batch = 0; batch < 8; batch++) {
                double dot = 0;
                for (unsigned i = 0; i < 256; i++)
                    dot += af[(i + offset) & 255] * bf[i];
                uint64_t exact = (uint64_t)(dot * 4294967296.0);
                checksum = cpu_hash(checksum ^ exact);
                offset = (unsigned)((offset + exact + 1) & 255);
                count++;
            }
        } else {
            for (unsigned batch = 0; batch < 4096; batch++) checksum = cpu_hash(checksum);
            count += 4096;
        }
    }
    w->finish = knurl_check_now();
    w->iterations = count;
    w->checksum = checksum;

    /* No verification competes with a worker still inside its timed window. */
    pthread_mutex_lock(&g->mutex);
    g->finished++;
    pthread_cond_broadcast(&g->condition);
    while (!g->verify) pthread_cond_wait(&g->condition, &g->mutex);
    pthread_mutex_unlock(&g->mutex);

    uint64_t reference = initial;
    if (w->floating) {
        unsigned ref_offset = 0;
        for (uint64_t n = 0; n < count; n++) {
            uint64_t dot = 0;
            for (unsigned i = 0; i < 256; i++)
                dot += (uint64_t)ai[(i + ref_offset) & 255] * bi[i];
            reference = cpu_hash(reference ^ dot);
            ref_offset = (unsigned)((ref_offset + dot + 1) & 255);
        }
        w->verified = reference == checksum && ref_offset == offset && count != 0;
    } else {
        for (uint64_t n = 0; n < count; n++) reference = cpu_hash(reference);
        w->verified = reference == checksum && count != 0;
    }
    return NULL;
}

static int cpu_run(const struct knurl_check_config *c, int floating,
                   double *elapsed, uint64_t *operations, uint64_t *checksum) {
    struct cpu_gate g;
    memset(&g, 0, sizeof g);
    int e = pthread_mutex_init(&g.mutex, NULL);
    if (e) return e;
    e = pthread_cond_init(&g.condition, NULL);
    if (e) { pthread_mutex_destroy(&g.mutex); return e; }
    struct cpu_worker *workers = calloc(c->threads, sizeof *workers);
    pthread_t *threads = calloc(c->threads, sizeof *threads);
    if (!workers || !threads) {
        free(workers); free(threads);
        pthread_cond_destroy(&g.condition); pthread_mutex_destroy(&g.mutex);
        return ENOMEM;
    }
    unsigned created = 0;
    for (; created < c->threads; created++) {
        workers[created].gate = &g;
        workers[created].seed = c->seed;
        workers[created].index = created;
        workers[created].floating = floating;
        e = pthread_create(&threads[created], NULL, cpu_worker_main, &workers[created]);
        if (e) break;
    }
    pthread_mutex_lock(&g.mutex);
    while (g.ready < created) pthread_cond_wait(&g.condition, &g.mutex);
    g.begin = knurl_check_now();
    g.deadline = g.begin + (created == c->threads ? c->duration_seconds : 0);
    g.start = 1;
    pthread_cond_broadcast(&g.condition);
    while (g.finished < created) pthread_cond_wait(&g.condition, &g.mutex);
    double finish = g.deadline;
    for (unsigned i = 0; i < created; i++)
        if (workers[i].finish > finish) finish = workers[i].finish;
    g.verify = 1;
    pthread_cond_broadcast(&g.condition);
    pthread_mutex_unlock(&g.mutex);
    *operations = 0;
    *checksum = 0;
    for (unsigned i = 0; i < created; i++) {
        int joined = pthread_join(threads[i], NULL);
        if (!e && joined) e = joined;
        if (!e && !workers[i].verified) e = EILSEQ;
        *operations += workers[i].iterations * (floating ? 512 : 1);
        *checksum ^= cpu_hash(workers[i].checksum ^ i);
    }
    *elapsed = finish - g.begin;
    pthread_cond_destroy(&g.condition);
    pthread_mutex_destroy(&g.mutex);
    free(workers); free(threads);
    return e;
}

int knurl_check_cpu(FILE *out, const struct knurl_check_config *c) {
    if (!c || !isfinite(c->duration_seconds) || c->duration_seconds <= 0 ||
        c->duration_seconds > 60 || c->threads < 1 || c->threads > 64 ||
        c->repetitions < 1 || c->repetitions > 100) {
        fputs("{\"module\":\"cpu\",\"complete\":false,\"error\":\"invalid CPU configuration\"}", out);
        return 1;
    }
    static volatile uint64_t known_seed = 1;
    uint64_t known = known_seed;
    for (unsigned i = 0; i < 64; i++) known = cpu_hash(known);
    /* This literal also lets tests detect an accidentally changed kernel. */
    const uint64_t expected = UINT64_C(0x43fc3d1de5ef95df);
    if (known != expected) {
        fputs("{\"module\":\"cpu\",\"complete\":false,\"error\":\"integer known-answer self-test failed\"}", out);
        return 1;
    }
    double *rates = calloc(c->repetitions, sizeof *rates);
    if (!rates) {
        fputs("{\"module\":\"cpu\",\"complete\":false,\"error\":\"allocation failed\"}", out);
        return 1;
    }
    int failed = 0;
    fprintf(out, "{\"module\":\"cpu\",\"threads\":%u,\"integer_self_test\":\"%016" PRIx64 "\",\"measurement\":\"common_monotonic_window_including_terminal_batch_excluding_setup_and_full_reference_verification\",\"cases\":[", c->threads, known);
    for (int floating = 0; floating < 2; floating++) {
        if (floating) fputc(',', out);
        const char *unit = floating ? "algorithmic_FLOP/s" : "hashes/s";
        fprintf(out, "{\"name\":\"%s\",\"unit\":\"%s\",\"repetitions\":[",
                floating ? "exact_fraction_dot_product" : "integer_hash_chain", unit);
        unsigned valid = 0;
        for (unsigned r = 0; r < c->repetitions; r++) {
            double elapsed = 0;
            uint64_t operations = 0, checksum = 0;
            int error = cpu_run(c, floating, &elapsed, &operations, &checksum);
            if (r) fputc(',', out);
            double rate = !error && elapsed > 0 ? (double)operations / elapsed : 0;
            fprintf(out, "{\"repetition\":%u,\"seconds\":%.17g,\"operations\":%" PRIu64 ",\"rate\":%.17g,\"checksum\":\"%016" PRIx64 "\",\"verified\":%s,\"error_code\":%d}",
                    r + 1, elapsed, operations, rate, checksum, error ? "false" : "true", error);
            if (!error) rates[valid++] = rate;
            else failed = 1;
        }
        fputs("],\"summary\":", out);
        knurl_check_summary(out, rates, valid, unit);
        fputc('}', out);
    }
    fprintf(out, "],\"complete\":%s}", failed ? "false" : "true");
    free(rates);
    return failed || ferror(out);
}
