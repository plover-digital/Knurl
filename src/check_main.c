#define _POSIX_C_SOURCE 200809L
#include "knurl_check.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void help(void) {
    puts("Usage: knurl-check inventory|cpu|memory|network|disk|check [options]\n"
         "       knurl-check server|client [explicit network options]\n"
         "Opt-in bounded system checkout; check runs inventory/CPU/memory/loopback network.\n"
         "Disk runs only with explicit --work-dir AND --output-dir.\n"
         "  --duration SECONDS   per case/repetition; default .25, range .001..5\n"
         "  --repetitions N      default 3, range 1..30\n"
         "  --threads N          default 1, range 1..64\n"
         "  --memory-bytes N     default 16777216, range 4096..1073741824\n"
         "  --seed N             default 20261027\n"
         "  --target PATH        existing filesystem inventory target; default .\n"
         "  --work-dir PATH      dedicated disk scratch directory (explicit opt-in)\n"
         "  --output-dir PATH    disk adapter raw output directory\n"
         "  --disk-bytes N      default 16777216, 65536 multiples up to 67108864\n"
         "  --disk-binary PATH   legacy disk engine; default ./knurl\n"
         "  --help               show this message\n"
         "JSON stdout; diagnostics stderr. No privileged settings are changed.\n"
         "network uses loopback only; server/client require explicit invocation.");
}
static int parse_u64(const char *s,uint64_t *v) {
    if(!s||!*s||*s=='-'||*s=='+')return 0;
    errno=0;char *end;unsigned long long n=strtoull(s,&end,10);
    if(errno||*end)return 0;
    *v=(uint64_t)n;return 1;
}
static int error(const char *message) {fprintf(stderr,"knurl-check: %s\n",message);return 2;}
typedef int (*Module)(FILE*,const struct knurl_check_config*);
int main(int argc,char **argv) {
    if(argc<2){help();return 2;}
    if(!strcmp(argv[1],"--help")||!strcmp(argv[1],"help")){help();return 0;}
    if(!strcmp(argv[1],"server")||!strcmp(argv[1],"client"))return knurl_check_network_command(argc-1,argv+1);
    const char *command=argv[1];
    if(strcmp(command,"inventory")&&strcmp(command,"cpu")&&strcmp(command,"memory")&&strcmp(command,"network")&&strcmp(command,"disk")&&strcmp(command,"check"))return error("unknown subcommand");
    char *sibling=NULL;
    const char *slash=strrchr(argv[0],'/');
    if(slash) {
        size_t prefix=(size_t)(slash-argv[0])+1;
        sibling=malloc(prefix+6);
        if(!sibling)return error("allocation failed");
        memcpy(sibling,argv[0],prefix);memcpy(sibling+prefix,"knurl",6);
    }
    struct knurl_check_config config={.duration_seconds=.25,.repetitions=3,.threads=1,
        .memory_bytes=16777216,.seed=20261027,.target=".",.disk_binary=sibling?sibling:"./knurl",.disk_bytes=16777216};
    for(int i=2;i<argc;i++) {
        const char *name=argv[i];
        if(!strcmp(name,"--help")){help();return 0;}
        if(++i>=argc)return error("missing option value");
        const char *value=argv[i];uint64_t n=0;
        if(!strcmp(name,"--duration")) {
            errno=0;char *end;double d=strtod(value,&end);
            if(errno||!*value||*end||!isfinite(d)||d<.001||d>5)return error("duration must be .001..5 seconds");
            config.duration_seconds=d;
        } else if(!strcmp(name,"--target"))config.target=value;
        else if(!strcmp(name,"--work-dir"))config.work_dir=value;
        else if(!strcmp(name,"--output-dir"))config.output_dir=value;
        else if(!strcmp(name,"--disk-binary"))config.disk_binary=value;
        else {
            if(!parse_u64(value,&n))return error("invalid integer option");
            if(!strcmp(name,"--repetitions")){if(n<1||n>30)return error("repetitions must be 1..30");config.repetitions=(unsigned)n;}
            else if(!strcmp(name,"--threads")){if(n<1||n>64)return error("threads must be 1..64");config.threads=(unsigned)n;}
            else if(!strcmp(name,"--memory-bytes")){if(n<4096||n>UINT64_C(1073741824))return error("memory bytes must be 4096..1073741824");config.memory_bytes=(size_t)n;}
            else if(!strcmp(name,"--disk-bytes")){if(n<65536||n>67108864||n%65536)return error("disk bytes must be 65536 multiples up to 67108864");config.disk_bytes=(size_t)n;}
            else if(!strcmp(name,"--seed"))config.seed=n;
            else return error("unknown option");
        }
    }
    if(config.memory_bytes<(size_t)config.threads*8192)return error("memory budget must allow two 4096-byte buffers per thread");
    if((config.work_dir&&!config.output_dir)||(!config.work_dir&&config.output_dir))return error("disk opt-in requires both --work-dir and --output-dir");
    if(!*config.target||!config.disk_binary||!*config.disk_binary)return error("paths must not be empty");
    if(config.work_dir&&(!*config.work_dir||!*config.output_dir))return error("disk paths must not be empty");
    FILE *modules=tmpfile();if(!modules)return error("cannot allocate output staging file");
    const char *names[]={"inventory","cpu","memory","network","disk"};
    Module funcs[]={knurl_check_inventory,knurl_check_cpu,knurl_check_memory,knurl_check_network,knurl_check_disk};
    int complete=1,first=1;
    for(size_t i=0;i<sizeof funcs/sizeof funcs[0];i++) {
        if(strcmp(command,"check")&&strcmp(command,names[i]))continue;
        if(!first)fputc(',',modules);
        first=0;
        if(i==4&&!config.work_dir)fputs("{\"module\":\"disk\",\"status\":\"not_run\",\"reason\":\"requires explicit --work-dir and --output-dir\"}",modules);
        else if(funcs[i](modules,&config))complete=0;
    }
    if(ferror(modules)||fflush(modules)){fclose(modules);return error("cannot stage module output");}
    printf("{\"schema_version\":1,\"suite\":\"knurl-check\",\"command\":");knurl_check_json_string(stdout,command);
    printf(",\"complete\":%s,\"config\":{\"duration_seconds\":%.17g,\"repetitions\":%u,\"threads\":%u,\"memory_bytes\":%zu,\"seed\":%"PRIu64",\"target\":",complete?"true":"false",config.duration_seconds,config.repetitions,config.threads,config.memory_bytes,config.seed);
    knurl_check_json_string(stdout,config.target);fputs("},\"modules\":[",stdout);
    rewind(modules);char buffer[8192];size_t got;
    while((got=fread(buffer,1,sizeof buffer,modules)))if(fwrite(buffer,1,got,stdout)!=got){fclose(modules);return error("output failed");}
    if(ferror(modules)){fclose(modules);return error("staged output read failed");}
    fclose(modules);fputs("]}\n",stdout);
    return complete?0:1;
}
