/*
** $Id: lua.h,v 1.332.1.2 2018/06/13 16:58:17 roberto Exp $
** Lua - A Scripting Language
** Lua.org, PUC-Rio, Brazil (http://www.lua.org)
** See Copyright Notice at the end of this file
*/


#ifndef lua_h
#define lua_h

#include <stdarg.h>
#include <stddef.h>


#include "luaconf.h"

// 版本信息
#define LUA_VERSION_MAJOR	"5"
#define LUA_VERSION_MINOR	"3"
#define LUA_VERSION_NUM		503
#define LUA_VERSION_RELEASE	"5"

#define LUA_VERSION	"Lua " LUA_VERSION_MAJOR "." LUA_VERSION_MINOR
#define LUA_RELEASE	LUA_VERSION "." LUA_VERSION_RELEASE
#define LUA_COPYRIGHT	LUA_RELEASE "  Copyright (C) 1994-2018 Lua.org, PUC-Rio"
#define LUA_AUTHORS	"R. Ierusalimschy, L. H. de Figueiredo, W. Celes"


/* mark for precompiled code ('<esc>Lua') */
/* 预编译lua代码文件的标记 */
#define LUA_SIGNATURE	"\x1bLua"

/* option for multiple returns in 'lua_pcall' and 'lua_call' */
#define LUA_MULTRET	(-1)


/*
** Pseudo-indices
** (-LUAI_MAXSTACK is the minimum valid index; we keep some free empty
** space after that to help overflow detection)
*/

/* 全局注册表和函数的自由变量用的都是伪索引。如果利用伪索引获取对应的地址，参考index2addr() */

/* 
** 伪栈索引，即这个索引所代表的地址并不在栈中。这个索引是lua中预定义的注册表的索引，
** C函数可以往这里存放任何想存放的Lua value。注册表是一个table，但是往这个table里面
** 添加元素的时候，键值不能用整数，因为整数的键值是被保留的。
*/
#define LUA_REGISTRYINDEX	(-LUAI_MAXSTACK - 1000)

/*
** 当一个C函数被调用的时候，它的所有自由变量都会存在某个特定的地方，宏lua_upvalueindex()
** 就是用来获取其自由变量的栈索引（其实也是一个伪栈索引）。例如某个被调用C函数有n个自由变量，
** 那么lua_upvalueindex(1)就是其第一个自由变量，lua_upvalueindex(n)是其第n个自由变量。
** C函数的自由变量是存放在对应的CClosure对象的upavlue数组成员中。
*/
#define lua_upvalueindex(i)	(LUA_REGISTRYINDEX - (i))


/* thread status */
#define LUA_OK		0
#define LUA_YIELD	1
#define LUA_ERRRUN	2
#define LUA_ERRSYNTAX	3
#define LUA_ERRMEM	4
#define LUA_ERRGCMM	5
#define LUA_ERRERR	6


typedef struct lua_State lua_State;


/*
** basic types
*/
#define LUA_TNONE		(-1)	// 无类型

#define LUA_TNIL		0		// 空类型
#define LUA_TBOOLEAN		1		// 布尔类型
#define LUA_TLIGHTUSERDATA	2		// 指针类型(void *)
#define LUA_TNUMBER		3		// 数据类型
#define LUA_TSTRING		4		// 字符串类型
#define LUA_TTABLE		5		// 表类型
#define LUA_TFUNCTION		6		// 函数类型
#define LUA_TUSERDATA		7		// 指针类型(void *)
#define LUA_TTHREAD		8		// Lua虚拟机、协程

#define LUA_NUMTAGS		9



/* minimum Lua stack available to a C function */
#define LUA_MINSTACK	20


/* predefined values in the registry */
#define LUA_RIDX_MAINTHREAD	1
/* _G表在栈索引值（伪索引）为LUA_REGISTRYINDEX的注册表中的索引 */
#define LUA_RIDX_GLOBALS	2
#define LUA_RIDX_LAST		LUA_RIDX_GLOBALS


/* type of numbers in Lua */
/* lua的number类型定义 */
typedef LUA_NUMBER lua_Number;


/* type for integer functions */
typedef LUA_INTEGER lua_Integer;

/* unsigned integer type */
typedef LUA_UNSIGNED lua_Unsigned;

