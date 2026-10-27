/* Diagnostic CPU/page-cache controls, not storage benchmark results.
 * Build: cc -O2 -std=c11 -pthread tools/generator-benchmark.c -lm -o /tmp/knurl-generator
 * Run: /tmp/knurl-generator [total-bytes-per-case]
 * The optional buffered write uses an unlinked /tmp regular file and at most
 * 64 MiB of space. It does not exercise O_DIRECT, fsync, or durable media.
 */
#define main knurl_engine_main
#include "../src/knurl.c"
#undef main

static void legacy_fill(unsigned char *p,size_t n,uint64_t off,uint64_t seed) {
    for(size_t i=0;i<n;i+=8) {
        uint64_t v=mix((off+i)^seed);
        for(unsigned j=0;j<8 && i+j<n;j++)p[i+j]=(unsigned char)(v>>(8*j));
    }
}
static void memcpy_fill(unsigned char *p,size_t n,uint64_t off,uint64_t seed) {
    const uint16_t one=1;
    size_t i=0;
    for(;n-i>=8;i+=8) {
        uint64_t v=mix((off+i)^seed);
        if(*(const unsigned char*)&one)memcpy(p+i,&v,8);
        else for(unsigned j=0;j<8;j++)p[i+j]=(unsigned char)(v>>(8*j));
    }
    if(i<n) {
        uint64_t v=mix((off+i)^seed);
        for(unsigned j=0;i+j<n;j++)p[i+j]=(unsigned char)(v>>(8*j));
    }
}
static void prefilled(unsigned char *p,size_t n,uint64_t off,uint64_t seed) {
    (void)p;(void)n;(void)off;(void)seed;
}
static volatile uint64_t checksum;
static double cpu_now(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t))die("cpu clock");
    return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
int main(int argc,char **argv) {
    uint64_t total=argc==2?number(argv[1]):UINT64_C(1073741824);
    if(argc>2||total<1048576||total%1048576)fail("usage: generator-benchmark [bytes multiple of 1048576]");
    unsigned char *a=NULL,*b=NULL;
    if(posix_memalign((void**)&a,4096,1048576)||posix_memalign((void**)&b,4096,1048576))fail("allocation");
    uint64_t offsets[]={0,4096,UINT64_C(9876543210),UINT64_C(17179869184)};
    for(size_t k=0;k<sizeof offsets/sizeof offsets[0];k++)
        for(size_t n=1;n<8192;n+=17) {
            legacy_fill(a,n,offsets[k],42);memcpy_fill(b,n,offsets[k],42);
            if(memcmp(a,b,n))fail("candidate byte equivalence failed");
            fill(b,n,offsets[k],42);
            if(memcmp(a,b,n))fail("engine byte equivalence failed");
        }
    char path[]="/tmp/knurl-generator-XXXXXX";
    int fd=mkstemp(path);
    if(fd<0)die("mkstemp");
    if(unlink(path))die("unlink scratch file");
    typedef void (*Fill)(unsigned char*,size_t,uint64_t,uint64_t);
    Fill volatile funcs[]={fill,legacy_fill,memcpy_fill,prefilled};
    const char *names[]={"engine","legacy","memcpy-le","prefilled"};
    size_t sizes[]={4096,16384,65536,1048576};
    puts("block,variant,buffered_pwrite,bytes,MiB_per_second,cpu_seconds,wall_seconds");
    for(int z=0;z<4;z++)for(int io=0;io<2;io++)for(int v=0;v<4;v++) {
        if(!io&&v==3)continue; /* An empty-loop rate has no useful interpretation. */
        size_t n=sizes[z];uint64_t count=total/n;
        double start=now(),cpu=cpu_now();
        for(uint64_t j=0;j<count;j++) {
            uint64_t off=(j*n)%UINT64_C(67108864);
            funcs[v](a,n,off,42);
            if(io&&transfer(fd,a,n,off,1))die("buffered pwrite");
            checksum+=a[(j*31)%n];
        }
        cpu=cpu_now()-cpu;
        double elapsed=now()-start;
        printf("%zu,%s,%d,%"PRIu64",%.1f,%.6f,%.6f\n",n,names[v],io,total,
               (double)total/1048576/elapsed,cpu,elapsed);
        fflush(stdout);
    }
    close(fd);free(a);free(b);
    fprintf(stderr,"byte_equivalence=passed checksum=%"PRIu64"\n",checksum);
    return 0;
}
