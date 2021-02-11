/*
** $Id: lfunc.h,v 2.15 2015/01/13 15:49:11 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#ifndef lfunc_h
#define lfunc_h


#include "lobject.h"

//CClosure中已有1个TValue，故而这里需要进行(n-1)下同
#define sizeCclosure(n)	(cast(int, sizeof(CClosure)) + \
                         cast(int, sizeof(TValue)*((n)-1)))

#define sizeLclosure(n)	(cast(int, sizeof(LClosure)) + \
                         cast(int, sizeof(TValue *)*((n)-1)))


/* test whether thread is in 'twups' list */
#define isintwups(L)	(L->twups != L)


/*
** maximum number of upvalues in a closure (both C and Lua). (Value
** must fit in a VM register.)
*/
#define MAXUPVAL	255


/*
** Upvalues for Lua closures
** 引用的值尚在堆栈上存在时==open,v指向堆栈对应位置
** 值消失后，v指向内部的value
** 根据v和&u.value即可知道是open还是close
*/
struct UpVal {
  /* 
  ** points to stack or to its own value
  **
  ** when.open:v指向对应的mem,v.value:无效
  ** when.closed:value:保存了值，v也指向了u.value
  */
  TValue *v;  
  
  lu_mem refcount;  /* reference counter:被引用计数，降到0时被回收 */
  
  union {
    /* (when open)
    **  next域用来组成链表
    ** 值在v域中
    */
    struct {        
      UpVal *next;  /* linked list */
      int touched;  /* mark to avoid cycles with dead threads */
    } open;

     /* (when closed)
     **  the value  
     */
    TValue value;  
  } u;
};

#define upisopen(up)	((up)->v != &(up)->u.value)


LUAI_FUNC Proto *luaF_newproto (lua_State *L);
LUAI_FUNC CClosure *luaF_newCclosure (lua_State *L, int nelems);
LUAI_FUNC LClosure *luaF_newLclosure (lua_State *L, int nelems);
LUAI_FUNC void luaF_initupvals (lua_State *L, LClosure *cl);
LUAI_FUNC UpVal *luaF_findupval (lua_State *L, StkId level);
LUAI_FUNC void luaF_close (lua_State *L, StkId level);
LUAI_FUNC void luaF_freeproto (lua_State *L, Proto *f);
LUAI_FUNC const char *luaF_getlocalname (const Proto *func, int local_number,
                                         int pc);


#endif