/* type for continuation-function contexts */
typedef LUA_KCONTEXT lua_KContext;


/*
** Type for C functions registered with Lua
*/
/* 注册到lua中的C函数的原型定义。 */
typedef int (*lua_CFunction) (lua_State *L);

/*
** Type for continuation functions
*/
typedef int (*lua_KFunction) (lua_State *L, int status, lua_KContext ctx);


/*
** Type for functions that read/write blocks when loading/dumping Lua chunks
*/
typedef const char * (*lua_Reader) (lua_State *L, void *ud, size_t *sz);

typedef int (*lua_Writer) (lua_State *L, const void *p, size_t sz, void *ud);


/*
** Type for memory-allocation functions
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
LUA_API lua_State *(lua_newstate) (lua_Alloc f, void *ud);
LUA_API void       (lua_close) (lua_State *L);
LUA_API lua_State *(lua_newthread) (lua_State *L);

LUA_API lua_CFunction (lua_atpanic) (lua_State *L, lua_CFunction panicf);


LUA_API const lua_Number *(lua_version) (lua_State *L);


/*
** basic stack manipulation
*/
LUA_API int   (lua_absindex) (lua_State *L, int idx);
LUA_API int   (lua_gettop) (lua_State *L);
LUA_API void  (lua_settop) (lua_State *L, int idx);
LUA_API void  (lua_pushvalue) (lua_State *L, int idx);
LUA_API void  (lua_rotate) (lua_State *L, int idx, int n);
LUA_API void  (lua_copy) (lua_State *L, int fromidx, int toidx);
LUA_API int   (lua_checkstack) (lua_State *L, int n);

LUA_API void  (lua_xmove) (lua_State *from, lua_State *to, int n);


/*
** access functions (stack -> C)
*/

LUA_API int             (lua_isnumber) (lua_State *L, int idx);
LUA_API int             (lua_isstring) (lua_State *L, int idx);
LUA_API int             (lua_iscfunction) (lua_State *L, int idx);
LUA_API int             (lua_isinteger) (lua_State *L, int idx);
LUA_API int             (lua_isuserdata) (lua_State *L, int idx);
LUA_API int             (lua_type) (lua_State *L, int idx);
LUA_API const char     *(lua_typename) (lua_State *L, int tp);

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
#define LUA_OPDIV	5
#define LUA_OPIDIV	6
#define LUA_OPBAND	7
#define LUA_OPBOR	8
#define LUA_OPBXOR	9
#define LUA_OPSHL	10
#define LUA_OPSHR	11
#define LUA_OPUNM	12
#define LUA_OPBNOT	13

LUA_API void  (lua_arith) (lua_State *L, int op);

#define LUA_OPEQ	0
#define LUA_OPLT	1
#define LUA_OPLE	2

LUA_API int   (lua_rawequal) (lua_State *L, int idx1, int idx2);
LUA_API int   (lua_compare) (lua_State *L, int idx1, int idx2, int op);


/*
** push functions (C -> stack)
*/
LUA_API void        (lua_pushnil) (lua_State *L);
LUA_API void        (lua_pushnumber) (lua_State *L, lua_Number n);
LUA_API void        (lua_pushinteger) (lua_State *L, lua_Integer n);
LUA_API const char *(lua_pushlstring) (lua_State *L, const char *s, size_t len);
LUA_API const char *(lua_pushstring) (lua_State *L, const char *s);
LUA_API const char *(lua_pushvfstring) (lua_State *L, const char *fmt,
                                                      va_list argp);
LUA_API const char *(lua_pushfstring) (lua_State *L, const char *fmt, ...);
LUA_API void  (lua_pushcclosure) (lua_State *L, lua_CFunction fn, int n);
LUA_API void  (lua_pushboolean) (lua_State *L, int b);
LUA_API void  (lua_pushlightuserdata) (lua_State *L, void *p);
LUA_API int   (lua_pushthread) (lua_State *L);


