/*
** $Id: lauxlib.h,v 1.129 2015/11/23 11:29:43 roberto Exp $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/


#ifndef lauxlib_h
#define lauxlib_h


#include <stddef.h>
#include <stdio.h>

#include "lua.h"



/* extra error code for 'luaL_load' */
#define LUA_ERRFILE     (LUA_ERRERR+1)


typedef struct luaL_Reg {
  const char *name;
  lua_CFunction func;
} luaL_Reg;


#define LUAL_NUMSIZES	(sizeof(lua_Integer)*16 + sizeof(lua_Number))

LUALIB_API void (luaL_checkversion_) (lua_State *L, lua_Number ver, size_t sz);
#define luaL_checkversion(L)  \
	  luaL_checkversion_(L, LUA_VERSION_NUM, LUAL_NUMSIZES)
/* 提取obj指定的表的原表的e的域的值 */
LUALIB_API int (luaL_getmetafield) (lua_State *L, int obj, const char *e);
/* 以obj为参数调用obj的元方法e */
LUALIB_API int (luaL_callmeta) (lua_State *L, int obj, const char *e);
/* 将idx指向的对象转为TString(放到栈顶),len:转换后的TString长度，返回值表示首地址 */
LUALIB_API const char *(luaL_tolstring) (lua_State *L, int idx, size_t *len);
LUALIB_API int (luaL_argerror) (lua_State *L, int arg, const char *extramsg);
/* 上面的加强版，arg必须能转为TString */
LUALIB_API const char *(luaL_checklstring) (lua_State *L, int arg,
                                                          size_t *l);
/* luaL_tolstring的提供默认字符串的版本 */
LUALIB_API const char *(luaL_optlstring) (lua_State *L, int arg,
                                          const char *def, size_t *l);
/* 核查指定参数是否为number */
LUALIB_API lua_Number (luaL_checknumber) (lua_State *L, int arg);
LUALIB_API lua_Number (luaL_optnumber) (lua_State *L, int arg, lua_Number def);

LUALIB_API lua_Integer (luaL_checkinteger) (lua_State *L, int arg);
LUALIB_API lua_Integer (luaL_optinteger) (lua_State *L, int arg,
                                          lua_Integer def);
/* 堆栈是有由sz的剩余空间 */
LUALIB_API void (luaL_checkstack) (lua_State *L, int sz, const char *msg);
/* arg指定的堆栈的元素类型是否为要求的t */
LUALIB_API void (luaL_checktype) (lua_State *L, int arg, int t);
/* arg指定的堆栈元素的类型是否合法 */
LUALIB_API void (luaL_checkany) (lua_State *L, int arg);
/* 
** 若注册表register[tname]存在则提取值到栈顶并返回0，
** 反之构建一个newTable={__name=tname,}的新表并执行register[tname]=newTable
** 并保留newTable到栈顶上
*/
LUALIB_API int   (luaL_newmetatable) (lua_State *L, const char *tname);
/* 先压入要被设置的表到栈顶，再用rigeter[tname]的val来更新被压入的表元表域 */
LUALIB_API void  (luaL_setmetatable) (lua_State *L, const char *tname);

LUALIB_API void *(luaL_testudata) (lua_State *L, int ud, const char *tname);
LUALIB_API void *(luaL_checkudata) (lua_State *L, int ud, const char *tname);
/* 打印lvl对应的文件名和行数 */
LUALIB_API void (luaL_where) (lua_State *L, int lvl);
/* 打印错误 */
LUALIB_API int (luaL_error) (lua_State *L, const char *fmt, ...);
/* 缺省参数是否在lst中 */
LUALIB_API int (luaL_checkoption) (lua_State *L, int arg, const char *def,
                                   const char *const lst[]);
/* 直接看源码,看error的相关信息？ */
LUALIB_API int (luaL_fileresult) (lua_State *L, int stat, const char *fname);
LUALIB_API int (luaL_execresult) (lua_State *L, int stat);

/* predefined references */
#define LUA_NOREF       (-2)
#define LUA_REFNIL      (-1)

/* 不太明白这组函数的用意 */
LUALIB_API int (luaL_ref) (lua_State *L, int t);
LUALIB_API void (luaL_unref) (lua_State *L, int t, int ref);

LUALIB_API int (luaL_loadfilex) (lua_State *L, const char *filename,
                                               const char *mode);

#define luaL_loadfile(L,f)	luaL_loadfilex(L,f,NULL)

LUALIB_API int (luaL_loadbufferx) (lua_State *L, const char *buff, size_t sz,
                                   const char *name, const char *mode);
LUALIB_API int (luaL_loadstring) (lua_State *L, const char *s);

/* 构造一台新的虚拟机和执行线程 */
LUALIB_API lua_State *(luaL_newstate) (void);
/* 计算指定元素的长度 */
LUALIB_API lua_Integer (luaL_len) (lua_State *L, int idx);

LUALIB_API const char *(luaL_gsub) (lua_State *L, const char *s, const char *p,
                                                  const char *r);
/* 已有一张表tbl1,将由luaL_Reg元素组成的数组，生成Clouse,并填入tbl1中 tbl1[l->name] = Clouse
** 特定环境，外围库调用
*/
LUALIB_API void (luaL_setfuncs) (lua_State *L, const luaL_Reg *l, int nup);

LUALIB_API int (luaL_getsubtable) (lua_State *L, int idx, const char *fname);

LUALIB_API void (luaL_traceback) (lua_State *L, lua_State *L1,
                                  const char *msg, int level);

LUALIB_API void (luaL_requiref) (lua_State *L, const char *modname,
                                 lua_CFunction openf, int glb);

/*
** ===============================================================
** some useful macros
** ===============================================================
*/

