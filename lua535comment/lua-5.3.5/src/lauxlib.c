/*
** $Id: lauxlib.c,v 1.289.1.1 2017/04/19 17:20:42 roberto Exp $
** Auxiliary functions for building Lua libraries
** See Copyright Notice in lua.h
*/

#define lauxlib_c
#define LUA_LIB

#include "lprefix.h"


#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** This file uses only the official API of Lua.
** Any function declared here could be written as an application function.
*/

#include "lua.h"

#include "lauxlib.h"


/*
** {======================================================
** Traceback
** =======================================================
*/


#define LEVELS1	10	/* size of the first part of the stack */
#define LEVELS2	11	/* size of the second part of the stack */



/*
** search for 'objidx' in table at index -1.
** return 1 + string at top if find a good name.
*/
static int findfield (lua_State *L, int objidx, int level) {
  if (level == 0 || !lua_istable(L, -1))
    return 0;  /* not found */
  lua_pushnil(L);  /* start 'next' loop */
  while (lua_next(L, -2)) {  /* for each pair in table */
    if (lua_type(L, -2) == LUA_TSTRING) {  /* ignore non-string keys */
      if (lua_rawequal(L, objidx, -1)) {  /* found object? */
        lua_pop(L, 1);  /* remove value (but keep name) */
        return 1;
      }
      else if (findfield(L, objidx, level - 1)) {  /* try recursively */
        lua_remove(L, -2);  /* remove table (but keep name) */
        lua_pushliteral(L, ".");
        lua_insert(L, -2);  /* place '.' between the two names */
        lua_concat(L, 3);
        return 1;
      }
    }
    lua_pop(L, 1);  /* remove value */
  }
  return 0;  /* not found */
}


/*
** Search for a name for a function in all loaded modules
*/
static int pushglobalfuncname (lua_State *L, lua_Debug *ar) {
  int top = lua_gettop(L);
  lua_getinfo(L, "f", ar);  /* push function */
  lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  if (findfield(L, top + 1, 2)) {
    const char *name = lua_tostring(L, -1);
    if (strncmp(name, "_G.", 3) == 0) {  /* name start with '_G.'? */
      lua_pushstring(L, name + 3);  /* push name without prefix */
      lua_remove(L, -2);  /* remove original name */
    }
    lua_copy(L, -1, top + 1);  /* move name to proper place */
    lua_pop(L, 2);  /* remove pushed values */
    return 1;
  }
  else {
    lua_settop(L, top);  /* remove function and global table */
    return 0;
  }
}


static void pushfuncname (lua_State *L, lua_Debug *ar) {
  if (pushglobalfuncname(L, ar)) {  /* try first a global name */
    lua_pushfstring(L, "function '%s'", lua_tostring(L, -1));
    lua_remove(L, -2);  /* remove name */
  }
  else if (*ar->namewhat != '\0')  /* is there a name from code? */
    lua_pushfstring(L, "%s '%s'", ar->namewhat, ar->name);  /* use it */
  else if (*ar->what == 'm')  /* main? */
      lua_pushliteral(L, "main chunk");
  else if (*ar->what != 'C')  /* for Lua functions, use <file:line> */
    lua_pushfstring(L, "function <%s:%d>", ar->short_src, ar->linedefined);
  else  /* nothing left... */
    lua_pushliteral(L, "?");
}


static int lastlevel (lua_State *L) {
  lua_Debug ar;
  int li = 1, le = 1;
  /* find an upper bound */
  while (lua_getstack(L, le, &ar)) { li = le; le *= 2; }
  /* do a binary search */
  while (li < le) {
    int m = (li + le)/2;
    if (lua_getstack(L, m, &ar)) li = m + 1;
    else le = m;
  }
  return le - 1;
}


LUALIB_API void luaL_traceback (lua_State *L, lua_State *L1,
                                const char *msg, int level) {
  lua_Debug ar;
  int top = lua_gettop(L);
  int last = lastlevel(L1);
  int n1 = (last - level > LEVELS1 + LEVELS2) ? LEVELS1 : -1;
  if (msg)
    lua_pushfstring(L, "%s\n", msg);
  luaL_checkstack(L, 10, NULL);
  lua_pushliteral(L, "stack traceback:");
  while (lua_getstack(L1, level++, &ar)) {
    if (n1-- == 0) {  /* too many levels? */
      lua_pushliteral(L, "\n\t...");  /* add a '...' */
      level = last - LEVELS2 + 1;  /* and skip to last ones */
    }
    else {
      lua_getinfo(L1, "Slnt", &ar);
      lua_pushfstring(L, "\n\t%s:", ar.short_src);
      if (ar.currentline > 0)
        lua_pushfstring(L, "%d:", ar.currentline);
      lua_pushliteral(L, " in ");
      pushfuncname(L, &ar);
      if (ar.istailcall)
        lua_pushliteral(L, "\n\t(...tail calls...)");
      lua_concat(L, lua_gettop(L) - top);
    }
  }
  lua_concat(L, lua_gettop(L) - top);
}

/* }====================================================== */


/*
** {======================================================
** Error-report functions
** =======================================================
*/

LUALIB_API int luaL_argerror (lua_State *L, int arg, const char *extramsg) {
  lua_Debug ar;
  if (!lua_getstack(L, 0, &ar))  /* no stack frame? */
    return luaL_error(L, "bad argument #%d (%s)", arg, extramsg);
  lua_getinfo(L, "n", &ar);
  if (strcmp(ar.namewhat, "method") == 0) {
    arg--;  /* do not count 'self' */
    if (arg == 0)  /* error is in the self argument itself? */
      return luaL_error(L, "calling '%s' on bad self (%s)",
                           ar.name, extramsg);
  }
  if (ar.name == NULL)
    ar.name = (pushglobalfuncname(L, &ar)) ? lua_tostring(L, -1) : "?";
  return luaL_error(L, "bad argument #%d to '%s' (%s)",
                        arg, ar.name, extramsg);
}


static int typeerror (lua_State *L, int arg, const char *tname) {
  const char *msg;
  const char *typearg;  /* name for the type of the actual argument */
  if (luaL_getmetafield(L, arg, "__name") == LUA_TSTRING)
    typearg = lua_tostring(L, -1);  /* use the given type name */
  else if (lua_type(L, arg) == LUA_TLIGHTUSERDATA)
    typearg = "light userdata";  /* special name for messages */
  else
    typearg = luaL_typename(L, arg);  /* standard name */
  msg = lua_pushfstring(L, "%s expected, got %s", tname, typearg);
  return luaL_argerror(L, arg, msg);
}


static void tag_error (lua_State *L, int arg, int tag) {
  typeerror(L, arg, lua_typename(L, tag));
}


/*
** The use of 'lua_pushfstring' ensures this function does not
** need reserved stack space when called.
*/
LUALIB_API void luaL_where (lua_State *L, int level) {
  lua_Debug ar;
  if (lua_getstack(L, level, &ar)) {  /* check function at level */
    lua_getinfo(L, "Sl", &ar);  /* get info about it */
    if (ar.currentline > 0) {  /* is there info? */
      lua_pushfstring(L, "%s:%d: ", ar.short_src, ar.currentline);
      return;
    }
  }
  lua_pushfstring(L, "");  /* else, no information available... */
}


