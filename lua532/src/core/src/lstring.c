/*
** $Id: lstring.c,v 2.56 2015/11/23 11:32:51 roberto Exp $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#define lstring_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"


#define MEMERRMSG       "not enough memory"


/*
** Lua will use at most (2^LUAI_HASHLIMIT) bytes from a string to
** compute its hash
*/
#if !defined(LUAI_HASHLIMIT)
#define LUAI_HASHLIMIT		5
#endif


/*
** equality for long strings
** returns:
**  true:自定义的字符串内容完全相同,或a和b指针相同。
**  false:其它情况则返回fales。
*/
int luaS_eqlngstr (TString *a, TString *b) {
  size_t len = a->u.lnglen;
  /* 
  ** 竟然是比较两个长字符串，那么这里做下assert的判断呗,
  ** 下面a==b则返回true,若不加判断，下面的判断条件就不是那么充分了。
  */
  lua_assert(a->tt == LUA_TLNGSTR && b->tt == LUA_TLNGSTR);
  return (a == b) ||  /* same instance or... */
    ((len == b->u.lnglen) &&  /* equal length and ... */
     (memcmp(getstr(a), getstr(b), len) == 0));  /* equal contents */
}

/* 这个算法非常值得一读
** 即兼顾了长字符串，也兼顾了短字符串
** 才用传入seed而不是global_state中的seed则是出于预防黑客攻击的考虑
*/
unsigned int luaS_hash (const char *str, size_t l, unsigned int seed) {
  unsigned int h = seed ^ cast(unsigned int, l);
  
  // 这一句是精华，+1更是妙哉
  size_t step = (l >> LUAI_HASHLIMIT) + 1;
  
  for (; l >= step; l -= step)
    h ^= ((h<<5) + (h>>2) + cast_byte(str[l - 1]));
  return h;
}

/* 计算长字符串的hash，已计算过则直接返回，反之计算后再返回 */
unsigned int luaS_hashlongstr (TString *ts) {
  lua_assert(ts->tt == LUA_TLNGSTR);
  if (ts->extra == 0) {  /* no hash? */
    /* 长字符串域中的hash在初始化时被设置为global_state的seed，这里拿出来用 */
    ts->hash  = luaS_hash(getstr(ts), ts->u.lnglen, ts->hash);
    ts->extra = 1;  /* now it has its hash */
  }
  return ts->hash;
}


/*
** resizes the string table
** 
** 这是个处理特定对象(开散列的哈希表)的函数
*/
void luaS_resize (lua_State *L, int newsize) {
  int i;
  stringtable *tb = &G(L)->strt;
  
  if (newsize > tb->size) {  /* grow table if needed */
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
    for (i = tb->size; i < newsize; i++)
      tb->hash[i] = NULL;	/* full nil at new'adder */
  }

  /* 
   ** 拿出笔和纸出来画一下就好理解了
   ** hash表是二级指针
   ** 这里画图可知，所有的element都至少被rehash处理了一次，至多被处理了2次(第二次仅计算hash值，不会被移动)
  */
  for (i = 0; i < tb->size; i++) {  /* rehash */
    TString *p  = tb->hash[i];
    /* 这一句是必须的，避免形成环形链表 */
    tb->hash[i] = NULL; 
    while (p) {  /* for each node in the list */
      TString *hnext = p->u.hnext;  /* save next */
      unsigned int h = lmod(p->hash, newsize);  /* new position */
      p->u.hnext     = tb->hash[h];  /* chain it */
      tb->hash[h]    = p;
      p = hnext;
    }
  }

  /* hash表是个2级指针结构，这里就可以luaM_reallocvetor了，妙吧 */
  if (newsize < tb->size) {  /* shrink(收缩) table if needed */
    /* 
    ** vanishing slice should be empty
    ** 这里画图，空出来的mem应该是空的
    */
    lua_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
  }
  tb->size = newsize;
}


/*
** Clear API string cache. (Entries cannot be empty, so fill them with
** a non-collectable string.)
*/
void luaS_clearcache (global_State *g) {
  int i, j;
  for (i = 0; i < STRCACHE_N; i++)
    for (j = 0; j < STRCACHE_M; j++) {
    if (iswhite(g->strcache[i][j]))  /* will entry be collected? */
      g->strcache[i][j] = g->memerrmsg;  /* replace it with something fixed */
    }
}


/*
** Initialize the string table and the string cache
*/
void luaS_init (lua_State *L) {
  global_State *g = G(L);
  int i, j;
  /* initial size of string table */
  luaS_resize(L, MINSTRTABSIZE); 
  
  /* pre-create memory-error message */
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG);

  /* 挂到 fixed_gc上，避免被回收 */
  luaC_fix(L, obj2gco(g->memerrmsg));   /* it should never be collected */
  
  for (i = 0; i < STRCACHE_N; i++)      /* fill cache with valid strings */
    for (j = 0; j < STRCACHE_M; j++)
      g->strcache[i][j] = g->memerrmsg;
}



