#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <limits.h>

#define BINS 1024
#define MAX_LIST 64
#define SECTOR 4096
#define MAGIC "KNURL_OWNED_V1\n"
static volatile sig_atomic_t interrupted;
static void on_signal(int s) { (void)s; interrupted=1; }
static void die(const char *s) { fprintf(stderr,"knurl: %s: %s\n",s,strerror(errno)); exit(1); }
static void fail(const char *s) { fprintf(stderr,"knurl: %s\n",s); exit(1); }
static double now(void) { struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) die("clock_gettime"); return (double)t.tv_sec+(double)t.tv_nsec/1e9; }
static uint64_t mix(uint64_t x) { x^=x>>30; x*=UINT64_C(0xbf58476d1ce4e5b9); x^=x>>27; x*=UINT64_C(0x94d049bb133111eb); return x^(x>>31); }
static uint64_t rng(uint64_t *s) { *s+=UINT64_C(0x9e3779b97f4a7c15); return mix(*s); }
static uint64_t bounded(uint64_t *s,uint64_t n) { uint64_t x,threshold=(uint64_t)(-n)%n; do{x=rng(s);}while(x<threshold); return x%n; }
/* Portable byte order: each 8-byte word is the little-endian hash of its absolute offset. */
static void fill(unsigned char *p, size_t n, uint64_t off, uint64_t seed) {
    const uint16_t endian_probe = 1;
    const int little_endian = *(const unsigned char *)&endian_probe;
    size_t i = 0;
    for (; n - i >= 8; i += 8) {
        uint64_t v = mix((off + i) ^ seed);
        if (little_endian) {
            /* memcpy permits unaligned destinations and optimizes to a word store. */
            memcpy(p + i, &v, sizeof v);
        } else {
            for (unsigned j = 0; j < 8; j++) p[i + j] = (unsigned char)(v >> (8 * j));
        }
    }
    if (i < n) {
        uint64_t v = mix((off + i) ^ seed);
        for (unsigned j = 0; i + j < n; j++) p[i + j] = (unsigned char)(v >> (8 * j));
    }
}
static void path_join(char *p,size_t n,const char *a,const char *b) { if(snprintf(p,n,"%s/%s",a,b)<0 || strlen(a)+strlen(b)+2>n)fail("path too long"); }
static int regular_open(const char *p,int flags,mode_t mode) { int fd=open(p,flags|O_NOFOLLOW|O_CLOEXEC,mode); if(fd<0)return -1; struct stat st; if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1){close(fd);errno=EINVAL;return -1;}return fd; }
static void directory(const char *p) { struct stat st; if(mkdir(p,0700)&&errno!=EEXIST)die(p); if(lstat(p,&st)||!S_ISDIR(st.st_mode))fail("directory must be a real directory, not a symlink"); }
static int transfer(int fd,void *buf,size_t n,uint64_t offset,int write_io) { size_t done=0; while(done<n) { if(interrupted){errno=EINTR;return -1;} ssize_t z=write_io?pwrite(fd,(char*)buf+done,n-done,(off_t)(offset+done)):pread(fd,(char*)buf+done,n-done,(off_t)(offset+done)); if(z<0){if(errno==EINTR)continue;return -1;} if(z==0){errno=EIO;return -1;} done+=(size_t)z; }return 0; }
static void sync_dir(const char *path) { int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)die("open directory");if(fsync(fd))die("fsync directory");close(fd); }
static uint64_t hash_bytes(const unsigned char *p,size_t n){uint64_t h=UINT64_C(1469598103934665603);for(size_t i=0;i<n;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;}
static char *read_file(const char *p,size_t *len) { int fd=regular_open(p,O_RDONLY,0); if(fd<0)return NULL;struct stat st;if(fstat(fd,&st)||st.st_size<0||st.st_size>1048576){close(fd);errno=EINVAL;return NULL;}size_t n=(size_t)st.st_size;char *s=malloc(n+1);if(!s)die("malloc");size_t d=0;while(d<n){ssize_t z=read(fd,s+d,n-d);if(z<=0){free(s);close(fd);return NULL;}d+=(size_t)z;}s[n]=0;close(fd);if(len)*len=n;return s; }
static void atomic_text(const char *dir,const char *name,const char *text) { char dst[PATH_MAX],tmp[PATH_MAX];path_join(dst,sizeof dst,dir,name);if(snprintf(tmp,sizeof tmp,"%s/.tmp-XXXXXX",dir)>=(int)sizeof tmp)fail("path too long");int fd=mkstemp(tmp);if(fd<0)die("create temporary output");size_t n=strlen(text),d=0;while(d<n){ssize_t z=write(fd,text+d,n-d);if(z<0&&errno==EINTR)continue;if(z<=0)die("write output");d+=(size_t)z;}if(fsync(fd))die("fsync output");if(close(fd))die("close output");if(rename(tmp,dst))die("rename output");sync_dir(dir); }

typedef struct {uint64_t sizes[MAX_LIST],blocks[MAX_LIST],conc[MAX_LIST];int ns,nb,nc,np,reps;int patterns[5];double duration,warmup;uint64_t seed;int direct,prepare,verify;const char *work,*output,*mode;} Options;
static const char *patterns[]={"seq-read","seq-write","rand-read","rand-write","rand-mix70read30write"};
static uint64_t number(const char *s){char *end;errno=0;if(!*s||*s=='-')fail("invalid unsigned number");unsigned long long v=strtoull(s,&end,10);if(errno||*end)fail("invalid unsigned number");return (uint64_t)v;}
static int list(const char *s,uint64_t *out){char *dup=strdup(s),*save=NULL,*p;int n=0;if(!dup)die("strdup");for(p=strtok_r(dup,",",&save);p;p=strtok_r(NULL,",",&save)){if(n==MAX_LIST)fail("list too long");out[n++]=number(p);}free(dup);if(!n)fail("empty list");for(int i=0;i<n;i++)for(int j=0;j<i;j++)if(out[i]==out[j])fail("duplicate list entry");return n;}
static void help(void){puts("Knurl 1: dedicated-file portable synchronous I/O benchmark\nUsage: knurl --work-dir DIR --output-dir DIR [--mode LABEL] [options]\n  --mode LABEL                descriptive experiment label, default baseline\n  --file-sizes BYTES,...       default 268435456,4294967296,17179869184\n  --block-sizes BYTES,...      default 4096,16384,65536,1048576\n  --concurrency N,...          default 1,8,32 (synchronous worker threads)\n  --patterns NAME,...          seq-read,seq-write,rand-read,rand-write,rand-mix70read30write\n  --repetitions N              default 10\n  --duration SECONDS           measured window, default 5\n  --warmup SECONDS             default 1\n  --seed N                    default 20261027\n  --buffered                  opt out of strict direct/cache-bypass I/O\n  --prepare-only              initialize files and exit\n  --verify                    verify entire files and exit (never repairs data)\n  --help                      show help\nLabels use 1-64 ASCII letters/digits/underscore/dot/hyphen, starting with a\nletter or digit. Labels describe experiments; they do not configure hardware.\nDefault I/O: Linux O_DIRECT, macOS F_NOCACHE; unsupported systems fail.\nExisting unowned files/directories and symlinks are rejected. File preparation\nuses deterministic offset-based bytes. Every trial checks 16 sectors before/after;\n--verify checks every byte. Writes preserve deterministic content.\nOutput includes per-direction fixed log-linear nanosecond histograms (16 bins\nper power of two), quantized percentile upper bounds, and actual elapsed time.\nWrite completion measures pwrite, not durable-media latency; fsync is outside\nthe measured window. Resume requires matching manifest and committed checksums.");}
static Options options(int argc, char **argv) {
    Options o = {0};
    o.sizes[0] = 268435456;
    o.sizes[1] = UINT64_C(4294967296);
    o.sizes[2] = UINT64_C(17179869184);
    o.ns = 3;
    o.blocks[0] = 4096;
    o.blocks[1] = 16384;
    o.blocks[2] = 65536;
    o.blocks[3] = 1048576;
    o.nb = 4;
    o.conc[0] = 1;
    o.conc[1] = 8;
    o.conc[2] = 32;
    o.nc = 3;
    o.np = 5;
    for (int i = 0; i < 5; i++) o.patterns[i] = i;
    o.reps = 10;
    o.duration = 5;
    o.warmup = 1;
    o.seed = 20261027;
    o.direct = 1;
    o.mode = "baseline";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help")) {
            help();
            exit(0);
        }
        if (!strcmp(a, "--buffered")) {
            o.direct = 0;
            continue;
        }
        if (!strcmp(a, "--prepare-only")) {
            o.prepare = 1;
            continue;
        }
        if (!strcmp(a, "--verify")) {
            o.verify = 1;
            continue;
        }
        if (++i >= argc) fail("option missing value");
        const char *v = argv[i];
        if (!strcmp(a, "--work-dir")) {
            o.work = v;
        } else if (!strcmp(a, "--output-dir")) {
            o.output = v;
        } else if (!strcmp(a, "--mode")) {
            o.mode = v;
        } else if (!strcmp(a, "--seed")) {
            o.seed = number(v);
        } else if (!strcmp(a, "--file-sizes")) {
            o.ns = list(v, o.sizes);
        } else if (!strcmp(a, "--block-sizes")) {
            o.nb = list(v, o.blocks);
        } else if (!strcmp(a, "--concurrency")) {
            o.nc = list(v, o.conc);
        } else if (!strcmp(a, "--repetitions")) {
            uint64_t n = number(v);
            if (n < 1 || n > 100000) fail("repetitions out of range");
            o.reps = (int)n;
        } else if (!strcmp(a, "--duration") || !strcmp(a, "--warmup")) {
            char *end;
            double d = strtod(v, &end);
            if (!*v || *end || !isfinite(d) || d < 0 || d > 86400)
                fail("invalid duration");
            if (!strcmp(a, "--duration")) o.duration = d;
            else o.warmup = d;
        } else if (!strcmp(a, "--patterns")) {
            char *dup = strdup(v), *save = NULL, *p;
            if (!dup) die("strdup");
            o.np = 0;
            for (p = strtok_r(dup, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
                int k;
                for (k = 0; k < 5; k++) {
                    if (!strcmp(p, patterns[k])) break;
                }
                if (k == 5 || o.np == 5) fail("invalid pattern list");
                for (int j = 0; j < o.np; j++) {
                    if (o.patterns[j] == k) fail("duplicate pattern");
                }
                o.patterns[o.np++] = k;
            }
            free(dup);
            if (!o.np) fail("empty pattern list");
        } else {
            fail("unknown option");
        }
    }

    if (!o.work || !o.output) fail("--work-dir and --output-dir required");
    size_t label_len = strlen(o.mode);
    if (label_len < 1 || label_len > 64)
        fail("mode label must contain 1-64 safe ASCII characters");
    for (size_t i = 0; i < label_len; i++) {
        unsigned char c = (unsigned char)o.mode[i];
        int alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '_' && c != '.' && c != '-')))
            fail("mode label must start with a letter/digit and contain only letters, digits, _, ., or -");
    }
    if (o.duration <= 0) fail("duration must be positive");
    for (int i = 0; i < o.ns; i++) {
        if (o.sizes[i] < SECTOR || o.sizes[i] % SECTOR || o.sizes[i] > INT64_MAX)
            fail("file sizes must be positive 4096 multiples <= INT64_MAX");
    }
    for (int i = 0; i < o.nb; i++) {
        if (o.blocks[i] < SECTOR || o.blocks[i] % SECTOR ||
            o.blocks[i] > UINT64_C(67108864))
            fail("blocks must be 4096 multiples <= 64MiB");
        for (int j = 0; j < o.ns; j++) {
            if (o.sizes[j] % o.blocks[i])
                fail("file sizes must be divisible by every block size");
        }
    }
    for (int i = 0; i < o.nc; i++) {
        if (!o.conc[i] || o.conc[i] > 1024) fail("concurrency must be 1..1024");
    }
    return o;
}