/*
** Again, the use of 'lua_pushvfstring' ensures this function does
** not need reserved stack space when called. (At worst, it generates
** an error with "stack overflow" instead of the given message.)
*/
LUALIB_API int luaL_error (lua_State *L, const char *fmt, ...) {
  va_list argp;
  va_start(argp, fmt);
  luaL_where(L, 1);
  lua_pushvfstring(L, fmt, argp);
  va_end(argp);
  lua_concat(L, 2);
  return lua_error(L);
}


LUALIB_API int luaL_fileresult (lua_State *L, int stat, const char *fname) {
  int en = errno;  /* calls to Lua API may change this value */
  if (stat) {
    lua_pushboolean(L, 1);
    return 1;
  }
  else {
    lua_pushnil(L);
    if (fname)
      lua_pushfstring(L, "%s: %s", fname, strerror(en));
    else
      lua_pushstring(L, strerror(en));
    lua_pushinteger(L, en);
    return 3;
  }
}


#if !defined(l_inspectstat)	/* { */

#if defined(LUA_USE_POSIX)

#include <sys/wait.h>

/*
** use appropriate macros to interpret 'pclose' return status
*/
#define l_inspectstat(stat,what)  \
   if (WIFEXITED(stat)) { stat = WEXITSTATUS(stat); } \
   else if (WIFSIGNALED(stat)) { stat = WTERMSIG(stat); what = "signal"; }

#else

#define l_inspectstat(stat,what)  /* no op */

#endif

#endif				/* } */


LUALIB_API int luaL_execresult (lua_State *L, int stat) {
  const char *what = "exit";  /* type of termination */
  if (stat == -1)  /* error? */
    return luaL_fileresult(L, 0, NULL);
  else {
    l_inspectstat(stat, what);  /* interpret result */
    if (*what == 'e' && stat == 0)  /* successful termination? */
      lua_pushboolean(L, 1);
    else
      lua_pushnil(L);
    lua_pushstring(L, what);
    lua_pushinteger(L, stat);
    return 3;  /* return true/nil,what,code */
  }
}

/* }====================================================== */


/*
** {======================================================
** Userdata's metatable manipulation
** =======================================================
*/
/* 创建userdata对象的元表 */
LUALIB_API int luaL_newmetatable (lua_State *L, const char *tname) {
  /*
  ** 从栈索引值为LUA_REGISTRYINDEX的table中获取键值内容为tname的value对象，
  ** 如果value对象不为nil，说明以tname为键的键值对已经存在了。luaL_getmetatable()
  ** 其实是一个宏，对应的是调用了lua_getfield()。lua_getfield()函数会将对应于
  ** tname的value对象压入栈顶部。
  */
  if (luaL_getmetatable(L, tname) != LUA_TNIL)  /* name already in use? */
    return 0;  /* leave previous value on top, but return 0 */

  /*
  ** 这个地方为什么要将此时位于栈顶的元素弹出堆栈呢？理由如下：
  ** 上面的if语句中调用了luaL_getmetatable()，这个宏实际是lua_getfield()，
  ** 该函数会以tname为键值从栈索引值为LUA_REGISTRYINDEX的table中获取对应的
  ** value对象，不管这个value对象是nil还是有效数据，都会将其压入堆栈。
  ** 上面的if语句中判断如果该value对象是一个nil对象，那么程序就执行到这里了，
  ** 由于下面的流程是重新创建一个元表，此时位于栈顶的那个nil对象就无用了，
  ** 因此将其弹出栈顶。
  */
  lua_pop(L, 1);

  /* 这里创建一个普通的lua table，并压入栈顶部，这个table后续会被当做userdata对象的元表 */
  lua_createtable(L, 0, 2);  /* create metatable */

  /* 将tname压入栈顶部，执行完下面这条语句之后，位于栈次顶部的就是上面刚刚创建的table */
  lua_pushstring(L, tname);

  /*
  ** 将位于栈顶部的tname以"__name"为键值设置到上面的table中，即那个即将被当做userdata对象的元表，
  ** 函数实参里面的-2就是该元表的栈索引值（实际地址为L->top - 2）。同时将tname对象从栈中弹出。
  */
  lua_setfield(L, -2, "__name");  /* metatable.__name = tname */
  /*
  ** 执行完上面这条语句后，位于栈顶的是userdata的元表，这里将该元表再次压入堆栈，此时栈顶部和栈次顶部
  ** 存放的都是指向这个metatable的指针。为什么要在栈顶部和栈次顶部都存放一样的内容呢？因为这个metatable
  ** 要设置到两个地方，一个是栈索引值为LUA_REGISTRYINDEX的表中，以键值为tname作为其子table，一个是
  ** userdata对象中，这个设置会在函数调用的外层中进行。
  */
  lua_pushvalue(L, -1);
  lua_setfield(L, LUA_REGISTRYINDEX, tname);  /* registry.name = metatable */
  return 1;
}


/* 
** luaL_setmetatable()会从栈索引值为LUA_REGISTRYINDEX中获取键值为tname的value对象（其实是
** 是一个元表），并将其压入栈顶部。同时将刚刚压入栈顶部的这个元表设置为此时位于栈次顶部（栈
** 索引值为-2）的value对象的元表。设置完成之后，会将栈顶部的这个元表弹出堆栈，value对象成为
** 新的栈顶部元素。
*/
LUALIB_API void luaL_setmetatable (lua_State *L, const char *tname) {
  luaL_getmetatable(L, tname);
  lua_setmetatable(L, -2);
}


LUALIB_API void *luaL_testudata (lua_State *L, int ud, const char *tname) {
  void *p = lua_touserdata(L, ud);
  if (p != NULL) {  /* value is a userdata? */
    if (lua_getmetatable(L, ud)) {  /* does it have a metatable? */
      luaL_getmetatable(L, tname);  /* get correct metatable */
      if (!lua_rawequal(L, -1, -2))  /* not the same? */
        p = NULL;  /* value is a userdata with wrong metatable */
      lua_pop(L, 2);  /* remove both metatables */
      return p;
    }
  }
  return NULL;  /* value is not a userdata with a metatable */
}


LUALIB_API void *luaL_checkudata (lua_State *L, int ud, const char *tname) {
  void *p = luaL_testudata(L, ud, tname);
  if (p == NULL) typeerror(L, ud, tname);
  return p;
}

/* }====================================================== */


/*
** {======================================================
** Argument check functions
** =======================================================
*/

LUALIB_API int luaL_checkoption (lua_State *L, int arg, const char *def,
                                 const char *const lst[]) {
  const char *name = (def) ? luaL_optstring(L, arg, def) :
                             luaL_checkstring(L, arg);
  int i;
  for (i=0; lst[i]; i++)
    if (strcmp(lst[i], name) == 0)
      return i;
  return luaL_argerror(L, arg,
                       lua_pushfstring(L, "invalid option '%s'", name));
}


