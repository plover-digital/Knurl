#ifndef KNURL_CHECK_H
#define KNURL_CHECK_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
struct knurl_check_config {
    double duration_seconds;
    unsigned repetitions;
    unsigned threads;
    size_t memory_bytes;
    uint64_t seed;
    const char *target;
    const char *work_dir;
    const char *output_dir;
    const char *disk_binary;
    size_t disk_bytes;
};
double knurl_check_now(void);
void knurl_check_json_string(FILE *out,const char *text);
void knurl_check_summary(FILE *out,const double *values,size_t n,const char *unit);
int knurl_check_inventory(FILE *out,const struct knurl_check_config *config);
int knurl_check_disk(FILE *out,const struct knurl_check_config *config);
int knurl_check_cpu(FILE *out,const struct knurl_check_config *config);
int knurl_check_memory(FILE *out,const struct knurl_check_config *config);
int knurl_check_network(FILE *out,const struct knurl_check_config *config);
int knurl_check_network_command(int argc,char **argv);
#endif
