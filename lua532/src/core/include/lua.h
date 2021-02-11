/*
** $Id: lua.h,v 1.329 2015/11/13 17:18:42 roberto Exp $
** Lua - A Scripting Language
** Lua.org, PUC-Rio, Brazil (http://www.lua.org)
** See Copyright Notice at the end of this file
*/


#ifndef lua_h
#define lua_h

#include <stdarg.h>
#include <stddef.h>


#include "luaconf.h"


#define LUA_VERSION_MAJOR	"5"
#define LUA_VERSION_MINOR	"3"
#define LUA_VERSION_NUM		503
#define LUA_VERSION_RELEASE	"2"

#define LUA_VERSION	"Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
#define LUA_RELEASE	LUA_VERSION "." LUA_VERSION_RELEASE
#define LUA_COPYRIGHT	LUA_RELEASE "  Copyright (C) 1994-2015 Lua.org, PUC-Rio"
#define LUA_AUTHORS	"R. Ierusalimschy, L. H. de Figueiredo, W. Celes"


/* mark for precompiled code ('<esc>Lua') */
#define LUA_SIGNATURE	"\x1bLua"

/* option for multiple returns in 'lua_pcall' and 'lua_call' */
#define LUA_MULTRET	(-1)


/*
** Pseudo-indices
** (-LUAI_MAXSTACK is the minimum valid index; we keep some free empty
** space after that to help overflow detection)
** 
** 这里要是定义一个类似于 #define isRegisteryindex(e) (e ==LUA_REGISTRYINDEX) 就更好了
**
** LUA_REGISTRYINDEX-----index----->&G(L)->l_registry
*/
#define LUA_REGISTRYINDEX	(-LUAI_MAXSTACK - 1000)
#define lua_upvalueindex(i)	(LUA_REGISTRYINDEX - (i))


/* thread status */
#define LUA_OK		0
#define LUA_YIELD	1
#define LUA_ERRRUN	2
#define LUA_ERRSYNTAX	3	/* 词法，语法错误？ */
#define LUA_ERRMEM	4
#define LUA_ERRGCMM	5       /* error in __gc metamethod */
#define LUA_ERRERR	6		/* error in error handling */

typedef struct lua_State lua_State;


/*
** basic types
*/
#define LUA_TNONE		(-1)

#define LUA_TNIL		0
#define LUA_TBOOLEAN		1
#define LUA_TLIGHTUSERDATA	2
#define LUA_TNUMBER		3
#define LUA_TSTRING		4
#define LUA_TTABLE		5
#define LUA_TFUNCTION		6
#define LUA_TUSERDATA		7
#define LUA_TTHREAD		8

#define LUA_NUMTAGS		9



/* minimum Lua stack available to a C function */
#define LUA_MINSTACK	20


/* 
 * predefined values in the registry 
 * 全局global_State的注册表中两个非常重要的slot
 * globle_State.register = {[1] = mainThread, [2] = { 存放全局变量}}
 */
#define LUA_RIDX_MAINTHREAD	1
#define LUA_RIDX_GLOBALS	2
#define LUA_RIDX_LAST		LUA_RIDX_GLOBALS


/* type of numbers in Lua */
typedef LUA_NUMBER lua_Number;


/* type for integer functions */
typedef LUA_INTEGER lua_Integer;

/* unsigned integer type */
typedef LUA_UNSIGNED lua_Unsigned;

/* type for continuation-function contexts */
typedef LUA_KCONTEXT lua_KContext;


/*
** Type for C functions registered with Lua
** C被Lua调用的句柄原型
*/
typedef int (*lua_CFunction) (lua_State *L);

/*
** Type for continuation functions
** 断点续go函数原型？
*/
typedef int (*lua_KFunction) (lua_State *L, int status, lua_KContext ctx);


/*
** Type for functions that read/write blocks when loading/dumping Lua chunks
*/
typedef const char * (*lua_Reader) (lua_State *L, void *ud, size_t *sz);

typedef int (*lua_Writer) (lua_State *L, const void *p, size_t sz, void *ud);


/*
** Type for memory-allocation functions
** 控制性函数
**
** Lua允许编程者自己提供alloc函数，函数行为满足一定要求即可，具体规则后续再更新
** 大致相当于linux中的realloc？
*/
typedef void * (*lua_Alloc) (void *ud, void *ptr, size_t osize, size_t nsize);