/*
** Ensures the stack has at least 'space' extra slots, raising an error
** if it cannot fulfill the request. (The error handling needs a few
** extra slots to format the error message. In case of an error without
** this extra space, Lua will generate the same 'stack overflow' error,
** but without 'msg'.)
*/
LUALIB_API void luaL_checkstack (lua_State *L, int space, const char *msg) {
  if (!lua_checkstack(L, space)) {
    if (msg)
      luaL_error(L, "stack overflow (%s)", msg);
    else
      luaL_error(L, "stack overflow");
  }
}

/* 检查栈索引为arg的TValue对象中包含了数据类型是不是和t一样，如果不一样，则报错。 */
LUALIB_API void luaL_checktype (lua_State *L, int arg, int t) {
  if (lua_type(L, arg) != t)
    tag_error(L, arg, t);
}


LUALIB_API void luaL_checkany (lua_State *L, int arg) {
  if (lua_type(L, arg) == LUA_TNONE)
    luaL_argerror(L, arg, "value expected");
}

/* 
** luaL_checklstring()调用lua_tolstring()，并判断其返回的结果是不是空字符串。
** lua_tolstring()函数用于判断堆栈中索引为arg的value对象是不是字符串，如果是的话，
** 就将该字符串的地址和长度返回给上层调用者；如果不是字符串的话，那么尝试将该value
** 对象中的实际内容转换为字符串，然后将地址和长度返回给调用者。len存放字符串的长度。
*/
LUALIB_API const char *luaL_checklstring (lua_State *L, int arg, size_t *len) {
  const char *s = lua_tolstring(L, arg, len);
  if (!s) tag_error(L, arg, LUA_TSTRING);
  return s;
}


LUALIB_API const char *luaL_optlstring (lua_State *L, int arg,
                                        const char *def, size_t *len) {
  if (lua_isnoneornil(L, arg)) {
    if (len)
      *len = (def ? strlen(def) : 0);
    return def;
  }
  else return luaL_checklstring(L, arg, len);
}


LUALIB_API lua_Number luaL_checknumber (lua_State *L, int arg) {
  int isnum;
  lua_Number d = lua_tonumberx(L, arg, &isnum);
  if (!isnum)
    tag_error(L, arg, LUA_TNUMBER);
  return d;
}


LUALIB_API lua_Number luaL_optnumber (lua_State *L, int arg, lua_Number def) {
  return luaL_opt(L, luaL_checknumber, arg, def);
}


static void interror (lua_State *L, int arg) {
  if (lua_isnumber(L, arg))
    luaL_argerror(L, arg, "number has no integer representation");
  else
    tag_error(L, arg, LUA_TNUMBER);
}


LUALIB_API lua_Integer luaL_checkinteger (lua_State *L, int arg) {
  int isnum;
  lua_Integer d = lua_tointegerx(L, arg, &isnum);
  if (!isnum) {
    interror(L, arg);
  }
  return d;
}


LUALIB_API lua_Integer luaL_optinteger (lua_State *L, int arg,
                                                      lua_Integer def) {
  return luaL_opt(L, luaL_checkinteger, arg, def);
}

/* }====================================================== */


/*
** {======================================================
** Generic Buffer manipulation
** =======================================================
*/

/* userdata to box arbitrary data */
/*
** box对象，用于容纳任意的数据，box基于lua的userdata类型来实现。
** box指向的容纳任意数据的缓冲区。
** bsize则是该缓冲区的大小。
*/
typedef struct UBox {
  void *box;
  size_t bsize;
} UBox;

/* 调整box对象内部缓冲区的大小，box对象位于栈索引值为idx的userdata对象的内部缓冲区中 */
static void *resizebox (lua_State *L, int idx, size_t newsize) {
  void *ud;
  /* 获取用于申请内存的函数 */
  lua_Alloc allocf = lua_getallocf(L, &ud);
  /* 
  ** 从栈索引值为idx的userdata对象中获取box对象，box对象其实就是存放在
  ** userdata对象内的缓冲区中。
  */
  UBox *box = (UBox *)lua_touserdata(L, idx);
  /* 申请box对象内部的缓冲区 */
  void *temp = allocf(ud, box->box, box->bsize, newsize);
  if (temp == NULL && newsize > 0) {  /* allocation error? */
    resizebox(L, idx, 0);  /* free buffer */
    luaL_error(L, "not enough memory for buffer allocation");
  }

  /* 记录box对象的缓冲区及其大小。 */
  box->box = temp;
  box->bsize = newsize;
  return temp;
}


/* boxgc()函数是box对象的内存回收函数，即将box内部的缓冲区大小清零 */
static int boxgc (lua_State *L) {
  resizebox(L, 1, 0);
  return 0;
}


/* newbox()用于创建一个新的box对象，其缓冲区大小由newsize指定 */
static void *newbox (lua_State *L, size_t newsize) {

  /* 
  ** 基于lua的userdata类型来创建box对象，box对象存储在userdata内部缓冲区中，
  ** 该缓冲区大小就是box对象的大小。box对象外层的userdata对象目前在栈的最顶部。
  */
  UBox *box = (UBox *)lua_newuserdata(L, sizeof(UBox));
  /* 对刚刚创建的box对象进行初始化 */
  box->box = NULL;
  box->bsize = 0;

  /*
  ** 为此时位于栈顶部的userdata对象创建一个元表，执行完下面if语句中的函数调用之后，
  ** 位于栈顶部的就是即将作为userdata对象的那个元表，而userdata对象则位于栈次顶部。
  ** 另外，这个userdata对象的元表也会以"LUABOX"为键值设置到栈索引值为LUA_REGISTRYINDEX
  ** 的table中，作为其子table。
  */
  if (luaL_newmetatable(L, "LUABOX")) {  /* creating metatable? */
    /* 往栈顶部压入box对象的内存回收函数boxgc */
    lua_pushcfunction(L, boxgc);

    /* 
    ** 在执行下面这条语句之前，位于栈顶部的元素是box对象的内存回收函数，
    ** 栈次顶部(栈索引值为-2)的元素就是上面创建的名为"LUABOX"的元表。
    ** 这里将box对象的内存回收函数boxgc以键值为"__gc"设置到元表中。同时
    ** 将boxgc函数从栈顶部弹出。执行完下面语句后，位于栈顶部的元素就是
    ** 上面创建的名字为"LUABOX"的元表。
    */
    lua_setfield(L, -2, "__gc");  /* metatable.__gc = boxgc */
  }

  /*
  ** 程序执行到这里，位于栈次顶部（索引值为-2）的元素就是封装有box对象的
  ** userdata对象，这里将位于栈顶部的名为"LUABOX"的表设置为userdata对象
  ** 的元表。
  */
  lua_setmetatable(L, -2);

  /* 调整box对象的大小 */
  return resizebox(L, -1, newsize);
}


/*
** check whether buffer is using a userdata on the stack as a temporary
** buffer
*/
/* 
** 判断buffer对象中用来存放字符串内容的缓冲区是在堆还是在栈中，该宏返回 1，
** 表明缓冲区是在堆中动态申请的
*/
#define buffonstack(B)	((B)->b != (B)->initb)