static int ownership(const Options *o){directory(o->work);char p[PATH_MAX];path_join(p,sizeof p,o->work,".knurl-owner");int fd=regular_open(p,O_RDWR,0600);if(fd<0&&errno==ENOENT){DIR *d=opendir(o->work);if(!d)die("opendir");struct dirent *e;while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))fail("unowned work directory must be empty");closedir(d);fd=regular_open(p,O_RDWR|O_CREAT|O_EXCL,0600);if(fd<0)die("create owner");if(write(fd,MAGIC,strlen(MAGIC))!=(ssize_t)strlen(MAGIC)||fsync(fd))die("write owner");sync_dir(o->work);}if(fd<0)die("open owner");char b[64]={0};if(pread(fd,b,sizeof b-1,0)!=(ssize_t)strlen(MAGIC)||strcmp(b,MAGIC))fail("invalid work directory ownership marker");struct flock l={0};l.l_type=F_WRLCK;l.l_whence=SEEK_SET;if(fcntl(fd,F_SETLK,&l))fail("work directory already in use");return fd;}
static void file_path(char *p,size_t n,const Options *o,uint64_t size){char name[128];snprintf(name,sizeof name,"knurl-%"PRIu64".dat",size);path_join(p,n,o->work,name);}
static void verify_fd(int fd,uint64_t size,uint64_t seed,int full){unsigned char *got,*want;if(posix_memalign((void**)&got,SECTOR,SECTOR)||posix_memalign((void**)&want,SECTOR,SECTOR))fail("aligned allocation failed");uint64_t count=full?size/SECTOR:16;for(uint64_t i=0;i<count;i++){uint64_t off=full?i*SECTOR:((i*(size/SECTOR-1))/15)*SECTOR;fill(want,SECTOR,off,seed);if(transfer(fd,got,SECTOR,off,0))die("integrity read");if(memcmp(got,want,SECTOR))fail("integrity mismatch; file left untouched");}free(got);free(want);}
static void prepare_file(const Options *o,uint64_t size){char p[PATH_MAX],meta[PATH_MAX],expected[128];file_path(p,sizeof p,o,size);if(snprintf(meta,sizeof meta,"%s.meta",p)>=(int)sizeof meta)fail("path too long");snprintf(expected,sizeof expected,"%sseed=%"PRIu64"\nsize=%"PRIu64"\n",MAGIC,o->seed,size);size_t n;char *existing=read_file(meta,&n);int fd=regular_open(p,O_RDWR,0);if(existing){if(strcmp(existing,expected)||fd<0)fail("file metadata mismatch or missing benchmark file");free(existing);struct stat st;if(fstat(fd,&st)||(uint64_t)st.st_size!=size)fail("benchmark file size mismatch");verify_fd(fd,size,o->seed,o->verify);close(fd);return;}if(fd>=0){close(fd);fail("refusing existing file without ownership metadata");}if(errno!=ENOENT)die("open benchmark file");if(o->verify)fail("cannot verify missing benchmark file");fd=regular_open(p,O_RDWR|O_CREAT|O_EXCL,0600);if(fd<0)die("create benchmark file");fprintf(stderr,"Preparing %"PRIu64" bytes\n",size);size_t chunk=1048576;unsigned char *buf=malloc(chunk);if(!buf)die("malloc");for(uint64_t off=0;off<size;off+=chunk){size_t len=size-off<chunk?(size_t)(size-off):chunk;fill(buf,len,off,o->seed);if(transfer(fd,buf,len,off,1))die("prepare write");}free(buf);if(fsync(fd))die("prepare fsync");verify_fd(fd,size,o->seed,0);close(fd);/* Commit metadata only after durable complete initialization. */char name[128];snprintf(name,sizeof name,"knurl-%"PRIu64".dat.meta",size);atomic_text(o->work,name,expected);}
static int io_open(const Options *o,uint64_t size){char p[PATH_MAX];file_path(p,sizeof p,o,size);int flags=O_RDWR;
#ifdef __linux__
if(o->direct)flags|=O_DIRECT;
#elif !defined(__APPLE__)
if(o->direct)fail("direct I/O unavailable on this platform; explicitly use --buffered");
#endif
int fd=regular_open(p,flags,0);if(fd<0)die("open benchmark I/O file (direct I/O is strict)");
#ifdef __APPLE__
if(o->direct&&fcntl(fd,F_NOCACHE,1))die("F_NOCACHE (strict cache bypass)");
#endif
return fd;}

