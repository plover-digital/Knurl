#define _POSIX_C_SOURCE 200809L
#include "knurl_check.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Keep memcpy an observable external call, including under ordinary LTO. */
static void *(*volatile memory_copy)(void *, const void *, size_t) = memcpy;
struct memory_node { volatile size_t next; unsigned char padding[64 - sizeof(size_t)]; };
_Static_assert(sizeof(struct memory_node) == 64, "pointer-chase node must be 64 bytes");
struct memory_gate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned ready, finished;
    int start, verify;
    double begin, deadline;
};
struct memory_worker {
    struct memory_gate *gate;
    unsigned char *a, *b;
    size_t bytes;
    uint64_t seed, iterations, checksum;
    unsigned kind;
    int verified;
    double finish;
};
static uint64_t memory_random(uint64_t *state) {
    *state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t x = *state;
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}
static uint64_t memory_bounded(uint64_t *state, uint64_t bound) {
    uint64_t threshold = (uint64_t)(-bound) % bound, x;
    do { x = memory_random(state); } while (x < threshold);
    return x % bound;
}

static void *memory_worker_main(void *argument) {
    struct memory_worker *w = argument;
    struct memory_gate *g = w->gate;
    size_t words = w->bytes / sizeof(uint64_t), nodes_count = w->bytes / 64;
    uint64_t *a = (uint64_t *)w->a, *b = (uint64_t *)w->b;
    struct memory_node *nodes = (struct memory_node *)w->a;
    size_t *permutation = (size_t *)w->b;
    uint64_t expected_sum = 0;
    size_t index = 0;
    if (w->kind == 3) {
        memset(w->a, 0, w->bytes);
        for (size_t i = 0; i < nodes_count; i++) permutation[i] = i;
        uint64_t random = w->seed;
        for (size_t i = nodes_count; i > 1; i--) {
            size_t j = (size_t)memory_bounded(&random, i);
            size_t swap = permutation[i - 1];
            permutation[i - 1] = permutation[j];
            permutation[j] = swap;
        }
        for (size_t i = 0; i < nodes_count; i++)
            nodes[permutation[i]].next = permutation[(i + 1) % nodes_count];
        index = permutation[0];
    } else {
        for (size_t i = 0; i < words; i++) {
            a[i] = w->seed + i;
            b[i] = 0;
            expected_sum += a[i];
        }
    }
    pthread_mutex_lock(&g->mutex);
    g->ready++;
    pthread_cond_broadcast(&g->condition);
    while (!g->start) pthread_cond_wait(&g->condition, &g->mutex);
    pthread_mutex_unlock(&g->mutex);

    uint64_t iterations = 0, checksum = 0;
    while (knurl_check_now() < g->deadline) {
        if (w->kind == 0) {
            uint64_t sum = 0;
            const volatile uint64_t *source = a;
            for (size_t i = 0; i < words; i++) sum += source[i];
            checksum += sum;
            iterations++;
        } else if (w->kind == 1) {
            volatile uint64_t *destination = a;
            iterations++;
            for (size_t i = 0; i < words; i++) destination[i] = (w->seed + i) ^ iterations;
        } else if (w->kind == 2) {
            iterations++;
            a[0] = w->seed ^ iterations;
            memory_copy(b, a, w->bytes);
        } else {
            /* Dependent volatile loads prevent vectorization or elimination.
             * The reported latency includes loop and periodic-clock overhead. */
            for (unsigned batch = 0; batch < 65536; batch++) index = nodes[index].next;
            iterations += 65536;
        }
    }
    w->finish = knurl_check_now();
    w->iterations = iterations;
    pthread_mutex_lock(&g->mutex);
    g->finished++;
    pthread_cond_broadcast(&g->condition);
    while (!g->verify) pthread_cond_wait(&g->condition, &g->mutex);
    pthread_mutex_unlock(&g->mutex);

    int valid = iterations != 0;
    if (w->kind == 3) {
        valid = valid && index == permutation[iterations % nodes_count];
        for (size_t i = 0; i < nodes_count; i++)
            if (nodes[permutation[i]].next != permutation[(i + 1) % nodes_count]) valid = 0;
        checksum = index;
    } else {
        if (w->kind == 0 && checksum != expected_sum * iterations) valid = 0;
        const volatile uint64_t *retained_a = a, *retained_b = b;
        for (size_t i = 0; i < words; i++) {
            uint64_t expected = w->seed + i;
            if (w->kind == 1) expected ^= iterations;
            if (w->kind == 2 && i == 0) expected = w->seed ^ iterations;
            if (retained_a[i] != expected || (w->kind == 2 && retained_b[i] != expected)) valid = 0;
            if (w->kind != 0) checksum += w->kind == 2 ? retained_b[i] : retained_a[i];
        }
    }
    w->checksum = checksum;
    w->verified = valid;
    return NULL;
}