/*
** returns a pointer to a free area with at least 'sz' bytes
*/
/* 
** luaL_prepbuffsize()函数将在buffer中取出一个长度为sz的小buffer，并将该
** 小buffer的首地址返回到上层。如果buffer中空闲空间的长度大于sz，那么可以直接
** 返回剩余空间的首地址给上层调用者；如果buffer中的空闲空间长度小于sz，那么这个
** 时候就需要重新分配一个大一点的buffer或者扩充原buffer，如果是重新分配buffer的话，
** 需要将原来buffer中的内容拷贝到新buffer中，然后返回新buffer或扩充后的buffer中的
** 剩余空间的首地址给上层调用者。新buffer的长度考虑到了sz的长度，因此创建的新buffer
** 中的剩余空间大小肯定不小于sz。
*/
LUALIB_API char *luaL_prepbuffsize (luaL_Buffer *B, size_t sz) {
  lua_State *L = B->L;
  /* buffer中剩余空间的长度小于sz，此时需要扩充原buffer或者申请一个新的buffer */
  if (B->size - B->n < sz) {  /* not enough space? */
    char *newbuff;
    size_t newsize = B->size * 2;  /* double buffer size */
    if (newsize - B->n < sz)  /* not big enough? */
      newsize = B->n + sz;
    if (newsize < B->n || newsize - B->n < sz)
      luaL_error(L, "buffer too large");
    /* create larger buffer */
    if (buffonstack(B))
      newbuff = (char *)resizebox(L, -1, newsize);
    else {  /* no buffer yet */
      newbuff = (char *)newbox(L, newsize);
      memcpy(newbuff, B->b, B->n * sizeof(char));  /* copy original content */
    }
    B->b = newbuff;
    B->size = newsize;
  }
  /* 返回buffer中剩余空间的首地址给上层调用者 */
  return &B->b[B->n];
}

/* luaL_addlstring()函数将长度为l，首地址为s的字符串追加到buffer对象B中 */
LUALIB_API void luaL_addlstring (luaL_Buffer *B, const char *s, size_t l) {
  if (l > 0) {  /* avoid 'memcpy' when 's' can be NULL */
    /* 
    ** 从buffer中找到用于存放字符串s的起始地址，然后将字符串s的内容从
    ** 该起始地址开始追加到buffer中，并更新目前buffer中已有的字符数量。
    */
    char *b = luaL_prepbuffsize(B, l);
    memcpy(b, s, l * sizeof(char));
    luaL_addsize(B, l);
  }
}

/* luaL_addstring()函数用于将字符串s追加到buffer中 */
LUALIB_API void luaL_addstring (luaL_Buffer *B, const char *s) {
  luaL_addlstring(B, s, strlen(s));
}


/* luaL_pushresult()函数将buffer对象中的实际内容（一个字符串）压入堆栈中 */
LUALIB_API void luaL_pushresult (luaL_Buffer *B) {
  lua_State *L = B->L;
  /* 将buffer中的实际内容（一个字符串）压入堆栈 */
  lua_pushlstring(L, B->b, B->n);
  if (buffonstack(B)) {
    /* 
    ** 如果buffer对象中的缓冲区是动态申请的，那么这里要将其释放，因为字符串已经
    ** 存放到全局堆栈中了
    */
    resizebox(L, -2, 0);  /* delete old buffer */
    lua_remove(L, -2);  /* remove its header from the stack */
  }
}


LUALIB_API void luaL_pushresultsize (luaL_Buffer *B, size_t sz) {
  luaL_addsize(B, sz);
  luaL_pushresult(B);
}


/* 
** luaL_addvalue()函数用于将位于堆栈顶部的元素转换成相应的字符串（不改变堆栈顶部的内容），
** 然后追加到buffer的缓冲区中。
*/
LUALIB_API void luaL_addvalue (luaL_Buffer *B) {
  lua_State *L = B->L;
  size_t l;
  
  /* 将堆栈顶部的元素转换为相应的字符串，返回值是字符串的首地址，l存放了该字符串的长度 */
  const char *s = lua_tolstring(L, -1, &l);
  if (buffonstack(B))
    lua_insert(L, -2);  /* put value below buffer */
  /* 将上面的到的字符串追加到buffer的缓冲区中，s是字符串首地址，l是字符串长度 */
  luaL_addlstring(B, s, l);
  lua_remove(L, (buffonstack(B)) ? -2 : -1);  /* remove value */
}


/* 初始化buffer对象B */
LUALIB_API void luaL_buffinit (lua_State *L, luaL_Buffer *B) {
  B->L = L;
  B->b = B->initb;
  B->n = 0;
  B->size = LUAL_BUFFERSIZE;
}

/* luaL_buffinitsize()函数初始化buffer对象，并根据sz申请内部缓冲区，然后返回可写缓冲区的首地址 */
LUALIB_API char *luaL_buffinitsize (lua_State *L, luaL_Buffer *B, size_t sz) {
  /* 初始化buffer对象B */
  luaL_buffinit(L, B);

  /* 
  ** luaL_prepbuffsize()函数将在buffer中取出一个长度为sz的小buffer，并将该
  ** 小buffer的首地址返回到上层。如果buffer中空闲空间的长度大于sz，那么可以直接
  ** 返回剩余空间的首地址给上层调用者；如果buffer中的空闲空间长度小于sz，那么这个
  ** 时候就需要重新分配一个大一点的buffer或者扩充原buffer，如果是重新分配buffer的话，
  ** 需要将原来buffer中的内容拷贝到新buffer中，然后返回新buffer或扩充后的buffer中的
  ** 剩余空间的首地址给上层调用者。新buffer的长度考虑到了sz的长度，因此创建的新buffer
  ** 中的剩余空间大小肯定不小于sz。
  */
  return luaL_prepbuffsize(B, sz);
}

/* }====================================================== */


/*
** {======================================================
** Reference system
** =======================================================
*/

/* index of free-list header */
#define freelist	0


LUALIB_API int luaL_ref (lua_State *L, int t) {
  int ref;
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);  /* remove from stack */
    return LUA_REFNIL;  /* 'nil' has a unique fixed reference */
  }
  t = lua_absindex(L, t);
  lua_rawgeti(L, t, freelist);  /* get first free element */
  ref = (int)lua_tointeger(L, -1);  /* ref = t[freelist] */
  lua_pop(L, 1);  /* remove it from stack */
  if (ref != 0) {  /* any free element? */
    lua_rawgeti(L, t, ref);  /* remove it from list */
    lua_rawseti(L, t, freelist);  /* (t[freelist] = t[ref]) */
  }
  else  /* no free elements */
    ref = (int)lua_rawlen(L, t) + 1;  /* get a new reference */
  lua_rawseti(L, t, ref);
  return ref;
}


LUALIB_API void luaL_unref (lua_State *L, int t, int ref) {
  if (ref >= 0) {
    t = lua_absindex(L, t);
    lua_rawgeti(L, t, freelist);
    lua_rawseti(L, t, ref);  /* t[ref] = t[freelist] */
    lua_pushinteger(L, ref);
    lua_rawseti(L, t, freelist);  /* t[freelist] = ref */
  }
}

