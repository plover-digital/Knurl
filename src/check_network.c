#define _POSIX_C_SOURCE 200809L
#include "knurl_check.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

/* Version 1: eight network-order uint32 words, followed by bounded payloads.
 * Words: magic, version, kind (1=send/ack,2=echo), length, iterations,
 * nonce high, nonce low, payload FNV1a32. Every receiver verifies every byte.
 * Throughput ACK is magic/checksum. No execution, file access or discovery. */
#define NET_MAGIC UINT32_C(0x4b4e4e31)
#define MAX_PAYLOAD (1024u*1024u)
#define MAX_TRANSFER (64u*1024u*1024u)
#define MAX_SIZES 12
#define MAX_REPS 30
#define MAX_ITER 65536
#define MAX_RTT 1000
struct net_options {const char *host,*port;size_t sizes[MAX_SIZES];unsigned ns,reps,rtt;size_t bytes;double timeout;uint64_t seed;};
static double nnow(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (double)t.tv_sec+t.tv_nsec/1e9;}
static uint32_t checksum(const unsigned char *p,size_t n){uint32_t v=2166136261u;for(size_t i=0;i<n;i++)v=(v^p[i])*16777619u;return v;}
static void payload(unsigned char *p,size_t n,uint64_t nonce){uint64_t x=nonce;for(size_t i=0;i<n;i++){x^=x<<13;x^=x>>7;x^=x<<17;p[i]=(unsigned char)(x>>24);}}
static void socket_options(int fd){
 int flags=fcntl(fd,F_GETFL,0);if(flags>=0)(void)fcntl(fd,F_SETFL,flags|O_NONBLOCK);
 int no_delay=1;(void)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&no_delay,sizeof no_delay);
#ifdef SO_NOSIGPIPE
 int one=1;(void)setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof one);
#else
 (void)fd;
#endif
}
static int exact(int fd,void *buffer,size_t n,int sending,double deadline){unsigned char *p=buffer;size_t done=0;while(done<n){double remain=deadline-nnow();if(remain<=0){errno=ETIMEDOUT;return -1;}struct pollfd f={fd,sending?POLLOUT:POLLIN,0};int ms=(int)(remain*1000)+1;int z=poll(&f,1,ms);if(z<0&&errno==EINTR)continue;if(z<=0){if(!z)errno=ETIMEDOUT;return -1;}ssize_t got;
#ifdef MSG_NOSIGNAL
 if(sending)got=send(fd,p+done,n-done,MSG_NOSIGNAL);
#else
 if(sending)got=send(fd,p+done,n-done,0);
#endif
 else got=recv(fd,p+done,n-done,0);
 if(got<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
 if(got<=0){if(!got)errno=ECONNRESET;return -1;}done+=(size_t)got;}return 0;}
static int listener(const char *host,const char *port,char *selected,size_t cap){struct addrinfo hints={0},*all=NULL;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;int e=getaddrinfo(host,port,&hints,&all);if(e){fprintf(stderr,"network address: %s\n",gai_strerror(e));return -1;}int fd=-1;for(struct addrinfo *a=all;a;a=a->ai_next){fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);if(fd<0)continue;int one=1;(void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);if(!bind(fd,a->ai_addr,a->ai_addrlen)&&!listen(fd,1))break;close(fd);fd=-1;}freeaddrinfo(all);if(fd>=0){struct sockaddr_storage a;socklen_t len=sizeof a;if(getsockname(fd,(struct sockaddr*)&a,&len)||getnameinfo((struct sockaddr*)&a,len,NULL,0,selected,(socklen_t)cap,NI_NUMERICSERV)){close(fd);return -1;}}return fd;}
static int connect_peer(const char *host,const char *port,double timeout){struct addrinfo hints={0},*all=NULL;hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;int e=getaddrinfo(host,port,&hints,&all);if(e){fprintf(stderr,"network address: %s\n",gai_strerror(e));return -1;}int fd=-1;/* Nonblocking connect bounds each resolved address. */
 for(struct addrinfo *a=all;a;a=a->ai_next){fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);if(fd<0)continue;socket_options(fd);int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);fd=-1;continue;}e=connect(fd,a->ai_addr,a->ai_addrlen);if(e&&errno==EINPROGRESS){struct pollfd p={fd,POLLOUT,0};e=poll(&p,1,(int)(timeout*1000));if(e>0){int error=0;socklen_t n=sizeof error;if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&n)||error)e=-1;else e=0;}else e=-1;}if(!e)break;close(fd);fd=-1;}freeaddrinfo(all);return fd;}
