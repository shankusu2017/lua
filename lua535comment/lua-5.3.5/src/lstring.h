/*
** $Id: lstring.h,v 1.61.1.1 2017/04/19 17:20:42 roberto Exp $
** String table (keep all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#ifndef lstring_h
#define lstring_h

#include "lgc.h"
#include "lobject.h"
#include "lstate.h"


/* 计算一个字符串对象需要的总内存大小，包括头部和内容两部分 */
#define sizelstring(l)  (sizeof(union UTString) + ((l) + 1) * sizeof(char))

/* 计算一个Userdata对象需要的总内存大小，包括头部和内容两部分 */
#define sizeludata(l)	(sizeof(union UUdata) + (l))

/* 获取Userdata对象中内容部分的大小 */
#define sizeudata(u)	sizeludata((u)->len)

#define luaS_newliteral(L, s)	(luaS_newlstr(L, "" s, \
                                 (sizeof(s)/sizeof(char))-1))


/*
** test whether a string is a reserved word
*/
/* 判断一个短字符串是否是lua解释器保留的字符串，如一些关键字之类的 */
#define isreserved(s)	((s)->tt == LUA_TSHRSTR && (s)->extra > 0)


/*
** equality for short strings, which are always internalized
*/
/* 判断短字符串是否相等 */
#define eqshrstr(a,b)	check_exp((a)->tt == LUA_TSHRSTR, (a) == (b))


LUAI_FUNC unsigned int luaS_hash (const char *str, size_t l, unsigned int seed);
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
