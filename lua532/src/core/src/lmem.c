/*
** $Id: lmem.c,v 1.91 2015/03/06 19:45:54 roberto Exp $
** Interface to Memory Manager
** See Copyright Notice in lua.h
*/

#define lmem_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"



/*
** About the realloc function:
** void * frealloc (void *ud, void *ptr, size_t osize, size_t nsize);
** ('osize' is the old size, 'nsize' is the new size)
**
** * frealloc(ud, NULL, x, s) creates a new block of size 's' (no
** matter 'x').
**
** * frealloc(ud, p, x, 0) frees the block 'p'
** (in this specific case, frealloc must return NULL);
** particularly, frealloc(ud, NULL, 0, 0) does nothing
** (which is equivalent to free(NULL) in ISO C)
**
** frealloc returns NULL if it cannot create or reallocate the area
** (any reallocation to an equal or smaller size cannot fail!)
*/



#define MINSIZEARRAY	4

/* 这个函数要弄明白、
** 
** 尝试扩容array类型的MEM
*/
void *luaM_growaux_ (lua_State *L, void *block, int *size, size_t size_elems,
                     int limit, const char *what) {
  void *newblock;
  int newsize;

  /* 这里的扩容规则简单易懂 */
  if (*size >= limit/2) {  /* cannot double it? */
    if (*size >= limit)  /* cannot grow even a little? */
      luaG_runerror(L, "too many %s (limit is %d)", what, limit);
    newsize = limit;  /* still have at least one free place */
  } else {
    newsize = (*size)*2;
    if (newsize < MINSIZEARRAY)
      newsize = MINSIZEARRAY;  /* minimum size */
  }
  
  newblock = luaM_reallocv(L, block, *size, newsize, size_elems);
  
  *size = newsize;  /* update only when everything else is OK,这个注释妙哉！ */
  return newblock;
}


l_noret luaM_toobig (lua_State *L) {
  luaG_runerror(L, "memory allocation error: block too big");
}



/*
** generic allocation routine.
** bolock ==nil:申请一片新MEM, osize一般是object'tag,nsize:则为实际申请的大小
** block~=nil:拓展或压缩旧MEM,osize是旧地址的大小
** 
** C语言中的realloc中会拷贝旧地址的数据到新地址中(LUA.API中设置内存分配函数时有此要求）
*/
void *luaM_realloc_(lua_State *L, void *block, size_t osize, size_t nsize) {
  void *newblock;
  global_State *g 	= G(L);
	
  size_t realosize 	= (block) ? osize : 0;
	/* block~=null则osize~=0 */
  lua_assert((realosize == 0) == (block == NULL));
	
#if defined(HARDMEMTESTS)
  if (nsize > realosize && g->gcrunning)
    luaC_fullgc(L, 1);  /* force a GC whenever possible */
#endif

  newblock = (*g->frealloc)(g->ud, block, osize, nsize);
  if (newblock == NULL && nsize > 0) {
		/* 缩小内存的行为不能失败 */
    lua_assert(nsize > realosize);  /* cannot fail when shrinking a block */
		
    if (g->version) {  		/* is state fully built? */
      luaC_fullgc(L, 1);  /* try to free some memory... */
      newblock = (*g->frealloc)(g->ud, block, osize, nsize);  /* try again */
    }
		/* 再试一次还是失败，抛出异常 */
    if (newblock == NULL) {
      luaD_throw(L, LUA_ERRMEM);
    }
  }
  lua_assert((nsize == 0) == (newblock == NULL));
  g->GCdebt = (g->GCdebt + nsize) - realosize;
  return newblock;
}

