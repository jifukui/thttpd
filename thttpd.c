/* thttpd.c - tiny/turbo/throttling HTTP server
**
** Copyright � 1995,1998,1999,2000,2001,2015 by
** Jef Poskanzer <jef@mail.acme.com>. All rights reserved.
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


#include "config.h"
#include "version.h"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <pwd.h>
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#ifdef TIME_WITH_SYS_TIME
#include <time.h>
#endif
#include <unistd.h>

#include "fdwatch.h"
#include "libhttpd.h"
#include "mmc.h"
#include "timers.h"
#include "match.h"
#define JIFUKUI_SSL
#ifdef JIFUKUI_SSL
#endif
#ifndef SHUT_WR
#define SHUT_WR 1
#endif

#ifndef HAVE_INT64T
typedef long long int64_t;
#endif


static char* argv0;
static int debug;					//debug标志
static unsigned short port;
static char* dir;
static char* data_dir;
static int do_chroot;
static int  no_log;
static int no_symlink_check;
static int do_vhost;
static int do_global_passwd;
static char* cgi_pattern;
static int cgi_limit;
static char* url_pattern;
static int no_empty_referrers;
static char* local_pattern;
static char* logfile;
static char* throttlefile;
static char* hostname;
static char* pidfile;
static char* user;
static char* charset;
static char* p3p;
static int max_age;

/**限制表
 * pattern：文件的正则表达式
 * max_limit：最大流量限制
 * min_limit：最小流量限制
 * rate：速率限制
 * bytes_since_avg:
 * num_sending
*/
typedef struct {
    char* pattern;
    long max_limit;
	long min_limit;
    long rate;
    off_t bytes_since_avg;
    int num_sending;
    } throttletab;
static throttletab* throttles;
static int numthrottles;
static int maxthrottles;

#define THROTTLE_NOLIMIT -1

/**连接结构*/
typedef struct {
    int conn_state;						//连接状态
    int next_free_connect;				//下一个可用的连接
    httpd_conn* hc;						//连接对象指针
    int tnums[MAXTHROTTLENUMS];         /* throttle indexes 限流文件结构体*/
    int numtnums;						/***/
    long max_limit;						/**最大限制*/
	long min_limit;						/**最小限制*/
    time_t started_at;					/**开始时间*/
	time_t active_at;					/**处理时的时间*/
    Timer* wakeup_timer;				/**唤醒时间*/
    Timer* linger_timer;				/**异常时间*/	
    long wouldblock_delay;				/***/
    off_t bytes;						/***/
    off_t end_byte_index;				//最后一个值的位置
    off_t next_byte_index;				//下一个连接的索引值
} connecttab;
static connecttab* connects;
static int num_connects;				/**当前链接表使用的数量*/
static int max_connects;				/**最大链接表数量*/
static int first_free_connect;			/**第一个可以使用的链接表*/
static int httpd_conn_count;			/**http连接的数量*/

/* The connection states. */
#define CNST_FREE 0
#define CNST_READING 1
#define CNST_SENDING 2
#define CNST_PAUSING 3
#define CNST_LINGERING 4

/**初始化全局静态对象http服务器*/
static httpd_server* hs = (httpd_server*) 0;
int terminate = 0;								//服务器暂停标记
time_t start_time;
time_t stats_time;
long stats_connections;
off_t stats_bytes;
int stats_simultaneous;

static volatile int got_hup;			//重新打开日志文件标记
static volatile int got_usr1;
static volatile int  watchdog_flag;		//看门狗标志


/* Forwards. */
static void parse_args( int argc, char** argv );
static void usage( void );
static void read_config( char* filename );
static void value_required( char* name, char* value );
static void no_value_required( char* name, char* value );
static char* e_strdup( char* oldstr );
static void lookup_hostname( httpd_sockaddr* sa4P, size_t sa4_len, int* gotv4P, httpd_sockaddr* sa6P, size_t sa6_len, int* gotv6P );
static void read_throttlefile( char* tf );
static void shut_down( void );
static int handle_newconnect( struct timeval* tvP, int listen_fd );
static void handle_read( connecttab* c, struct timeval* tvP );
static void handle_send( connecttab* c, struct timeval* tvP );
static void handle_linger( connecttab* c, struct timeval* tvP );
static int check_throttles( connecttab* c );
static void clear_throttles( connecttab* c, struct timeval* tvP );
static void update_throttles( ClientData client_data, struct timeval* nowP );
static void finish_connection( connecttab* c, struct timeval* tvP );
static void clear_connection( connecttab* c, struct timeval* tvP );
static void really_clear_connection( connecttab* c, struct timeval* tvP );
static void idle( ClientData client_data, struct timeval* nowP );
static void wakeup_connection( ClientData client_data, struct timeval* nowP );
static void linger_clear_connection( ClientData client_data, struct timeval* nowP );
static void occasional( ClientData client_data, struct timeval* nowP );
#ifdef STATS_TIME
static void show_stats( ClientData client_data, struct timeval* nowP );
#endif /* STATS_TIME */
static void logstats( struct timeval* nowP );
static void thttpd_logstats( long secs );


/* SIGTERM and SIGINT say to exit immediately. */
static void handle_term( int sig )
{
    /* Don't need to set up the handler again, since it's a one-shot. */

    shut_down();
    syslog( LOG_NOTICE, "exiting due to signal %d", sig );
    closelog();
    exit( 1 );
}


/* SIGCHLD - a chile process exitted, so we need to reap the zombie */
static void handle_chld( int sig )
{
    const int oerrno = errno;
    pid_t pid;
    int status;

#ifndef HAVE_SIGSET
    /* Set up handler again. */
    (void) signal( SIGCHLD, handle_chld );
#endif /* ! HAVE_SIGSET */

    /* Reap defunct children until there aren't any more. */
    for (;;)
	{
#ifdef HAVE_WAITPID
		pid = waitpid( (pid_t) -1, &status, WNOHANG );
#else /* HAVE_WAITPID */
		pid = wait3( &status, WNOHANG, (struct rusage*) 0 );
#endif /* HAVE_WAITPID */
		if ( (int) pid == 0 )		/* none left */
	    {
			break;
		}
		if ( (int) pid < 0 )
	    {
	    	if ( errno == EINTR || errno == EAGAIN )
			{
				continue;
			}
	    	/* ECHILD shouldn't happen with the WNOHANG option,
	    	** but with some kernels it does anyway.  Ignore it.
	    	*/
	    	if ( errno != ECHILD )
			{
				syslog( LOG_ERR, "child wait - %m" );
			}
	    	break;
	    }
		/* Decrement the CGI count.  Note that this is not accurate, since
		** each CGI can involve two or even three child processes.
		** Decrementing for each child means that when there is heavy CGI
		** activity, the count will be lower than it should be, and therefore
		** more CGIs will be allowed than should be.
		*/
		if ( hs != (httpd_server*) 0 )
	    {
	    	--hs->cgi_count;
	    	if ( hs->cgi_count < 0 )
			{
				hs->cgi_count = 0;
			}
	    }
	}

    /* Restore previous errno. */
    errno = oerrno;
}


/* SIGHUP says to re-open the log file. */
static void handle_hup( int sig )
{
    const int oerrno = errno;

#ifndef HAVE_SIGSET
    /* Set up handler again. */
    (void) signal( SIGHUP, handle_hup );
#endif /* ! HAVE_SIGSET */

    /* Just set a flag that we got the signal. */
    got_hup = 1;

    /* Restore previous errno. */
    errno = oerrno;
}


/* SIGUSR1 says to exit as soon as all current connections are done. */
static void handle_usr1( int sig )
{
    /* Don't need to set up the handler again, since it's a one-shot. */

    if ( num_connects == 0 )
	{
		/* If there are no active connections we want to exit immediately
		** here.  Not only is it faster, but without any connections the
		** main loop won't wake up until the next new connection.
		*/
		shut_down();
		syslog( LOG_NOTICE, "exiting" );
		closelog();
		exit( 0 );
	}

    /* Otherwise, just set a flag that we got the signal. */
    got_usr1 = 1;

    /* Don't need to restore old errno, since we didn't do any syscalls. */
}


/* SIGUSR2 says to generate the stats syslogs immediately. */
static void handle_usr2( int sig )
{
    const int oerrno = errno;

#ifndef HAVE_SIGSET
    /* Set up handler again. */
    (void) signal( SIGUSR2, handle_usr2 );
#endif /* ! HAVE_SIGSET */

    logstats( (struct timeval*) 0 );

    /* Restore previous errno. */
    errno = oerrno;
}


/* SIGALRM is used as a watchdog. */
/**计时器处理函数*/
static void handle_alrm( int sig )
{
    const int oerrno = errno;

    /* If nothing has been happening */
    if ( ! watchdog_flag )
	{
		/* Try changing dirs to someplace we can write. */
		(void) chdir( "/tmp" );
		/* Dump core. */
		abort();
	}
    watchdog_flag = 0;

#ifndef HAVE_SIGSET
    /* Set up handler again. */
    (void) signal( SIGALRM, handle_alrm );
#endif /* ! HAVE_SIGSET */
    /* Set up alarm again. */
	/**设置闹钟时间*/
    (void) alarm( OCCASIONAL_TIME * 3 );

    /* Restore previous errno. */
    errno = oerrno;
}

/**冲洗打开日志文件*/
static void re_open_logfile( void )
{
    FILE* logfp;
	/**没有配置日志文件或者服务器没有初始化的处理*/
    if ( no_log || hs == (httpd_server*) 0 )
	{
		return;
	}

    /* Re-open the log file. */
    if ( logfile != (char*) 0 && strcmp( logfile, "-" ) != 0 )
	{
		syslog( LOG_NOTICE, "re-opening logfile" );
		logfp = fopen( logfile, "a" );
		if ( logfp == (FILE*) 0 )
	    {
	    	syslog( LOG_CRIT, "re-opening %.80s - %m", logfile );
	    	return;
	    }
		(void) fcntl( fileno( logfp ), F_SETFD, 1 );
		httpd_set_logfp( hs, logfp );
	}
}