/*
** generic extra include file
*/
#if defined(LUA_USER_H)
#include LUA_USER_H
#endif


/*
** RCS ident string
*/
extern const char lua_ident[];


/*
** state manipulation
*/
/* 构建一台虚拟机global_State以及一条执行线程lua_State */
LUA_API lua_State *(lua_newstate) (lua_Alloc f, void *ud);
/* 关闭虚拟机和所有的执行线程? */
LUA_API void       (lua_close) (lua_State *L);
/* 重启一条执行线程 */
LUA_API lua_State *(lua_newthread) (lua_State *L);
/* 设置panic点回调函数 */
LUA_API lua_CFunction (lua_atpanic) (lua_State *L, lua_CFunction panicf);
/* 这里返回了指针,预防静态链接问题以及中途值被修改的问题 */
LUA_API const lua_Number *(lua_version) (lua_State *L);


/*
** basic stack manipulation
*/
/* 求解有效的堆栈栈顶指针 */
LUA_API int   (lua_absindex) (lua_State *L, int idx);
/* 栈内已压入的参数个数(元素的函数指针不算) */
LUA_API int   (lua_gettop) (lua_State *L);
/* 
** 调整栈内参数个数, idx>=0：保留N个元素,idx<0:表示保留至倒数第N个参数
** 这里idx的参数意义实际上和absindex()中的一样，也和Lua接口文档一致
*/
LUA_API void  (lua_settop) (lua_State *L, int idx);
/* 拷贝一份idx指定的数据到栈顶并++top */
LUA_API void  (lua_pushvalue) (lua_State *L, int idx);
/* 旋转statck[idx,n]? */
LUA_API void  (lua_rotate) (lua_State *L, int idx, int n);
/* 拷贝数据fromidx->toidx */
LUA_API void  (lua_copy) (lua_State *L, int fromidx, int toidx);
/* 调整堆栈大小确保空闲的slot>=n */
LUA_API int   (lua_checkstack) (lua_State *L, int n);
/* 从一个执行线程的堆栈上移动n个元素到另外一个执行线程 */
LUA_API void  (lua_xmove) (lua_State *from, lua_State *to, int n);


/*
** access functions (stack -> C)
*/

/* 判断栈上元素是否为指定(或兼容)类型 */
LUA_API int             (lua_isnumber) (lua_State *L, int idx);
LUA_API int             (lua_isstring) (lua_State *L, int idx);
LUA_API int             (lua_iscfunction) (lua_State *L, int idx);
LUA_API int             (lua_isinteger) (lua_State *L, int idx);
LUA_API int             (lua_isuserdata) (lua_State *L, int idx);
// lua_istable 有对应的宏定义而不是函数
LUA_API int             (lua_type) (lua_State *L, int idx);
LUA_API const char     *(lua_typename) (lua_State *L, int tp);

/* 将栈上指定元素转到对应的类型 */
LUA_API lua_Number      (lua_tonumberx) (lua_State *L, int idx, int *isnum);
LUA_API lua_Integer     (lua_tointegerx) (lua_State *L, int idx, int *isnum);
LUA_API int             (lua_toboolean) (lua_State *L, int idx);
LUA_API const char     *(lua_tolstring) (lua_State *L, int idx, size_t *len);
LUA_API size_t          (lua_rawlen) (lua_State *L, int idx);
LUA_API lua_CFunction   (lua_tocfunction) (lua_State *L, int idx);
LUA_API void	       *(lua_touserdata) (lua_State *L, int idx);
LUA_API lua_State      *(lua_tothread) (lua_State *L, int idx);
LUA_API const void     *(lua_topointer) (lua_State *L, int idx);


/*
** Comparison and arithmetic functions
*/

#define LUA_OPADD	0	/* ORDER TM, ORDER OP */
#define LUA_OPSUB	1
#define LUA_OPMUL	2
#define LUA_OPMOD	3
#define LUA_OPPOW	4
#define LUA_OPDIV	5   /* /  */
#define LUA_OPIDIV	6   /* // */
#define LUA_OPBAND	7   /* &  */
#define LUA_OPBOR	8   /* |  */
#define LUA_OPBXOR	9   /* ^  */
#define LUA_OPSHL	10
#define LUA_OPSHR	11
#define LUA_OPUNM	12
#define LUA_OPBNOT	13

LUA_API void  (lua_arith) (lua_State *L, int op);

