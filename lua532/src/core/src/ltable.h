/*
** $Id: ltable.h,v 2.21 2015/11/03 15:47:30 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#ifndef ltable_h
#define ltable_h

#include "lobject.h"

/*
** local tbl   = {a = aStr, b = bStr,}
**    	 tbl.a = nil 
** 上面的这句话设置了.a的Node中的val为空，key实际上还是a
*/

/*
** 常用的宏定义，非常好用
*/

//取表node中idx=i的Node值
#define gnode(t,i)	(&(t)->node[i])
//提取Node中的val
#define gval(n)		(&(n)->i_val)
/* 'const' to avoid wrong writings that can mess up field 'next' */ 
#define gkey(n)		cast(const TValue*, (&(n)->i_key.tvk))
/*
** writable version of 'gkey'; allows updates to individual fields,
** but not to the whole (which has incompatible type)
*/
#define wgkey(n)		(&(n)->i_key.nk)
//提取Node中的key的next
#define gnext(n)	((n)->i_key.nk.next)

/* invalidate:使xx无效
 * flags.bit=1:元方法不存在, 0:可能存在，需尝试读取才能确定是否真的存在(若不存在则更新为1)
 * 这里置为0就是让调用者自己去试一下是否存在
*/
#define invalidateTMcache(t)	((t)->flags = 0)


/* returns the key, given the value of a table entry 
** 这里的方法可以学下，留个印象
*/
#define keyfromval(v) \
  (gkey(cast(Node *, cast(char *, (v)) - offsetof(Node, i_val))))

/* 提取t[key]对应的val的地址，注意这里返回的值加上了const修饰哦 */
LUAI_FUNC const TValue *luaH_getint (Table *t, lua_Integer key);
/* 设置t[key]=val */
LUAI_FUNC void luaH_setint (lua_State *L, Table *t, lua_Integer key,TValue *value);
/* 提取t[key]的val */
LUAI_FUNC const TValue *luaH_getshortstr (Table *t, TString *key);
LUAI_FUNC const TValue *luaH_getstr (Table *t, TString *key);
LUAI_FUNC const TValue *luaH_get (Table *t, const TValue *key);

/* 尝试构建一个newKey并返回对应的value域 */
LUAI_FUNC TValue *luaH_newkey (lua_State *L, Table *t, const TValue *key);
/* 提取t[key]的对应的value的地址，调用本函数后，再在返回的value.addr上写上对应的值 */
LUAI_FUNC TValue *luaH_set (lua_State *L, Table *t, const TValue *key);

/* 构造一张空表 */
LUAI_FUNC Table *luaH_new (lua_State *L);
/* 调整array,node部分大小并rehash */
LUAI_FUNC void luaH_resize (lua_State *L, Table *t, unsigned int nasize,
                                                    unsigned int nhsize);
/* 仅调整array部分的大小并rehash */
LUAI_FUNC void luaH_resizearray (lua_State *L, Table *t, unsigned int nasize);
/* 释放h占用的MEM */
LUAI_FUNC void luaH_free (lua_State *L, Table *t);
/* 查找key的下一个key->val对 */
LUAI_FUNC int luaH_next (lua_State *L, Table *t, StkId key);
/* 计算"整数类型"的下标部分的大小 */
LUAI_FUNC int luaH_getn (Table *t);


#if defined(LUA_DEBUG)
LUAI_FUNC Node *luaH_mainposition (const Table *t, const TValue *key);
LUAI_FUNC int luaH_isdummy (Node *n);
#endif


#endif