typedef struct {uint64_t bytes,ops,sum,min,max,hist[BINS];} Stats;
typedef struct {uint64_t size,block;unsigned workers;int pattern,rep;size_t id;} Trial;
typedef struct Run Run;
typedef struct {Run *run;unsigned index;Stats read,write;int err;double finish;uint64_t random;} Worker;
struct Run {const Options *o;Trial t;int fd;pthread_mutex_t mutex;pthread_cond_t cond;unsigned ready;int go;atomic_int abort;double warm_end,start,end;};
static unsigned bin_index(uint64_t ns){if(!ns)return 0;unsigned e=0;uint64_t v=ns;while(v>>=1)e++;uint64_t base=UINT64_C(1)<<e;unsigned sub=(unsigned)(e>=4 ? (ns-base)>>(e-4) : (ns-base)<<(4-e));unsigned b=e*16+sub;return b<BINS?b:BINS-1;}
static uint64_t bin_upper(unsigned b){unsigned e=b/16,s=b%16;long double x=ldexpl(1.0L,(int)e)*(1.0L+(long double)(s+1)/16.0L);if(x>UINT64_MAX)return UINT64_MAX;return (uint64_t)ceill(x)-1;}
static uint64_t percentile(const Stats *s,double q){if(!s->ops)return 0;uint64_t rank=(uint64_t)ceill((long double)s->ops*q),c=0;for(unsigned i=0;i<BINS;i++){c+=s->hist[i];if(c>=rank)return bin_upper(i);}return UINT64_MAX;}
static void add(Stats *a,const Stats *b){a->bytes+=b->bytes;a->ops+=b->ops;a->sum+=b->sum;if(b->ops&&(a->ops==b->ops||b->min<a->min))a->min=b->min;if(b->max>a->max)a->max=b->max;for(int i=0;i<BINS;i++)a->hist[i]+=b->hist[i];}
static int one_io(Worker *w,unsigned char *buf,uint64_t *seq,int measured){Run *r=w->run;uint64_t blocks=r->t.size/r->t.block,idx;int write_io=r->t.pattern==1||r->t.pattern==3;if(r->t.pattern==4)write_io=bounded(&w->random,10)>=7;if(r->t.pattern<2){idx=*seq;*seq=(*seq+r->t.workers)%blocks;}else idx=bounded(&w->random,blocks);uint64_t off=idx*r->t.block;if(write_io)fill(buf,(size_t)r->t.block,off,r->o->seed);double start=now();if(measured&&start>=r->end)return 1;if(transfer(r->fd,buf,(size_t)r->t.block,off,write_io)){w->err=errno;atomic_store(&r->abort,1);return -1;}double end=now();if(measured){Stats *s=write_io?&w->write:&w->read;uint64_t ns=(uint64_t)((end-start)*1e9+0.5);s->bytes+=r->t.block;s->ops++;s->sum+=ns;if(s->ops==1||ns<s->min)s->min=ns;if(ns>s->max)s->max=ns;s->hist[bin_index(ns)]++;w->finish=end;}return 0;}
static void *worker_main(void *arg){Worker *w=arg;Run *r=w->run;unsigned char *buf=NULL;int e=posix_memalign((void**)&buf,SECTOR,(size_t)r->t.block);if(e){w->err=e;atomic_store(&r->abort,1);}uint64_t seq=w->index%(r->t.size/r->t.block);w->random=mix(r->o->seed^((uint64_t)r->t.id<<32)^w->index);while(!e&&!interrupted&&!atomic_load(&r->abort)&&now()<r->warm_end)if(one_io(w,buf,&seq,0))break;pthread_mutex_lock(&r->mutex);r->ready++;pthread_cond_broadcast(&r->cond);while(!r->go)pthread_cond_wait(&r->cond,&r->mutex);pthread_mutex_unlock(&r->mutex);seq=w->index%(r->t.size/r->t.block);w->random=mix(r->o->seed^((uint64_t)r->t.id<<32)^w->index);while(!e&&!interrupted&&!atomic_load(&r->abort)&&now()<r->end)if(one_io(w,buf,&seq,1))break;if(!w->finish)w->finish=now();free(buf);return NULL;}
static char *config_text(const Options *o,const Trial *t){char *s=NULL;size_t n=0;FILE *f=open_memstream(&s,&n);if(!f)die("open_memstream");fprintf(f,"{\"mode\":\"%s\",\"file_size\":%"PRIu64",\"block_size\":%"PRIu64",\"concurrency\":%u,\"pattern\":\"%s\",\"repetition\":%d,\"seed\":%"PRIu64",\"duration_seconds\":%.17g,\"warmup_seconds\":%.17g,\"direct\":%s}",o->mode,t->size,t->block,t->workers,patterns[t->pattern],t->rep,o->seed,o->duration,o->warmup,o->direct?"true":"false");if(fclose(f))die("close memory stream");return s;}
static char *manifest_text(const Options *o,size_t total){char *s=NULL;size_t n=0;FILE *f=open_memstream(&s,&n);if(!f)die("open_memstream");fprintf(f,"{\n\"schema_version\":1,\"engine_version\":\"1.0\",\"mode\":\"%s\",\"seed\":%"PRIu64",\"duration_seconds\":%.17g,\"warmup_seconds\":%.17g,\"direct\":%s,\"repetitions\":%d,\"total_trials\":%zu,\"concurrency_kind\":\"synchronous_worker_threads\",\"order\":\"balanced_repetition_blocks_fisher_yates_splitmix64_v1\",\"write_buffer_generation\":\"seeded_offset_hash_each_write_outside_latency_timer_inside_measured_window\",\"histogram\":{\"unit\":\"ns\",\"bins\":1024,\"sub_bins_per_octave\":16,\"percentiles\":\"bin_upper_bound\"},\"file_sizes\":[",o->mode,o->seed,o->duration,o->warmup,o->direct?"true":"false",o->reps,total);for(int i=0;i<o->ns;i++)fprintf(f,"%s%"PRIu64,i?",":"",o->sizes[i]);fputs("],\"block_sizes\":[",f);for(int i=0;i<o->nb;i++)fprintf(f,"%s%"PRIu64,i?",":"",o->blocks[i]);fputs("],\"concurrency\":[",f);for(int i=0;i<o->nc;i++)fprintf(f,"%s%"PRIu64,i?",":"",o->conc[i]);fputs("],\"patterns\":[",f);for(int i=0;i<o->np;i++)fprintf(f,"%s\"%s\"",i?",":"",patterns[o->patterns[i]]);fputs("]\n}\n",f);if(fclose(f))die("memory stream");return s;}
static void progress(const Options *o,size_t completed,size_t total,size_t current,unsigned errors,const char *status){char b[512];snprintf(b,sizeof b,"{\"schema_version\":1,\"completed\":%zu,\"total\":%zu,\"current_trial_id\":%zu,\"errors\":%u,\"status\":\"%s\"}\n",completed,total,current,errors,status);atomic_text(o->output,"progress.json",b);}
static void stats_json(FILE *f,const Stats *s,double elapsed){fprintf(f,"{\"bytes\":%"PRIu64",\"ops\":%"PRIu64",\"bw_Bps\":%.17g,\"iops\":%.17g,\"latency_ns\":{\"sum\":%"PRIu64",\"min\":%"PRIu64",\"max\":%"PRIu64",\"mean\":%.17g,\"p50\":%"PRIu64",\"p95\":%"PRIu64",\"p99\":%"PRIu64",\"p999\":%"PRIu64",\"histogram\":[",s->bytes,s->ops,s->bytes/elapsed,s->ops/elapsed,s->sum,s->min,s->max,s->ops?(double)s->sum/s->ops:0,percentile(s,.5),percentile(s,.95),percentile(s,.99),percentile(s,.999));for(int i=0;i<BINS;i++)fprintf(f,"%s%"PRIu64,i?",":"",s->hist[i]);fputs("]}}",f);}
static void trial_name(char *p,size_t n,size_t id,const char *suffix){snprintf(p,n,"%06zu.%s",id,suffix);}
static int resume_trial(const char *dir,const Options *o,const Trial *t){char name[128],p[PATH_MAX],okpath[PATH_MAX];trial_name(name,sizeof name,t->id,"json");path_join(p,sizeof p,dir,name);trial_name(name,sizeof name,t->id,"ok");path_join(okpath,sizeof okpath,dir,name);size_t n=0;char *s=read_file(p,&n),*ok=read_file(okpath,NULL);if(!s||!ok){free(s);free(ok);return 0;}char digest[64];snprintf(digest,sizeof digest,"%016"PRIx64"\n",hash_bytes((unsigned char*)s,n));char *cfg=config_text(o,t);int valid=!strcmp(ok,digest)&&strstr(s,cfg)&&strstr(s,"\"complete\":true,\"verified\":true")&&n>=3&&!strcmp(s+n-3,"\n}\n");free(cfg);free(s);free(ok);return valid;}
static int run_trial(const Options *o,const Trial *t,const char *outdir){Run r={0};r.o=o;r.t=*t;atomic_init(&r.abort,0);r.fd=io_open(o,t->size);verify_fd(r.fd,t->size,o->seed,0);if(pthread_mutex_init(&r.mutex,NULL)||pthread_cond_init(&r.cond,NULL))fail("pthread synchronization initialization failed");Worker *w=calloc(t->workers,sizeof *w);pthread_t *threads=calloc(t->workers,sizeof *threads);if(!w||!threads)die("calloc workers");r.warm_end=now()+o->warmup;unsigned created=0;for(unsigned i=0;i<t->workers;i++){w[i].run=&r;w[i].index=i;w[i].random=mix(o->seed^((uint64_t)t->id<<32)^i);int e=pthread_create(&threads[i],NULL,worker_main,&w[i]);if(e){atomic_store(&r.abort,1);errno=e;fprintf(stderr,"knurl: pthread_create: %s\n",strerror(e));break;}created++;}pthread_mutex_lock(&r.mutex);while(r.ready<created)pthread_cond_wait(&r.cond,&r.mutex);r.start=now();r.end=r.start+o->duration;r.go=1;pthread_cond_broadcast(&r.cond);pthread_mutex_unlock(&r.mutex);Stats reads={0},writes={0};double finish=r.start;unsigned errors=created==t->workers?0:1;int first_errno=0;for(unsigned i=0;i<created;i++){int e=pthread_join(threads[i],NULL);if(e)fail("pthread_join failed");add(&reads,&w[i].read);add(&writes,&w[i].write);if(w[i].finish>finish)finish=w[i].finish;if(w[i].err){errors++;if(!first_errno)first_errno=w[i].err;}}if(interrupted)errors++;if(!errors&&finish<r.end)finish=r.end;double elapsed=finish-r.start;if(elapsed<=0)elapsed=1e-9;if(!reads.ops&&!writes.ops)errors++;if(!errors&&fsync(r.fd)){first_errno=errno;errors++;}if(!errors)verify_fd(r.fd,t->size,o->seed,0);if(close(r.fd)&&!errors){first_errno=errno;errors++;}pthread_cond_destroy(&r.cond);pthread_mutex_destroy(&r.mutex);free(w);free(threads);
char *json=NULL,*cfg=config_text(o,t);size_t n=0;FILE *f=open_memstream(&json,&n);if(!f)die("open_memstream");fprintf(f,"{\n\"schema_version\":1,\"trial_id\":%zu,\"complete\":%s,\"verified\":%s,\"verification\":\"16_evenly_spaced_4096_byte_sectors_before_and_after\",\"concurrency_kind\":\"synchronous_worker_threads\",\"config\":%s,\"measured_seconds\":%.17g,\"monotonic_start_seconds\":%.17g,\"monotonic_finish_seconds\":%.17g,\"requested_window_seconds\":%.17g,\"measurement_rule\":\"launch_before_deadline_include_terminal_completions\",\"read\":",t->id,errors?"false":"true",errors?"false":"true",cfg,elapsed,r.start,finish,o->duration);stats_json(f,&reads,elapsed);fputs(",\"write\":",f);stats_json(f,&writes,elapsed);fprintf(f,",\"errors\":%u,\"first_errno\":%d\n}\n",errors,first_errno);if(fclose(f))die("close memory stream");free(cfg);char name[128];trial_name(name,sizeof name,t->id,"json");atomic_text(outdir,name,json);if(!errors){char digest[64];snprintf(digest,sizeof digest,"%016"PRIx64"\n",hash_bytes((unsigned char*)json,n));trial_name(name,sizeof name,t->id,"ok");atomic_text(outdir,name,digest);}free(json);if(errors)fprintf(stderr,"knurl: trial failed, errors=%u errno=%d (%s)\n",errors,first_errno,strerror(first_errno));return errors?1:0;}
int main(int argc,char **argv){Options o=options(argc,argv);struct sigaction sa;memset(&sa,0,sizeof sa);sa.sa_handler=on_signal;sigemptyset(&sa.sa_mask);sigaction(SIGINT,&sa,NULL);sigaction(SIGTERM,&sa,NULL);int owner=ownership(&o);for(int i=0;i<o.ns;i++)prepare_file(&o,o.sizes[i]);if(o.prepare||o.verify){close(owner);fprintf(stderr,"%s complete\n",o.verify?"Full integrity verification":"Preparation");return 0;}
size_t cells=(size_t)o.ns*(size_t)o.nb*(size_t)o.nc*(size_t)o.np;if(cells>1000000/(size_t)o.reps)fail("matrix too large");size_t total=cells*(size_t)o.reps;directory(o.output);char trialdir[PATH_MAX],mp[PATH_MAX];path_join(trialdir,sizeof trialdir,o.output,"trials");path_join(mp,sizeof mp,o.output,"manifest.json");char *manifest=manifest_text(&o,total),*existing=read_file(mp,NULL);if(existing){if(strcmp(existing,manifest))fail("manifest differs; use a separate output directory");free(existing);}else{if(errno!=ENOENT)die("read manifest");DIR *d=opendir(o.output);if(!d)die("open output directory");struct dirent *e;while((e=readdir(d)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,".."))fail("new output directory must be empty");closedir(d);atomic_text(o.output,"manifest.json",manifest);}free(manifest);directory(trialdir);
Trial *trials=calloc(total,sizeof *trials);if(!trials)die("allocate matrix");uint64_t state=o.seed;size_t k=0;for(int rep=1;rep<=o.reps;rep++){size_t begin=k;for(int a=0;a<o.ns;a++)for(int b=0;b<o.nb;b++)for(int c=0;c<o.nc;c++)for(int d=0;d<o.np;d++){Trial t={o.sizes[a],o.blocks[b],(unsigned)o.conc[c],o.patterns[d],rep,0};trials[k++]=t;}for(size_t j=cells;j>1;j--){size_t ix=(size_t)bounded(&state,j);Trial tmp=trials[begin+j-1];trials[begin+j-1]=trials[begin+ix];trials[begin+ix]=tmp;}}for(size_t i=0;i<total;i++)trials[i].id=i;
size_t completed=0;for(size_t i=0;i<total;i++)if(resume_trial(trialdir,&o,&trials[i]))completed++;progress(&o,completed,total,0,0,"running");for(size_t i=0;i<total;i++){if(resume_trial(trialdir,&o,&trials[i]))continue;if(interrupted){progress(&o,completed,total,i,1,"interrupted");free(trials);close(owner);return 130;}progress(&o,completed,total,i,0,"running");fprintf(stderr,"Trial %zu/%zu: %s size=%"PRIu64" block=%"PRIu64" workers=%u repetition=%d\n",i+1,total,patterns[trials[i].pattern],trials[i].size,trials[i].block,trials[i].workers,trials[i].rep);if(run_trial(&o,&trials[i],trialdir)){progress(&o,completed,total,i,1,interrupted?"interrupted":"failed");free(trials);close(owner);return interrupted?130:1;}completed++;progress(&o,completed,total,i,0,"running");}progress(&o,completed,total,total,0,"complete");free(trials);close(owner);return 0;}