#define LUA_OPEQ	0		/* eqal       == */
#define LUA_OPLT	1		/* less than  < */
#define LUA_OPLE	2		/* less equal <= */
/* 大于符号的判断逻，调换相关操作数的位置或加一个前置的not即可 */

/* 比较idx1,idx2是否相等 */
LUA_API int   (lua_rawequal) (lua_State *L, int idx1, int idx2);
/* 比较两个obj相对(op)关系 */
LUA_API int   (lua_compare) (lua_State *L, int idx1, int idx2, int op);


/*
** push functions (C -> stack)
**
** 压入值到stack
*/
LUA_API void        (lua_pushnil) (lua_State *L);
LUA_API void        (lua_pushnumber) (lua_State *L, lua_Number n);
LUA_API void        (lua_pushinteger) (lua_State *L, lua_Integer n);
LUA_API const char * (lua_pushlstring) (lua_State *L, const char *s, size_t len);
LUA_API const char * (lua_pushstring) (lua_State *L, const char *s);
LUA_API const char * (lua_pushvfstring) (lua_State *L, const char *fmt,
                                                      va_list argp);
LUA_API const char *(lua_pushfstring) (lua_State *L, const char *fmt, ...);
LUA_API void  (lua_pushcclosure) (lua_State *L, lua_CFunction fn, int n);
LUA_API void  (lua_pushboolean) (lua_State *L, int b);
LUA_API void  (lua_pushlightuserdata) (lua_State *L, void *p);
LUA_API int   (lua_pushthread) (lua_State *L);


/*
** get functions (Lua -> stack)
*/
/* 获取全局表的某个域的值 */
LUA_API int (lua_getglobal) (lua_State *L, const char *name);
/* 查找由idx指定的table中L->top-1指定的key的val值，并把结果放到L->top-1上 */
LUA_API int (lua_gettable) (lua_State *L, int idx);
/* 查找由idx指定的table中,key=k的值，并把结果放到L->top-1上，返回type(val) */
LUA_API int (lua_getfield) (lua_State *L, int idx, const char *k);
/* 查找由idx指定的table中,key=n的值，并把结果放到L->top-1上，返回type(val) */
LUA_API int (lua_geti) (lua_State *L, int idx, lua_Integer n);
/* raw查找由idx指定的table中key=top-1的值，并将结果放到L->top-1上，返回type(val) */
LUA_API int (lua_rawget) (lua_State *L, int idx);
LUA_API int (lua_rawgeti) (lua_State *L, int idx, lua_Integer n);
LUA_API int (lua_rawgetp) (lua_State *L, int idx, const void *p);
/* 构建一个指定narray,nrec域大小的表并压入栈顶 */
LUA_API void  (lua_createtable) (lua_State *L, int narr, int nrec);
LUA_API void *(lua_newuserdata) (lua_State *L, size_t sz);
/* 提取由objindex指定的table或userdata的原表并压入栈顶(若存在) */
LUA_API int   (lua_getmetatable) (lua_State *L, int objindex);
/* 提取由idx指定的userdata中的userVal */
LUA_API int  (lua_getuservalue) (lua_State *L, int idx);


/*
** set functions (stack -> Lua)
*/
/* 设置全局表中域(name)的值L->top-1 */
LUA_API void  (lua_setglobal) (lua_State *L, const char *name);
/* 设置由idx指定的表中的key=L->top-2的val=L->top-1 */
LUA_API void  (lua_settable) (lua_State *L, int idx);
/* 设置由idx指定的表中的k的val=L->top-1 */
LUA_API void  (lua_setfield) (lua_State *L, int idx, const char *k);
LUA_API void  (lua_seti) (lua_State *L, int idx, lua_Integer n);
LUA_API void  (lua_rawset) (lua_State *L, int idx);
LUA_API void  (lua_rawseti) (lua_State *L, int idx, lua_Integer n);
LUA_API void  (lua_rawsetp) (lua_State *L, int idx, const void *p);
/* statck[objindex]->metatable = L->top-1 */
LUA_API int   (lua_setmetatable) (lua_State *L, int objindex);
/* statck[objindex]->.userVal = L->top-1 */
LUA_API void  (lua_setuservalue) (lua_State *L, int idx);


/*
** 'load' and 'call' functions (load and run Lua code)
*/
LUA_API void  (lua_callk) (lua_State *L, int nargs, int nresults,
                           lua_KContext ctx, lua_KFunction k);
