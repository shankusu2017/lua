/*
** $Id: lstring.c,v 2.56.1.1 2017/04/19 17:20:42 roberto Exp $
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
** Lua will use at most ~(2^LUAI_HASHLIMIT) bytes from a string to
** compute its hash
*/
#if !defined(LUAI_HASHLIMIT)
#define LUAI_HASHLIMIT		5
#endif


/*
** equality for long strings
*/
int luaS_eqlngstr (TString *a, TString *b) {
  size_t len = a->u.lnglen;
  lua_assert(a->tt == LUA_TLNGSTR && b->tt == LUA_TLNGSTR);
  return (a == b) ||  /* same instance or... */
    ((len == b->u.lnglen) &&  /* equal length and ... */
     (memcmp(getstr(a), getstr(b), len) == 0));  /* equal contents */
}

/* 计算字符串对应的hash值 */
unsigned int luaS_hash (const char *str, size_t l, unsigned int seed) {
  unsigned int h = seed ^ cast(unsigned int, l);
  size_t step = (l >> LUAI_HASHLIMIT) + 1;
  for (; l >= step; l -= step)
    h ^= ((h<<5) + (h>>2) + cast_byte(str[l - 1]));
  return h;
}


/* 计算长字符串对应的hash值 */
unsigned int luaS_hashlongstr (TString *ts) {
  lua_assert(ts->tt == LUA_TLNGSTR);
  if (ts->extra == 0) {  /* no hash? */
    ts->hash = luaS_hash(getstr(ts), ts->u.lnglen, ts->hash);
    ts->extra = 1;  /* now it has its hash */
  }
  return ts->hash;
}


/*
** resizes the string table
*/
/* 对存放string的hash表进行重hash */
void luaS_resize (lua_State *L, int newsize) {
  int i;

  /* 获取存放系统中所有string的全局散列表 */
  stringtable *tb = &G(L)->strt;

  /* 如果新的桶的数量大于原有桶的数量，那么需要申请多出来的那部分桶 */
  if (newsize > tb->size) {  /* grow table if needed */
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);

    /* 将新分配的那部分桶中的hash值清零，用于后续存放hash值 */
    for (i = tb->size; i < newsize; i++)
      tb->hash[i] = NULL;
  }

  /* 遍历所有的桶，对每一个桶中的所有字符串根据新的桶的数量重新进行存放 */
  for (i = 0; i < tb->size; i++) {  /* rehash */
    TString *p = tb->hash[i];
    tb->hash[i] = NULL;
    while (p) {  /* for each node in the list */
      TString *hnext = p->u.hnext;  /* save next */

      /*
      ** 为字符串重新选择一个桶，对新的桶的数量取模得到新的桶。举例如下：
      ** 1.当新桶数量大于旧桶数量时，如新桶数量为6，旧桶数量为4，那么在旧桶中
      ** hash值为2和6的字符串都会存放在同一个桶中，桶号为2（从0开始）。那么在
      ** 新桶中，hash值为2和6的字符串分别会存放在桶2和桶0中，达到了重hash的效果。
      ** 2.当新桶的数量小于旧桶数量时，如新桶数量为4，旧桶数量为6则是上述过程的
      ** 逆过程，即旧桶中桶2和桶6中的字符串都会存放在桶2中。
      */
      unsigned int h = lmod(p->hash, newsize);  /* new position */

      /*
      ** 从下面这两句可以看出，将一个新的字符串放入桶中的链表时，
      ** 是插入到链表的头部，而不是尾部
      */
      p->u.hnext = tb->hash[h];  /* chain it */
      tb->hash[h] = p;

      p = hnext;
    }
  }

  /*
  ** 如果新的桶数量更小，那么对散列表进行收缩，因为上面的循环中已经将散列表中的
  ** 中的字符串根据新的桶数量进行的重hash，因此这里可以释放掉多出来的那部分桶
  ** 对应的内存。
  */
  if (newsize < tb->size) {  /* shrink table if needed */
    /* vanishing slice should be empty */
    lua_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
  }

  /* 更新散列表中桶的个数 */
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
/* 初始化string table及string cache */
void luaS_init (lua_State *L) {
  global_State *g = G(L);		/* 获取全局状态 */
  int i, j;
  /* 以最小的桶数量来初始化string table */
  luaS_resize(L, MINSTRTABSIZE);  /* initial size of string table */

  /* pre-create memory-error message */
  /* 预创建内存错误信息，即创建一个内容为"not enough memory"的字符串字面值 */
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG);
  luaC_fix(L, obj2gco(g->memerrmsg));  /* it should never be collected */
  for (i = 0; i < STRCACHE_N; i++)  /* fill cache with valid strings */
    for (j = 0; j < STRCACHE_M; j++)
      g->strcache[i][j] = g->memerrmsg;
}