/* 构建一个node.size为指定大小的table */
#define luaL_newlibtable(L,l)	\
  lua_createtable(L, 0, sizeof(l)/sizeof((l)[0]) - 1)

#define luaL_newlib(L,l)  \
  (luaL_checkversion(L), luaL_newlibtable(L,l), luaL_setfuncs(L,l,0))

//对参数进行某个cond检查，若失败，则按照格式输出arg,extramsg
#define luaL_argcheck(L, cond,arg,extramsg)	\
		((void)((cond) || luaL_argerror(L, (arg), (extramsg))))
/* 核查指定参数n是否为string */
#define luaL_checkstring(L,n)	(luaL_checklstring(L, (n), NULL))
/* 核查指定参数n是否为string，若不是则用缺省参数d替换 */
#define luaL_optstring(L,n,d)	(luaL_optlstring(L, (n), (d), NULL))
/* 指定参数i的类型名 */
#define luaL_typename(L,i)	lua_typename(L, lua_type(L,(i)))
/* 加载并执行lua脚本文件 */
#define luaL_dofile(L, fn) \
	(luaL_loadfile(L, fn) || lua_pcall(L, 0, LUA_MULTRET, 0))

#define luaL_dostring(L, s) \
	(luaL_loadstring(L, s) || lua_pcall(L, 0, LUA_MULTRET, 0))

//看到了么，这里的原表name特指LUA_REGISTERINDEX表中的node中的name的指向的表
#define luaL_getmetatable(L,n)	(lua_getfield(L, LUA_REGISTRYINDEX, (n)))

#define luaL_opt(L,f,n,d)	(lua_isnoneornil(L,(n)) ? (d) : f(L,(n)))

#define luaL_loadbuffer(L,s,sz,n)	luaL_loadbufferx(L,s,sz,n,NULL)


/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

typedef struct luaL_Buffer {
  char *b;  		/* buffer address,初始化后指向initb域 */
  size_t size;      /* buffer size：缓冲区总大小 */
  size_t n;  		/* number of characters in buffer：缓冲区，已用空间 */
  lua_State *L;
  char initb[LUAL_BUFFERSIZE];  /* initial buffer */
} luaL_Buffer;


#define luaL_addchar(B,c) \
  ((void)((B)->n < (B)->size || luaL_prepbuffsize((B), 1)), \
   ((B)->b[(B)->n++] = (c)))

#define luaL_addsize(B,s)	((B)->n += (s))

LUALIB_API void (luaL_buffinit) (lua_State *L, luaL_Buffer *B);
LUALIB_API char *(luaL_prepbuffsize) (luaL_Buffer *B, size_t sz);
LUALIB_API void (luaL_addlstring) (luaL_Buffer *B, const char *s, size_t l);
LUALIB_API void (luaL_addstring) (luaL_Buffer *B, const char *s);
LUALIB_API void (luaL_addvalue) (luaL_Buffer *B);
LUALIB_API void (luaL_pushresult) (luaL_Buffer *B);
LUALIB_API void (luaL_pushresultsize) (luaL_Buffer *B, size_t sz);
LUALIB_API char *(luaL_buffinitsize) (lua_State *L, luaL_Buffer *B, size_t sz);

#define luaL_prepbuffer(B)	luaL_prepbuffsize(B, LUAL_BUFFERSIZE)

/* }====================================================== */



/*
** {======================================================
** File handles for IO library
** =======================================================
*/

/*
** A file handle is a userdata with metatable 'LUA_FILEHANDLE' and
** initial structure 'luaL_Stream' (it may contain other fields
** after that initial structure).
*/

#define LUA_FILEHANDLE          "FILE*"


typedef struct luaL_Stream {
  FILE *f;  /* stream (NULL for incompletely created streams) */
  lua_CFunction closef;  /* to close stream (NULL for closed streams) */
} luaL_Stream;

/* }====================================================== */



/* compatibility with old module system */
#if defined(LUA_COMPAT_MODULE)

LUALIB_API void (luaL_pushmodule) (lua_State *L, const char *modname,
                                   int sizehint);
LUALIB_API void (luaL_openlib) (lua_State *L, const char *libname,
                                const luaL_Reg *l, int nup);

#define luaL_register(L,n,l)	(luaL_openlib(L,(n),(l),0))

#endif


/*
** {==================================================================
** "Abstraction Layer" for basic report of messages and errors
** ===================================================================
*/

/* print a string */
#if !defined(lua_writestring)
#define lua_writestring(s,l)   fwrite((s), sizeof(char), (l), stdout)
#endif

/* print a newline and flush the output */
#if !defined(lua_writeline)
#define lua_writeline()        (lua_writestring("\n", 1), fflush(stdout))
#endif

/* print an error message */
#if !defined(lua_writestringerror)
#define lua_writestringerror(s,p) \
        (fprintf(stderr, (s), (p)), fflush(stderr))
#endif

/* }================================================================== */


/*
** {============================================================
** Compatibility with deprecated conversions
** =============================================================
*/
#if defined(LUA_COMPAT_APIINTCASTS)

#define luaL_checkunsigned(L,a)	((lua_Unsigned)luaL_checkinteger(L,a))
#define luaL_optunsigned(L,a,d)	\
	((lua_Unsigned)luaL_optinteger(L,a,(lua_Integer)(d)))

#define luaL_checkint(L,n)	((int)luaL_checkinteger(L, (n)))
#define luaL_optint(L,n,d)	((int)luaL_optinteger(L, (n), (d)))

#define luaL_checklong(L,n)	((long)luaL_checkinteger(L, (n)))
#define luaL_optlong(L,n,d)	((long)luaL_optinteger(L, (n), (d)))

#endif
/* }============================================================ */



#endif