static int serve(int listenfd,double timeout){struct pollfd p={listenfd,POLLIN,0};int ready;do{ready=poll(&p,1,(int)(timeout*1000));}while(ready<0&&errno==EINTR);if(ready<=0)return -1;int fd=accept(listenfd,NULL,NULL);if(fd<0)return -1;socket_options(fd);unsigned char *got=malloc(MAX_PAYLOAD),*expected=malloc(MAX_PAYLOAD);if(!got||!expected){free(got);free(expected);close(fd);return -1;}int result=-1;double session_end=nnow()+120;for(unsigned trial=0;trial<MAX_SIZES*MAX_REPS*2+1;trial++){uint32_t h[8];double deadline=fmin(nnow()+timeout,session_end);if(exact(fd,h,sizeof h,0,deadline))break;for(unsigned i=0;i<8;i++)h[i]=ntohl(h[i]);if(h[0]!=NET_MAGIC||h[1]!=1)break;if(h[2]==0){uint32_t ack=htonl(NET_MAGIC);if(!exact(fd,&ack,sizeof ack,1,deadline))result=0;break;}size_t len=h[3];unsigned iterations=h[4];if((h[2]!=1&&h[2]!=2)||!len||len>MAX_PAYLOAD||!iterations||iterations>MAX_ITER||(uint64_t)len*iterations>MAX_TRANSFER)break;uint64_t nonce=((uint64_t)h[5]<<32)|h[6];payload(expected,len,nonce);if(checksum(expected,len)!=h[7])break;int bad=0;for(unsigned j=0;j<iterations;j++){if(exact(fd,got,len,0,deadline)||memcmp(got,expected,len)){bad=1;break;}if(h[2]==2&&exact(fd,got,len,1,deadline)){bad=1;break;}}if(bad)break;if(h[2]==1){uint32_t ack[2]={htonl(NET_MAGIC),htonl(h[7])};if(exact(fd,ack,sizeof ack,1,deadline))break;}}free(got);free(expected);close(fd);return result;}
static int header(int fd,unsigned kind,size_t len,unsigned iterations,uint64_t nonce,uint32_t check,double deadline){uint32_t h[8]={NET_MAGIC,1,kind,(uint32_t)len,iterations,(uint32_t)(nonce>>32),(uint32_t)nonce,check};for(unsigned i=0;i<8;i++)h[i]=htonl(h[i]);return exact(fd,h,sizeof h,1,deadline);}
static int run_client_raw(FILE *out,const struct net_options *o,int loopback){int fd=connect_peer(o->host,o->port,o->timeout);if(fd<0)return -1;unsigned char *buf=malloc(MAX_PAYLOAD),*echo=malloc(MAX_PAYLOAD);double bw[MAX_SIZES*MAX_REPS],rtt_means[MAX_SIZES*MAX_REPS];if(!buf||!echo){free(buf);free(echo);close(fd);return -1;}unsigned count=0;int result=-1;fprintf(out,"{\"module\":\"network\",\"protocol_version\":1,\"scope\":\"%s\",\"physical_link_test\":%s,\"verified\":true,\"throughput_method\":\"client_send_verified_server_ack\",\"latency_method\":\"application_pingpong_round_trip\",\"trials\":[",loopback?"loopback_selftest":"explicit_peer","false");for(unsigned s=0;s<o->ns;s++)for(unsigned rep=0;rep<o->reps;rep++){size_t len=o->sizes[s];unsigned iterations=(unsigned)((o->bytes+len-1)/len);if((uint64_t)iterations*len>MAX_TRANSFER||iterations>MAX_ITER)goto done;uint64_t nonce=o->seed+UINT64_C(0x9e3779b97f4a7c15)*(count+1);payload(buf,len,nonce);uint32_t check=checksum(buf,len);double deadline=nnow()+o->timeout;if(header(fd,1,len,iterations,nonce,check,deadline))goto done;double start=nnow();for(unsigned j=0;j<iterations;j++)if(exact(fd,buf,len,1,deadline))goto done;uint32_t ack[2];if(exact(fd,ack,sizeof ack,0,deadline)||ntohl(ack[0])!=NET_MAGIC||ntohl(ack[1])!=check)goto done;double seconds=nnow()-start;if(seconds<=0)goto done;bw[count]=(double)len*iterations/1048576/seconds;deadline=nnow()+o->timeout;if(header(fd,2,len,o->rtt,nonce,check,deadline))goto done;double latency[MAX_RTT],sum=0;for(unsigned j=0;j<o->rtt;j++){start=nnow();if(exact(fd,buf,len,1,deadline)||exact(fd,echo,len,0,deadline))goto done;latency[j]=(nnow()-start)*1e6;if(memcmp(buf,echo,len))goto done;sum+=latency[j];}rtt_means[count]=sum/o->rtt;if(count)fputc(',',out);fprintf(out,"{\"payload_bytes\":%zu,\"repetition\":%u,\"sent_bytes\":%zu,\"seconds\":%.9g,\"throughput_mib_s\":%.9g,\"rtt_us\":[",len,rep+1,len*iterations,seconds,bw[count]);for(unsigned j=0;j<o->rtt;j++)fprintf(out,"%s%.9g",j?",":"",latency[j]);fputs("]}",out);count++;}if(header(fd,0,0,0,0,0,nnow()+o->timeout))goto done;uint32_t final_ack;if(exact(fd,&final_ack,sizeof final_ack,0,nnow()+o->timeout)||ntohl(final_ack)!=NET_MAGIC)goto done;fputs("],\"summary_by_payload\":[",out);for(unsigned s=0;s<o->ns;s++){if(s)fputc(',',out);fprintf(out,"{\"payload_bytes\":%zu,\"throughput\":",o->sizes[s]);knurl_check_summary(out,bw+s*o->reps,o->reps,"MiB/s");fputs(",\"trial_mean_rtt\":",out);knurl_check_summary(out,rtt_means+s*o->reps,o->reps,"us");fputc('}',out);}fputs("]}",out);result=0;
done:free(buf);free(echo);close(fd);return result;}
static int run_client(FILE *out,const struct net_options *o,int loopback){char *json=NULL;size_t length=0;FILE *temp=open_memstream(&json,&length);if(!temp){fputs("{\"module\":\"network\",\"verified\":false,\"error\":\"allocation failed\"}",out);return -1;}int rc=run_client_raw(temp,o,loopback);if(fclose(temp))rc=-1;if(!rc)fwrite(json,1,length,out);else fputs("{\"module\":\"network\",\"verified\":false,\"error\":\"transport timeout, disconnect, bounds or payload verification failure\"}",out);free(json);return rc;}
static struct net_options defaults(void){struct net_options o={0};o.host="127.0.0.1";o.port="46039";o.sizes[0]=1024;o.sizes[1]=65536;o.sizes[2]=1048576;o.ns=3;o.reps=3;o.rtt=20;o.bytes=1024*1024;o.timeout=5;o.seed=20261027;return o;}
int knurl_check_network(FILE *out,const struct knurl_check_config *config){
 struct net_options o=defaults();
 if(!config||!config->repetitions||config->repetitions>MAX_REPS){fputs("{\"module\":\"network\",\"verified\":false,\"error\":\"invalid repetitions\"}",out);return -1;}
 o.reps=config->repetitions;o.seed=config->seed;
 char port[32];int fd=listener("127.0.0.1","0",port,sizeof port);
 if(fd<0){fputs("{\"module\":\"network\",\"verified\":false,\"error\":\"loopback listen failed\"}",out);return -1;}
 fflush(NULL);pid_t pid=fork();
 if(pid<0){close(fd);fputs("{\"module\":\"network\",\"verified\":false,\"error\":\"fork failed\"}",out);return -1;}
 if(!pid){int rc=serve(fd,o.timeout);close(fd);_exit(rc?1:0);}
 o.port=port;
 /* Publish success only after both the protocol and child status are accepted. */
 char *json=NULL;size_t length=0;FILE *staged=open_memstream(&json,&length);
 int rc=staged?run_client(staged,&o,1):-1;
 if(staged&&fclose(staged))rc=-1;
 close(fd);int status=0;pid_t waited;
 do{waited=waitpid(pid,&status,0);}while(waited<0&&errno==EINTR);
 if(waited!=pid||!WIFEXITED(status)||WEXITSTATUS(status))rc=-1;
 if(!rc){if(fwrite(json,1,length,out)!=length)rc=-1;}
 else fputs("{\"module\":\"network\",\"verified\":false,\"error\":\"loopback transport or receiver process failed\"}",out);
 free(json);return rc;
}
static int parse_number(const char *s,unsigned long long *value){char *end;errno=0;if(!s||!*s||*s=='-')return -1;*value=strtoull(s,&end,10);return errno||*end?-1:0;}
int knurl_check_network_command(int argc,char **argv){if(argc<1)return 2;int server=!strcmp(argv[0],"server"),client=!strcmp(argv[0],"client");if(!server&&!client)return 2;struct net_options o=defaults();int host_given=0;for(int i=1;i<argc;i++){if(!strcmp(argv[i],"--help")){puts("knurl-check server [--bind 127.0.0.1] [--port 46039] [--timeout 5]\nknurl-check client --host HOST [--port 46039] [--sizes 1024,65536,1048576] [--bytes 1048576] [--repetitions 3] [--rtt-iterations 20] [--timeout 5]\nOne bounded session per server; explicit nonloopback bind is opt-in. Network RTT is round-trip, not one-way latency.");return 0;}const char *key=argv[i];if(++i>=argc)return 2;const char *value=argv[i];unsigned long long n;if(!strcmp(key,"--host")&&client){o.host=value;host_given=1;}else if(!strcmp(key,"--bind")&&server)o.host=value;else if(!strcmp(key,"--port")){if(parse_number(value,&n)||n>65535||(!n&&client))return 2;o.port=value;}else if(!strcmp(key,"--sizes")&&client){char *copy=strdup(value),*save=NULL;if(!copy)return 1;o.ns=0;for(char *p=strtok_r(copy,",",&save);p;p=strtok_r(NULL,",",&save)){if(o.ns==MAX_SIZES||parse_number(p,&n)||!n||n>MAX_PAYLOAD){free(copy);return 2;}o.sizes[o.ns++]=(size_t)n;}free(copy);if(!o.ns)return 2;}else{if(parse_number(value,&n))return 2;if(!strcmp(key,"--bytes")&&client&&n&&n<=MAX_TRANSFER)o.bytes=(size_t)n;else if(!strcmp(key,"--repetitions")&&client&&n&&n<=MAX_REPS)o.reps=(unsigned)n;else if(!strcmp(key,"--rtt-iterations")&&client&&n&&n<=MAX_RTT)o.rtt=(unsigned)n;else if(!strcmp(key,"--timeout")&&n&&n<=60)o.timeout=(double)n;else return 2;}}
 if(client&&!host_given){fprintf(stderr,"client requires explicit --host\n");return 2;}if(client){for(unsigned i=0;i<o.ns;i++)if((o.bytes+o.sizes[i]-1)/o.sizes[i]>MAX_ITER||(uint64_t)o.sizes[i]*o.rtt>MAX_TRANSFER)return 2;int rc=run_client(stdout,&o,0);fputc('\n',stdout);if(rc)fprintf(stderr,"network client failed; JSON reports verified=false\n");return rc?1:0;}char selected[32];int fd=listener(o.host,o.port,selected,sizeof selected);if(fd<0){perror("network listen");return 1;}fprintf(stderr,"Network server listening on explicitly selected bind %s port %s; one session\n",o.host,selected);int rc=serve(fd,o.timeout);close(fd);printf("{\"module\":\"network_server\",\"protocol_version\":1,\"verified\":%s}\n",rc?"false":"true");return rc?1:0;}
