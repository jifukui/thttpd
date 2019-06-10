/* mmc.c - mmap cache
**
** Copyright � 1998,2001,2014 by Jef Poskanzer <jef@mail.acme.com>.
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

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <syslog.h>
#include <errno.h>

#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif /* HAVE_MMAP */

#include "mmc.h"
#include "libhttpd.h"

#ifndef HAVE_INT64T
typedef long long int64_t;
#endif


/* Defines. */
#ifndef DEFAULT_EXPIRE_AGE
/**设置默认的到期时间为600*/
#define DEFAULT_EXPIRE_AGE 600
#endif
#ifndef DESIRED_FREE_COUNT
#define DESIRED_FREE_COUNT 100
#endif
#ifndef DESIRED_MAX_MAPPED_FILES
#define DESIRED_MAX_MAPPED_FILES 2000
#endif
#ifndef DESIRED_MAX_MAPPED_BYTES
#define DESIRED_MAX_MAPPED_BYTES 1000000000
#endif
#ifndef INITIAL_HASH_SIZE
#define INITIAL_HASH_SIZE (1 << 10)
#endif

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif


/* The Map struct. */
typedef struct MapStruct {
    ino_t ino;						/**文件索引节点号*/
    dev_t dev;						/**文件使用的设备号*/
    off_t size;						/**文件大小*/
    time_t ct;						/**文件最后一次修改时间*/
    int refcount;					/**引用次数*/
    time_t reftime;					/**引用时间*/
    void* addr;						/**地址*/
    unsigned int hash;				/**hash值*/
    int hash_idx;					/**hash索引值*/
    struct MapStruct* next;			/**下一个指针*/
} Map;


/* Globals. */
static Map* maps = (Map*) 0;							/**映射的地址*/
static Map* free_maps = (Map*) 0;						/**释放映射的地址*/
static int alloc_count = 0;								/**申请内存的数量*/
static int map_count = 0;								/**映射的数量*/
static int free_count = 0;								/**空闲的数量*/
static Map** hash_table = (Map**) 0;					/**映射表*/
static int hash_size;									/**hash表的大小*/
static unsigned int hash_mask;							/**hash表的掩码*/
static time_t expire_age = DEFAULT_EXPIRE_AGE;			/**雇佣时间*/
static off_t mapped_bytes = 0;							/**映射的字节数*/



/* Forwards. */
static void panic( void );
static void really_unmap( Map** mm );
static int check_hash_size( void );
static int add_hash( Map* m );
static Map* find_hash( ino_t ino, dev_t dev, off_t size, time_t ct );
static unsigned int hash( ino_t ino, dev_t dev, off_t size, time_t ct );