/*
** 构造一个裸符串
** 类型由tag指出，其它字段没有过多地处理
*/
static TString *createstrobj (lua_State *L, size_t l, int tag, unsigned int h) {
  TString *ts;
  GCObject *o;
  size_t totalsize;  /* total size of TString object */
  totalsize = sizelstring(l);
  /* 构造一个裸符串 */
  o     = luaC_newobj(L, tag, totalsize);
  ts    = gco2ts(o);
  ts->hash  = h;
  ts->extra = 0;
  getstr(ts)[l] = '\0';  /* ending 0 */
  return ts;
}

TString *luaS_createlngstrobj (lua_State *L, size_t l) {
  TString *ts  = createstrobj(L, l, LUA_TLNGSTR, G(L)->seed);
  ts->u.lnglen = l;
  return ts;
}

/* 
** 这里没有做*p==NULL的判空处理，故而默认被删除的元素一定是存在于表的 
** 理解此函数最好画图
*/ 
void luaS_remove (lua_State *L, TString *ts) {
  stringtable *tb = &G(L)->strt;
  TString **p = &tb->hash[lmod(ts->hash, tb->size)];

  while (*p != ts)  /* find previous element */
    p = &(*p)->u.hnext;	/* p是2级指针，所以这里最后要& */
  
  *p = (*p)->u.hnext;  /* remove element from its list：画图便于理解 */
  tb->nuse--;
}


/*
** checks whether short string exists and reuses it or creates a new one
*/
static TString *internshrstr (lua_State *L, const char *str, size_t l) {
  TString *ts;
  global_State *g   = G(L);
  unsigned int h 	= luaS_hash(str, l, g->seed);
  TString **list 	= &g->strt.hash[lmod(h, g->strt.size)];
  lua_assert(str != NULL);  /* otherwise 'memcmp'/'memcpy' are undefined */
  for (ts = *list; ts != NULL; ts = ts->u.hnext) {
    if (l == ts->shrlen && (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) { /* found! */
      if (isdead(g, ts)) {  /* dead (but not collected yet)? */
        changewhite(ts);    /* resurrect it：重新启用呗 */
      }
      return ts;
    }
  }

  /* 小了则扩容 */  
  if (g->strt.nuse >= g->strt.size && g->strt.size <= MAX_INT/2) {
    luaS_resize(L, g->strt.size * 2);
    list = &g->strt.hash[lmod(h, g->strt.size)];  /* recompute with new size */
  }

  /* 
  ** 构建一份String实例，看得出短字符串的hash在构造时便计算好了
  */
  ts = createstrobj(L, l, LUA_TSHRSTR, h);  
  memcpy(getstr(ts), str, l * sizeof(char));
  ts->shrlen = cast_byte(l);

  /* insert chaint list，链表操作，看过了就轻车熟路了吧 */
  ts->u.hnext = *list;
  *list = ts;
  
  g->strt.nuse++;
  return ts;
}


/*
** new string (with explicit(显式的) length)
*/
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  if (l <= LUAI_MAXSHORTLEN)  /* short string? */
    return internshrstr(L, str, l);
  else {
    TString *ts;
    if (l >= (MAX_SIZE - sizeof(TString))/sizeof(char))
      luaM_toobig(L);
    ts = luaS_createlngstrobj(L, l);
    memcpy(getstr(ts), str, l * sizeof(char));
    return ts;
  }
}


/*
** Create or reuse a zero-terminated string, first checking in the
** cache (using the string address as a key). The cache can contain
** only zero-terminated strings, so it is safe to use 'strcmp' to
** check hits.
*/
TString *luaS_new (lua_State *L, const char *str) {
  unsigned int i = point2uint(str) % STRCACHE_N;  /* hash */
  int j;
  TString **p = G(L)->strcache[i];
  for (j = 0; j < STRCACHE_M; j++) {
    if (strcmp(str, getstr(p[j])) == 0)  /* hit? */
      return p[j];  /* that is it */
  }
  /* normal route */
  for (j = STRCACHE_M - 1; j > 0; j--)
    p[j] = p[j - 1];  /* move out last element */
  /* new element is first in the list */
  p[0] = luaS_newlstr(L, str, strlen(str));
  return p[0];
}

//构建一个UserData，s是附带的数据大小
Udata *luaS_newudata (lua_State *L, size_t s) {
  Udata *u;
  GCObject *o;
  if (s > MAX_SIZE - sizeof(Udata))
    luaM_toobig(L);
  o = luaC_newobj(L, LUA_TUSERDATA, sizeludata(s));
  u = gco2u(o);
  u->len = s;   //这里记录了s
  u->metatable = NULL;
  setuservalue(L, u, luaO_nilobject);  //这里进行一些初始化
  return u;
}

