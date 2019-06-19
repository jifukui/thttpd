/* libhttpd.h - defines for libhttpd
**
** Copyright � 1995,1998,1999,2000,2001 by Jef Poskanzer <jef@mail.acme.com>.
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
** ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
** FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
** DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
** OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
** HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
** LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
** OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
** SUCH DAMAGE.
*/

#ifndef _LIBHTTPD_H_
#define _LIBHTTPD_H_

#include <sys/types.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#if defined(AF_INET6) && defined(IN6_IS_ADDR_V4MAPPED)
//#define USE_IPV6 /**禁止使用IPV6*/
#endif


/* A few convenient defines. */

#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#define NEW(t,n) ((t*) malloc( sizeof(t) * (n) ))
#define RENEW(o,t,n) ((t*) realloc( (void*) o, sizeof(t) * (n) ))

/* Do overlapping strcpy safely, by using memmove. */
#define ol_strcpy(dst,src) memmove(dst,src,strlen(src)+1)


/* The httpd structs. */

/* A multi-family sockaddr. */
/**http地址联合体*/
typedef union {
    struct sockaddr sa;             //IP地址类型
    struct sockaddr_in sa_in;       //IP地址
#ifdef USE_IPV6
    struct sockaddr_in6 sa_in6;
    struct sockaddr_storage sa_stor;
#endif /* USE_IPV6 */
} httpd_sockaddr;

/* A server. */
/**服务器结构对象*/
typedef struct {
    char* binding_hostname;
    char* server_hostname;
    unsigned short port;
    char* cgi_pattern;
    int cgi_limit;
    int cgi_count;
    char* charset;
    char* p3p;
    int max_age;
    char* cwd;
    int listen4_fd;
    int listen6_fd;
    int no_log;
    FILE* logfp;
    int no_symlink_check;           //不检查符号连接标记，对于设置切换到根目录此值为1对于设置不切换到根目录此值为0
    int vhost;
    int global_passwd;
    char* url_pattern;
    char* local_pattern;
    int no_empty_referrers;
} httpd_server;

/* A connection. */
/**http连接结构对象*/
typedef struct {
    int initialized;                //http服务器是否初始化状态标志位
    httpd_server* hs;               //http服务器结构对象
    httpd_sockaddr client_addr;     //http客户端地址信息
    char* read_buf;                 //读取的数据存放的位置
    size_t read_size;               //读取数据的数据空间大小
    size_t read_idx;                //当前读取数据的索引值
    size_t checked_idx;             //当前处理的索引值
    int checked_state;              //处理请求行的状态标志
    int method;                     //请求方式
    int status;                     //http处理状态码
    off_t bytes_to_send;            //需要发送数据的数量
    off_t bytes_sent;               //已经发送数据的数量
    char* encodedurl;               //原始的编码的url
    char* decodedurl;               //解码的URL
    char* protocol;                 //协议字符串
    char* origfilename;             //请求的url经过转换为ASCII字符后的除第一个字节的内容
    char* expnfilename;             //如果未经过处理此值基本上等于origfilename的值
    char* encodings;                //
    char* pathinfo;                 //
    char* query;                    //
    char* referrer;                 //
    char* useragent;                //
    char* accept;                   //
    char* accepte;                  //
    char* acceptl;                  //
    char* cookie;                   //
    char* contenttype;              //
    char* reqhost;                  //
    char* hdrhost;                  //
    char* hostdir;                  //
    char* authorization;            //
    char* remoteuser;               //
    char* response;                 //
    size_t maxdecodedurl;           //最大的url转换为ASCII的字符长度
    size_t maxorigfilename;         //
    size_t maxexpnfilename;         //
    size_t maxencodings;            //
	size_t maxpathinfo;             //
    size_t maxquery;                //
    size_t maxaccept;               //
    size_t maxaccepte;              //
    size_t maxreqhost;              //
    size_t maxhostdir;              //
	size_t maxremoteuser;           //
    size_t maxresponse;             //
#ifdef TILDE_MAP_2
    char* altdir;                   //
    size_t maxaltdir;               //
#endif /* TILDE_MAP_2 */
    size_t responselen;             //
    time_t if_modified_since;       //
    time_t range_if;                //
    size_t contentlength;           //
    char* type;                     /* not malloc()ed */             
    char* hostname;	                /* not malloc()ed */        
    int mime_flag;                  //对于HTTP1.1需要处理请求首部的标志
    int one_one;	                /* 是否是HTTP1.1之后的 协议标志位 */
    int got_range;                  //用户请求是否定义Range
    int tildemapped;	            /* this connection got tilde-mapped */
    off_t first_byte_index;         //发送数据的第一个字节的位置
    off_t last_byte_index;          //发送数据的最后一个字节的位置
    int keep_alive;                 //是否保持连接状态标记
    int should_linger;              //延迟关闭状态标记
    struct stat sb;                 //文件状态值
    int conn_fd;                    //连接的文件描述符
    char* file_address;             //映射的文件的内存地址
} httpd_conn;