/* }====================================================== */


/*
** {======================================================
** Load functions
** =======================================================
*/

/* LoadF结构体用于保存lua源文件的相关信息 */
typedef struct LoadF {
  /* 预读的字符数量 */
  int n;  /* number of pre-read characters */
  /* 用于保存被读文件对应的文件指针 */
  FILE *f;  /* file being read */
  /* 保存从文件中读取的内容 */
  char buff[BUFSIZ];  /* area for reading file */
} LoadF;

/* getF()函数用于从lua源文件中读取一个字符块，并用size保存大小，字符块存放在lf->buff中 */
static const char *getF (lua_State *L, void *ud, size_t *size) {
  LoadF *lf = (LoadF *)ud;
  (void)L;  /* not used */

  /* 判断lf->buff中是否已经有预读取的文件内容，如果有的话，那么就保存其字符数量 */
  if (lf->n > 0) {  /* are there pre-read characters to be read? */
    /* *size中保存预读取的字符数量 */
    *size = lf->n;  /* return them (chars already in buffer) */
    lf->n = 0;  /* no more pre-read characters */
  }
  else {  /* read a block from file */
    /* 'fread' can return > 0 *and* set the EOF flag. If next call to
       'getF' called 'fread', it might still wait for user input.
       The next check avoids this problem. */
    if (feof(lf->f)) return NULL;

    /* fread从lf->f指向的文件对象中读取lf->buff大小的字符 */
    *size = fread(lf->buff, 1, sizeof(lf->buff), lf->f);  /* read block */
  }
  /* 返回lf->buff首地址 */
  return lf->buff;
}


static int errfile (lua_State *L, const char *what, int fnameindex) {
  const char *serr = strerror(errno);
  const char *filename = lua_tostring(L, fnameindex) + 1;
  lua_pushfstring(L, "cannot %s %s: %s", what, filename, serr);
  lua_remove(L, fnameindex);
  return LUA_ERRFILE;
}

/*
** BOM（byte-order mark）文件编码头，即 字节顺序标记.它是插入到以
** UTF-8、UTF16或UTF-32编码文件开头的特殊标记，用来标记多字节编码
** 文件的编码类型和字节顺序（big-endian或little- endian）。一般用
** 来识别文件的编码类型。
** skipBOM函数就是要跳过lua源文件开头的BOM，并返回BOM后的第一个有效
** 字符
*/
static int skipBOM (LoadF *lf) {
  const char *p = "\xEF\xBB\xBF";  /* UTF-8 BOM mark */
  int c;
  lf->n = 0;
  do {
    /* 逐个字符读取lua源文件 */
    c = getc(lf->f);
    if (c == EOF || c != *(const unsigned char *)p++) return c;

    /* 这里将读取到的BOM mark记录下来，提供给parser使用 */
    lf->buff[lf->n++] = c;  /* to be read by the parser */
  } while (*p != '\0');

  /*
  ** 程序执行到这里，说明BOM mark已经匹配上了，故丢弃保存在lf->buff中的BOM，
  ** 因为lf->buff由lf->n作为下标来访问，故这里将lf->n重置为0，相当于将lf->buff
  ** 这个缓冲区清空。
  */
  lf->n = 0;  /* prefix matched; discard it */

  /* 返回lua源文件中第一个有效字符 */
  return getc(lf->f);  /* return next character */
}


/*
** reads the first character of file 'f' and skips an optional BOM mark
** in its beginning plus its first line if it starts with '#'. Returns
** true if it skipped the first line.  In any case, '*cp' has the
** first "valid" character of the file (after the optional BOM and
** a first-line comment).
*/
/*
** skipcomment()函数会跳过lua源文件中的BOM mark，以及第一行注释（如果有的话），
** 然后cp会指向第一个有效字符。
*/
static int skipcomment (LoadF *lf, int *cp) {
  /* skipBOM()会返回lua源文件中第一个有效字符，可能是注释，可能是lua代码 */
  int c = *cp = skipBOM(lf);

  /* 如果lua源文件的第一行是一个注释，那么就跳过这条注释，并返回1；否则返回0. */
  if (c == '#') {  /* first line is a comment (Unix exec. file)? */
    do {  /* skip first line */
      c = getc(lf->f);
    } while (c != EOF && c != '\n');

    /*
    ** 如果lua源文件第一行是注释，那么需要再读一次，让cp指向第一个有效字符，
    ** 即源文件第二行的第一个字符。
    */
    *cp = getc(lf->f);  /* skip end-of-line, if present */
    return 1;  /* there was a comment */
  }
  else return 0;  /* no comment */
}

/* luaL_loadfilex()用于对lua代码文件进行词法和语法分析，进而生成虚拟机执行的字节码 */
LUALIB_API int luaL_loadfilex (lua_State *L, const char *filename,
                                             const char *mode) {
  LoadF lf;
  int status, readstatus;
  int c;

  /* 获取用于存放lua代码文件名的栈地址 */
  int fnameindex = lua_gettop(L) + 1;  /* index of filename on the stack */
  if (filename == NULL) {
    lua_pushliteral(L, "=stdin");
    lf.f = stdin;
  }
  else {

    /* 将lua代码文件名压入调用栈顶部，作为chunk的名字 */
    lua_pushfstring(L, "@%s", filename);
    lf.f = fopen(filename, "r");
    if (lf.f == NULL) return errfile(L, "open", fnameindex);
  }

  /*
  ** skipcomment()函数会跳过lua源文件中的BOM mark，以及第一行注释（如果有的话），
  ** 然后c会指向第一个有效字符。
  */
  if (skipcomment(&lf, &c))  /* read initial portion */
    lf.buff[lf.n++] = '\n';  /* add line to correct line numbers */

  /* 判断lua文件是不是预编译了的二进制代码文件，如果是，那么以二进制模式重读文件 */
  if (c == LUA_SIGNATURE[0] && filename) {  /* binary file? */
    lf.f = freopen(filename, "rb", lf.f);  /* reopen in binary mode */
    if (lf.f == NULL) return errfile(L, "reopen", fnameindex);
    skipcomment(&lf, &c);  /* re-read initial portion */
  }

  /* 开始读文件并保存文件内容，然后进行词法和语法分析，进而生成虚拟机执行的字节码 */
  if (c != EOF)
    lf.buff[lf.n++] = c;  /* 'c' is the first character of the stream */

  /* 执行完下面这条语句之后，保存了分析结果的LClosure对象已经在栈顶部了。 */
  status = lua_load(L, getF, &lf, lua_tostring(L, -1), mode);

  /* 获取文件操作的结果，并判断读文件是否出错 */
  readstatus = ferror(lf.f);
  if (filename) fclose(lf.f);  /* close file (even in case of errors) */
  if (readstatus) {
    lua_settop(L, fnameindex);  /* ignore results from 'lua_load' */
    return errfile(L, "read", fnameindex);
  }

  /* 读完lua源文件之后，将文件名从堆栈中弹出，并返回读文件的结果 */
  lua_remove(L, fnameindex);
  return status;
}


typedef struct LoadS {
  const char *s;
  size_t size;
} LoadS;


