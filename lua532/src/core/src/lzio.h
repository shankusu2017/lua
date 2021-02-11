/*
** $Id: lzio.h,v 1.31 2015/09/08 15:41:05 roberto Exp $
** Buffered streams
** See Copyright Notice in lua.h
*/


#ifndef lzio_h
#define lzio_h

#include "lua.h"

#include "lmem.h"


#define EOZ	(-1)			/* end of stream */

typedef struct Zio ZIO;

/* 
 * 从 Zio 本身的buf中读一个字节，
 * 若本身的buf空了，则调用reader读一片mem到本省的buf，且返回一个字节
*/
#define zgetc(z)  (((z)->n--)>0 ?  cast_uchar(*(z)->p++) : luaZ_fill(z))

/* 处理单个token的buffer
** 一个标识符eg:local var = "people"
** 上面这句话中var, people就属于多个字符组成的标识符，这里就需要用到buffer了
*/
typedef struct Mbuffer {
  char *buffer;		  /* addr */
  size_t n;			    /* 读入的有效字符个数 */
  size_t buffsize;	/* buf的总长 */
} Mbuffer;

#define luaZ_initbuffer(L, buff) ((buff)->buffer = NULL, (buff)->buffsize = 0)

#define luaZ_buffer(buff)	((buff)->buffer)
#define luaZ_sizebuffer(buff)	((buff)->buffsize)
#define luaZ_bufflen(buff)	((buff)->n)

#define luaZ_buffremove(buff,i)	((buff)->n -= (i))
#define luaZ_resetbuffer(buff) ((buff)->n = 0)

/* realloc会拷贝旧地址的数据到新地址上 */
#define luaZ_resizebuffer(L, buff, size) \
	((buff)->buffer = luaM_reallocvchar(L, (buff)->buffer, \
				(buff)->buffsize, size), \
	(buff)->buffsize = size)

#define luaZ_freebuffer(L, buff)	luaZ_resizebuffer(L, buff, 0)

/* 初始化ZIO */
LUAI_FUNC void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader,
                                        void *data);
LUAI_FUNC size_t luaZ_read (ZIO* z, void *b, size_t n);	/* read next n bytes */



/* --------- Private Part ------------------ */

struct Zio {
  size_t n;				/* bytes still unread */
  const char *p;		/* current position in buffer */
  
  lua_Reader reader;	/* reader function */
  void *data;			/* additional(附带的) data，此域可能是结构体，被reader使用，包含FILE...? */
  lua_State *L;			/* Lua state (for reader) */
};


LUAI_FUNC int luaZ_fill (ZIO *z);

#endif
