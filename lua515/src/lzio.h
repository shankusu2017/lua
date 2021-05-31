/*
** $Id: lzio.h,v 1.21.1.1 2007/12/27 13:02:25 roberto Exp $
** Buffered streams
** See Copyright Notice in lua.h
*/


#ifndef lzio_h
#define lzio_h

#include "lua.h"

#include "lmem.h"


#define EOZ	(-1)			/* end of stream */

typedef struct Zio ZIO;

#define char2int(c)	cast(int, cast(unsigned char, (c)))

#define zgetc(z)  (((z)->n--)>0 ?  char2int(*(z)->p++) : luaZ_fill(z))

/* 
** 独立的Buf,存储的数据是属于Mbuffer,而不单是个指针 
** 存放当前已解析出来的Token？
*/
typedef struct Mbuffer {
  char *buffer;		/* 数据指针 */
  size_t buffsize;	/* buf大小 */
  size_t n;			/* 已使用buf的长度,当n>=buffsize时，就需要resizeMbuffer了 */
} Mbuffer;

/* 设置Mbuffer的默认值，尚未申请内存和填充数据 */
#define luaZ_initbuffer(L, buff) ((buff)->buffer = NULL, (buff)->buffsize = 0)

#define luaZ_buffer(buff)	((buff)->buffer)
#define luaZ_sizebuffer(buff)	((buff)->buffsize)
#define luaZ_bufflen(buff)	((buff)->n)

#define luaZ_resetbuffer(buff) ((buff)->n = 0)


#define luaZ_resizebuffer(L, buff, size) \
	(luaM_reallocvector(L, (buff)->buffer, (buff)->buffsize, size, char), \
	(buff)->buffsize = size)

#define luaZ_freebuffer(L, buff)	luaZ_resizebuffer(L, buff, 0)


LUAI_FUNC char *luaZ_openspace (lua_State *L, Mbuffer *buff, size_t n);
LUAI_FUNC void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader,
                                        void *data);
LUAI_FUNC size_t luaZ_read (ZIO* z, void* b, size_t n);	/* read next n bytes */
LUAI_FUNC int luaZ_lookahead (ZIO *z);



/* --------- Private Part ------------------ */

struct Zio {
  lua_State *L;		  	/* Lua state (for reader) */

  /* 最底层的读句柄，以及用于读byte的辅助数据结构 */
  lua_Reader reader;	/* 底层读句柄，返回读取数据的结果cache以及读取的总数量 */
  void* data; 		  	/* additional data, type(LoadF) for reader */

  /* 指向上述被读出来的数据，及相关信息
  ** 上层的LexState从此处提取数据，这里做了一层对上面的封装
  */
  const char *p;		/* current position in buffer 下一个可读的pos */
  size_t n;				/* bytes still unread         未被读取的byte数量*/
};


LUAI_FUNC int luaZ_fill (ZIO *z);

#endif