static const char *getS (lua_State *L, void *ud, size_t *size) {
  LoadS *ls = (LoadS *)ud;
  (void)L;  /* not used */
  if (ls->size == 0) return NULL;
  *size = ls->size;
  ls->size = 0;
  return ls->s;
}


LUALIB_API int luaL_loadbufferx (lua_State *L, const char *buff, size_t size,
                                 const char *name, const char *mode) {
  LoadS ls;
  ls.s = buff;
  ls.size = size;
  return lua_load(L, getS, &ls, name, mode);
}


LUALIB_API int luaL_loadstring (lua_State *L, const char *s) {
  return luaL_loadbuffer(L, s, strlen(s), s);
}

/* }====================================================== */



LUALIB_API int luaL_getmetafield (lua_State *L, int obj, const char *event) {
  if (!lua_getmetatable(L, obj))  /* no metatable? */
    return LUA_TNIL;
  else {
    int tt;
    lua_pushstring(L, event);
    tt = lua_rawget(L, -2);
    if (tt == LUA_TNIL)  /* is metafield nil? */
      lua_pop(L, 2);  /* remove metatable and metafield */
    else
      lua_remove(L, -2);  /* remove only metatable */
    return tt;  /* return metafield type */
  }
}


LUALIB_API int luaL_callmeta (lua_State *L, int obj, const char *event) {
  obj = lua_absindex(L, obj);
  if (luaL_getmetafield(L, obj, event) == LUA_TNIL)  /* no metafield? */
    return 0;
  lua_pushvalue(L, obj);
  lua_call(L, 1, 1);
  return 1;
}


LUALIB_API lua_Integer luaL_len (lua_State *L, int idx) {
  lua_Integer l;
  int isnum;
  lua_len(L, idx);
  l = lua_tointegerx(L, -1, &isnum);
  if (!isnum)
    luaL_error(L, "object length is not an integer");
  lua_pop(L, 1);  /* remove object */
  return l;
}


LUALIB_API const char *luaL_tolstring (lua_State *L, int idx, size_t *len) {
  if (luaL_callmeta(L, idx, "__tostring")) {  /* metafield? */
    if (!lua_isstring(L, -1))
      luaL_error(L, "'__tostring' must return a string");
  }
  else {
    switch (lua_type(L, idx)) {
      case LUA_TNUMBER: {
        if (lua_isinteger(L, idx))
          lua_pushfstring(L, "%I", (LUAI_UACINT)lua_tointeger(L, idx));
        else
          lua_pushfstring(L, "%f", (LUAI_UACNUMBER)lua_tonumber(L, idx));
        break;
      }
      case LUA_TSTRING:
        lua_pushvalue(L, idx);
        break;
      case LUA_TBOOLEAN:
        lua_pushstring(L, (lua_toboolean(L, idx) ? "true" : "false"));
        break;
      case LUA_TNIL:
        lua_pushliteral(L, "nil");
        break;
      default: {
        int tt = luaL_getmetafield(L, idx, "__name");  /* try name */
        const char *kind = (tt == LUA_TSTRING) ? lua_tostring(L, -1) :
                                                 luaL_typename(L, idx);
        lua_pushfstring(L, "%s: %p", kind, lua_topointer(L, idx));
        if (tt != LUA_TNIL)
          lua_remove(L, -2);  /* remove '__name' */
        break;
      }
    }
  }
  return lua_tolstring(L, -1, len);
}


/*
** {======================================================
** Compatibility with 5.1 module functions
** =======================================================
*/
#if defined(LUA_COMPAT_MODULE)

static const char *luaL_findtable (lua_State *L, int idx,
                                   const char *fname, int szhint) {
  const char *e;
  if (idx) lua_pushvalue(L, idx);
  do {
    e = strchr(fname, '.');
    if (e == NULL) e = fname + strlen(fname);
    lua_pushlstring(L, fname, e - fname);
    if (lua_rawget(L, -2) == LUA_TNIL) {  /* no such field? */
      lua_pop(L, 1);  /* remove this nil */
      lua_createtable(L, 0, (*e == '.' ? 1 : szhint)); /* new table for field */
      lua_pushlstring(L, fname, e - fname);
      lua_pushvalue(L, -2);
      lua_settable(L, -4);  /* set new table into field */
    }
    else if (!lua_istable(L, -1)) {  /* field has a non-table value? */
      lua_pop(L, 2);  /* remove table and value */
      return fname;  /* return problematic part of the name */
    }
    lua_remove(L, -2);  /* remove previous table */
    fname = e + 1;
  } while (*e == '.');
  return NULL;
}


/*
** Count number of elements in a luaL_Reg list.
*/
static int libsize (const luaL_Reg *l) {
  int size = 0;
  for (; l && l->name; l++) size++;
  return size;
}


/*
** Find or create a module table with a given name. The function
** first looks at the LOADED table and, if that fails, try a
** global variable with that name. In any case, leaves on the stack
** the module table.
*/
LUALIB_API void luaL_pushmodule (lua_State *L, const char *modname,
                                 int sizehint) {
  luaL_findtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE, 1);
  if (lua_getfield(L, -1, modname) != LUA_TTABLE) {  /* no LOADED[modname]? */
    lua_pop(L, 1);  /* remove previous result */
    /* try global variable (and create one if it does not exist) */
    lua_pushglobaltable(L);
    if (luaL_findtable(L, 0, modname, sizehint) != NULL)
      luaL_error(L, "name conflict for module '%s'", modname);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, modname);  /* LOADED[modname] = new table */
  }
  lua_remove(L, -2);  /* remove LOADED table */
}


LUALIB_API void luaL_openlib (lua_State *L, const char *libname,
                               const luaL_Reg *l, int nup) {
  luaL_checkversion(L);
  if (libname) {
    luaL_pushmodule(L, libname, libsize(l));  /* get/create library table */
    lua_insert(L, -(nup + 1));  /* move library table to below upvalues */
  }
  if (l)
    luaL_setfuncs(L, l, nup);
  else
    lua_pop(L, nup);  /* remove upvalues */
}

#endif
/* }====================================================== */