/**内存映射
 * filename文件名称
 * sbP文件属性
 * nowP当前时间
*/
void* mmc_map( char* filename, struct stat* sbP, struct timeval* nowP )
{
    time_t now;
    struct stat sb;
    Map* m;
    int fd;

    /* Stat the file, if necessary. */
	/**对于传入的sbp已经被分配过内存的处理
	 * 设置sb使用此处内存
	*/
    if ( sbP != (struct stat*) 0 )
	{
		sb = *sbP;
	}
	/**对于传入的sbp没有被分配过内存的处理*/
    else
	{
		/**对于文件存在的处理*/
		if ( stat( filename, &sb ) != 0 )
	    {
	    	syslog( LOG_ERR, "stat - %m" );
	    	return (void*) 0;
	    }
	}

    /* Get the current time, if necessary. */
	/**获取当前时间*/
    if ( nowP != (struct timeval*) 0 )
	{
		now = nowP->tv_sec;
	}
    else
	{
		now = time( (time_t*) 0 );
	}

    /* See if we have it mapped already, via the hash table. */
	/**检测内存空间是否足够使用*/
    if ( check_hash_size() < 0 )
	{
		syslog( LOG_ERR, "check_hash_size() failure" );
		return (void*) 0;
	}
	/**寻找此的值的映射文件信息*/
    m = find_hash( sb.st_ino, sb.st_dev, sb.st_size, sb.st_ctime );
	/**对于找到的直接返回映射的地址*/
    if ( m != (Map*) 0 )
	{
		/* Yep.  Just return the existing map */
		++m->refcount;
		m->reftime = now;
		return m->addr;
	}

    /* Open the file. */
	/**对于没有找到的打开文件*/
    fd = open( filename, O_RDONLY );
    if ( fd < 0 )
	{
		syslog( LOG_ERR, "open - %m" );
		return (void*) 0;
	}

    /* Find a free Map entry or make a new one. */
	/**对于打开文件成功且没有被映射的处理*/
    if ( free_maps != (Map*) 0 )
	{
		m = free_maps;
		free_maps = m->next;
		--free_count;
	}
    else
	{
		m = (Map*) malloc( sizeof(Map) );
		if ( m == (Map*) 0 )
	    {
	    	(void) close( fd );
	    	syslog( LOG_ERR, "out of memory allocating a Map" );
	    	return (void*) 0;
	    }
		++alloc_count;
	}

    /* Fill in the Map entry. */
    m->ino = sb.st_ino;			//文件索引节点号
    m->dev = sb.st_dev;			//文件使用的设备号
    m->size = sb.st_size;		//文件大小信息
    m->ct = sb.st_ctime;		//文件最后一次修改时间
    m->refcount = 1;			//设置文件的引用数量
    m->reftime = now;			//设置文件的引用时间

    /* Avoid doing anything for zero-length files; some systems don't like
    ** to mmap them, other systems dislike mallocing zero bytes.
    */
   	/**对于文件的大小为0的处理*/
    if ( m->size == 0 )
	{
		m->addr = (void*) 1;	/* arbitrary non-NULL address */
	}
    else
	{
		size_t size_size = (size_t) m->size;	/* loses on files >2GB */
#ifdef HAVE_MMAP
		/* Map the file into memory. */
		/**将文件映射到内存中
		 * 设置映射区的起始地址由系统决定
		 * 最后返回映射的地址
		*/
		m->addr = mmap( 0, size_size, PROT_READ, MAP_PRIVATE, fd, 0 );
		/**对于映射错误的处理*/
		if ( m->addr == (void*) -1 && errno == ENOMEM )
	    {
	    	/* Ooo, out of address space.  Free all unreferenced maps
	    	** and try again.
	    	*/
			/**释放引用值为0的映射*/
	    	panic();
			/**再次进行映射*/
	    	m->addr = mmap( 0, size_size, PROT_READ, MAP_PRIVATE, fd, 0 );
	    }
		/**最终的对于映射失败的处理*/
		if ( m->addr == (void*) -1 )
	    {
	    	syslog( LOG_ERR, "mmap - %m" );
	    	(void) close( fd );
	    	free( (void*) m );
	    	--alloc_count;
	    	return (void*) 0;
	    }
#else /* HAVE_MMAP */
		/* Read the file into memory. */
		m->addr = (void*) malloc( size_size );
		if ( m->addr == (void*) 0 )
	    {
	    	/* Ooo, out of memory.  Free all unreferenced maps
	    	** and try again.
	    	*/
	    	panic();
	    	m->addr = (void*) malloc( size_size );
	    }
		if ( m->addr == (void*) 0 )
	    {
	    	syslog( LOG_ERR, "out of memory storing a file" );
	    	(void) close( fd );
	    	free( (void*) m );
	    	--alloc_count;
	    	return (void*) 0;
	    }
		if ( httpd_read_fully( fd, m->addr, size_size ) != size_size )
	    {
	    	syslog( LOG_ERR, "read - %m" );
	    	(void) close( fd );
	    	free( (void*) m );
	    	--alloc_count;
	    	return (void*) 0;
	    }
#endif /* HAVE_MMAP */
	}
    (void) close( fd );

    /* Put the Map into the hash table. */
	/**添加到hash表中失败的处理*/
    if ( add_hash( m ) < 0 )
	{
		syslog( LOG_ERR, "add_hash() failure" );
		free( (void*) m );
		--alloc_count;
		return (void*) 0;
	}

    /* Put the Map on the active list. */
    m->next = maps;
    maps = m;
    ++map_count;

    /* Update the total byte count. */
    mapped_bytes += m->size;

    /* And return the address. */
	/**返回映射的地址*/
    return m->addr;
}

