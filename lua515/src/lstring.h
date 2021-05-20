/*
** $Id: lstring.h,v 1.43.1.1 2007/12/27 13:02:25 roberto Exp $
** String table (keep all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#ifndef lstring_h
#define lstring_h


#include "lgc.h"
#include "lobject.h"
#include "lstate.h"

/* +1 是系统自动额外补了一个\0在末尾 */
#define sizestring(s)	(sizeof(union TString)+((s)->len+1)*sizeof(char))

#define sizeudata(u)	(sizeof(union Udata)+(u)->len)
/* strlen() 不包含结尾的\0 */
#define luaS_new(L, s)	(luaS_newlstr(L, s, strlen(s)))
/* 本函数可用上面的代替，uuid性不高，不过，保持理想和现实的平衡吧，哈哈 */
#define luaS_newliteral(L, s)	(luaS_newlstr(L, "" s, \
                                 (sizeof(s)/sizeof(char))-1))
/* fix:固定，打上fix的gc标签 */
#define luaS_fix(s)	l_setbit((s)->tsv.marked, FIXEDBIT)
/* 调整哈希桶的高 */
LUAI_FUNC void luaS_resize (lua_State *L, int newsize);
LUAI_FUNC Udata *luaS_newudata (lua_State *L, size_t s, Table *e);
LUAI_FUNC TString *luaS_newlstr (lua_State *L, const char *str, size_t l);


#endif
