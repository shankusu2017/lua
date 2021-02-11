/*
** $Id: lstring.h,v 1.61 2015/11/03 15:36:01 roberto Exp $
** String table (keep all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#ifndef lstring_h
#define lstring_h

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"

/* 这里的+1:给结尾的\0留出空间 */
#define sizelstring(l)  (sizeof(union UTString) + ((l) + 1) * sizeof(char))

//这里之所以能直接+，得利于union UUdata的巧妙结构(对齐的安排)
#define sizeludata(l)	(sizeof(union UUdata) + (l))

/* user部分的数据长度 */
#define sizeudata(u)	sizeludata((u)->len)

/* 
** literal:文字
** 这里传入的s是常量字符串 eg:str = "local",自带'\0'结尾
*/
#define luaS_newliteral(L, s)	(luaS_newlstr(L, "" s, \
                                 (sizeof(s)/sizeof(char))-1))


/*
** test whether a string is a reserved word
** 是保留字符串吗？eg：语法关键字
*/
#define isreserved(s)	((s)->tt == LUA_TSHRSTR && (s)->extra > 0)


/* equality for short strings, which are always internalized */
#define eqshrstr(a,b)	check_exp((a)->tt == LUA_TSHRSTR, (a) == (b))
/* 根据传入参数str,l以及传入种子seed,计算出新的seed */
LUAI_FUNC unsigned int luaS_hash (const char *str, size_t l, unsigned int seed);
/* 计算长字符串的hash值 */
LUAI_FUNC unsigned int luaS_hashlongstr (TString *ts);
LUAI_FUNC int luaS_eqlngstr (TString *a, TString *b);
LUAI_FUNC void luaS_resize (lua_State *L, int newsize);
LUAI_FUNC void luaS_clearcache (global_State *g);
LUAI_FUNC void luaS_init (lua_State *L);
LUAI_FUNC void luaS_remove (lua_State *L, TString *ts);
LUAI_FUNC Udata *luaS_newudata (lua_State *L, size_t s);
LUAI_FUNC TString *luaS_newlstr (lua_State *L, const char *str, size_t l);
LUAI_FUNC TString *luaS_new (lua_State *L, const char *str);
LUAI_FUNC TString *luaS_createlngstrobj (lua_State *L, size_t l);


#endif