/***/
void mmc_unmap( void* addr, struct stat* sbP, struct timeval* nowP )
{
    Map* m = (Map*) 0;

    /* Find the Map entry for this address.  First try a hash. */
	/**对于文件状态值不为空的处理*/
    if ( sbP != (struct stat*) 0 )
	{
		/**获取索引值*/
		m = find_hash( sbP->st_ino, sbP->st_dev, sbP->st_size, sbP->st_ctime );
		/**对于映射对应不为0且文件地址与映射地址不一致的处理
		 * 设置映射对象的的内存地址为空
		*/
		if ( m != (Map*) 0 && m->addr != addr )
	    {
			m = (Map*) 0;
		}
	}
    /* If that didn't work, try a full search. */
	/**对于映射的地址为空的处理*/
    if ( m == (Map*) 0 )
	{
		/**找到maps地址中映射的地址与传入的地址一致的映射对象*/
		for ( m = maps; m != (Map*) 0; m = m->next )
	    {
			if ( m->addr == addr )
			{
				break;
			}
		}
	}
	/**对于寻找到的映射对象的值为空的处理*/
    if ( m == (Map*) 0 )
	{
		syslog( LOG_ERR, "mmc_unmap failed to find entry!" );
	}
	/**对于寻找的的映射对象的值的引用为不大于0的处理*/
    else if ( m->refcount <= 0 )
	{
		syslog( LOG_ERR, "mmc_unmap found zero or negative refcount!" );
	}
	/**对于找到的映射文件的存在且引用值大于0的处理
	 * 更新引用时间
	*/
    else
	{
		--m->refcount;
		if ( nowP != (struct timeval*) 0 )
	    {
			m->reftime = nowP->tv_sec;
		}
		else
	    {
			m->reftime = time( (time_t*) 0 );
		}
	}
}

/**定时清除引用值为0且超过雇佣时间的文件映射的内存*/
void mmc_cleanup( struct timeval* nowP )
{
    time_t now;
    Map** mm;
    Map* m;

    /* Get the current time, if necessary. */
    if ( nowP != (struct timeval*) 0 )
	{
		now = nowP->tv_sec;
	}
    else
	{
		now = time( (time_t*) 0 );
	}

    /* Really unmap any unreferenced entries older than the age limit. */
    for ( mm = &maps; *mm != (Map*) 0; )
	{
		m = *mm;
		if ( m->refcount == 0 && now - m->reftime >= expire_age )
	    {
			really_unmap( mm );
		}
		else
	    {
			mm = &(*mm)->next;
		}
	}

    /* Adjust the age limit if there are too many bytes mapped, or
    ** too many or too few files mapped.
    */
    if ( mapped_bytes > DESIRED_MAX_MAPPED_BYTES )
	{
		expire_age = MAX( ( expire_age * 2 ) / 3, DEFAULT_EXPIRE_AGE / 10 );
	}
    else if ( map_count > DESIRED_MAX_MAPPED_FILES )
	{
		expire_age = MAX( ( expire_age * 2 ) / 3, DEFAULT_EXPIRE_AGE / 10 );
	}
    else if ( map_count < DESIRED_MAX_MAPPED_FILES / 2 )
	{
		expire_age = MIN( ( expire_age * 5 ) / 4, DEFAULT_EXPIRE_AGE * 3 );
	}

    /* Really free excess blocks on the free list. */
    while ( free_count > DESIRED_FREE_COUNT )
	{
		m = free_maps;
		free_maps = m->next;
		--free_count;
		free( (void*) m );
		--alloc_count;
	}
}

/**释放引用值为0的映射*/
static void panic( void )
{
    Map** mm;
    Map* m;

    syslog( LOG_ERR, "mmc panic - freeing all unreferenced maps" );

    /* Really unmap all unreferenced entries. */
    for ( mm = &maps; *mm != (Map*) 0; )
	{
		m = *mm;
		if ( m->refcount == 0 )
	    {
			really_unmap( mm );
		}
		else
	    {
			mm = &(*mm)->next;
		}
	}
}


static void really_unmap( Map** mm )
{
    Map* m;

    m = *mm;
    if ( m->size != 0 )
	{
#ifdef HAVE_MMAP
		if ( munmap( m->addr, m->size ) < 0 )
	    {
			syslog( LOG_ERR, "munmap - %m" );
		}
#else /* HAVE_MMAP */
		free( (void*) m->addr );
#endif /* HAVE_MMAP */
	}
    /* Update the total byte count. */
    mapped_bytes -= m->size;
    /* And move the Map to the free list. */
    *mm = m->next;
    --map_count;
    m->next = free_maps;
    free_maps = m;
    ++free_count;
    /* This will sometimes break hash chains, but that's harmless; the
    ** unmapping code that searches the hash table knows to keep searching.
    */
    hash_table[m->hash_idx] = (Map*) 0;
}

