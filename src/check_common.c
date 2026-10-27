#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "knurl_check.h"
#include <math.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

double knurl_check_now(void) {
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)){perror("knurl-check: monotonic clock");exit(3);}
    return (double)ts.tv_sec+(double)ts.tv_nsec/1e9;
}
void knurl_check_json_string(FILE *out,const char *text) {
    fputc('"',out);
    if(text)for(const unsigned char *p=(const unsigned char*)text;*p;p++) {
        if(*p=='"'||*p=='\\'){fputc('\\',out);fputc(*p,out);}
        else if(*p<32)fprintf(out,"\\u%04x",(unsigned)*p);
        else fputc(*p,out);
    }
    fputc('"',out);
}
static int compare_double(const void *a,const void *b) {
    double x=*(const double*)a,y=*(const double*)b;
    return (x>y)-(x<y);
}
static double t_critical(size_t n) {
    static const double table[]={0,12.7062047364,4.30265272975,3.18244630528,2.77644510520,
        2.57058183564,2.44691184879,2.36462425159,2.30600413520,2.26215716285,
        2.22813885196,2.20098516008,2.17881282966,2.16036865646,2.14478668792,
        2.13144954556,2.11990529922,2.10981557783,2.10092204024,2.09302405441,
        2.08596344727,2.07961384473,2.07387306790,2.06865761042,2.06389856163,
        2.05953855275,2.05552943864,2.05183051648,2.04840714180,2.04522964213,2.04227245630};
    return n>=2&&n<=31?table[n-1]:1.96;
}
void knurl_check_summary(FILE *out,const double *values,size_t n,const char *unit) {
    fprintf(out,"{\"count\":%zu,\"unit\":",n);knurl_check_json_string(out,unit);
    if(!n) {fputs(",\"min\":null,\"max\":null,\"mean\":null,\"median\":null,\"sd\":null,\"p95\":null,\"mean_ci95_low\":null,\"mean_ci95_high\":null,\"ci_method\":\"unavailable_without_repetitions\",\"sample_std\":null,\"cv\":null,\"p5\":null,\"p99\":null,\"exact_mode\":[],\"exact_mode_status\":\"none/no samples\"}",out);return;}
    double *sorted=malloc(n*sizeof *sorted);
    if(!sorted){fputs(",\"error\":\"allocation_failed\"}",out);return;}
    double mean=0;
    for(size_t i=0;i<n;i++){sorted[i]=values[i];mean+=values[i];}
    mean/=n;
    double sum=0;
    for(size_t i=0;i<n;i++){double d=values[i]-mean;sum+=d*d;}
    qsort(sorted,n,sizeof *sorted,compare_double);
    double sd=n>1?sqrt(sum/(n-1)):0;
    double median=n%2?sorted[n/2]:(sorted[n/2-1]+sorted[n/2])/2;
    size_t rank=(size_t)ceil(.95*n);if(!rank)rank=1;
    fprintf(out,",\"min\":%.17g,\"max\":%.17g,\"mean\":%.17g,\"median\":%.17g,",sorted[0],sorted[n-1],mean,median);
    if(n>1)fprintf(out,"\"sd\":%.17g,",sd);else fputs("\"sd\":null,",out);
    fprintf(out,"\"p95\":%.17g,",sorted[rank-1]);
    if(n>1)fprintf(out,"\"mean_ci95_low\":%.17g,\"mean_ci95_high\":%.17g,",mean-t_critical(n)*sd/sqrt((double)n),mean+t_critical(n)*sd/sqrt((double)n));
    else fputs("\"mean_ci95_low\":null,\"mean_ci95_high\":null,",out);
    fputs("\"ci_method\":",out);
    knurl_check_json_string(out,n<2?"unavailable_without_repetitions":n<=31?"student_t_0.975_df_n_minus_1":"normal_approximation_1.96_large_df");
    fprintf(out,",\"ci_degrees_of_freedom\":%zu,\"sd_method\":\"sample_n_minus_1\",\"p95_method\":\"nearest_rank\",\"sample_std\":",n-1);
    if(n>1)fprintf(out,"%.17g",sd);else fputs("null",out);
    fputs(",\"cv\":",out);
    if(n>1&&mean!=0)fprintf(out,"%.17g",sd/mean);else fputs("null",out);
    size_t low_rank=(size_t)ceil(.05*n),high_rank=(size_t)ceil(.99*n);
    if(!low_rank)low_rank=1;
    if(!high_rank)high_rank=1;
    fprintf(out,",\"p5\":%.17g,\"p99\":%.17g,\"percentile_method\":\"nearest_rank\",\"exact_mode\":[",sorted[low_rank-1],sorted[high_rank-1]);
    size_t highest=1;
    for(size_t i=0;i<n;) {
        size_t end=i+1;while(end<n&&sorted[end]==sorted[i])end++;
        if(end-i>highest)highest=end-i;
        i=end;
    }
    int first=1;
    if(highest>1)for(size_t i=0;i<n;) {
        size_t end=i+1;while(end<n&&sorted[end]==sorted[i])end++;
        if(end-i==highest){if(!first)fputc(',',out);fprintf(out,"%.17g",sorted[i]);first=0;}
        i=end;
    }
    fputs("],\"exact_mode_status\":",out);
    knurl_check_json_string(out,highest>1?"mode(s)":"none/all unique");
    fputc('}',out);
    free(sorted);
}
static void cpu_description(char *out,size_t capacity) {
    snprintf(out,capacity,"unavailable");
#ifdef __APPLE__
    size_t n=capacity;
    if(sysctlbyname("machdep.cpu.brand_string",out,&n,NULL,0))snprintf(out,capacity,"unavailable");
#elif defined(__linux__)
    FILE *f=fopen("/proc/cpuinfo","r");
    if(f){char line[1024];while(fgets(line,sizeof line,f)) {
        if(!strncmp(line,"model name",10)||!strncmp(line,"Model",5)) {
            char *p=strchr(line,':');if(!p)continue;p++;while(*p==' '||*p=='\t')p++;
            p[strcspn(p,"\r\n")]=0;snprintf(out,capacity,"%s",p);break;
        }
    }fclose(f);}
#endif
    out[capacity-1]=0;
}
int knurl_check_inventory(FILE *out,const struct knurl_check_config *config) {
    struct utsname u;int have_uname=uname(&u)==0;
    char cpu[512];cpu_description(cpu,sizeof cpu);
    fputs("{\"module\":\"inventory\",\"os\":",out);knurl_check_json_string(out,have_uname?u.sysname:"unavailable");
    fputs(",\"kernel\":",out);knurl_check_json_string(out,have_uname?u.release:"unavailable");
    fputs(",\"architecture\":",out);knurl_check_json_string(out,have_uname?u.machine:"unavailable");
    fputs(",\"cpu_description\":",out);knurl_check_json_string(out,cpu);
    long logical=sysconf(_SC_NPROCESSORS_ONLN),page=sysconf(_SC_PAGESIZE);
    fputs(",\"logical_cpus\":",out);
    if(logical>0)fprintf(out,"%ld",logical);else fputs("null",out);
    uint64_t ram=0,available=0;
#ifdef __APPLE__
    size_t len=sizeof ram;if(sysctlbyname("hw.memsize",&ram,&len,NULL,0))ram=0;
#elif defined(__linux__)
    long pages=sysconf(_SC_PHYS_PAGES);if(pages>0&&page>0)ram=(uint64_t)pages*(uint64_t)page;
    FILE *mem=fopen("/proc/meminfo","r");
    if(mem){char line[256];while(fgets(line,sizeof line,mem)){unsigned long long kib;if(sscanf(line,"MemAvailable: %llu kB",&kib)==1){available=(uint64_t)kib*1024;break;}}fclose(mem);}
#endif
    (void)page;
    fputs(",\"ram_total_bytes\":",out);
    if(ram)fprintf(out,"%"PRIu64,ram);else fputs("null",out);
    fputs(",\"ram_total_note\":",out);knurl_check_json_string(out,ram?"OS reported physical RAM":"unavailable on this platform");
    fputs(",\"ram_available_bytes\":",out);
    if(available)fprintf(out,"%"PRIu64,available);else fputs("null",out);
    fputs(",\"ram_available_note\":",out);knurl_check_json_string(out,available?"OS estimated available":"unavailable on this platform");
    fputs(",\"filesystem\":{\"target\":",out);knurl_check_json_string(out,config->target);
    struct statvfs fs;
    int rc=statvfs(config->target,&fs);
    if(!rc)fprintf(out,",\"available\":true,\"block_bytes\":%lu,\"total_bytes\":%.0Lf,\"available_bytes\":%.0Lf,\"read_only\":%s",(unsigned long)fs.f_frsize,(long double)fs.f_blocks*fs.f_frsize,(long double)fs.f_bavail*fs.f_frsize,(fs.f_flag&ST_RDONLY)?"true":"false");
    else fputs(",\"available\":false,\"reason\":\"statvfs_failed\"",out);
    fputs("},\"storage_health\":{\"status\":\"unavailable\",\"reason\":\"requires optional platform-specific device tooling; not inferred from filesystem metadata\"},\"private_identifiers_collected\":false}",out);
    return rc?1:0;
}