/**主要的thttpd服务器主程序*/
int main( int argc, char** argv )
{
    char* cp;
    struct passwd* pwd;
    uid_t uid = 32767;
    gid_t gid = 32767;
    char cwd[MAXPATHLEN+1];
    FILE* logfp;
    int num_ready;
    int cnum;
    connecttab* c;
    httpd_conn* hc;
    httpd_sockaddr sa4;
    httpd_sockaddr sa6;
    int gotv4;
	int gotv6;
    struct timeval tv;
#ifdef JIFUKUI_SSL
	void		*ssllib;
#endif
    argv0 = argv[0];
#ifdef JI_DEBUG
	printf("The argv0 is %s\n",argv0);
#endif
	/**strrchr函数是用于查找字符串中第一次出现/字符的位置*/
    cp = strrchr( argv0, '/' );
    if ( cp != (char*) 0 )
	{
		++cp;
	}
    else
	{
		cp = argv0;
	}
	/**打开日志系统
	 * cp为固定打印在日志文件中的内容
	 * LOG_NDELAY设置立即打开
	 * LOG_PID包括每个消息的PID
	 * LOG_FACILITY
	*/
    openlog( cp, LOG_NDELAY|LOG_PID, LOG_FACILITY );

    /* Handle command-line arguments. */
	/**输入命令处理*/
    parse_args( argc, argv );

    /* Read zone info now, in case we chroot(). */
	/**c语言标准函数，设置时区*/
    tzset();

    /* Look up hostname now, in case we chroot(). */
	/***/
    lookup_hostname( &sa4, sizeof(sa4), &gotv4, &sa6, sizeof(sa6), &gotv6 );
    if ( ! ( gotv4 || gotv6 ) )
	{
		syslog( LOG_ERR, "can't find any valid address" );
		(void) fprintf( stderr, "%s: can't find any valid address\n", argv0 );
		exit( 1 );
	}

    /* Throttle file. */
	/**读取限流设置*/
    numthrottles = 0;
    maxthrottles = 0;
    throttles = (throttletab*) 0;
    if ( throttlefile != (char*) 0 )
	{
		read_throttlefile( throttlefile );
	}

    /* If we're root and we're going to become another user, get the uid/gid
    ** now.
    */
   /***对于用户id为root用户的处理*/
    if ( getuid() == 0 )
	{
		/**获取登录用户信息*/
		pwd = getpwnam( user );
		/**对于获取用户信息失败的处理*/
		if ( pwd == (struct passwd*) 0 )
	    {
	    	syslog( LOG_CRIT, "unknown user - '%.80s'", user );
	    	(void) fprintf( stderr, "%s: unknown user - '%s'\n", argv0, user );
	    	exit( 1 );
	    }
		/**设置uid和gid*/
		uid = pwd->pw_uid;
		gid = pwd->pw_gid;
	}

    /* Log file. */
	/**对于配置了logfile的处理*/
    if ( logfile != (char*) 0 )
	{
		if ( strcmp( logfile, "/dev/null" ) == 0 )
	    {
	    	no_log = 1;
	    	logfp = (FILE*) 0;
	    }
		else if ( strcmp( logfile, "-" ) == 0 )
	    {
			logfp = stdout;
		}
		else
	    {
	    	logfp = fopen( logfile, "a" );
	    	if ( logfp == (FILE*) 0 )
			{
				syslog( LOG_CRIT, "%.80s - %m", logfile );
				perror( logfile );
				exit( 1 );
			}
	    	if ( logfile[0] != '/' )
			{
				syslog( LOG_WARNING, "logfile is not an absolute path, you may not be able to re-open it" );
				(void) fprintf( stderr, "%s: logfile is not an absolute path, you may not be able to re-open it\n", argv0 );
			}
	    	(void) fcntl( fileno( logfp ), F_SETFD, 1 );
	    	if ( getuid() == 0 )
			{
			/* If we are root then we chown the log file to the user we'll
			** be switching to.
			*/
				if ( fchown( fileno( logfp ), uid, gid ) < 0 )
		    	{
		    		syslog( LOG_WARNING, "fchown logfile - %m" );
		    		perror( "fchown logfile" );
		    	}
			}
	    }
	}
    else
	{
		logfp = (FILE*) 0;
	}

    /* Switch directories if requested. */
	/**对于设置工作目录的处理*/
    if ( dir != (char*) 0 )
	{
		if ( chdir( dir ) < 0 )
	    {
	    	syslog( LOG_CRIT, "chdir - %m" );
	    	perror( "chdir" );
	    	exit( 1 );
	    }
	}
#ifdef USE_USER_DIR
    else if ( getuid() == 0 )
	{
	/* No explicit directory was specified, we're root, and the
	** USE_USER_DIR option is set - switch to the specified user's
	** home dir.
	*/
		if ( chdir( pwd->pw_dir ) < 0 )
	    {
	    	syslog( LOG_CRIT, "chdir - %m" );
	    	perror( "chdir" );
	    	exit( 1 );
	    }
	}
#endif /* USE_USER_DIR */

    /* Get current directory. */
	/**获取当前的路径 */
    (void) getcwd( cwd, sizeof(cwd) - 1 );
    if ( cwd[strlen( cwd ) - 1] != '/' )
	{
		(void) strcat( cwd, "/" );
	}
	/**对于设置为debug状态的处理*/
    if ( ! debug )
	{
	/* We're not going to use stdin stdout or stderr from here on, so close
	** them to save file descriptors.
	*/
	/*
		(void) fclose( stdin );
		if ( logfp != stdout )
	    {
			(void) fclose( stdout );
		}
		(void) fclose( stderr );
	*/

	/* Daemonize - make ourselves a subprocess. */
#ifdef HAVE_DAEMON
		if ( daemon( 1, 1 ) < 0 )
	    {
	    	syslog( LOG_CRIT, "daemon - %m" );
	    	exit( 1 );
	    }
#else /* HAVE_DAEMON */
		switch ( fork() )
	    {
	    	case 0:
	    		break;
	    	case -1:
	    		syslog( LOG_CRIT, "fork - %m" );
	    		exit( 1 );
	    	default:
	    		exit( 0 );
	    }
#ifdef HAVE_SETSID
        (void) setsid();
#endif /* HAVE_SETSID */
#endif /* HAVE_DAEMON */
	}
    else
	{
	/* Even if we don't daemonize, we still want to disown our parent
	** process.
	*/
#ifdef HAVE_SETSID
        (void) setsid();
#endif /* HAVE_SETSID */
	}
	/**对于设置进程文件的处理*/
    if ( pidfile != (char*) 0 )
	{
		/* Write the PID file. */
		FILE* pidfp = fopen( pidfile, "w" );
		if ( pidfp == (FILE*) 0 )
	    {
	    	syslog( LOG_CRIT, "%.80s - %m", pidfile );
	    	exit( 1 );
	    }
		(void) fprintf( pidfp, "%d\n", (int) getpid() );
		(void) fclose( pidfp );
	}

    /* Initialize the fdwatch package.  Have to do this before chroot,
    ** if /dev/poll is used.
    */
   	/**获取支持的最大链接数量*/
    max_connects = fdwatch_get_nfiles();
    if ( max_connects < 0 )
	{
		syslog( LOG_CRIT, "fdwatch initialization failure" );
		exit( 1 );
	}
    max_connects -= SPARE_FDS;

    /* Chroot if requested. */
    if ( do_chroot )
	{
		if ( chroot( cwd ) < 0 )
	    {
	    	syslog( LOG_CRIT, "chroot - %m" );
	    	perror( "chroot" );
	    	exit( 1 );
	    }
	/* If we're logging and the logfile's pathname begins with the
	** chroot tree's pathname, then elide the chroot pathname so
	** that the logfile pathname still works from inside the chroot
	** tree.
	*/
		if ( logfile != (char*) 0 && strcmp( logfile, "-" ) != 0 )
	    {
	    	if ( strncmp( logfile, cwd, strlen( cwd ) ) == 0 )
			{
				(void) ol_strcpy( logfile, &logfile[strlen( cwd ) - 1] );
				/* (We already guaranteed that cwd ends with a slash, so leaving
				** that slash in logfile makes it an absolute pathname within
				** the chroot tree.)
				*/
			}
	    	else
			{
				syslog( LOG_WARNING, "logfile is not within the chroot tree, you will not be able to re-open it" );
				(void) fprintf( stderr, "%s: logfile is not within the chroot tree, you will not be able to re-open it\n", argv0 );
			}
	    }
		(void) strcpy( cwd, "/" );
		/* Always chdir to / after a chroot. */
		if ( chdir( cwd ) < 0 )
	    {
	    	syslog( LOG_CRIT, "chroot chdir - %m" );
	    	perror( "chroot chdir" );
	    	exit( 1 );
	    }
	}

    /* Switch directories again if requested. */
    if ( data_dir != (char*) 0 )
	{
		if ( chdir( data_dir ) < 0 )
	    {
	    	syslog( LOG_CRIT, "data_dir chdir - %m" );
	    	perror( "data_dir chdir" );
	    	exit( 1 );
	    }
	}

    /* Set up to catch signals. */
	/**信号事件处理函数*/
#ifdef HAVE_SIGSET
    (void) sigset( SIGTERM, handle_term );		//系统提供给普通用户，用于终止一个进程
    (void) sigset( SIGINT, handle_term );		//对于用户输出Ctrl+C键的处理
    (void) sigset( SIGCHLD, handle_chld );		//子进程结束
    (void) sigset( SIGPIPE, SIG_IGN );          /* get EPIPE instead 对不存在的管道写数据时产生 忽略此信号*/
    (void) sigset( SIGHUP, handle_hup );		//终端关闭时的处理函数
    (void) sigset( SIGUSR1, handle_usr1 );		//用户定义信号处理1
    (void) sigset( SIGUSR2, handle_usr2 );		//用户定义信号处理2
    (void) sigset( SIGALRM, handle_alrm );		//时钟信号处理函数
#else /* HAVE_SIGSET */
    (void) signal( SIGTERM, handle_term );
    (void) signal( SIGINT, handle_term );
    (void) signal( SIGCHLD, handle_chld );
    (void) signal( SIGPIPE, SIG_IGN );          /* get EPIPE instead */
    (void) signal( SIGHUP, handle_hup );
    (void) signal( SIGUSR1, handle_usr1 );
    (void) signal( SIGUSR2, handle_usr2 );
    (void) signal( SIGALRM, handle_alrm );
#endif /* HAVE_SIGSET */
    got_hup = 0;
    got_usr1 = 0;
    watchdog_flag = 0;
    (void) alarm( OCCASIONAL_TIME * 3 );

    /* Initialize the timer package. */
    tmr_init();

    /* Initialize the HTTP layer.  Got to do this before giving up root,
    ** so that we can bind to a privileged port.
    */

    hs = httpd_initialize(
	hostname,
	gotv4 ? &sa4 : (httpd_sockaddr*) 0, gotv6 ? &sa6 : (httpd_sockaddr*) 0,
	port, cgi_pattern, cgi_limit, charset, p3p, max_age, cwd, no_log, logfp,
	no_symlink_check, do_vhost, do_global_passwd, url_pattern,
	local_pattern, no_empty_referrers );
	/**http服务器对象未初始化的错误处理*/
    if ( hs == (httpd_server*) 0 )
	{
		exit( 1 );
	}
#ifdef JIFUKUI_SSL
	SSL_CTX *jifukuictx;
	if ((jifukuictx = SSL_CTX_new(SSLv23_server_method())) == NULL)
	{
		printf("load method over\r\n");
	}
	else if (SSL_CTX_use_certificate_file(jifukuictx, "thttpd.pem", SSL_FILETYPE_PEM) == 0)
	{
		printf("cannot open certificate\r\n");
	}
	else if (SSL_CTX_use_PrivateKey_file(jifukuictx, "thttpd.pem", SSL_FILETYPE_PEM) == 0)
	{
		printf("cannot open PrivateKey\r\n");
	}else{
		printf("good for openssl\r\n");
	}
	hs->ssl_ctx=jifukuictx;
#endif
    /* Set up the occasional timer. */
    if ( tmr_create( (struct timeval*) 0, occasional, JunkClientData, OCCASIONAL_TIME * 1000L, 1 ) == (Timer*) 0 )
	{
		syslog( LOG_CRIT, "tmr_create(occasional) failed" );
		exit( 1 );
	}
    /* Set up the idle timer. */
    if ( tmr_create( (struct timeval*) 0, idle, JunkClientData, 5 * 1000L, 1 ) == (Timer*) 0 )
	{
		syslog( LOG_CRIT, "tmr_create(idle) failed" );
		exit( 1 );
	}
    if ( numthrottles > 0 )
	{
		/* Set up the throttles timer. */
		if ( tmr_create( (struct timeval*) 0, update_throttles, JunkClientData, THROTTLE_TIME * 1000L, 1 ) == (Timer*) 0 )
	    {
	    	syslog( LOG_CRIT, "tmr_create(update_throttles) failed" );
	    	exit( 1 );
	    }
	}
#ifdef STATS_TIME
    /* Set up the stats timer. */
	/**创建计时器*/
    if ( tmr_create( (struct timeval*) 0, show_stats, JunkClientData, STATS_TIME * 1000L, 1 ) == (Timer*) 0 )
	{
		syslog( LOG_CRIT, "tmr_create(show_stats) failed" );
		exit( 1 );
	}
#endif /* STATS_TIME */
    start_time = stats_time = time( (time_t*) 0 );
    stats_connections = 0;
    stats_bytes = 0;
    stats_simultaneous = 0;

    /* If we're root, try to become someone else. */
    if ( getuid() == 0 )
	{
		/* Set aux groups to null. */
		if ( setgroups( 0, (const gid_t*) 0 ) < 0 )
	    {
	    	syslog( LOG_CRIT, "setgroups - %m" );
	    	exit( 1 );
	    }
		/* Set primary group. */
		if ( setgid( gid ) < 0 )
	    {
	    	syslog( LOG_CRIT, "setgid - %m" );
	    	exit( 1 );
	    }
		/* Try setting aux groups correctly - not critical if this fails. */
		if ( initgroups( user, gid ) < 0 )
		{
			syslog( LOG_WARNING, "initgroups - %m" );
		}
#ifdef HAVE_SETLOGIN
	/* Set login name. */
        (void) setlogin( user );
#endif /* HAVE_SETLOGIN */
	/* Set uid. */
		if ( setuid( uid ) < 0 )
	    {
	    	syslog( LOG_CRIT, "setuid - %m" );
	    	exit( 1 );
	    }
		/* Check for unnecessary security exposure. */
		if ( ! do_chroot )
	    {
			syslog(LOG_WARNING,"started as root without requesting chroot(), warning only" );
		}
	}

    /* Initialize our connections table. */
    connects = NEW( connecttab, max_connects );
    if ( connects == (connecttab*) 0 )
	{
		syslog( LOG_CRIT, "out of memory allocating a connecttab" );
		exit( 1 );
	}
    for ( cnum = 0; cnum < max_connects; ++cnum )
	{
		connects[cnum].conn_state = CNST_FREE;
		connects[cnum].next_free_connect = cnum + 1;
		connects[cnum].hc = (httpd_conn*) 0;
	}
    connects[max_connects - 1].next_free_connect = -1;	/* end of link list */
    first_free_connect = 0;
    num_connects = 0;
    httpd_conn_count = 0;

    if ( hs != (httpd_server*) 0 )
	{
		if ( hs->listen4_fd != -1 )
	    {
			fdwatch_add_fd( hs->listen4_fd, (void*) 0, FDW_READ );
		}
		if ( hs->listen6_fd != -1 )
	    {
			fdwatch_add_fd( hs->listen6_fd, (void*) 0, FDW_READ );
		}
	}
#ifdef JI__DEBUG
	printf("start to debug\n");
#endif
    /* Main loop. */
    (void) gettimeofday( &tv, (struct timezone*) 0 );
    while ( ( ! terminate ) || num_connects > 0 )
	{
		/* Do we need to re-open the log file? */
		if ( got_hup )
	    {
	    	re_open_logfile();
	    	got_hup = 0;
	    }

		/* Do the fd watch. */
		/**获取是否有程序的超时，正数表示的是准备好的文件描述符的数量*/
		num_ready = fdwatch( tmr_mstimeout( &tv ) );
		if ( num_ready < 0 )
	    {
	    	if ( errno == EINTR || errno == EAGAIN )
			{
				continue;       /* try again */
			}
	    	syslog( LOG_ERR, "fdwatch - %m" );
	    	exit( 1 );
	    }
		(void) gettimeofday( &tv, (struct timezone*) 0 );

		if ( num_ready == 0 )
	    {
	    	/* No fd's are ready - run the timers. */
	    	tmr_run( &tv );
	    	continue;
	    }
		/**对于可用的文件描述符的值为正数的处理*/
		/**对于IP地址为IPv6进行处理*/
		if ( hs != (httpd_server*) 0 && hs->listen6_fd != -1 &&fdwatch_check_fd( hs->listen6_fd ) )
	    {
#ifdef JI_DEBUG
			printf("The ipv6 fd is %d\n",hs->listen6_fd);
#endif 
	    	if ( handle_newconnect( &tv, hs->listen6_fd ) )
			/* Go around the loop and do another fdwatch, rather than
			** dropping through and processing existing connections.
			** New connections always get priority.
			*/
			{
				continue;
			}
	    }
		/**对IP地址为IPV4的处理*/
		if ( hs != (httpd_server*) 0 && hs->listen4_fd != -1 &&fdwatch_check_fd( hs->listen4_fd ) )
	    {
#ifdef JI_DEBUG
			printf("The ipv4 fd is %d\n",hs->listen4_fd);
#endif 
	    	if ( handle_newconnect( &tv, hs->listen4_fd ) )
			/* Go around the loop and do another fdwatch, rather than
			** dropping through and processing existing connections.
			** New connections always get priority.
			*/
			{
				continue;
			}
	    }

		/* Find the connections that need servicing. */
		while ( ( c = (connecttab*) fdwatch_get_next_client_data() ) != (connecttab*) -1 )
	    {
	    	if ( c == (connecttab*) 0 )
			{
				continue;
			}
	    	hc = c->hc;
			/**对于文件描述符的值为非法的文件描述符的处理*/
	    	if ( ! fdwatch_check_fd( hc->conn_fd ) )
			/* Something went wrong. */
			{
				clear_connection( c, &tv );
			}
	    	else
			{
				switch ( c->conn_state )
		    	{
		    		case CNST_READING: 
					{
						/**对于连接状态为读数据时的处理*/
						handle_read( c, &tv ); 
						break;
					}
		    		case CNST_SENDING: 
					{
						/**将生成的数据进行发送处理*/
						handle_send( c, &tv ); 
						break;
					}
		    		case CNST_LINGERING: 
					{
						handle_linger( c, &tv ); 
						break;
					}
		    	}
			}
	    }
		tmr_run( &tv );

		if ( got_usr1 && ! terminate )
	    {
	    	terminate = 1;
	    	if ( hs != (httpd_server*) 0 )
			{
				if ( hs->listen4_fd != -1 )
		    	{
					fdwatch_del_fd( hs->listen4_fd );
				}
				if ( hs->listen6_fd != -1 )
		   		{
				    fdwatch_del_fd( hs->listen6_fd );
				}
				httpd_unlisten( hs );
			}
	    }
	}

    /* The main loop terminated. */
    shut_down();
	/**日志文件中进行记录*/
    syslog( LOG_NOTICE, "exiting" );
	/**关闭日志文件*/
    closelog();
	/**程序退出*/
    exit( 0 );
}