/* Methods. */
#define METHOD_UNKNOWN 0
#define METHOD_GET 1
#define METHOD_HEAD 2
#define METHOD_POST 3
#define METHOD_PUT 4
#define METHOD_DELETE 5
#define METHOD_TRACE 6

/* States for checked_state. */
#define CHST_FIRSTWORD 0
#define CHST_FIRSTWS 1
#define CHST_SECONDWORD 2
#define CHST_SECONDWS 3
#define CHST_THIRDWORD 4
#define CHST_THIRDWS 5
#define CHST_LINE 6
#define CHST_LF 7
#define CHST_CR 8
#define CHST_CRLF 9
#define CHST_CRLFCR 10
#define CHST_BOGUS 11


/* Initializes.  Does the socket(), bind(), and listen().   Returns an
** httpd_server* which includes a socket fd that you can select() on.
** Return (httpd_server*) 0 on error.
*/
httpd_server* httpd_initialize(
    char* hostname, httpd_sockaddr* sa4P, httpd_sockaddr* sa6P,
    unsigned short port, char* cgi_pattern, int cgi_limit, char* charset,
    char* p3p, int max_age, char* cwd, int no_log, FILE* logfp,
    int no_symlink_check, int vhost, int global_passwd, char* url_pattern,
    char* local_pattern, int no_empty_referrers );

/* Change the log file. */
void httpd_set_logfp( httpd_server* hs, FILE* logfp );

/* Call to unlisten/close socket(s) listening for new connections. */
void httpd_unlisten( httpd_server* hs );

/* Call to shut down. */
void httpd_terminate( httpd_server* hs );


/* When a listen fd is ready to read, call this.  It does the accept() and
** returns an httpd_conn* which includes the fd to read the request from and
** write the response to.  Returns an indication of whether the accept()
** failed, succeeded, or if there were no more connections to accept.
**
** In order to minimize malloc()s, the caller passes in the httpd_conn.
** The caller is also responsible for setting initialized to zero before the
** first call using each different httpd_conn.
*/
int httpd_get_conn( httpd_server* hs, int listen_fd, httpd_conn* hc );
#define GC_FAIL 0
#define GC_OK 1
#define GC_NO_MORE 2

/* Checks whether the data in hc->read_buf constitutes a complete request
** yet.  The caller reads data into hc->read_buf[hc->read_idx] and advances
** hc->read_idx.  This routine checks what has been read so far, using
** hc->checked_idx and hc->checked_state to keep track, and returns an
** indication of whether there is no complete request yet, there is a
** complete request, or there won't be a valid request due to a syntax error.
*/
int httpd_got_request( httpd_conn* hc );
#define GR_NO_REQUEST 0
#define GR_GOT_REQUEST 1
#define GR_BAD_REQUEST 2

/* Parses the request in hc->read_buf.  Fills in lots of fields in hc,
** like the URL and the various headers.
**
** Returns -1 on error.
*/
int httpd_parse_request( httpd_conn* hc );

/* Starts sending data back to the client.  In some cases (directories,
** CGI programs), finishes sending by itself - in those cases, hc->file_fd
** is <0.  If there is more data to be sent, then hc->file_fd is a file
** descriptor for the file to send.  If you don't have a current timeval
** handy just pass in 0.
**
** Returns -1 on error.
*/
int httpd_start_request( httpd_conn* hc, struct timeval* nowP );

/* Actually sends any buffered response text. */
void httpd_write_response( httpd_conn* hc );

/* Call this to close down a connection and free the data.  A fine point,
** if you fork() with a connection open you should still call this in the
** parent process - the connection will stay open in the child.
** If you don't have a current timeval handy just pass in 0.
*/
void httpd_close_conn( httpd_conn* hc, struct timeval* nowP );

/* Call this to de-initialize a connection struct and *really* free the
** mallocced strings.
*/
void httpd_destroy_conn( httpd_conn* hc );


/* Send an error message back to the client. */
void httpd_send_err(
    httpd_conn* hc, int status, char* title, char* extraheads, char* form,
    char* arg );

/* Some error messages. */
extern char* httpd_err400title;
extern char* httpd_err400form;
extern char* httpd_err408title;
extern char* httpd_err408form;
extern char* httpd_err503title;
extern char* httpd_err503form;

/* Generate a string representation of a method number. */
char* httpd_method_str( int method );

/* Reallocate a string. */
void httpd_realloc_str( char** strP, size_t* maxsizeP, size_t size );

/* Format a network socket to a string representation. */
char* httpd_ntoa( httpd_sockaddr* saP );

/* Set NDELAY mode on a socket. */
void httpd_set_ndelay( int fd );

/* Clear NDELAY mode on a socket. */
void httpd_clear_ndelay( int fd );

/* Read the requested buffer completely, accounting for interruptions. */
int httpd_read_fully( int fd, void* buf, size_t nbytes );

/* Write the requested buffer completely, accounting for interruptions. */
int httpd_write_fully( int fd, const char* buf, size_t nbytes );

/* Generate debugging statistics syslog message. */
void httpd_logstats( long secs );

#endif /* _LIBHTTPD_H_ */