/**释放hash申请的内存空间*/
void mmc_term( void )
{
    Map* m;

    while ( maps != (Map*) 0 )
	{
		really_unmap( &maps );
	}
    while ( free_maps != (Map*) 0 )
	{
		m = free_maps;
		free_maps = m->next;
		--free_count;
		free( (void*) m );
		--alloc_count;
	}
}


/* Make sure the hash table is big enough. */
/**确定映射空间足够使用，对于未初始化和不够的进行内存申请*/
static int check_hash_size( void )
{
    int i;
    Map* m;

    /* Are we just starting out? */
	/**对于hash表未使用过的处理*/
    if ( hash_table == (Map**) 0 )
	{
		/**初始化hash表的大小*/
		hash_size = INITIAL_HASH_SIZE;
		/**设置Hash表的掩码*/
		hash_mask = hash_size - 1;
	}
    /* Is it at least three times bigger than the number of entries? */
	/**对于hash表的大小大于映射文件的数量的三倍*/
    else if ( hash_size >= map_count * 3 )
	{
		return 0;
	}
	/**如果大小不够大进行扩容*/
    else
	{
		/* No, got to expand. */
		free( (void*) hash_table );
		/* Double the hash size until it's big enough. */
		do{
	    	hash_size = hash_size << 1;
	    }while ( hash_size < map_count * 6 );
		hash_mask = hash_size - 1;
	}
    /* Make the new table. */
    hash_table = (Map**) malloc( hash_size * sizeof(Map*) );
    if ( hash_table == (Map**) 0 )
	{
		return -1;
	}
    /* Clear it. */
    for ( i = 0; i < hash_size; ++i )
	{
		hash_table[i] = (Map*) 0;
	}
    /* And rehash all entries. */
    for ( m = maps; m != (Map*) 0; m = m->next )
	{
		if ( add_hash( m ) < 0 )
	    {
			return -1;
		}
	}
    return 0;
}

/** 向hash表中添加数据*/
static int add_hash( Map* m )
{
    unsigned int h, he, i;
	/**计算hash值*/
    h = hash( m->ino, m->dev, m->size, m->ct );
    he = ( h + hash_size - 1 ) & hash_mask;
    for ( i = h; ; i = ( i + 1 ) & hash_mask )
	{
		if ( hash_table[i] == (Map*) 0 )
	    {
	    	hash_table[i] = m;
	 		m->hash = h;
	    	m->hash_idx = i;
	    	return 0;
	    }
		if ( i == he )
	    {
			break;
		}
	}
    return -1;
}

/**根据传入的信息计算hash值返回在hash表中寻找的结果
 * ino:文件索引号
 * dev：文件使用的设备号
 * size：文件大小
 * ct：最后一次改变文件状态的时间
*/
static Map* find_hash( ino_t ino, dev_t dev, off_t size, time_t ct )
{
    unsigned int h, he, i;
    Map* m;
	/**根据文件信息计算文件的hash值*/
    h = hash( ino, dev, size, ct );
	/***/
    he = ( h + hash_size - 1 ) & hash_mask;
	/***/
    for ( i = h; ; i = ( i + 1 ) & hash_mask )
	{
		m = hash_table[i];
		if ( m == (Map*) 0 )
	    {
			break;
		}
		if ( m->hash == h && m->ino == ino && m->dev == dev &&m->size == size && m->ct == ct )
	    {
			return m;
		}
		if ( i == he )
	    {
			break;
		}
	}
    return (Map*) 0;
}

/**根据传入的数据计算hash值
 * ino:文件索引号
 * dev：文件使用的设备号
 * size：文件大小
 * ct：最后一次改变文件状态的时间
*/
static unsigned int hash( ino_t ino, dev_t dev, off_t size, time_t ct )
{
    unsigned int h = 177573;

    h ^= ino;
    h += h << 5;
    h ^= dev;
    h += h << 5;
    h ^= size;
    h += h << 5;
    h ^= ct;

    return h & hash_mask;
}


/* Generate debugging statistics syslog message. */
/**打印内存映射状态信息*/
void mmc_logstats( long secs )
{
    syslog(
	LOG_NOTICE, "  map cache - %d allocated, %d active (%lld bytes), %d free; hash size: %d; expire age: %lld",
	alloc_count, map_count, (long long) mapped_bytes, free_count, hash_size,(long long) expire_age );
    if ( map_count + free_count != alloc_count )
	{
		syslog( LOG_ERR, "map counts don't add up!" );
	}
}