/*
** creates a new string object
*/
/* 创建一个新的字符串对象，未填入具体的字符串内容，只是申请了内存空间 */
static TString *createstrobj (lua_State *L, size_t l, int tag, unsigned int h) {
  TString *ts;
  GCObject *o;
  size_t totalsize;  /* total size of TString object */

  /* 计算字符串对应需要的内存大小，包括头部和内容，内容紧跟在头部之后 */
  totalsize = sizelstring(l);

  /* 根据存放字符串所需的内存大小和标记创建一个新的GCObject对象 */
  o = luaC_newobj(L, tag, totalsize);

  /* 将GCObject类型转换为具体的string类型 */
  ts = gco2ts(o);

  /* 保存字符串对应的hash值 */
  ts->hash = h;
  ts->extra = 0;

  /* 字符串以'\0'结尾 */
  getstr(ts)[l] = '\0';  /* ending 0 */
  return ts;
}

/* 创建一个新的长字符串对象，未填入具体的字符串内容，只是申请了内存空间 */
TString *luaS_createlngstrobj (lua_State *L, size_t l) {
  /*
  ** 从这里可以看出，长字符串对象一开始并没有计算hash值，而只是将用于计算hash值得种子传递进去，
  ** 保存到TString对象的hash成员中，同时extra设置为0，表明并没有计算hash，即此时hash成员的值并不是
  ** 字符串的hash值，后续需要进行字符串键匹配的时候，才会真正计算长字符串对象的hash值，并保存到
  ** hash成员中，extra也会被置为1，表示此时的hash成员的值是字符串的hash值。
  */
  TString *ts = createstrobj(L, l, LUA_TLNGSTR, G(L)->seed);
  ts->u.lnglen = l;
  return ts;
}

/*
** 取出存放系统中所有字符串的全局hash表，并根据待删除的字符串中的hash值
** 找到相应的hash桶，然后遍历桶中的所有字符串，找到待删除的字符串，将其中
** hash桶的链表中移除，并更新全局hash表中字符串的数量（减1）。
** 注意：这里不负责释放对应的内存，只是将其中桶链表中移除。
*/
void luaS_remove (lua_State *L, TString *ts) {
  stringtable *tb = &G(L)->strt;
  TString **p = &tb->hash[lmod(ts->hash, tb->size)];
  while (*p != ts)  /* find previous element */
    p = &(*p)->u.hnext;
  *p = (*p)->u.hnext;  /* remove element from its list */
  tb->nuse--;
}