/**主程序传入参数处理*/
static void parse_args( int argc, char** argv )
{
    int argn;

    debug = 0;
    port = DEFAULT_PORT;
    dir = (char*) 0;
    data_dir = (char*) 0;
#ifdef ALWAYS_CHROOT
    do_chroot = 1;
#else /* ALWAYS_CHROOT */
    do_chroot = 0;
#endif /* ALWAYS_CHROOT */
    no_log = 0;
    no_symlink_check = do_chroot;
#ifdef ALWAYS_VHOST
    do_vhost = 1;
#else /* ALWAYS_VHOST */
    do_vhost = 0;
#endif /* ALWAYS_VHOST */
#ifdef ALWAYS_GLOBAL_PASSWD
    do_global_passwd = 1;
#else /* ALWAYS_GLOBAL_PASSWD */
    do_global_passwd = 0;
#endif /* ALWAYS_GLOBAL_PASSWD */
#ifdef CGI_PATTERN
    cgi_pattern = CGI_PATTERN;
#else /* CGI_PATTERN */
    cgi_pattern = (char*) 0;
#endif /* CGI_PATTERN */
#ifdef CGI_LIMIT
    cgi_limit = CGI_LIMIT;
#else /* CGI_LIMIT */
    cgi_limit = 0;
#endif /* CGI_LIMIT */
    url_pattern = (char*) 0;
    no_empty_referrers = 0;
    local_pattern = (char*) 0;
    throttlefile = (char*) 0;
    hostname = (char*) 0;
    logfile = (char*) 0;
    pidfile = (char*) 0;
    user = DEFAULT_USER;
    charset = DEFAULT_CHARSET;
    p3p = "";
    max_age = -1;
    argn = 1;
    while ( argn < argc && argv[argn][0] == '-' )
	{
		/**获取版本信息 */
		if ( strcmp( argv[argn], "-V" ) == 0 )
	    {
	    	(void) printf( "%s\n", SERVER_SOFTWARE );
	    	exit( 0 );
	    }
		/**读取配置文件 */
		else if ( strcmp( argv[argn], "-C" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	read_config( argv[argn] );
	    }
		/**设置监听的端口号 */
		else if ( strcmp( argv[argn], "-p" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	port = (unsigned short) atoi( argv[argn] );
	    }
		/**设置工作目录 */
		else if ( strcmp( argv[argn], "-d" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	dir = argv[argn];
	    }
		/**设置是否切换到根目录 */
		else if ( strcmp( argv[argn], "-r" ) == 0 )
	    {
	    	do_chroot = 1;
	    	no_symlink_check = 1;
	    }
		/**设置不切换到根目录 */
		else if ( strcmp( argv[argn], "-nor" ) == 0 )
	    {
	    	do_chroot = 0;
	    	no_symlink_check = 0;
	    }
		else if ( strcmp( argv[argn], "-dd" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	data_dir = argv[argn];
	    }
		/**设置进行符号连接检测 */
		else if ( strcmp( argv[argn], "-s" ) == 0 )
	    {
			no_symlink_check = 0;
		}
		/**设置不进行符号连接检测 */
		else if ( strcmp( argv[argn], "-nos" ) == 0 )
	    {
			no_symlink_check = 1;
		}
		/**设置用户 */
		else if ( strcmp( argv[argn], "-u" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	user = argv[argn];
	    }
		/**设置CGI的路径 */
		else if ( strcmp( argv[argn], "-c" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	cgi_pattern = argv[argn];
	    }
		/**设置限流文件 */
		else if ( strcmp( argv[argn], "-t" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	throttlefile = argv[argn];
	    }
		/**设置主机名称 */
		else if ( strcmp( argv[argn], "-h" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	hostname = argv[argn];
	    }
		/**设置日志文件的名称 */
		else if ( strcmp( argv[argn], "-l" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	logfile = argv[argn];
	    }
		/**设置使用虚拟主机 */
		else if ( strcmp( argv[argn], "-v" ) == 0 )
	    {
			do_vhost = 1;
		}
		/**设置不使用虚拟主机 */
		else if ( strcmp( argv[argn], "-nov" ) == 0 )
	    {
			do_vhost = 0;
		}
		/**设置使用全局的密码 */
		else if ( strcmp( argv[argn], "-g" ) == 0 )
	    {
			do_global_passwd = 1;
		}
		/**设置使用全局的密码 */
		else if ( strcmp( argv[argn], "-nog" ) == 0 )
	    {
			do_global_passwd = 0;
		}
		/**设置 */
		else if ( strcmp( argv[argn], "-i" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	pidfile = argv[argn];
	    }
		/**设置字符集 */
		else if ( strcmp( argv[argn], "-T" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	charset = argv[argn];
	    }
		/**设置使用在线隐私平台技术 */
		else if ( strcmp( argv[argn], "-P" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	p3p = argv[argn];
	    }
		/**设置最大的生存时间 */
		else if ( strcmp( argv[argn], "-M" ) == 0 && argn + 1 < argc )
	    {
	    	++argn;
	    	max_age = atoi( argv[argn] );
	    }
		/**设置是否为debug模式 */
		else if ( strcmp( argv[argn], "-D" ) == 0 )
	    {
			debug = 1;
		}
		/**错误处理 */
		else
	    {
			usage();
		}
		++argn;
	}
    if ( argn != argc )
	{
		usage();
	}
}


static void usage( void )
{
    (void) fprintf( stderr,
"usage:  %s [-C configfile] [-p port] [-d dir] [-r|-nor] [-dd data_dir] [-s|-nos] [-v|-nov] [-g|-nog] [-u user] [-c cgipat] [-t throttles] [-h host] [-l logfile] [-i pidfile] [-T charset] [-P P3P] [-M maxage] [-V] [-D]\n",
	argv0 );
    exit( 1 );
}

/**配置文件处理*/
static void read_config( char* filename )
{
    FILE* fp;
    char line[10000];
    char* cp;
    char* cp2;
    char* name;
    char* value;

    fp = fopen( filename, "r" );
    if ( fp == (FILE*) 0 )
	{
		perror( filename );
		exit( 1 );
	}

    while ( fgets( line, sizeof(line), fp ) != (char*) 0 )
	{
		/* Trim comments. */
		if ( ( cp = strchr( line, '#' ) ) != (char*) 0 )
	    {
			*cp = '\0';
		}

		/* Skip leading whitespace. */
		cp = line;
		cp += strspn( cp, " \t\n\r" );

		/* Split line into words. */
		while ( *cp != '\0' )
	    {
	    	/* Find next whitespace. */
	    	cp2 = cp + strcspn( cp, " \t\n\r" );
	    	/* Insert EOS and advance next-word pointer. */
	    	while ( *cp2 == ' ' || *cp2 == '\t' || *cp2 == '\n' || *cp2 == '\r' )
			{
				*cp2++ = '\0';
			}
	    	/* Split into name and value. */
	    	name = cp;
	    	value = strchr( name, '=' );
	    	if ( value != (char*) 0 )
			{
				*value++ = '\0';
			}
	    	/* Interpret. */
	    	if ( strcasecmp( name, "debug" ) == 0 )
			{
				no_value_required( name, value );
				debug = 1;
				}
	    	else if ( strcasecmp( name, "port" ) == 0 )
			{
				value_required( name, value );
				port = (unsigned short) atoi( value );
			}
	    	else if ( strcasecmp( name, "dir" ) == 0 )
			{
				value_required( name, value );
				dir = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "chroot" ) == 0 )
			{
				no_value_required( name, value );
				do_chroot = 1;
				no_symlink_check = 1;
			}
	    	else if ( strcasecmp( name, "nochroot" ) == 0 )
			{
				no_value_required( name, value );
				do_chroot = 0;
				no_symlink_check = 0;
			}
	    	else if ( strcasecmp( name, "data_dir" ) == 0 )
			{
				value_required( name, value );
				data_dir = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "nosymlinkcheck" ) == 0 )
			{
				no_value_required( name, value );
				no_symlink_check = 1;
			}
	    	else if ( strcasecmp( name, "symlinkcheck" ) == 0 )
			{
				no_value_required( name, value );
				no_symlink_check = 0;
			}
	    	else if ( strcasecmp( name, "user" ) == 0 )
			{
				value_required( name, value );
				user = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "cgipat" ) == 0 )
			{
				value_required( name, value );
				cgi_pattern = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "cgilimit" ) == 0 )
			{
				value_required( name, value );
				cgi_limit = atoi( value );
			}
	    	else if ( strcasecmp( name, "urlpat" ) == 0 )
			{
				value_required( name, value );
				url_pattern = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "noemptyreferers" ) == 0 ||strcasecmp( name, "noemptyreferrers" ) == 0 )
			{
				no_value_required( name, value );
				no_empty_referrers = 1;
			}
	    	else if ( strcasecmp( name, "localpat" ) == 0 )
			{
				value_required( name, value );
				local_pattern = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "throttles" ) == 0 )
			{
				value_required( name, value );
				throttlefile = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "host" ) == 0 )
			{
				value_required( name, value );
				hostname = e_strdup( value );
			}
	   		else if ( strcasecmp( name, "logfile" ) == 0 )
			{
				value_required( name, value );
				logfile = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "vhost" ) == 0 )
			{
				no_value_required( name, value );
				do_vhost = 1;
			}
	    	else if ( strcasecmp( name, "novhost" ) == 0 )
			{
				no_value_required( name, value );
				do_vhost = 0;
			}
	    	else if ( strcasecmp( name, "globalpasswd" ) == 0 )
			{
				no_value_required( name, value );
				do_global_passwd = 1;
			}
	    	else if ( strcasecmp( name, "noglobalpasswd" ) == 0 )
			{
				no_value_required( name, value );
				do_global_passwd = 0;
			}
	    	else if ( strcasecmp( name, "pidfile" ) == 0 )
			{
				value_required( name, value );
				pidfile = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "charset" ) == 0 )
			{
				value_required( name, value );
				charset = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "p3p" ) == 0 )
			{
				value_required( name, value );
				p3p = e_strdup( value );
			}
	    	else if ( strcasecmp( name, "max_age" ) == 0 )
			{
				value_required( name, value );
				max_age = atoi( value );
			}
	    	else
			{
				(void) fprintf(stderr, "%s: unknown config option '%s'\n", argv0, name );
				exit( 1 );
			}

	    	/* Advance to next word. */
	    	cp = cp2;
	    	cp += strspn( cp, " \t\n\r" );
	    }
	}

    (void) fclose( fp );
}


static void	value_required( char* name, char* value )
{
    if ( value == (char*) 0 )
	{
		(void) fprintf(stderr, "%s: value required for %s option\n", argv0, name );
		exit( 1 );
	}
}


static void no_value_required( char* name, char* value )
{
    if ( value != (char*) 0 )
	{
		(void) fprintf(stderr, "%s: no value required for %s option\n",argv0, name );
		exit( 1 );
	}
}


static char* e_strdup( char* oldstr )
{
    char* newstr;

    newstr = strdup( oldstr );
    if ( newstr == (char*) 0 )
	{
		syslog( LOG_CRIT, "out of memory copying a string" );
		(void) fprintf( stderr, "%s: out of memory copying a string\n", argv0 );
		exit( 1 );
	}
    return newstr;
}

/**根据配置的信息配置IPV6或者IPV4的地址*/
static void lookup_hostname( httpd_sockaddr* sa4P, size_t sa4_len, int* gotv4P, httpd_sockaddr* sa6P, size_t sa6_len, int* gotv6P )
{
#ifdef USE_IPV6

    struct addrinfo hints;
    char portstr[10];
    int gaierr;
    struct addrinfo* ai;
    struct addrinfo* ai2;
    struct addrinfo* aiv6;
    struct addrinfo* aiv4;

    (void) memset( &hints, 0, sizeof(hints) );
    hints.ai_family = PF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_STREAM;
    (void) snprintf( portstr, sizeof(portstr), "%d", (int) port );
    if ( (gaierr = getaddrinfo( hostname, portstr, &hints, &ai )) != 0 )
	{
		syslog(LOG_CRIT, "getaddrinfo %.80s - %.80s",hostname, gai_strerror( gaierr ) );
		(void) fprintf(stderr, "%s: getaddrinfo %s - %s\n",argv0, hostname, gai_strerror( gaierr ) );
		exit( 1 );
	}

    /* Find the first IPv6 and IPv4 entries. */
    aiv6 = (struct addrinfo*) 0;
    aiv4 = (struct addrinfo*) 0;
    for ( ai2 = ai; ai2 != (struct addrinfo*) 0; ai2 = ai2->ai_next )
	{
		switch ( ai2->ai_family )
	    {
	    	case AF_INET6:
	    	if ( aiv6 == (struct addrinfo*) 0 )
			{
				aiv6 = ai2;
			}
	    	break;
	    	case AF_INET:
	    	if ( aiv4 == (struct addrinfo*) 0 )
			{
				aiv4 = ai2;
			}
	    	break;
	    }
	}

    if ( aiv6 == (struct addrinfo*) 0 )
	{
		*gotv6P = 0;
	}
    else
	{
		if ( sa6_len < aiv6->ai_addrlen )
	    {
	    	syslog(LOG_CRIT, "%.80s - sockaddr too small (%lu < %lu)",hostname, (unsigned long) sa6_len,(unsigned long) aiv6->ai_addrlen );
	    	exit( 1 );
	    }
		(void) memset( sa6P, 0, sa6_len );
		(void) memmove( sa6P, aiv6->ai_addr, aiv6->ai_addrlen );
		*gotv6P = 1;
	}

    if ( aiv4 == (struct addrinfo*) 0 )
	{
		*gotv4P = 0;
	}
    else
	{
		if ( sa4_len < aiv4->ai_addrlen )
	    {
	    	syslog(LOG_CRIT, "%.80s - sockaddr too small (%lu < %lu)",hostname, (unsigned long) sa4_len,(unsigned long) aiv4->ai_addrlen );
	    	exit( 1 );
	    }
		(void) memset( sa4P, 0, sa4_len );
		(void) memmove( sa4P, aiv4->ai_addr, aiv4->ai_addrlen );
		*gotv4P = 1;
	}

    freeaddrinfo( ai );

#else /* USE_IPV6 */

    struct hostent* he;

    *gotv6P = 0;

    (void) memset( sa4P, 0, sa4_len );
    sa4P->sa.sa_family = AF_INET;
    if ( hostname == (char*) 0 )
	{
		/**对于没有设置ip地址的随便找一个值*/
		sa4P->sa_in.sin_addr.s_addr = htonl( INADDR_ANY );
	}
    else
	{
		/**对于设置主机的IP地址的将主机的IP地址字符型转化为一个32位的值*/
		sa4P->sa_in.sin_addr.s_addr = inet_addr( hostname );
		/**对于设置地址错误的处理*/
		if ( (int) sa4P->sa_in.sin_addr.s_addr == -1 )
	    {
	    	he = gethostbyname( hostname );
	    	if ( he == (struct hostent*) 0 )
			{
#ifdef HAVE_HSTRERROR
			syslog(LOG_CRIT, "gethostbyname %.80s - %.80s",hostname, hstrerror( h_errno ) );
			(void) fprintf(stderr, "%s: gethostbyname %s - %s\n",argv0, hostname, hstrerror( h_errno ) );
#else /* HAVE_HSTRERROR */
			syslog( LOG_CRIT, "gethostbyname %.80s failed", hostname );
			(void) fprintf(stderr, "%s: gethostbyname %s failed\n", argv0, hostname );
#endif /* HAVE_HSTRERROR */
			exit( 1 );
		}
	    	if ( he->h_addrtype != AF_INET )
			{
				syslog( LOG_CRIT, "%.80s - non-IP network address", hostname );
				(void) fprintf(stderr, "%s: %s - non-IP network address\n",argv0, hostname );
				exit( 1 );
			}
	    	(void) memmove(&sa4P->sa_in.sin_addr.s_addr, he->h_addr, he->h_length );
	    }
	}
    sa4P->sa_in.sin_port = htons( port );
    *gotv4P = 1;

#endif /* USE_IPV6 */
}


static void read_throttlefile( char* tf )
{
    FILE* fp;
    char buf[5000];
    char* cp;
    int len;
    char pattern[5000];
    long max_limit, min_limit;
    struct timeval tv;

    fp = fopen( tf, "r" );
    if ( fp == (FILE*) 0 )
	{
		syslog( LOG_CRIT, "%.80s - %m", tf );
		perror( tf );
		exit( 1 );
	}

    (void) gettimeofday( &tv, (struct timezone*) 0 );

    while ( fgets( buf, sizeof(buf), fp ) != (char*) 0 )
	{
		/* Nuke comments. */
		cp = strchr( buf, '#' );
		/**#号不是此行的第一个参数
		 * 在此行数据的末尾添加结束符
		*/
		if ( cp != (char*) 0 )
	    {
			*cp = '\0';
		}

		/* Nuke trailing whitespace. */
		len = strlen( buf );
		while ( len > 0 &&( buf[len-1] == ' ' || buf[len-1] == '\t' ||buf[len-1] == '\n' || buf[len-1] == '\r' ) )
	    {
			buf[--len] = '\0';
		}

		/* Ignore empty lines. */
		if ( len == 0 )
	    {
			continue;
		}

		/** Parse line.
		 * 获取三个参数
		 */
		if ( sscanf( buf, " %4900[^ \t] %ld-%ld", pattern, &min_limit, &max_limit ) == 3 )
	    {}
		/**获取两个参数*/
		else if ( sscanf( buf, " %4900[^ \t] %ld", pattern, &max_limit ) == 2 )
	    {
			min_limit = 0;
		}
		/**获取一个参数*/
		else
	    {
	    	syslog( LOG_CRIT,"unparsable line in %.80s - %.80s", tf, buf );
	    	(void) fprintf( stderr,"%s: unparsable line in %.80s - %.80s\n",argv0, tf, buf );
	    	continue;
	    }

		/* Nuke any leading slashes in pattern. */
		if ( pattern[0] == '/' )
	    {
			(void) ol_strcpy( pattern, &pattern[1] );
		}
		while ( ( cp = strstr( pattern, "|/" ) ) != (char*) 0 )
	    {
			(void) ol_strcpy( cp + 1, cp + 2 );
		}

		/* Check for room in throttles. */
		/**对于限流文件的数据大于最大的限流文件的数量的处理*/
		if ( numthrottles >= maxthrottles )
	    {
	    	if ( maxthrottles == 0 )
			{
				maxthrottles = 100;     /* arbitrary */
				throttles = NEW( throttletab, maxthrottles );
			}
	    	else
			{
				maxthrottles *= 2;
				throttles = RENEW( throttles, throttletab, maxthrottles );
			}
	    	if ( throttles == (throttletab*) 0 )
			{
				syslog( LOG_CRIT, "out of memory allocating a throttletab" );
				(void) fprintf(stderr, "%s: out of memory allocating a throttletab\n",argv0 );
				exit( 1 );
			}
	    }

		/* Add to table. */
		/**初始化相关的参数*/
		throttles[numthrottles].pattern = e_strdup( pattern );
		throttles[numthrottles].max_limit = max_limit;
		throttles[numthrottles].min_limit = min_limit;
		throttles[numthrottles].rate = 0;
		throttles[numthrottles].bytes_since_avg = 0;
		throttles[numthrottles].num_sending = 0;
		/**更新流量限制数据*/
		++numthrottles;
	}
    (void) fclose( fp );
}

/**获取当前时间
 * 在日志文件中记录
 * 复位连接数组中的状态值释放连接数组申请的内存
 * 释放服务器申请的内存
 * 
*/
static void shut_down( void )
{
    int cnum;
    struct timeval tv;

    (void) gettimeofday( &tv, (struct timezone*) 0 );
    logstats( &tv );
    for ( cnum = 0; cnum < max_connects; ++cnum )
	{
		if ( connects[cnum].conn_state != CNST_FREE )
	    {
			httpd_close_conn( connects[cnum].hc, &tv );
		}
		if ( connects[cnum].hc != (httpd_conn*) 0 )
	    {
	    	httpd_destroy_conn( connects[cnum].hc );
	    	free( (void*) connects[cnum].hc );
	    	--httpd_conn_count;
	    	connects[cnum].hc = (httpd_conn*) 0;
	    }
	}
    if ( hs != (httpd_server*) 0 )
	{
		httpd_server* ths = hs;
		hs = (httpd_server*) 0;
		if ( ths->listen4_fd != -1 )
	    {
			fdwatch_del_fd( ths->listen4_fd );
		}
		if ( ths->listen6_fd != -1 )
	    {
			fdwatch_del_fd( ths->listen6_fd );
		}
		httpd_terminate( ths );
	}
    mmc_term();
    tmr_term();
    free( (void*) connects );
    if ( throttles != (throttletab*) 0 )
	{
		free( (void*) throttles );
	}
}

/**对于有新的连接进行处理，这个程序使用定时器进行程序的切换*/
static int handle_newconnect( struct timeval* tvP, int listen_fd )
{
    connecttab* c;
    ClientData client_data;

    /* This loops until the accept() fails, trying to start new
    ** connections as fast as possible so we don't overrun the
    ** listen queue.
    */
#ifdef JI_DEBUG
	printf("Test new connect\n");
#endif
    for (;;)
	{
		/* Is there room in the connection table? */
		/**对于连接数量超过最大连接数量的处理*/
		if ( num_connects >= max_connects )
	    {
	    	/* Out of connection slots.  Run the timers, then the
	    	** existing connections, and maybe we'll free up a slot
	    	** by the time we get back here.
	    	*/
			/**在系统日志中记录错误信息执行计时器运行程序*/
	    	syslog( LOG_WARNING, "too many connections!" );
#ifdef JI_DEBUG
			printf("http connect return maybe in this for to many\n");
#endif
	    	tmr_run( tvP );
	    	return 0;
	    }
		/* Get the first free connection entry off the free list. */
		/**对于当前无可用的文件描述符的处理*/
		if ( first_free_connect == -1 || connects[first_free_connect].conn_state != CNST_FREE )
	    {
	    	syslog( LOG_CRIT, "the connects free list is messed up" );
#ifdef JI_DEBUG
			printf("http connect return maybe in this for first_free_connect\n");
#endif
	    	exit( 1 );
	    }
		c = &connects[first_free_connect];
		/* Make the httpd_conn if necessary. */
		/**对http连接没有初始化进行初始化操作*/
		if ( c->hc == (httpd_conn*) 0 )
	    {
#ifdef JI_DEBUG
			printf("http connect is no initialized\n");
#endif 			
	    	c->hc = NEW( httpd_conn, 1 );
			/**对初始化http连接失败进行处理*/
	    	if ( c->hc == (httpd_conn*) 0 )
			{
				syslog( LOG_CRIT, "out of memory allocating an httpd_conn" );
#ifdef JI_DEBUG
				printf("http connect return maybe in this for mallco error\n");
#endif
				exit( 1 );
			}
			/**设置http初始化标志为未初始化*/
	    	c->hc->initialized = 0;
	    	++httpd_conn_count;
	    }

		/* Get the connection. */
		/**对连接进行处理*/
		switch ( httpd_get_conn( hs, listen_fd, c->hc ) )
	    {
	    	/* Some error happened.  Run the timers, then the
	    	** existing connections.  Maybe the error will clear.
	    	*/
	    	case GC_FAIL:
	    		tmr_run( tvP );
#ifdef JI_DEBUG
				printf("http connect return maybe in this for GC_FAIL\n");
#endif
	    		return 0;

	    	/* No more connections to accept for now. */
	    	case GC_NO_MORE:
#ifdef JI_DEBUG
				printf("http connect return maybe in this for GC_NO_MORE\n");
#endif
	    		return 1;
	    }
		/**设置连接状态*/
		c->conn_state = CNST_READING;
		/* Pop it off the free list. */
		/**设置下一个可以使用的文件描述符*/
		first_free_connect = c->next_free_connect;
		c->next_free_connect = -1;
		++num_connects;
		client_data.p = c;
		/**设置执行的时间*/
		c->active_at = tvP->tv_sec;
		c->wakeup_timer = (Timer*) 0;
		c->linger_timer = (Timer*) 0;
		c->next_byte_index = 0;
		c->numtnums = 0;

		/* Set the connection file descriptor to no-delay mode. */
		/**设置此文件描述符为非阻塞模式*/
		httpd_set_ndelay( c->hc->conn_fd );
		/**向文件队列中添加需要此文件描述符*/
		fdwatch_add_fd( c->hc->conn_fd, c, FDW_READ );
		/**更新已经连接的http数量*/
		++stats_connections;
		if ( num_connects > stats_simultaneous )
	    {
			stats_simultaneous = num_connects;
		}
#ifdef JI_DEBUG
		printf("Test new connect end\n");
#endif
	}
}

/**从客户端的连接获取数据*/
static void handle_read( connecttab* c, struct timeval* tvP )
{
    int sz;
    ClientData client_data;
    httpd_conn* hc = c->hc;
#ifdef JI_DEBUG
		char ji_debugbuf[601];
		printf("connect fd is %d\n",hc->conn_fd);
		printf("the read size is %d\n",hc->read_size);
#endif
    /* Is there room in our buffer to read more bytes? */
	/**对于读取数据的索引值大于数据存储空间时的处理*/
    if ( hc->read_idx >= hc->read_size )
	{
#ifdef JI_DEBUG
		printf("The read size is to small\n");
#endif
		/**对于读数据空间的大小大于5000字节时的处理*/
		if ( hc->read_size > 5000 )
	    {
#ifdef JI_DEBUG
			printf("The read size is to big\n");
#endif
	    	httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
	    	finish_connection( c, tvP );
	    	return;
	    }
		/**给此连接的读数据的数据空间增加1000字节*/
#ifdef JI_DEBUG
		printf("The add read size 1000\n");
#endif
		httpd_realloc_str(&hc->read_buf, &hc->read_size, hc->read_size + 1000 );
	}
#ifdef JI_DEBUG
		printf("The read size is ok \n");
#endif
    /* Read some more bytes. */
	/**服务器从客户端读取数据hc->read_size - hc->read_idx个字节，并将数据存储至read_buf中*/
    sz = read(hc->conn_fd, &(hc->read_buf[hc->read_idx]),hc->read_size - hc->read_idx );
	/**对于读取到文件结束时的处理*/
    if ( sz == 0 )
	{
		httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
		finish_connection( c, tvP );
		return;
	}
	/**对于读取数据错误的处理*/
    if ( sz < 0 )
	{
		/* Ignore EINTR and EAGAIN.  Also ignore EWOULDBLOCK.  At first glance
		** you would think that connections returned by fdwatch as readable
		** should never give an EWOULDBLOCK; however, this apparently can
		** happen if a packet gets garbled.
		*/
		if ( errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK )
	    {
			return;
		}
		httpd_send_err(hc, 400, httpd_err400title, "", httpd_err400form, "" );
		finish_connection( c, tvP );
		return;
	}
	/**更新读取数据的位置*/
    hc->read_idx += sz;
	/**更新时间*/
    c->active_at = tvP->tv_sec;

    /* Do we have a complete request yet? */
	/**获取文件中的请求头HTTP/0.9 request 即第一行*/
    switch ( httpd_got_request( hc ) )
	{
#ifdef JI_DEBUG
			printf("call httpd_got_request");
#endif
		case GR_NO_REQUEST:

			return;
		case GR_BAD_REQUEST:
			httpd_send_err( hc, 400, httpd_err400title, "", httpd_err400form, "" );
			finish_connection( c, tvP );
			return;
		case GR_GOT_REQUEST:
#ifdef JI_DEBUG
			strncpy(ji_debugbuf,hc->read_buf,600);
			ji_debugbuf[600]=NULL;
			printf("The buf is %s\n",ji_debugbuf);
#endif
	}
#ifdef JI_DEBUG
		printf("call httpd_got_request\n",ji_debugbuf);
#endif
    /* Yes.  Try parsing and resolving it. */
	/**对请求头进行处理*/
    if ( httpd_parse_request( hc ) < 0 )
	{
		finish_connection( c, tvP );
		return;
	}

    /* Check the throttle table */
    if ( ! check_throttles( c ) )
	{
		httpd_send_err(hc, 503, httpd_err503title, "", httpd_err503form, hc->encodedurl );
		finish_connection( c, tvP );
		return;
	}

    /* Start the connection going. */
	/**这里为Http数据的处理和返回数据的生成，同时对错误的处理*/
    if ( httpd_start_request( hc, tvP ) < 0 )
	{
		/* Something went wrong.  Close down the connection. */
		finish_connection( c, tvP );
		return;
	}

    /* Fill in end_byte_index. */
	/**对于定义range的处理*/
    if ( hc->got_range )
	{
		/**设置发送数据的起始位置和结束位置 */
		c->next_byte_index = hc->first_byte_index;
		c->end_byte_index = hc->last_byte_index + 1;
	}
	/**对于需要发送的数据的处理*/
    else if ( hc->bytes_to_send < 0 )
	{
		c->end_byte_index = 0;
	}
	/**对于正常的处理*/
    else
	{
		c->end_byte_index = hc->bytes_to_send;
	}

    /* Check if it's already handled. */
	/**对于文件的映射地址为0的处理*/
    if ( hc->file_address == (char*) 0 )
	{
		/* No file address means someone else is handling it. */
		int tind;
		for ( tind = 0; tind < c->numtnums; ++tind )
	    {
			throttles[c->tnums[tind]].bytes_since_avg += hc->bytes_sent;
		}
		c->next_byte_index = hc->bytes_sent;
		finish_connection( c, tvP );
		return;
	}
	/***/
    if ( c->next_byte_index >= c->end_byte_index )
	{
		/* There's nothing to send. */
		finish_connection( c, tvP );
		return;
	}

    /* Cool, we have a valid connection and a file to send to it. */
	/**设置连接的状态为发送状态*/
    c->conn_state = CNST_SENDING;
    c->started_at = tvP->tv_sec;
    c->wouldblock_delay = 0;
    client_data.p = c;

    fdwatch_del_fd( hc->conn_fd );
    fdwatch_add_fd( hc->conn_fd, c, FDW_WRITE );
}

/**回送处理函数函数*/
static void handle_send( connecttab* c, struct timeval* tvP )
{
    size_t max_bytes;
    int sz, coast;
    ClientData client_data;
    time_t elapsed;
    httpd_conn* hc = c->hc;
    int tind;
	/**对于没有限制的处理*/
    if ( c->max_limit == THROTTLE_NOLIMIT )
	{
		max_bytes = 1000000000L;
	}
	/**对于有限制的处理*/
    else
	{
		max_bytes = c->max_limit / 4;	/* send at most 1/4 seconds worth */
	}

    /* Do we need to write the headers first? */
	/**对于hc->responselen的值为0的处理，即没有数据的处理*/
    if ( hc->responselen == 0 )
	{
		/* No, just write the file. */
		sz = write(hc->conn_fd, &(hc->file_address[c->next_byte_index]),MIN( c->end_byte_index - c->next_byte_index, max_bytes ) );
	}
	/**对于hc->responselen的值不为0的处理*/
    else
	{
		/* Yes.  We'll combine headers and file into a single writev(),
		** hoping that this generates a single packet.
		*/
		/**一次函数对象*/
		struct iovec iv[2];

		iv[0].iov_base = hc->response;
		iv[0].iov_len = hc->responselen;
		iv[1].iov_base = &(hc->file_address[c->next_byte_index]);
		iv[1].iov_len = MIN( c->end_byte_index - c->next_byte_index, max_bytes );
		/**一次写两个数据块的数据*/
		sz = writev( hc->conn_fd, iv, 2 );
	}
	/**对于写数据错误的处理但是错误的值为eintr的处理*/
    if ( sz < 0 && errno == EINTR )
	{
		return;
	}
	/***/
    if ( sz == 0 ||( sz < 0 && ( errno == EWOULDBLOCK || errno == EAGAIN ) ) )
	{
		/* This shouldn't happen, but some kernels, e.g.
		** SunOS 4.1.x, are broken and select() says that
		** O_NDELAY sockets are always writable even when
		** they're actually not.
		**
		** Current workaround is to block sending on this
		** socket for a brief adaptively-tuned period.
		** Fortunately we already have all the necessary
		** blocking code, for use with throttling.
		*/
		c->wouldblock_delay += MIN_WOULDBLOCK_DELAY;
		c->conn_state = CNST_PAUSING;
		fdwatch_del_fd( hc->conn_fd );
		client_data.p = c;
		if ( c->wakeup_timer != (Timer*) 0 )
	    {
			syslog( LOG_ERR, "replacing non-null wakeup_timer!" );
		}
		c->wakeup_timer = tmr_create(tvP, wakeup_connection, client_data, c->wouldblock_delay, 0 );
		if ( c->wakeup_timer == (Timer*) 0 )
	    {
	    	syslog( LOG_CRIT, "tmr_create(wakeup_connection) failed" );
	    	exit( 1 );
	    }
		return;
	}
	/**对于写失败的处理*/
    if ( sz < 0 )
	{
	/* Something went wrong, close this connection.
	**
	** If it's just an EPIPE, don't bother logging, that
	** just means the client hung up on us.
	**
	** On some systems, write() occasionally gives an EINVAL.
	** Dunno why, something to do with the socket going
	** bad.  Anyway, we don't log those either.
	**
	** And ECONNRESET isn't interesting either.
	*/
		if ( errno != EPIPE && errno != EINVAL && errno != ECONNRESET )
	    {
			syslog( LOG_ERR, "write - %m sending %.80s", hc->encodedurl );
		}
		clear_connection( c, tvP );
		return;
	}

    /* Ok, we wrote something. */
    c->active_at = tvP->tv_sec;
    /* Was this a headers + file writev()? */
	/**对于调用写的方式不同的处理*/
    if ( hc->responselen > 0 )
	{
		/* Yes; did we write only part of the headers? */
		if ( sz < hc->responselen )
	    {
	    	/* Yes; move the unwritten part to the front of the buffer. */
	    	int newlen = hc->responselen - sz;
	    	(void) memmove( hc->response, &(hc->response[sz]), newlen );
	    	hc->responselen = newlen;
	    	sz = 0;
	    }
		else
	    {
	    	/* Nope, we wrote the full headers, so adjust accordingly. */
	    	sz -= hc->responselen;
	    	hc->responselen = 0;
	    }
	}
    /* And update how much of the file we wrote. */
	/**更新当前已经写的数据的数量*/
    c->next_byte_index += sz;
    c->hc->bytes_sent += sz;
    for ( tind = 0; tind < c->numtnums; ++tind )
	{
		throttles[c->tnums[tind]].bytes_since_avg += sz;
	}

    /* Are we done? */
	/**对于已经写完数据的处理*/
    if ( c->next_byte_index >= c->end_byte_index )
	{
		/* This connection is finished! */
		finish_connection( c, tvP );
		return;
	}

    /* Tune the (blockheaded) wouldblock delay. */
	/**对于没有写完数据的处理*/
    if ( c->wouldblock_delay > MIN_WOULDBLOCK_DELAY )
	{
		c->wouldblock_delay -= MIN_WOULDBLOCK_DELAY;
	}

    /* If we're throttling, check if we're sending too fast. */
    if ( c->max_limit != THROTTLE_NOLIMIT )
	{
		elapsed = tvP->tv_sec - c->started_at;
		if ( elapsed == 0 )
	    {
			elapsed = 1;	/* count at least one second */
		}
		if ( c->hc->bytes_sent / elapsed > c->max_limit )
	    {
	    	c->conn_state = CNST_PAUSING;
	    	fdwatch_del_fd( hc->conn_fd );
	    	/* How long should we wait to get back on schedule?  If less
	    	** than a second (integer math rounding), use 1/2 second.
	    	*/
	    	coast = c->hc->bytes_sent / c->max_limit - elapsed;
	    	client_data.p = c;
	    	if ( c->wakeup_timer != (Timer*) 0 )
			{
				syslog( LOG_ERR, "replacing non-null wakeup_timer!" );
			}
	    	c->wakeup_timer = tmr_create(tvP, wakeup_connection, client_data,coast > 0 ? ( coast * 1000L ) : 500L, 0 );
	    	if ( c->wakeup_timer == (Timer*) 0 )
			{
				syslog( LOG_CRIT, "tmr_create(wakeup_connection) failed" );
				exit( 1 );
			}
	    }
	}
    /* (No check on min_limit here, that only controls connection startups.) */
}


static void handle_linger( connecttab* c, struct timeval* tvP )
{
    char buf[4096];
    int r;

    /* In lingering-close mode we just read and ignore bytes.  An error
    ** or EOF ends things, otherwise we go until a timeout.
    */
    r = read( c->hc->conn_fd, buf, sizeof(buf) );
    if ( r < 0 && ( errno == EINTR || errno == EAGAIN ) )
	{
		return;
	}
    if ( r <= 0 )
	{
		really_clear_connection( c, tvP );
	}
}

/**检测流量限制，检测此连接的扩展的文件名是否在流量限制文件中找到匹配项对于有匹配项的设置相关的参数*/
static int check_throttles( connecttab* c )
{
    int tnum;
    long l;

    c->numtnums = 0;
    c->max_limit = c->min_limit = THROTTLE_NOLIMIT;
    for ( tnum = 0; tnum < numthrottles && c->numtnums < MAXTHROTTLENUMS;++tnum )
	{
		/**在流量限制表中找到于扩展文件名匹配的文件*/
		if ( match( throttles[tnum].pattern, c->hc->expnfilename ) )
		{
	    	/* If we're way over the limit, don't even start. */
			/**匹配项的速率是最大限制的2倍*/
	    	if ( throttles[tnum].rate > throttles[tnum].max_limit * 2 )
			{
				return 0;
			}
	    	/* Also don't start if we're under the minimum. */
			/**速率小于最小限制*/
	    	if ( throttles[tnum].rate < throttles[tnum].min_limit )
			{
				return 0;
			}
			/**发送数据小于0*/
	    	if ( throttles[tnum].num_sending < 0 )
			{
				syslog( LOG_ERR, "throttle sending count was negative - shouldn't happen!" );
				throttles[tnum].num_sending = 0;
			}
			/**设置此连接表的匹配的tnums值*/
	    	c->tnums[c->numtnums++] = tnum;
			/**设置此表项的状态为有效状态*/
	    	++throttles[tnum].num_sending;
			/**获取设置限制的最大值*/
	    	l = throttles[tnum].max_limit / throttles[tnum].num_sending;
	    	if ( c->max_limit == THROTTLE_NOLIMIT )
			{
				c->max_limit = l;
			}
	    	else
			{
				c->max_limit = MIN( c->max_limit, l );
			}
			/**设置限制的最小值*/
	    	l = throttles[tnum].min_limit;
	    	if ( c->min_limit == THROTTLE_NOLIMIT )
			{
				c->min_limit = l;
			}
	    	else
			{
				c->min_limit = MAX( c->min_limit, l );
			}
	    }
	}
    return 1;
}

/**清除流量限制，设置流量限制表中的状态为无效状态*/
static void clear_throttles( connecttab* c, struct timeval* tvP )
{
    int tind;

    for ( tind = 0; tind < c->numtnums; ++tind )
	{
		--throttles[c->tnums[tind]].num_sending;
	}
}

/**更新流量限制*/
static void update_throttles( ClientData client_data, struct timeval* nowP )
{
    int tnum, tind;
    int cnum;
    connecttab* c;
    long l;

    /* Update the average sending rate for each throttle.  This is only used
    ** when new connections start up.
    */
   /** 更新所有流量限制表*/
    for ( tnum = 0; tnum < numthrottles; ++tnum )
	{
		throttles[tnum].rate = ( 2 * throttles[tnum].rate + throttles[tnum].bytes_since_avg / THROTTLE_TIME ) / 3;
		throttles[tnum].bytes_since_avg = 0;
	/* Log a warning message if necessary. */
		if ( throttles[tnum].rate > throttles[tnum].max_limit && throttles[tnum].num_sending != 0 )
	    {
	    	if ( throttles[tnum].rate > throttles[tnum].max_limit * 2 )
			{
				syslog( LOG_NOTICE, "throttle #%d '%.80s' rate %ld greatly exceeding limit %ld; %d sending", tnum, throttles[tnum].pattern, throttles[tnum].rate, throttles[tnum].max_limit, throttles[tnum].num_sending );
			}
	 		else
			{
				syslog( LOG_INFO, "throttle #%d '%.80s' rate %ld exceeding limit %ld; %d sending", tnum, throttles[tnum].pattern, throttles[tnum].rate, throttles[tnum].max_limit, throttles[tnum].num_sending );
			}
	    }
		if ( throttles[tnum].rate < throttles[tnum].min_limit && throttles[tnum].num_sending != 0 )
	    {
	    	syslog( LOG_NOTICE, "throttle #%d '%.80s' rate %ld lower than minimum %ld; %d sending", tnum, throttles[tnum].pattern, throttles[tnum].rate, throttles[tnum].min_limit, throttles[tnum].num_sending );
	    }
	}

    /* Now update the sending rate on all the currently-sending connections,
    ** redistributing it evenly.
    */
   	/**设置每个连接表的浏览限制状态*/
    for ( cnum = 0; cnum < max_connects; ++cnum )
	{
		c = &connects[cnum];
		if ( c->conn_state == CNST_SENDING || c->conn_state == CNST_PAUSING )
	    {
	    	c->max_limit = THROTTLE_NOLIMIT;
	    	for ( tind = 0; tind < c->numtnums; ++tind )
			{
				tnum = c->tnums[tind];
				l = throttles[tnum].max_limit / throttles[tnum].num_sending;
				if ( c->max_limit == THROTTLE_NOLIMIT )
		    	{
					c->max_limit = l;
				}
				else
		    	{
					c->max_limit = MIN( c->max_limit, l );
				}
			}
	    }
	}
}

/**结束连接*/
static void finish_connection( connecttab* c, struct timeval* tvP )
{
    /* If we haven't actually sent the buffered response yet, do so now. */
	/**将数据写入缓冲中*/
    httpd_write_response( c->hc );

    /* And clear. */
    clear_connection( c, tvP );
}

/**清除连接*/
static void clear_connection( connecttab* c, struct timeval* tvP )
{
    ClientData client_data;
	/**对于连接的唤醒计时器不为野指针，清除此计时器同时设置指针的值为0*/
    if ( c->wakeup_timer != (Timer*) 0 )
	{
		tmr_cancel( c->wakeup_timer );
		c->wakeup_timer = 0;
	}

    /* This is our version of Apache's lingering_close() routine, which is
    ** their version of the often-broken SO_LINGER socket option.  For why
    ** this is necessary, see http://www.apache.org/docs/misc/fin_wait_2.html
    ** What we do is delay the actual closing for a few seconds, while reading
    ** any bytes that come over the connection.  However, we don't want to do
    ** this unless it's necessary, because it ties up a connection slot and
    ** file descriptor which means our maximum connection-handling rate
    ** is lower.  So, elsewhere we set a flag when we detect the few
    ** circumstances that make a lingering close necessary.  If the flag
    ** isn't set we do the real close now.
    */
   /**对于连接状态为错误时的处理*/
    if ( c->conn_state == CNST_LINGERING )
	{
		/* If we were already lingering, shut down for real. */
		tmr_cancel( c->linger_timer );
		c->linger_timer = (Timer*) 0;
		c->hc->should_linger = 0;
	}
	/**对于应该处于异常状态的*/
    if ( c->hc->should_linger )
	{
		/**对于连接状态不为暂停的处理
		 * 从set集合中删除此文件描述符
		*/
		if ( c->conn_state != CNST_PAUSING )
	    {
			fdwatch_del_fd( c->hc->conn_fd );
		}
		/**设置连接状态为CNST_LINGERING*/
		c->conn_state = CNST_LINGERING;
		shutdown( c->hc->conn_fd, SHUT_WR );
		fdwatch_add_fd( c->hc->conn_fd, c, FDW_READ );
		client_data.p = c;
		if ( c->linger_timer != (Timer*) 0 )
	    {
			syslog( LOG_ERR, "replacing non-null linger_timer!" );
		}
		c->linger_timer = tmr_create(tvP, linger_clear_connection, client_data, LINGER_TIME, 0 );
		if ( c->linger_timer == (Timer*) 0 )
	    {
	    	syslog( LOG_CRIT, "tmr_create(linger_clear_connection) failed" );
	    	exit( 1 );
	    }
	}
    else
	{
		really_clear_connection( c, tvP );
	}
}

/**彻底的关闭连接*/
static void really_clear_connection( connecttab* c, struct timeval* tvP )
{
    stats_bytes += c->hc->bytes_sent;
    if ( c->conn_state != CNST_PAUSING )
	{
		fdwatch_del_fd( c->hc->conn_fd );
	}
    httpd_close_conn( c->hc, tvP );
    clear_throttles( c, tvP );
    if ( c->linger_timer != (Timer*) 0 )
	{
		tmr_cancel( c->linger_timer );
		c->linger_timer = 0;
	}
    c->conn_state = CNST_FREE;
    c->next_free_connect = first_free_connect;
    first_free_connect = c - connects;	/* division by sizeof is implied */
    --num_connects;
}


static void idle( ClientData client_data, struct timeval* nowP )
{
    int cnum;
    connecttab* c;

    for ( cnum = 0; cnum < max_connects; ++cnum )
	{
		c = &connects[cnum];
		switch ( c->conn_state )
	    {
	    	case CNST_READING:
	    		if ( nowP->tv_sec - c->active_at >= IDLE_READ_TIMELIMIT )
				{
					syslog( LOG_INFO,"%.80s connection timed out reading",httpd_ntoa( &c->hc->client_addr ) );
					httpd_send_err(c->hc, 408, httpd_err408title, "", httpd_err408form, "" );
					finish_connection( c, nowP );
				}
	    		break;
	    	case CNST_SENDING:
	    	case CNST_PAUSING:
	    		if ( nowP->tv_sec - c->active_at >= IDLE_SEND_TIMELIMIT )
				{
					syslog( LOG_INFO,"%.80s connection timed out sending",httpd_ntoa( &c->hc->client_addr ) );
					clear_connection( c, nowP );
				}
	    		break;
	    }
	}
}

/**连接唤醒*/
static void wakeup_connection( ClientData client_data, struct timeval* nowP )
{
    connecttab* c;

    c = (connecttab*) client_data.p;
    c->wakeup_timer = (Timer*) 0;
    if ( c->conn_state == CNST_PAUSING )
	{
		c->conn_state = CNST_SENDING;
		fdwatch_add_fd( c->hc->conn_fd, c, FDW_WRITE );
	}
}
/**清除异常连接*/
static void linger_clear_connection( ClientData client_data, struct timeval* nowP )
{
    connecttab* c;

    c = (connecttab*) client_data.p;
    c->linger_timer = (Timer*) 0;
    really_clear_connection( c, nowP );
}

/**关联函数*/
static void occasional( ClientData client_data, struct timeval* nowP )
{
	/**设置内存上映射的文件需要进行是否清除处理*/
    mmc_cleanup( nowP );
    tmr_cleanup();
    watchdog_flag = 1;		/* let the watchdog know that we are alive */
}


#ifdef STATS_TIME
static void show_stats( ClientData client_data, struct timeval* nowP )
{
    logstats( nowP );
}
#endif /* STATS_TIME */


/* Generate debugging statistics syslog messages for all packages. */
static void logstats( struct timeval* nowP )
{
    struct timeval tv;
    time_t now;
    long up_secs, stats_secs;

    if ( nowP == (struct timeval*) 0 )
	{
		(void) gettimeofday( &tv, (struct timezone*) 0 );
		nowP = &tv;
	}
    now = nowP->tv_sec;
    up_secs = now - start_time;
    stats_secs = now - stats_time;
    if ( stats_secs == 0 )
	{
		stats_secs = 1;	/* fudge */
	}
    stats_time = now;
    syslog( LOG_NOTICE,
	"up %ld seconds, stats for %ld seconds:", up_secs, stats_secs );

    thttpd_logstats( stats_secs );
    httpd_logstats( stats_secs );
    mmc_logstats( stats_secs );
    fdwatch_logstats( stats_secs );
    tmr_logstats( stats_secs );
}


/* Generate debugging statistics syslog message. */
static void thttpd_logstats( long secs )
{
    if ( secs > 0 )
	{
		syslog( LOG_NOTICE,"  thttpd - %ld connections (%g/sec), %d max simultaneous, %lld bytes (%g/sec), %d httpd_conns allocated",stats_connections, (float) stats_connections / secs,stats_simultaneous, (long long) stats_bytes,(float) stats_bytes / secs, httpd_conn_count );
	}
    stats_connections = 0;
    stats_bytes = 0;
    stats_simultaneous = 0;
}