/*
**调用一个函数，传入n个参数，并期待返回r个返回值
**调用前必须先把函数压栈，再把参数压栈，调用完成后，将结果依次压栈
**销毁函数指正和传入值？
*/
#define lua_call(L,n,r)		lua_callk(L, (n), (r), 0, NULL)
LUA_API int   (lua_pcallk) (lua_State *L, int nargs, int nresults, int errfunc,
                            lua_KContext ctx, lua_KFunction k);
#define lua_pcall(L,n,r,f)	lua_pcallk(L, (n), (r), (f), 0, NULL)

LUA_API int   (lua_load) (lua_State *L, lua_Reader reader, void *dt,
                          const char *chunkname, const char *mode);
LUA_API int (lua_dump) (lua_State *L, lua_Writer writer, void *data, int strip);


/*
** coroutine functions
*/
LUA_API int  (lua_yieldk)     (lua_State *L, int nresults, lua_KContext ctx,
                               lua_KFunction k);
LUA_API int  (lua_resume)     (lua_State *L, lua_State *from, int narg);
LUA_API int  (lua_status)     (lua_State *L);
LUA_API int (lua_isyieldable) (lua_State *L);

#define lua_yield(L,n)		lua_yieldk(L, (n), 0, NULL)


/*
** garbage-collection function and options
*/

#define LUA_GCSTOP		0
#define LUA_GCRESTART		1
#define LUA_GCCOLLECT		2
#define LUA_GCCOUNT		3
#define LUA_GCCOUNTB		4
#define LUA_GCSTEP		5
#define LUA_GCSETPAUSE		6
#define LUA_GCSETSTEPMUL	7
#define LUA_GCISRUNNING		9

LUA_API int (lua_gc) (lua_State *L, int what, int data);


/*
** miscellaneous(杂) functions
*/

LUA_API int   (lua_error) (lua_State *L);
/* 查找由idx指定的表中key=L->top-1的下一对key,val
** 若存在下一队key<->val则返回key,val,否则清空传入的key
*/
LUA_API int   lua_next (lua_State *L, int idx);
/* 将栈顶的n个元素拼接起来? */
LUA_API void  (lua_concat) (lua_State *L, int n);
/* 计算obj=stack[idx]的元素的长度 */
LUA_API void  (lua_len)    (lua_State *L, int idx);
/* 直接看实现 */
LUA_API size_t   (lua_stringtonumber) (lua_State *L, const char *s);
/* 获取，设置allocf */
LUA_API lua_Alloc (lua_getallocf) (lua_State *L, void **ud);
LUA_API void      (lua_setallocf) (lua_State *L, lua_Alloc f, void *ud);



/*
** {==============================================================
** some useful macros
** ===============================================================
*/

/* 看lua_newstate函数就明白了,提取一个额外空间的指针 */
#define lua_getextraspace(L)	((void *)((char *)(L) - LUA_EXTRASPACE))
/* 将i指向的栈顶元素转换为number或int并返回(转换失败则返回0) */
#define lua_tonumber(L,i)	lua_tonumberx(L,(i),NULL)
#define lua_tointeger(L,i)	lua_tointegerx(L,(i),NULL)

//弹掉n个栈顶元素
#define lua_pop(L,n)		lua_settop(L, -(n)-1)

//构建一张表
#define lua_newtable(L)		lua_createtable(L, 0, 0)
/* 做如下操作 register[name] = f */
#define lua_register(L,n,f) (lua_pushcfunction(L, (f)), lua_setglobal(L, (n)))

//压人一个不带上值的特定C函数，组成普通的闭包
#define lua_pushcfunction(L,f)	lua_pushcclosure(L, (f), 0)
/* 判断栈顶元素n是否为指定的类型 */
#define lua_isfunction(L,n)	(lua_type(L, (n)) == LUA_TFUNCTION)
#define lua_istable(L,n)	(lua_type(L, (n)) == LUA_TTABLE)
#define lua_islightuserdata(L,n)	(lua_type(L, (n)) == LUA_TLIGHTUSERDATA)
#define lua_isnil(L,n)		(lua_type(L, (n)) == LUA_TNIL)
#define lua_isboolean(L,n)	(lua_type(L, (n)) == LUA_TBOOLEAN)
#define lua_isthread(L,n)	(lua_type(L, (n)) == LUA_TTHREAD)
#define lua_isnone(L,n)		(lua_type(L, (n)) == LUA_TNONE)
#define lua_isnoneornil(L, n)	(lua_type(L, (n)) <= 0)
/* 压入一个常量字符串，形成一个TString */
#define lua_pushliteral(L, s)	lua_pushstring(L, "" s)
/* 提取全局表到栈顶 */
#define lua_pushglobaltable(L)  \
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS)
/* 将i指向的栈顶转换为TString,且存在i的原位置 */
#define lua_tostring(L,i)	lua_tolstring(L, (i), NULL)