/*
** get functions (Lua -> stack)
*/
LUA_API int (lua_getglobal) (lua_State *L, const char *name);
LUA_API int (lua_gettable) (lua_State *L, int idx);
LUA_API int (lua_getfield) (lua_State *L, int idx, const char *k);
LUA_API int (lua_geti) (lua_State *L, int idx, lua_Integer n);
LUA_API int (lua_rawget) (lua_State *L, int idx);
LUA_API int (lua_rawgeti) (lua_State *L, int idx, lua_Integer n);
LUA_API int (lua_rawgetp) (lua_State *L, int idx, const void *p);

LUA_API void  (lua_createtable) (lua_State *L, int narr, int nrec);
LUA_API void *(lua_newuserdata) (lua_State *L, size_t sz);
LUA_API int   (lua_getmetatable) (lua_State *L, int objindex);
LUA_API int  (lua_getuservalue) (lua_State *L, int idx);


/*
** set functions (stack -> Lua)
*/
LUA_API void  (lua_setglobal) (lua_State *L, const char *name);
LUA_API void  (lua_settable) (lua_State *L, int idx);
LUA_API void  (lua_setfield) (lua_State *L, int idx, const char *k);
LUA_API void  (lua_seti) (lua_State *L, int idx, lua_Integer n);
LUA_API void  (lua_rawset) (lua_State *L, int idx);
LUA_API void  (lua_rawseti) (lua_State *L, int idx, lua_Integer n);
LUA_API void  (lua_rawsetp) (lua_State *L, int idx, const void *p);
LUA_API int   (lua_setmetatable) (lua_State *L, int objindex);
LUA_API void  (lua_setuservalue) (lua_State *L, int idx);


/*
** 'load' and 'call' functions (load and run Lua code)
*/
LUA_API void  (lua_callk) (lua_State *L, int nargs, int nresults,
                           lua_KContext ctx, lua_KFunction k);

/* 
** lua中要调用某个函数时，就会用这个函数来执行具体的函数调用。n是参数个数，r是返回值个数。
** 函数的返回值在栈顶部（r个栈单元）。
*/
#define lua_call(L,n,r)		lua_callk(L, (n), (r), 0, NULL)

LUA_API int   (lua_pcallk) (lua_State *L, int nargs, int nresults, int errfunc,
                            lua_KContext ctx, lua_KFunction k);

/* 用保护模式来执行某个函数调用，其中待执行的函数指针可以由L->top - (n+1)得到 */
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
** miscellaneous functions
*/

LUA_API int   (lua_error) (lua_State *L);

LUA_API int   (lua_next) (lua_State *L, int idx);

LUA_API void  (lua_concat) (lua_State *L, int n);
LUA_API void  (lua_len)    (lua_State *L, int idx);

LUA_API size_t   (lua_stringtonumber) (lua_State *L, const char *s);

LUA_API lua_Alloc (lua_getallocf) (lua_State *L, void **ud);
LUA_API void      (lua_setallocf) (lua_State *L, lua_Alloc f, void *ud);



/*
** {==============================================================
** some useful macros
** ===============================================================
*/

#define lua_getextraspace(L)	((void *)((char *)(L) - LUA_EXTRASPACE))

#define lua_tonumber(L,i)	lua_tonumberx(L,(i),NULL)
#define lua_tointeger(L,i)	lua_tointegerx(L,(i),NULL)

#define lua_pop(L,n)		lua_settop(L, -(n)-1)

/*
** lua_createtable()函数用于创建一个table，然后压入堆栈，并根据narray和nrec对新创建的table
** 的数组部分或者hash表部分的进行大小更新。narray和nrec均为0，说明这个table是一个空table。
*/
#define lua_newtable(L)		lua_createtable(L, 0, 0)

/*
** 宏lua_register()用于将C函数注册到_G这个表中，键值为函数的名字。
** 先将函数对应的CClosure对象压入栈顶部，然后将位于栈顶部的这个CClosure对象添加到_G表中，
** 并将其弹出栈顶。
*/
#define lua_register(L,n,f) (lua_pushcfunction(L, (f)), lua_setglobal(L, (n)))

/* 将函数对应的CClosure对象压入栈顶部 */
#define lua_pushcfunction(L,f)	lua_pushcclosure(L, (f), 0)