/*
** set functions from list 'l' into table at top - 'nup'; each
** function gets the 'nup' elements at the top as upvalues.
** Returns with only the table at the stack.
*/
/* luaL_setfuncs()用于将待注册的函数保存到lua表中。 */
LUALIB_API void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup) {
  luaL_checkstack(L, nup, "too many upvalues");
  for (; l->name != NULL; l++) {  /* fill the table with given functions */
    int i;
	/* 将函数的upvalues均压入堆栈 */
    for (i = 0; i < nup; i++)  /* copy upvalues to the top */
      /*
      ** 将待注册函数的自由变量一一再次全部压入栈中，而这些自由变量在调用luaL_setfuncs()函数的
      ** 外层函数中已经按顺序一一压入栈中了，因此这里只是将他们再一次一一全部压入栈中。为什么需要
      ** 重复做这样的事情呢？因为下面的lua_pushcclosre()函数会将本次循环压入栈中的自由变量保存到
      ** CClosure对象的upvalues数组中，并从栈中弹出。为了下一个待注册函数能找到所需自由变量
      ** 每个待注册的函数要自己负责将需要的自由变量再次压入栈中，使用完后再将它们弹出。
      ** 这样就保证栈中始终有一份自由变量列表，可以被下一个待注册函数使用。
      */
      lua_pushvalue(L, -nup);
    /*
    ** 将函数指针压入堆栈，从这个函数的处理流程可以看到，如果函数有upvalue，那么
    ** 函数的upvalue比函数指针先压入堆栈。在这个函数中，如果函数有upvalues时，那么
    ** 函数对应的CClosure对象会覆盖该函数第一个upvalue所在的堆栈位置，而原先存放在
    ** 堆栈中的upvalue都保存到了CClosure中，因此覆盖了也没关系。下面也需要将剩余的
    ** 存放在堆栈中的upvalue都弹出堆栈。
    */
    lua_pushcclosure(L, l->func, nup);  /* closure with those upvalues */
    /* 
    ** L->top -(nup+2)其实就是存放待注册函数的table的地址，这个地方参考
    ** luaL_newlib()这个宏就知道了。那么这里的意思就是将上面刚刚压入堆栈
    ** 的CClosure对象保存到这个table中，对应的键值为函数的名字。因为lua_setfiled()
    ** 会将位于堆栈顶部的对象(在这个上下文中就是函数的CClosure对象)以l->name(即函数名字)
    ** 为键值保存到用于存放待注册函数的table中。当函数对应的CClosure对象保存到table之后，
    ** 这个对象就会从栈顶弹出，此时位于堆栈最顶部的元素就是那个table了。
    */
    lua_setfield(L, -(nup + 2), l->name);
  }
  /* 
  ** 由于上面已经将upvalue保存到了CClosure对象中，因此原先存放在堆栈中的upvalue就可以
  ** 移除了。这个地方是不是多余了？这个地方其实没有多余，因为如果nup不为0的话，
  ** 那么在调用该函数luaL_setfuncs()的外层函数中会将待注册函数的upvalue压入栈顶。现在
  ** 这些自由变量已经成功添加到了待注册函数中了，那么在这里就可以将外层函数压入到栈中的
  ** 自由变量全部弹出了。
  */
  lua_pop(L, nup);  /* remove upvalues */
}


/*
** ensure that stack[idx][fname] has a table and push that table
** into the stack
*/
/* 
** luaL_getsubtable()用于判断在堆栈索引值为idx处是否有一个名字为fname的table，
** 如果这个table存在，那么在将这个table压入堆栈顶部的之后就直接返回；如果这个地方
** 没有符合条件的table，那么就在创建一个table，并压入堆栈两次，然后将这个table以fname
** 为键值加入到这个堆栈索引值为idx处的table中，成为一个子table。加入完成之后，这个子table
** 就会被弹出堆栈，因为已经加入到了其他table中，就没必要在放在堆栈中占位置了。由于新建的
** 这个table之前被压入堆栈两次，因此这次弹出之后，这个table仍处于堆栈顶部。这样就保证了
** if和else两个分支执行的效果是一样的，那就是目标的table被压入堆栈顶部。
*/
LUALIB_API int luaL_getsubtable (lua_State *L, int idx, const char *fname) {
  /* 
  ** 从堆栈索引值为idx出的table中取出键值为fname的value对象，判断这个value是不是
  ** 一个table，如果是的话，在将这个table压入堆栈顶部的之后，就返回了；如果不是一个
  ** table，那么需要在堆栈顶部创建一个新的table，然后将这个位于堆栈顶部的table以fname
  ** 为键值加入到堆栈索引值为idx处的那个table中，成为一个子table。
  */
  if (lua_getfield(L, idx, fname) == LUA_TTABLE)
    return 1;  /* table already there */
  else {
    /*
    ** lua_pop()这个宏其实就是调用lua_settop()函数来重新设置堆栈指针L->top，
    ** 以参数1来调用lua_pop()的效果就是将堆栈指针L->top减1，即将当前堆栈最顶部
    ** 元素弹出堆栈，L->top指向了该位置。L->top指向的位置是没有存入有效数据的，
    ** 且是下一次将存入有效数据的位置。
    */
    lua_pop(L, 1);  /* remove previous result */
    /*
    ** lua_absindex()用于取堆栈索引值得绝对值。这个绝对值并不完全和数学上的绝对值一样，
    ** 是具有上下文含义的绝对值，如下：
    ** 1.如果idx大于0，那么直接返回该值；
    ** 2.如果idx小于等于LUA_REGISTRYINDEX（一个负值），那么也直接返回该值；
    ** 3.如果idx在(LUA_REGISTRYINDEX,0]之间，则返回对应的绝对值。
    */
    idx = lua_absindex(L, idx);
  
    /* 创建一个新的table，并将这个table压入堆栈，此时这个table是堆栈最顶部元素 */
    lua_newtable(L);
	
    /* 
    ** 上面执行“lua_newtable(L);”语句之后新创建的table就在堆栈最顶部了，下面即将执行的
    ** 语句“lua_pushvalue(L, -1);”会将已经位于堆栈顶部的这个table再一次压入堆栈。
    ** 这里为什么要这样做呢？因为luaL_getsubtable()这个函数的最终目的就是将键值fname对应的
    ** 这个table压入堆栈顶部。上面这句和下面这句的结果是会导致新创建的这个table会压入堆栈
    ** 顶部和次顶部，因为下面的lua_setfield()函数会将位于栈顶的新建的table加入父table之后，
    ** 将新建的table弹出堆栈，这个时候位于次顶部的table就变成了堆栈最顶部元素。这样就和
    ** 该函数第一条语句完成了一样的功能了。
    */
    lua_pushvalue(L, -1);  /* copy to be left at top */
	
	/* 
	** 将上面新创建的table以fname为键值，加入到堆栈索引值为idx的那个table中，
	** 成为一个子table。然后新创建的这个子table会被弹出堆栈。
	*/
    lua_setfield(L, idx, fname);  /* assign new table to field */
    return 0;  /* false, because did not find table there */
  }
}