/* 将栈顶元素插入到由idx指定的位置(即栈顶元素和指定idx交换下) */
#define lua_insert(L,idx)	lua_rotate(L, (idx), 1)
/* 移除idx指定的元素 */
#define lua_remove(L,idx)	(lua_rotate(L, (idx), -1), lua_pop(L, 1))
/* 将栈顶元素index[-1]拷贝到idx指定的位置，并删除栈顶元素 */
#define lua_replace(L,idx)	(lua_copy(L, -1, (idx)), lua_pop(L, 1))

/* }============================================================== */


/*
** {==============================================================
** compatibility macros for unsigned conversions
** ===============================================================
*/
#if defined(LUA_COMPAT_APIINTCASTS)

#define lua_pushunsigned(L,n)	lua_pushinteger(L, (lua_Integer)(n))
#define lua_tounsignedx(L,i,is)	((lua_Unsigned)lua_tointegerx(L,i,is))
#define lua_tounsigned(L,i)	lua_tounsignedx(L,(i),NULL)

#endif
/* }============================================================== */

/*
** {======================================================================
** Debug API
** =======================================================================
*/


/*
** Event codes
*/
#define LUA_HOOKCALL	0
#define LUA_HOOKRET	1
#define LUA_HOOKLINE	2
#define LUA_HOOKCOUNT	3
#define LUA_HOOKTAILCALL 4
/*
** Event masks
*/
#define LUA_MASKCALL	(1 << LUA_HOOKCALL)
#define LUA_MASKRET	(1 << LUA_HOOKRET)
#define LUA_MASKLINE	(1 << LUA_HOOKLINE)
#define LUA_MASKCOUNT	(1 << LUA_HOOKCOUNT)

typedef struct lua_Debug lua_Debug;  /* activation record */


/* Functions to be called by the debugger in specific events */
typedef void (*lua_Hook) (lua_State *L, lua_Debug *ar);


LUA_API int (lua_getstack) (lua_State *L, int level, lua_Debug *ar);
LUA_API int (lua_getinfo) (lua_State *L, const char *what, lua_Debug *ar);
LUA_API const char *(lua_getlocal) (lua_State *L, const lua_Debug *ar, int n);
LUA_API const char *(lua_setlocal) (lua_State *L, const lua_Debug *ar, int n);
LUA_API const char *(lua_getupvalue) (lua_State *L, int funcindex, int n);
LUA_API const char *(lua_setupvalue) (lua_State *L, int funcindex, int n);

LUA_API void *(lua_upvalueid) (lua_State *L, int fidx, int n);
LUA_API void  (lua_upvaluejoin) (lua_State *L, int fidx1, int n1,
                                               int fidx2, int n2);

LUA_API void (lua_sethook) (lua_State *L, lua_Hook func, int mask, int count);
LUA_API lua_Hook (lua_gethook) (lua_State *L);
LUA_API int (lua_gethookmask) (lua_State *L);
LUA_API int (lua_gethookcount) (lua_State *L);


struct lua_Debug {
  int event;
  const char *name;	/* (n) */
  const char *namewhat;	/* (n) 'global', 'local', 'field', 'method' */
  const char *what;	/* (S) 'Lua', 'C', 'main', 'tail' */
  const char *source;	/* (S) */
  int currentline;	/* (l) */
  int linedefined;	/* (S) */
  int lastlinedefined;	/* (S) */
  unsigned char nups;	/* (u) number of upvalues */
  unsigned char nparams;/* (u) number of parameters */
  char isvararg;        /* (u) */
  char istailcall;	/* (t) */
  char short_src[LUA_IDSIZE]; /* (S) */
  /* private part */
  struct CallInfo *i_ci;  /* active function */
};

/* }====================================================================== */


/******************************************************************************
* Copyright (C) 1994-2015 Lua.org, PUC-Rio.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/


#endif