/*
** checks whether short string exists and reuses it or creates a new one
*/
/*
** 短字符串对象的创建，并且创建之后会立即加入到全局状态信息（global_State）的strt成员中。
** strt成员是一个hash表，专门用来存放Lua中的全部短字符串对象。长字符串对象则不会放置在这里。
*/
static TString *internshrstr (lua_State *L, const char *str, size_t l) {
  TString *ts;

  global_State *g = G(L);  /* 获取全局状态信息 */
  unsigned int h = luaS_hash(str, l, g->seed);  /* 计算字符串对应的hash值 */
  TString **list = &g->strt.hash[lmod(h, g->strt.size)];  /* 找到对应的hash桶 */
  lua_assert(str != NULL);  /* otherwise 'memcmp'/'memcpy' are undefined */

  /*
  ** 遍历这个hash桶中的字符串链表，查找是否有相同内容的字符串对象，如果找到了，
  ** 就复用这个字符串对象并返回。isdead()用于判断是否该字符串对象处于待回收状态，
  ** 如果处于待回收状态，那么就将其待回收状态撤销。
  */
  for (ts = *list; ts != NULL; ts = ts->u.hnext) {
    if (l == ts->shrlen &&
        (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) {
      /* found! */
      if (isdead(g, ts))  /* dead (but not collected yet)? */
        changewhite(ts);  /* resurrect it */
      return ts;
    }
  }

  /*
  ** 判断是否需要进行重hash，如果整个hash表中字符串的数量大于hash桶的数量，并且
  ** hash桶的数量不大于MAX_INT/2，那么就进行重hash
  */
  if (g->strt.nuse >= g->strt.size && g->strt.size <= MAX_INT/2) {
    luaS_resize(L, g->strt.size * 2);
    list = &g->strt.hash[lmod(h, g->strt.size)];  /* recompute with new size */
  }

  /* 创建一个新的字符串对象，并填入字符串内容，字符串的内容紧跟在字符串头部后面 */
  ts = createstrobj(L, l, LUA_TSHRSTR, h);
  memcpy(getstr(ts), str, l * sizeof(char));

  ts->shrlen = cast_byte(l); /* 保存字符串长度 */

  /* 将新创建的字符串对象插入到hash桶中字符串链表的头部 */
  ts->u.hnext = *list;
  *list = ts;

  g->strt.nuse++;  /* 更新hash表中字符串的数量 */
  return ts;
}


/*
** new string (with explicit length)
*/
/* 创建一个字符串对象(显示指定长度) */
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  /* 判断是否是短字符串，如果是则调用internshrstr()创建 */
  if (l <= LUAI_MAXSHORTLEN)  /* short string? */
    return internshrstr(L, str, l);
  else {
    TString *ts;
    /*
    ** 如果字符串长度超过了lua中规定的最大长度，那么会报错。其中MAX_SIZE指定的
    ** 长度是包括了字符串头部的长度，故在下面会先减去头部的长度再进行比较。
    */
    if (l >= (MAX_SIZE - sizeof(TString))/sizeof(char))
      luaM_toobig(L);

    /* 创建一个长字符串对象，然后将参数传递进来的字符串拷贝到长字符串对象内部 */
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
/*
** 创建或者复用一个以'\0'结尾的字符串对象，首先检查是否在
** global_State的strcache中，如果在的话，那么就直接复用该字符串对象，
** 否则创建一个新的字符串对象，并将该字符串对象存放在
** global_State的strcache[i]字符串数组中的第一个位置处。
*/
TString *luaS_new (lua_State *L, const char *str) {

  /* 计算参数指定的字符串str对应的hash值，即在global_State的strcache的索引值 */
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

/* 
** 创建一个Udata对象，并申请一块紧跟在Udata对象后面的大小为s的内存区域，Udata对象
** 其实是userdata对象的头部，而紧跟在Udata对象后面的内存区域则为userdata对象用于存放
** 实际内容的区域。
*/
Udata *luaS_newudata (lua_State *L, size_t s) {
  Udata *u;
  GCObject *o;
  if (s > MAX_SIZE - sizeof(Udata))
    luaM_toobig(L);
  /* 申请一个GCobject对象，其内部类型是LUA_TUSERDATA，大小由sizeludata(s)指定 */
  o = luaC_newobj(L, LUA_TUSERDATA, sizeludata(s));
  /*
  ** 将GCObject对象转换为Udata对象，之所以这样子做，是为了更好地管理需要进行垃圾回收的类型。
  ** 在创建某个需要进行内存回收的类型对象时，用GCobject作为统一借口进行申请，然后将申请到的
  ** GCObject对象在强转为某一具体的类型。因为所有需要进行内存回收的类型都和GCObject有共同的
  ** 头部。
  */
  u = gco2u(o);
  u->len = s;
  u->metatable = NULL;
  
  /* 用nil对象进行初始化 */
  setuservalue(L, u, luaO_nilobject);
  return u;
}