/*
** Stripped-down 'require': After checking "loaded" table, calls 'openf'
** to open a module, registers the result in 'package.loaded' table and,
** if 'glb' is true, also registers the result in the global table.
** Leaves resulting module on the top.
*/
/* 
** luaL_requiref()函数的作用就是将模块modname对应的table加入到_LOADED和_G
** 这两个table中，成为它们的子table。
*/
LUALIB_API void luaL_requiref (lua_State *L, const char *modname,
                               lua_CFunction openf, int glb) {
  /*
  ** 判断在堆栈索引值为LUA_REGISTRYINDEX处的table中有没有一个名为LUA_LOADED_TABLE("_LOADED")的
  ** 子table，如果有的话，那么在将这个table压入堆栈顶部的之后就返回了；如果没有的话，就创建一个
  ** 新的table，并压入堆栈两次，然后将位于堆栈顶部table以键值为LUA_LOADED_TABLE("_LOADED")添加进
  ** 堆栈索引值为LUA_REGISTRYINDEX的table中，成为一个子table。加入成功之后，子table就会被弹出堆栈，
  ** 但由于子table之前被压入两次，因此此时位于堆栈最顶部的元素仍然是这个table。执行完这个语句后，
  ** 堆栈最顶部的元素就是名为"_LOADED"的table。
  */
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  /* 
  ** 我们知道，执行完上面的语句后，堆栈最顶部的元素就是名为"_LOADED"的table，
  ** lua_getfield()从idx为-1（idx为-1，对应的就是堆栈最顶部元素）的table（就是
  ** 上面的"_LOADED" table）中获取键值为modname对应的table，因为lua在引入一个库时，
  ** 是用一个库（即模块）级别的table来存放库里面的函数信息的，因此这里就是从堆栈顶部的
  ** 这个"_LOADED" table中获取库对应的那个table，键值就是模块名字（库名字）。执行完下面
  ** 这条语句之后，位于堆栈顶部的元素就是库modname对应的那个table了(如果有这个库的话)。
  */
  lua_getfield(L, -1, modname);  /* LOADED[modname] */
  if (!lua_toboolean(L, -1)) {  /* package not already loaded? */
    /*
    ** 进入这个分支说明名字为modname的模块还没有加载过，那么这里就准备将这个module进行加载。
    ** 流程如下：
    ** 1. 由于库之前没有进行加载，那么这个时候位于栈顶的那个table就不作数了，这里就调用lua_pop()
    **    将其弹出堆栈；
    ** 2. 调用lua_pushcfunction()将modname对应的库加载函数压入堆栈顶部；
    ** 3. 调用lua_pushstring()将模块名字modname压入堆栈顶部，作为库加载函数的参数；
    ** 4. 由于模块对应的加载函数openf已经处于此堆栈顶部了，因此可以调用该函数来
    **    执行加载库的操作了，模块加载函数执行的结果会存放在堆栈顶部，结果其实就是模块对应的table。
    **    以math库为例，其库加载函数luaopen_math()的执行结果就是会在堆栈顶部创建一个包含了math库函数
    **    及常量的table。lua_call(L, 1, 1)完成的就是调用库的加载函数的功能。执行完lua_call()语句后，
    **    被加载模块对应的table就在堆栈顶部了。
    ** 5. 语句“lua_pushvalue(L, -1);”的作用就是将位于堆栈顶部的库对应的table再一次压入堆栈，这个时候
    **    堆栈最顶部和次顶部的就都是库对应的table了。
    ** 6. 语句“lua_setfield(L, -3, modname);”的作用就是将位于堆栈顶部的库对应的table以modname为键值
    **    添加进"_LOADED"这个table中，作为其的一个子table，并将最顶部的table弹出堆栈。语句中的“-3”
    **    就是"_LOADED"table的索引，为什么它的索引是-3呢？我们知道L->top指向的是堆栈中下一个即将存放
    **    内容的地址，L->top - 1就是堆栈最顶部的元素，L->top - 2就是次顶部元素，上面的第4和5步会将库
    **    对应的table加入堆栈两次，分别位于堆栈最顶部和次顶部，该函数第一条语句
    **    “luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);”执行完的时候，"_LOADED"z table
    **    就在当时的堆栈最顶部，也就是这个时候的(L->top - 3)这个位置。
    */
    lua_pop(L, 1);  /* remove field */
    lua_pushcfunction(L, openf);
    lua_pushstring(L, modname);  /* argument to open function */
    lua_call(L, 1, 1);  /* call 'openf' to open module */
    lua_pushvalue(L, -1);  /* make copy of module (call result) */
    lua_setfield(L, -3, modname);  /* LOADED[modname] = module */
  }

  /* 
  ** 这里调用以参数-2来调用lua_remove()，目的是将位于堆栈次顶部的那个"_LOADED" table
  ** 从堆栈中移除，因为上面的流程中已经将modename对应的table添加到"_LOADED" table中了，
  ** 或者modename对应的table本身就应经在"_LOADED" table中了，因此可以将"_LOADED" table
  ** 从堆栈中移除了，节省堆栈空间。这里的-2其实就是此时"_LOADED" table在堆栈中的索引值，
  ** 即"_LOADED" table位于堆栈次顶部，modname对应的table才位于堆栈顶部。
  */
  lua_remove(L, -2);  /* remove LOADED table */
  
  /*
  ** 如果glb为1的话，那么就将该module对应的table存放全局的table中，即_G这个table。
  ** “lua_pushvalue(L, -1);”这条语句的作用是将要加入_G表的那个module对应的table
  ** 压入堆栈顶部，因为lua_setglobal()函数中会将位于堆栈顶部的table以modname
  ** 为键值加入_G中，所以要提前将module对应的table写入堆栈顶部，另外，在将table加入
  ** _G之后会将这个table从堆栈中弹出。
  */
  if (glb) {
    lua_pushvalue(L, -1);  /* copy of module */
    lua_setglobal(L, modname);  /* _G[modname] = module */
  }
  
  /* 
  ** 程序执行到这里，位于堆栈顶部的元素仍然是模块modname对应的table，这个位于堆栈顶部的table
  ** 在上层调用者中会将其从堆栈顶部中弹出。参考luaL_openlibs()。
  */
}


LUALIB_API const char *luaL_gsub (lua_State *L, const char *s, const char *p,
                                                               const char *r) {
  const char *wild;
  size_t l = strlen(p);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  while ((wild = strstr(s, p)) != NULL) {
    luaL_addlstring(&b, s, wild - s);  /* push prefix */
    luaL_addstring(&b, r);  /* push replacement in place of pattern */
    s = wild + l;  /* continue after 'p' */
  }
  luaL_addstring(&b, s);  /* push last suffix */
  luaL_pushresult(&b);
  return lua_tostring(L, -1);
}

/* lua中用于内存申请的函数，其实主要就是对realloc()的封装，osize不用，ud不用 */
static void *l_alloc (void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud; (void)osize;  /* not used */
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  else
    return realloc(ptr, nsize);
}


static int panic (lua_State *L) {
  lua_writestringerror("PANIC: unprotected error in call to Lua API (%s)\n",
                        lua_tostring(L, -1));
  return 0;  /* return to Lua to abort */
}


LUALIB_API lua_State *luaL_newstate (void) {
  lua_State *L = lua_newstate(l_alloc, NULL);
  if (L) lua_atpanic(L, &panic);
  return L;
}


/* 检查参数中传入的版本与当前的lua版本号是否一致，如果版本号不一致，lua进程会异常退出 */
LUALIB_API void luaL_checkversion_ (lua_State *L, lua_Number ver, size_t sz) {
  /* 获取当前lua的版本号 */
  const lua_Number *v = lua_version(L);
  if (sz != LUAL_NUMSIZES)  /* check numeric types */
    luaL_error(L, "core and library have incompatible numeric types");
  if (v != lua_version(NULL))
    luaL_error(L, "multiple Lua VMs detected");
  else if (*v != ver)
    luaL_error(L, "version mismatch: app. needs %f, Lua core provides %f",
                  (LUAI_UACNUMBER)ver, (LUAI_UACNUMBER)*v);
}

