#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "knurl_check.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int result_error(FILE *out,const char *phase,int code) {
    fputs("{\"module\":\"disk\",\"status\":\"error\",\"phase\":",out);
    knurl_check_json_string(out,phase);
    fprintf(out,",\"code\":%d}",code);
    return 1;
}
static int child_run(const char *binary,char *const args[],int logfd) {
    pid_t pid=fork();
    if(pid<0)return errno?errno:1;
    if(pid==0) {
        if(dup2(logfd,STDOUT_FILENO)<0||dup2(logfd,STDERR_FILENO)<0)_exit(126);
        close(logfd);
        execvp(binary,args);
        _exit(127);
    }
    int status;
    while(waitpid(pid,&status,0)<0)if(errno!=EINTR)return errno?errno:1;
    return WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);
}
int knurl_check_disk(FILE *out,const struct knurl_check_config *c) {
    if(!c->work_dir||!c->output_dir) {
        fputs("{\"module\":\"disk\",\"status\":\"not_run\",\"reason\":\"Disk writes require explicit --work-dir and --output-dir.\",\"physical_device_health\":\"not_assessed\"}",out);
        return 0;
    }
    if(!c->disk_binary||c->disk_bytes<65536||c->disk_bytes>67108864||c->disk_bytes%65536)
        return result_error(out,"configuration",EINVAL);
    struct stat st;
    if(mkdir(c->output_dir,0700)&&errno!=EEXIST)return result_error(out,"output_directory",errno);
    if(lstat(c->output_dir,&st)||!S_ISDIR(st.st_mode))return result_error(out,"output_directory",EINVAL);
    char run[PATH_MAX],log[PATH_MAX];
    int a=snprintf(run,sizeof run,"%s/disk-run",c->output_dir);
    int b=snprintf(log,sizeof log,"%s/check-disk.log",c->output_dir);
    if(a<0||b<0||(size_t)a>=sizeof run||(size_t)b>=sizeof log)return result_error(out,"paths",ENAMETOOLONG);
    int fd=open(log,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return result_error(out,"new_log_file",errno);
    char bytes[32],duration[32],reps[32],threads[32],seed[32];
    snprintf(bytes,sizeof bytes,"%zu",c->disk_bytes);
    snprintf(duration,sizeof duration,"%.17g",c->duration_seconds);
    snprintf(reps,sizeof reps,"%u",c->repetitions);
    snprintf(threads,sizeof threads,"%u",c->threads);
    snprintf(seed,sizeof seed,"%"PRIu64,c->seed);
    char *args[]={
      (char*)c->disk_binary,"--work-dir",(char*)c->work_dir,
      "--output-dir",run,"--mode","system-checkout","--file-sizes",bytes,
      "--block-sizes","4096,65536","--concurrency",threads,
      "--patterns","seq-read,seq-write,rand-read,rand-write,rand-mix70read30write",
      "--repetitions",reps,"--duration",duration,"--warmup","0.1","--seed",seed,
      NULL,NULL
    };
    const char *phases[]={"prepare","full_verify_before","measure","full_verify_after"};
    const char *options[]={"--prepare-only","--verify",NULL,"--verify"};
    size_t option_index=sizeof args/sizeof args[0]-2;
    for(size_t i=0;i<4;i++) {
        args[option_index]=(char*)options[i];
        dprintf(fd,"\n--- phase=%s ---\n",phases[i]);
        int code=child_run(c->disk_binary,args,fd);
        if(code){close(fd);return result_error(out,phases[i],code);}
    }
    if(close(fd))return result_error(out,"close_log",errno);
    fprintf(out,"{\"module\":\"disk\",\"status\":\"passed\",\"scratch_file_bytes\":%zu,\"expected_and_completed_trials\":%u,\"full_byte_verification_before_and_after\":true,\"raw_results_directory\":",c->disk_bytes,c->repetitions*10U);
    knurl_check_json_string(out,run);
    fputs(",\"log\":",out);knurl_check_json_string(out,log);
    fputs(",\"io_mode\":\"platform_strict_cache_bypass\",\"concurrency_kind\":\"synchronous_worker_threads\",\"physical_device_health\":\"not_assessed\",\"scope\":\"Dedicated scratch-file test only; this does not certify the entire device or its SMART health.\"}",out);
    return 0;
}