#define lua_isfunction(L,n)	(lua_type(L, (n)) == LUA_TFUNCTION)
#define lua_istable(L,n)	(lua_type(L, (n)) == LUA_TTABLE)
#define lua_islightuserdata(L,n)	(lua_type(L, (n)) == LUA_TLIGHTUSERDATA)
#define lua_isnil(L,n)		(lua_type(L, (n)) == LUA_TNIL)
#define lua_isboolean(L,n)	(lua_type(L, (n)) == LUA_TBOOLEAN)
#define lua_isthread(L,n)	(lua_type(L, (n)) == LUA_TTHREAD)
#define lua_isnone(L,n)		(lua_type(L, (n)) == LUA_TNONE)
#define lua_isnoneornil(L, n)	(lua_type(L, (n)) <= 0)

#define lua_pushliteral(L, s)	lua_pushstring(L, "" s)

/* 从注册表中取出_G table，并压入栈顶部 */
#define lua_pushglobaltable(L)  \
	((void)lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS))

#define lua_tostring(L,i)	lua_tolstring(L, (i), NULL)


/* 
** 将栈顶部元素插入到栈索引值为idx的栈单元，同时将栈顶部和idx之间（包括索引值idx的栈单元）的
** 所有栈单元值依次往上挪一个栈单元。
*/
#define lua_insert(L,idx)	lua_rotate(L, (idx), 1)

#define lua_remove(L,idx)	(lua_rotate(L, (idx), -1), lua_pop(L, 1))

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
/*
** 触发钩子函数调用的事件
** call事件： 调用了一个函数的时候
** return事件：函数返回
** line事件：开始执行新的一行代码（接下来即将执行的指令是不是
**           对应了源代码文件中新的一行代码，是的话，则触发该事件）
** count事件：执行的指令数达到指定的数量
** tailcall事件：如果被调用的函数是一个尾调用，那么会触发该事件
*/
#define LUA_HOOKCALL	0
#define LUA_HOOKRET	1
#define LUA_HOOKLINE	2
#define LUA_HOOKCOUNT	3
#define LUA_HOOKTAILCALL 4


/*
** Event masks
*/
/* 触发钩子函数调用的事件掩码。 */
#define LUA_MASKCALL	(1 << LUA_HOOKCALL)
#define LUA_MASKRET	(1 << LUA_HOOKRET)
#define LUA_MASKLINE	(1 << LUA_HOOKLINE)
#define LUA_MASKCOUNT	(1 << LUA_HOOKCOUNT)

typedef struct lua_Debug lua_Debug;  /* activation record */


/* Functions to be called by the debugger in specific events */
/* 钩子函数的原型 */
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


/* 用于记录当前正在被调用的函数的一些信息。 */
struct lua_Debug {
  /* 触发钩子函数调用的事件。 */
  int event;

  /* 当前正在被调用的函数的名字 */
  const char *name;	/* (n) */

  /* 函数的角色 */
  const char *namewhat;	/* (n) 'global', 'local', 'field', 'method' */

  /* 
  ** 当前正在被调用的函数的类型，比如是不是lua函数，c函数，是不是一个chunk的主体，
  ** 是不是一个被尾调用的函数等
  */
  const char *what;	/* (S) 'Lua', 'C', 'main', 'tail' */

  /*
  ** 如果当前被调用的函数定义在字符串中，那么source就是这个字符串，
  ** 如果定义在文件中，那么source是一个以"@"开头的函数所在的文件名。
  */
  const char *source;	/* (S) */
  
  /* 触发钩子函数调用的代码行 */
  int currentline;	/* (l) */

  /* 函数定义开始处的行号 */
  int linedefined;	/* (S) */

  /* 函数定义结束处的行号 */
  int lastlinedefined;	/* (S) */

  /* 函数自由变量的个数 */
  unsigned char nups;	/* (u) number of upvalues */

  /* 函数参数的个数 */
  unsigned char nparams;/* (u) number of parameters */

  /* 函数是不是可变参数类型 */
  char isvararg;        /* (u) */

  /* 该函数是不是一个被尾调用的函数 */
  char istailcall;	/* (t) */
  char short_src[LUA_IDSIZE]; /* (S) */
  
  /* private part */
  /* 当前正在被调用函数对应的函数调用信息 */
  struct CallInfo *i_ci;  /* active function */
};

/* }====================================================================== */


/******************************************************************************
* Copyright (C) 1994-2018 Lua.org, PUC-Rio.
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