static int memory_run(const struct knurl_check_config *c, size_t bytes, unsigned kind,
                      double *elapsed, uint64_t *count, uint64_t *checksum, double *access_ns) {
    struct memory_gate g;
    memset(&g, 0, sizeof g);
    int e = pthread_mutex_init(&g.mutex, NULL);
    if (e) return e;
    e = pthread_cond_init(&g.condition, NULL);
    if (e) { pthread_mutex_destroy(&g.mutex); return e; }
    struct memory_worker *workers = calloc(c->threads, sizeof *workers);
    pthread_t *threads = calloc(c->threads, sizeof *threads);
    if (!workers || !threads) { e = ENOMEM; goto cleanup; }
    for (unsigned i = 0; i < c->threads; i++) {
        workers[i].gate = &g;
        workers[i].bytes = bytes;
        workers[i].kind = kind;
        workers[i].seed = c->seed + i;
        e = posix_memalign((void **)&workers[i].a, 64, bytes);
        if (e) goto cleanup;
        e = posix_memalign((void **)&workers[i].b, 64, bytes);
        if (e) goto cleanup;
    }
    unsigned created = 0;
    for (; created < c->threads; created++) {
        e = pthread_create(&threads[created], NULL, memory_worker_main, &workers[created]);
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
    *count = 0;
    *checksum = 0;
    double worker_seconds = 0;
    for (unsigned i = 0; i < created; i++) {
        int joined = pthread_join(threads[i], NULL);
        if (!e && joined) e = joined;
        if (!e && !workers[i].verified) e = EILSEQ;
        *count += workers[i].iterations;
        worker_seconds += workers[i].finish - g.begin;
        *checksum ^= workers[i].checksum;
    }
    *elapsed = finish - g.begin;
    *access_ns = *count ? worker_seconds * 1e9 / (double)*count : 0;
cleanup:
    if (workers) {
        for (unsigned i = 0; i < c->threads; i++) {
            free(workers[i].a); free(workers[i].b);
        }
    }
    free(workers); free(threads);
    pthread_cond_destroy(&g.condition);
    pthread_mutex_destroy(&g.mutex);
    return e;
}

int knurl_check_memory(FILE *out, const struct knurl_check_config *c) {
    const size_t maximum = (size_t)1024 * 1024 * 1024;
    if (!c || !isfinite(c->duration_seconds) || c->duration_seconds <= 0 ||
        c->duration_seconds > 60 || c->threads < 1 || c->threads > 64 ||
        c->repetitions < 1 || c->repetitions > 100 || c->memory_bytes > maximum ||
        c->memory_bytes < (size_t)2 * c->threads * 4096) {
        fputs("{\"module\":\"memory\",\"complete\":false,\"error\":\"invalid memory configuration or allocation budget (8KiB/thread minimum, 1GiB total maximum)\"}", out);
        return 1;
    }
    size_t max_set = c->memory_bytes / (2 * c->threads);
    max_set -= max_set % 64;
    size_t candidates[] = {4096, 65536, 1048576, max_set};
    size_t sets[4];
    unsigned nsets = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (candidates[i] > max_set) continue;
        int duplicate = 0;
        for (unsigned j = 0; j < nsets; j++)
            if (sets[j] == candidates[i]) duplicate = 1;
        if (!duplicate) sets[nsets++] = candidates[i];
    }
    double *rates = calloc(c->repetitions, sizeof *rates);
    if (!rates) {
        fputs("{\"module\":\"memory\",\"complete\":false,\"error\":\"allocation failed\"}", out);
        return 1;
    }
    const char *names[] = {"scalar_sequential_read", "scalar_sequential_write", "memcpy", "random_pointer_chase"};
    fprintf(out, "{\"module\":\"memory\",\"threads\":%u,\"allocation_budget_bytes\":%zu,\"pointer_node_bytes\":64,\"measurement\":\"common_monotonic_window_including_terminal_sweep_excluding_setup_and_verification\",\"verification\":\"full_retained_working_set_and_expected_checksum_or_cycle_position\",\"cases\":[", c->threads, c->memory_bytes);
    int failed = 0, first = 1;
    for (unsigned set = 0; set < nsets; set++) {
        for (unsigned kind = 0; kind < 4; kind++) {
            if (!first) fputc(',', out);
            first = 0;
            const char *unit = kind == 3 ? "ns/access" : "B/s";
            fprintf(out, "{\"name\":\"%s\",\"working_set_bytes_per_thread\":%zu,\"unit\":\"%s\",\"interpretation\":\"%s\",\"repetitions\":[", names[kind], sets[set], unit, kind == 3 ? "dependent_chain_time_per_access_including_loop_and_scheduler" : kind == 2 ? "application_read_plus_write_bytes_not_physical_bus_traffic" : "scalar_application_bytes_not_peak_memory_bandwidth");
            unsigned valid = 0;
            for (unsigned r = 0; r < c->repetitions; r++) {
                double elapsed = 0, access_ns = 0;
                uint64_t iterations = 0, checksum = 0;
                int error = memory_run(c, sets[set], kind, &elapsed, &iterations, &checksum, &access_ns);
                uint64_t bytes = kind == 3 ? 0 : iterations * sets[set] * (kind == 2 ? 2 : 1);
                /* Pooled chain time/access includes scheduling, loop, and
                 * periodic clock overhead; never label it raw DRAM latency. */
                double value = 0;
                if (!error && elapsed > 0 && iterations) {
                    value = kind == 3 ? access_ns : (double)bytes / elapsed;
                }
                if (r) fputc(',', out);
                fprintf(out, "{\"repetition\":%u,\"seconds\":%.17g,\"iterations\":%" PRIu64 ",\"accesses\":%" PRIu64 ",\"traffic_bytes\":%" PRIu64 ",\"value\":%.17g,\"checksum\":\"%016" PRIx64 "\",\"verified\":%s,\"error_code\":%d}",
                        r + 1, elapsed, iterations, kind == 3 ? iterations : 0, bytes, value, checksum, error ? "false" : "true", error);
                if (!error) rates[valid++] = value;
                else failed = 1;
            }
            fputs("],\"summary\":", out);
            knurl_check_summary(out, rates, valid, unit);
            fputc('}', out);
        }
    }
    fprintf(out, "],\"complete\":%s}", failed ? "false" : "true");
    free(rates);
    return failed || ferror(out);
}
