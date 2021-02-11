/*
** $Id: lvm.h,v 2.39 2015/09/09 13:44:07 roberto Exp $
** Lua virtual machine
** See Copyright Notice in lua.h
*/

#ifndef lvm_h
#define lvm_h


#include "ldo.h"
#include "lobject.h"
#include "ltm.h"


#if !defined(LUA_NOCVTN2S)
#define cvt2str(o)	ttisnumber(o)
#else
#define cvt2str(o)	0	/* no conversion from numbers to strings */
#endif


#if !defined(LUA_NOCVTS2N)
#define cvt2num(o)	ttisstring(o)
#else
#define cvt2num(o)	0	/* no conversion from strings to numbers */
#endif


/*
** You can define LUA_FLOORN2I if you want to convert floats to integers
** by flooring them (instead of raising an error if they are not
** integral values)
*/
#if !defined(LUA_FLOORN2I)
#define LUA_FLOORN2I		0
#endif


#define tonumber(o,n) \
	(ttisfloat(o) ? (*(n) = fltvalue(o), 1) : luaV_tonumber_(o,n))

#define tointeger(o,i) \
    (ttisinteger(o) ? (*(i) = ivalue(o), 1) : luaV_tointeger(o,i,LUA_FLOORN2I))

/* 这是一个非常关键的宏定义,要对C的整型转换非常熟悉才行，尚未分析完，有时间要分析一遍 */
#define intop(op,v1,v2) l_castU2S(l_castS2U(v1) op l_castS2U(v2))

#define luaV_rawequalobj(t1,t2)		luaV_equalobj(NULL,t1,t2)


/*
** fast track for 'gettable': 1 means 'aux' points to resulted value;
** 0 means 'aux' is metamethod (if 't' is a table) or NULL. 'f' is
** the raw get function to use.
** 
** 尝试从t指向的table中用f函数查找k键对应的值，并赋值给aux
** 用f找不到时，则尝试查找t的原表的index方法，并赋值给aux
**
** 返回值分类 1：
**              a) t表中用f函数能找到k, 
**              b) t表的元表存在，但元表中的index元方法不存在
**            上述两种情况都意味着查找结束了！！！！！！！！且aux被赋了相应的值
**            0：
**             a) t:非表类型
**             b) t中没找到，但对应的元方法存在，还可以继续通过元方法找下去
*/
#define luaV_fastget(L,t,k,aux,f) \
  (!ttistable(t)  \
   ? (aux = NULL, 0)  /* not a table; 'aux' is NULL and result is 0 */  \
   : (aux = f(hvalue(t), k),  /* else, do raw access */  \
      !ttisnil(aux) ? 1  /* result not nil? 'aux' has it */  \
      : (aux = fasttm(L, hvalue(t)->metatable, TM_INDEX),  /* get metamethod */\
         aux != NULL  ? 0  /* has metamethod? must call it */  \
         : (aux = luaO_nilobject, 1))))  /* else, final result is nil */

/*
** standard implementation for 'gettable'
** 先尝试快速查找，找不到再尝试递归查找
*/
#define luaV_gettable(L,t,k,v) { const TValue *aux; \
  if (luaV_fastget(L,t,k,aux,luaH_get)) { setobj2s(L, v, aux); } \
  else luaV_finishget(L,t,k,v,aux); }


/*
** Fast track for set table. If 't' is a table and 't[k]' is not nil,
** call GC barrier, do a raw 't[k]=v', and return true; otherwise,
** return false with 'slot' equal to NULL (if 't' is not a table) or
** 'nil'. (This is needed by 'luaV_finishget'.) Note that, if the macro
** returns true, there is no need to 'invalidateTMcache', because the
** call is not creating a new entry.
**
** 仔细看实现，t为表，且若t[k] != nil,则slot=k对应的val值（k类型为String，要换成table的Key），再把v的值拷贝给slot所在的slot
**
** 返回值
**       1:表中key=t的键已存在
**       0:t不是表或key=t的键不存在
*/
#define luaV_fastset(L,t,k,slot,f,v) \
  (!ttistable(t) \
   ? (slot = NULL, 0) \
   : (slot = f(hvalue(t), k), \
     ttisnil(slot) ? 0 \
     : (luaC_barrierback(L, hvalue(t), v), \
        setobj2t(L, cast(TValue *,slot), v), \
        1)))


#define luaV_settable(L,t,k,v) { const TValue *slot; \
  if (!luaV_fastset(L,t,k,slot,luaH_get,v)) \
    luaV_finishset(L,t,k,v,slot); }
  


LUAI_FUNC int luaV_equalobj (lua_State *L, const TValue *t1, const TValue *t2);
LUAI_FUNC int luaV_lessthan (lua_State *L, const TValue *l, const TValue *r);
LUAI_FUNC int luaV_lessequal (lua_State *L, const TValue *l, const TValue *r);
LUAI_FUNC int luaV_tonumber_ (const TValue *obj, lua_Number *n);
LUAI_FUNC int luaV_tointeger (const TValue *obj, lua_Integer *p, int mode);
LUAI_FUNC void luaV_finishget (lua_State *L, const TValue *t, TValue *key,
                               StkId val, const TValue *tm);
LUAI_FUNC void luaV_finishset (lua_State *L, const TValue *t, TValue *key,
                               StkId val, const TValue *oldval);
LUAI_FUNC void luaV_finishOp (lua_State *L);
LUAI_FUNC void luaV_execute (lua_State *L);
LUAI_FUNC void luaV_concat (lua_State *L, int total);
LUAI_FUNC lua_Integer luaV_div (lua_State *L, lua_Integer x, lua_Integer y);
LUAI_FUNC lua_Integer luaV_mod (lua_State *L, lua_Integer x, lua_Integer y);
LUAI_FUNC lua_Integer luaV_shiftl (lua_Integer x, lua_Integer y);
LUAI_FUNC void luaV_objlen (lua_State *L, StkId ra, const TValue *rb);

#endif
