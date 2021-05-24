/*
** $Id: lapi.c,v 2.259.1.2 2017/12/06 18:35:12 roberto Exp $
** Lua API
** See Copyright Notice in lua.h
*/

#define lapi_c
#define LUA_CORE

#include "lprefix.h"


#include <stdarg.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"



const char lua_ident[] =
  "$LuaVersion: " LUA_COPYRIGHT " $"
  "$LuaAuthors: " LUA_AUTHORS " $";


/* value at a non-valid index */
#define NONVALIDVALUE		cast(TValue *, luaO_nilobject)

/* corresponding test */
/* 判断是不是非nil对象 */
#define isvalid(o)	((o) != luaO_nilobject)

/* test for pseudo index */
#define ispseudo(i)		((i) <= LUA_REGISTRYINDEX)

/* test for upvalue */
/* upvalue使用的也是伪索引，参考lua_upvalueindex() */
#define isupvalue(i)		((i) < LUA_REGISTRYINDEX)

/* test for valid but not pseudo index */
#define isstackindex(i, o)	(isvalid(o) && !ispseudo(i))

#define api_checkvalidindex(l,o)  api_check(l, isvalid(o), "invalid index")

#define api_checkstackindex(l, i, o)  \
	api_check(l, isstackindex(i, o), "index not in the stack")

/*
** index2addr函数用于返回idx对应的value对象的内存地址。
** 如果idx>0，则返回的是ci->func+idx；如果idx<0，那么返回的是L->top + idx；
** idx>0的情况，多用于操作函数调用栈时，idx<0多用于操作数据栈时和伪索引地址。
*/
static TValue *index2addr (lua_State *L, int idx) {
  /* 获取线程正在执行的函数对应的函数调用信息 */
  CallInfo *ci = L->ci;
  if (idx > 0) {
    /* 如果idx > 0，那么先获取idx对应的value的地址，并返回这个地址 */
    TValue *o = ci->func + idx;
    api_check(L, idx <= ci->top - (ci->func + 1), "unacceptable index");
    if (o >= L->top) return NONVALIDVALUE;
    else return o;
  }
  else if (!ispseudo(idx)) {  /* negative index */
    /* 
    ** 如果进入这个分支，说明函数调用者传入的idx是一个负数，这个时候返回
    ** idx对应的value的堆栈内存地址。
    */
    api_check(L, idx != 0 && -idx <= L->top - (ci->func + 1), "invalid index");
    return L->top + idx;
  }
  else if (idx == LUA_REGISTRYINDEX)
    /* LUA_REGISTRYINDEX是一个伪索引，其实获取的是全局注册表，为了统一接口，而使用伪索引。 */
    return &G(L)->l_registry;
  else {  /* upvalues */
  	/*
  	** 这个地方需要结合lua_upvalueindex()这个宏来理解，假如我想要获取当前被调用C函数的第i（
  	** 从1开始算）个自由变量，那么在调用index2addr()获取自由变量的地址时，我们要先通过调用宏
  	** lua_upvalueindex()来获取传递给index2addr()的自由变量对应的索引值，那么宏lua_upvalueindex(i)
  	** 返回的就是LUA_REGISTRYINDEX - (i)，即在这里index2addr()参数中的idx = (LUA_REGISTRYINDEX - (i))，
  	** 那么下面的这条语句执行完之后，idx = i，也就是我们所需要的自由变量的编号。由于在lua中数组从1开始
  	** 编号，而C中从0开始编号，因此用idx去索引upvalue数组时要减1。
  	*/
    idx = LUA_REGISTRYINDEX - idx;
    api_check(L, idx <= MAXUPVAL + 1, "upvalue index too large");
    if (ttislcf(ci->func))  /* light C function? */
      return NONVALIDVALUE;  /* it has no upvalues */
    else {
      /* C闭包含有自由变量，且存放在CClosure的upvalue数组成员中。这里返回upvalue数组元素的地址 */
      CClosure *func = clCvalue(ci->func);
      return (idx <= func->nupvalues) ? &func->upvalue[idx-1] : NONVALIDVALUE;
    }
  }
}


/*
** to be called by 'lua_checkstack' in protected mode, to grow stack
** capturing memory errors
*/
/* 延长虚拟栈大小，ud用于辅助计算新的虚拟栈大小。 */
static void growstack (lua_State *L, void *ud) {
  int size = *(int *)ud;
  luaD_growstack(L, size);
}


LUA_API int lua_checkstack (lua_State *L, int n) {
  int res;
  CallInfo *ci = L->ci;
  lua_lock(L);
  api_check(L, n >= 0, "negative 'n'");
  if (L->stack_last - L->top > n)  /* stack large enough? */
    res = 1;  /* yes; check is OK */
  else {  /* no; need to grow stack */
    int inuse = cast_int(L->top - L->stack) + EXTRA_STACK;
    if (inuse > LUAI_MAXSTACK - n)  /* can grow without overflow? */
      res = 0;  /* no */
    else  /* try to grow stack */
      res = (luaD_rawrunprotected(L, &growstack, &n) == LUA_OK);
  }
  if (res && ci->top < L->top + n)
    ci->top = L->top + n;  /* adjust frame top */
  lua_unlock(L);
  return res;
}

/*
** 从from的栈中取最顶部的n个栈单元内容到to的栈中。即将from的栈顶部往下的n个元素移到to的栈上。
** from的栈中会删除移动到to的栈中的这个几个元素。
*/
LUA_API void lua_xmove (lua_State *from, lua_State *to, int n) {
  int i;
  if (from == to) return;
  lua_lock(to);
  api_checknelems(from, n);
  api_check(from, G(from) == G(to), "moving among independent states");
  api_check(from, to->ci->top - to->top >= n, "stack overflow");
  
  /* 这两步相当于是移动完之后就删除了这n个元素。 */
  from->top -= n;
  for (i = 0; i < n; i++) {
    setobj2s(to, to->top, from->top + i);
    to->top++;  /* stack already checked by previous 'api_check' */
  }
  
  lua_unlock(to);
}


LUA_API lua_CFunction lua_atpanic (lua_State *L, lua_CFunction panicf) {
  lua_CFunction old;
  lua_lock(L);
  old = G(L)->panic;
  G(L)->panic = panicf;
  lua_unlock(L);
  return old;
}


/* 获取当前的lua version，一般在global_State中会存放版本号 */
LUA_API const lua_Number *lua_version (lua_State *L) {
  static const lua_Number version = LUA_VERSION_NUM;
  if (L == NULL) return &version;
  else return G(L)->version;
}



/*
** basic stack manipulation
*/


/*
** convert an acceptable stack index into an absolute index
*/
/*
** lua_absindex()用于取堆栈索引值得绝对值。这个绝对值并不完全和数学上的绝对值一样，
** 是具有上下文含义的绝对值，如下：
** 1.如果idx大于0，那么直接返回该值；
** 2.如果idx小于等于LUA_REGISTRYINDEX（一个负值），那么也直接返回该值；
** 3.如果idx在(LUA_REGISTRYINDEX,0]之间，则返回对应的绝对值。
*/
LUA_API int lua_absindex (lua_State *L, int idx) {
  return (idx > 0 || ispseudo(idx))
         ? idx
         : cast_int(L->top - L->ci->func) + idx;
}

/* 计算函数参数的个数 */
LUA_API int lua_gettop (lua_State *L) {
  return cast_int(L->top - (L->ci->func + 1));
}

/* 
** 重新设置堆栈的堆栈指针，
** idx >= 0时，L->top = L->ci_func + 1 + idx；
** idx < 0时，L->top= L->top + 1 + idx 
*/
LUA_API void lua_settop (lua_State *L, int idx) {
  /* 当前正在进行的函数调用对应的Closure对象所在栈单元 */
  StkId func = L->ci->func;
  lua_lock(L);
  if (idx >= 0) {
    /* idx >= 0时，检查上层传入的idx会不会超过整个堆栈的剩余的长度 */
    api_check(L, idx <= L->stack_last - (func + 1), "new top too large");
	/* 将从[func+1, func+1+idx]这部分的堆栈内容设置为nil */
    while (L->top < (func + 1) + idx)
      setnilvalue(L->top++);
    /* 更新堆栈指针，如果idx > 0，那么就是func+1+idx */
    L->top = (func + 1) + idx;
  }
  else {
    /* 
    ** 进入这个分支，说明idx < 0，这个时候要检查新的堆栈指针设置之后不能小于
    ** 当前函数的地址，必须大于或者等于当前函数指针。
    */
    api_check(L, -(idx+1) <= (L->top - (func + 1)), "invalid new top");
    L->top += idx+1;  /* 'subtract' index (index is negative) */
  }
  lua_unlock(L);
}


/*
** Reverse the stack segment from 'from' to 'to'
** (auxiliary to 'lua_rotate')
*/
static void reverse (lua_State *L, StkId from, StkId to) {
  for (; from < to; from++, to--) {
    TValue temp;
    setobj(L, &temp, from);
    setobjs2s(L, from, to);
    setobj2s(L, to, &temp);
  }
}


/*
** Let x = AB, where A is a prefix of length 'n'. Then,
** rotate x n == BA. But BA == (A^r . B^r)^r.
*/
LUA_API void lua_rotate (lua_State *L, int idx, int n) {
  StkId p, t, m;
  lua_lock(L);
  t = L->top - 1;  /* end of stack segment being rotated */
  p = index2addr(L, idx);  /* start of segment */
  api_checkstackindex(L, idx, p);
  api_check(L, (n >= 0 ? n : -n) <= (t - p + 1), "invalid 'n'");
  m = (n >= 0 ? t - n : p - n - 1);  /* end of prefix */
  reverse(L, p, m);  /* reverse the prefix with length 'n' */
  reverse(L, m + 1, t);  /* reverse the suffix */
  reverse(L, p, t);  /* reverse the entire segment */
  lua_unlock(L);
}


LUA_API void lua_copy (lua_State *L, int fromidx, int toidx) {
  TValue *fr, *to;
  lua_lock(L);
  fr = index2addr(L, fromidx);
  to = index2addr(L, toidx);
  api_checkvalidindex(L, to);
  setobj(L, to, fr);
  if (isupvalue(toidx))  /* function upvalue? */
    luaC_barrier(L, clCvalue(L->ci->func), fr);
  /* LUA_REGISTRYINDEX does not need gc barrier
     (collector revisits it before finishing collection) */
  lua_unlock(L);
}


/* lua_pushvalue()用于将idx对应的value压入栈顶，并更新栈顶 */
LUA_API void lua_pushvalue (lua_State *L, int idx) {
  lua_lock(L);
  setobj2s(L, L->top, index2addr(L, idx));
  /* 更新栈顶指针，堆栈增长+1 */
  api_incr_top(L);
  lua_unlock(L);
}



/*
** access functions (stack -> C)
*/

/* lua_type()函数用于获取在栈中索引为idx的TValue对象所包含数据的基本类型 */
LUA_API int lua_type (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  /* 
  ** 首先判断是不是非nil对象，如果不是nil对象，则获取该对象的基本类型并返回，
  ** 否则返回无类型
  */
  return (isvalid(o) ? ttnov(o) : LUA_TNONE);
}

/* lua_typename()函数用于获取类型t对应的名字，t是在lua.h中定义的类型对应的数字 */
LUA_API const char *lua_typename (lua_State *L, int t) {
  UNUSED(L);
  api_check(L, LUA_TNONE <= t && t < LUA_NUMTAGS, "invalid tag");
  return ttypename(t);
}

/*
** lua_iscfunction()函数用于判断在堆栈中索引值为idx的Value对象是不是一个函数，
** 注意，在lua中函数分为了两种类型，一种是没有自由变量的轻量级函数，另一种是闭包，
** 即带有自由变量的函数。
*/
LUA_API int lua_iscfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (ttislcf(o) || (ttisCclosure(o)));
}

/* lua_isinteger()函数用于判断在堆栈中索引值为idx的Value对象是不是一个整数 */
LUA_API int lua_isinteger (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return ttisinteger(o);
}

/* lua_isnumber()函数用于判断在堆栈中索引值为idx的Value对象是不是一个浮点数 */
LUA_API int lua_isnumber (lua_State *L, int idx) {
  lua_Number n;
  const TValue *o = index2addr(L, idx);
  return tonumber(o, &n);
}

/*
** lua_isstring()函数用于判断在堆栈中索引值为idx的Value对象是不是一个字符串，
** 如果是字符串，那么返回1；如果不是字符串，则判断该Value对象的实际内容是不是
** 数字，如果是数字的话，在定了宏LUA_NOCVTN2S的情况下，也返回1，因为数字是
** 可以转换为对应的字符串的。
*/
LUA_API int lua_isstring (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisstring(o) || cvt2str(o));
}

/* 
** lua_isuserdata()函数用于判断在堆栈中索引值为idx的Value对象是不是一个指针类型，
** lua中的指针类型包括两种，full userdata和light userdata，full userdata有独立
** 的元表，light userdata没有独立的元表，和其他类型共享元表。
*/
LUA_API int lua_isuserdata (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return (ttisfulluserdata(o) || ttislightuserdata(o));
}

/*
** lua_rawequal()函数用于判断在堆栈中索引值为index1和index2的两个Value对象是不是相等，
** 这个相等，不仅仅是值相等，还有基本类型、变种类型等都要相等。
*/
LUA_API int lua_rawequal (lua_State *L, int index1, int index2) {
  StkId o1 = index2addr(L, index1);
  StkId o2 = index2addr(L, index2);
  return (isvalid(o1) && isvalid(o2)) ? luaV_rawequalobj(o1, o2) : 0;
}


/* 
** lua_arith()函数用于对位于栈顶和次栈顶的两个操作数进行op操作，对于一元操作，
** 则在栈顶安排一个假的操作数，真的操作数位于次栈顶。如果有两个操作数，那么
** 分别位于栈顶和次栈顶。操作的结果位于次栈顶，即(L->top - 2)处。
*/
LUA_API void lua_arith (lua_State *L, int op) {
  lua_lock(L);
  /* 
  ** LUA_OPUNM和LUA_OPBNOT只需要一个操作数，因此这里做个判断，如果是这两种类型，
  ** 就看看操作数的个数是不是只有一个；如果不是这两种类型，那么就需要两个操作数。
  */
  if (op != LUA_OPUNM && op != LUA_OPBNOT)
    api_checknelems(L, 2);  /* all other operations expect two operands */
  else {  /* for unary operations, add fake 2nd operand */
    /* 
    ** 进入这个分支，说明是上述两种一元操作的某一种，这种情况下就增加一个假的操作数，
    ** 让其也有两个操作数。
    */
    api_checknelems(L, 1);
    setobjs2s(L, L->top, L->top - 1);
    api_incr_top(L);
  }
  
  /* first operand at top - 2, second at top - 1; result go to top - 2 */
  luaO_arith(L, op, L->top - 2, L->top - 1, L->top - 2);
  /* 
  ** 因为计算结果已经存放到了次栈顶，那么这个时候位于栈顶的操作数就无用了，
  ** 因此可以将其弹出堆栈了，弹出堆栈只要将堆栈指针减1即可。
  */
  L->top--;  /* remove second operand */
  lua_unlock(L);
}


/* 比较堆栈中存放在index1和index2这两个位置的value对象的大小 */
LUA_API int lua_compare (lua_State *L, int index1, int index2, int op) {
  StkId o1, o2;
  int i = 0;
  lua_lock(L);  /* may call tag method */
  /* 取出待比较的两个value */
  o1 = index2addr(L, index1);
  o2 = index2addr(L, index2);
  if (isvalid(o1) && isvalid(o2)) {
    /* 根据调用者传入的比较符号进行比较，如果满足o1 op o2，则返回1 */
    switch (op) {
      case LUA_OPEQ: i = luaV_equalobj(L, o1, o2); break;
      case LUA_OPLT: i = luaV_lessthan(L, o1, o2); break;
      case LUA_OPLE: i = luaV_lessequal(L, o1, o2); break;
      default: api_check(L, 0, "invalid option");
    }
  }
  lua_unlock(L);
  return i;
}

/*
** lua_stringtonumber()函数用于将字符串s转换为对应的数值，可能是整数，也可能是浮点数，
** 转换的结果存放在栈顶，并更新堆栈指针，然后返回字符串的长度。
*/
LUA_API size_t lua_stringtonumber (lua_State *L, const char *s) {
  /* 将字符串s转换为数值，并存放在栈顶，然后染回字符串长度。 */
  size_t sz = luaO_str2num(s, L->top);
  if (sz != 0)
    api_incr_top(L);
  return sz;
}


/* 将栈索引值为idx对应的TValue对象转换为number，即实数，如果转换成功，则结果作为返回值，pisnum为1。*/
LUA_API lua_Number lua_tonumberx (lua_State *L, int idx, int *pisnum) {
  lua_Number n;
  const TValue *o = index2addr(L, idx);
  int isnum = tonumber(o, &n);
  if (!isnum)
    n = 0;  /* call to 'tonumber' may change 'n' even if it fails */
  if (pisnum) *pisnum = isnum;
  return n;
}

/* 将栈索引值为idx的TValue对象转换为integer，即整数，如果转换成功，则结果作为返回值，pisnum为1。 */
LUA_API lua_Integer lua_tointegerx (lua_State *L, int idx, int *pisnum) {
  lua_Integer res;
  /* 取出idx在堆栈中对应的value值 */
  const TValue *o = index2addr(L, idx);
  int isnum = tointeger(o, &res);
  if (!isnum)
    res = 0;  /* call to 'tointeger' may change 'n' even if it fails */
  if (pisnum) *pisnum = isnum;
  return res;
}


LUA_API int lua_toboolean (lua_State *L, int idx) {
  const TValue *o = index2addr(L, idx);
  return !l_isfalse(o);
}

/* 
** lua_tolstring()函数用于判断堆栈中索引为idx的value对象是不是字符串，如果是的话，
** 就将该字符串的地址和长度返回给上层调用者；如果不是字符串的话，那么尝试将该value
** 对象中的实际内容转换为字符串，然后将地址和长度返回给调用者。
*/
LUA_API const char *lua_tolstring (lua_State *L, int idx, size_t *len) {
  StkId o = index2addr(L, idx);
  /* 
  ** 判断value的类型的是不是字符串。如果不是字符串的话，那么尝试将该value
  ** 对象中的实际内容转换为字符串，然后将地址和长度返回给调用者。
  */
  if (!ttisstring(o)) {
    if (!cvt2str(o)) {  /* not convertible? */
      if (len != NULL) *len = 0;
      return NULL;
    }
    lua_lock(L);  /* 'luaO_tostring' may create a new string */
    /* 将o中存放的数字转换为字符串，并存回o中 */
    luaO_tostring(L, o);
    luaC_checkGC(L);
    o = index2addr(L, idx);  /* previous call may reallocate the stack */
    lua_unlock(L);
  }
  
  /* 将字符串长度通过入参传递给调用者，并返回字符串的地址给调用者 */
  if (len != NULL)
    *len = vslen(o);
  return svalue(o);
}


LUA_API size_t lua_rawlen (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LUA_TSHRSTR: return tsvalue(o)->shrlen;
    case LUA_TLNGSTR: return tsvalue(o)->u.lnglen;
    case LUA_TUSERDATA: return uvalue(o)->len;
    case LUA_TTABLE: return luaH_getn(hvalue(o));
    default: return 0;
  }
}


LUA_API lua_CFunction lua_tocfunction (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  if (ttislcf(o)) return fvalue(o);
  else if (ttisCclosure(o))
    return clCvalue(o)->f;
  else return NULL;  /* not a C function */
}

/* 返回栈索引值为idx的TValue对象内的userdata对象的用户数据首地址 */
LUA_API void *lua_touserdata (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttnov(o)) {
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}


LUA_API lua_State *lua_tothread (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  return (!ttisthread(o)) ? NULL : thvalue(o);
}


LUA_API const void *lua_topointer (lua_State *L, int idx) {
  StkId o = index2addr(L, idx);
  switch (ttype(o)) {
    case LUA_TTABLE: return hvalue(o);
    case LUA_TLCL: return clLvalue(o);
    case LUA_TCCL: return clCvalue(o);
    case LUA_TLCF: return cast(void *, cast(size_t, fvalue(o)));
    case LUA_TTHREAD: return thvalue(o);
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}



/*
** push functions (C -> stack)
*/


LUA_API void lua_pushnil (lua_State *L) {
  lua_lock(L);
  setnilvalue(L->top);
  api_incr_top(L);
  lua_unlock(L);
}

/* 将实数n压入栈顶部，并更新栈指针+1 */
LUA_API void lua_pushnumber (lua_State *L, lua_Number n) {
  lua_lock(L);
  /* 将实数n存入L->top指定的栈位置，然后栈顶递增 */
  setfltvalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


/* 将整数n压入堆栈，并更新栈指针+1 */
LUA_API void lua_pushinteger (lua_State *L, lua_Integer n) {
  lua_lock(L);
  /* 将整数n存入L->top指定的堆栈位置，然后栈顶递增 */
  setivalue(L->top, n);
  api_incr_top(L);
  lua_unlock(L);
}


/*
** Pushes on the stack a string with given length. Avoid using 's' when
** 'len' == 0 (as 's' can be NULL in that case), due to later use of
** 'memcmp' and 'memcpy'.
*/
/* 
** 将C类型字符串s压入栈顶，如果s为NULL或者长度为0，那么压入空字符串对象（区别nil对象）；
** 如果不为NULL，则创建相应的string类型，将s存入string后将其压入栈顶。然后返回数据在
** string中的新地址。
*/
LUA_API const char *lua_pushlstring (lua_State *L, const char *s, size_t len) {
  TString *ts;
  lua_lock(L);
  ts = (len == 0) ? luaS_new(L, "") : luaS_newlstr(L, s, len);
  setsvalue2s(L, L->top, ts);
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return getstr(ts);
}

/* 
** 将C类型字符串s压入栈顶，如果s为NULL，那么压入nil对象；如果不为NULL，则
** 创建相应的string类型，将s存入string后将其压入栈顶。然后返回数据在string中
** 的新地址。
*/
LUA_API const char *lua_pushstring (lua_State *L, const char *s) {
  lua_lock(L);
  if (s == NULL)
    setnilvalue(L->top);
  else {
    TString *ts;
    ts = luaS_new(L, s);
    setsvalue2s(L, L->top, ts);
    s = getstr(ts);  /* internal copy's address */
  }
  api_incr_top(L);
  luaC_checkGC(L);
  lua_unlock(L);
  return s;
}


LUA_API const char *lua_pushvfstring (lua_State *L, const char *fmt,
                                      va_list argp) {
  const char *ret;
  lua_lock(L);
  ret = luaO_pushvfstring(L, fmt, argp);
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}


LUA_API const char *lua_pushfstring (lua_State *L, const char *fmt, ...) {
  const char *ret;
  va_list argp;
  lua_lock(L);
  va_start(argp, fmt);
  ret = luaO_pushvfstring(L, fmt, argp);
  va_end(argp);
  luaC_checkGC(L);
  lua_unlock(L);
  return ret;
}

/*
** lua_pushcclosure()将函数指针压入栈顶，分两种情况：
** 1. 如果是light c closure，那么直接将函数指针压入栈顶；
** 2. 如果是带有upvalue的c closure，那么需要创建一个CClosure对象，并
**    将函数指针保存到CClosure对象中，以及将该函数的所有upvalue也存放
**    到CClosure对象的upvalue数组成员中，然后将CClosure对象压入到该函数
**    第一个upvalue所在的堆栈位置（此时也就是堆栈的第一个元素），由于原先
**    存放在堆栈中的upvalue都已经存放到CClosure对象中了，因此覆盖也没关系。
*/
LUA_API void lua_pushcclosure (lua_State *L, lua_CFunction fn, int n) {
  lua_lock(L);
  if (n == 0) {
    /*
    ** 进入这个分支，说明函数没有upvalue，是一个light c closure，对于这种函数，
    ** 直接将函数指针压入栈顶，栈顶指针+1，堆栈增长。
    */
    setfvalue(L->top, fn);
    api_incr_top(L);
  }
  else {
    /*
    ** 进入这个分支，说明函数带有upvalue，处理步骤如下：
    ** 1. 检查该函数已经压入堆栈的upvalue的个数是否多余n，如果是，那么出错，进程退出；
    ** 2. 检查改函数的upvalue个数是否超过最大值，如果是，那么出错，进程退出；
    ** 3. 创建一个新的C Closure对象，并将待压入堆栈的函数指针存放到C Closure对象的fn成员中；
    ** 4. 将该函数已经压入堆栈的upvalue存放到C Closure对象的upvalue数组成员中；
    ** 5. 将该C Closure对象压入堆栈，栈顶指针+1，堆栈增长。
    */
    CClosure *cl;
    api_checknelems(L, n);
    api_check(L, n <= MAXUPVAL, "upvalue index too large");
	
    cl = luaF_newCclosure(L, n);
    cl->f = fn;
	/* 
	** L->top -= n执行后，L->top指向了该函数的第一个upvalue，然后将upvalue
	** 依次将upvalue保存到C Closure对象的upvalue数组中
	*/
    L->top -= n;
    while (n--) {
      setobj2n(L, &cl->upvalue[n], L->top + n);
      /* does not need barrier because closure is white */
    }
    /*
    ** 将CClosure对象压入堆栈，注意到，上面执行了语句“L->top -= n;”，因此这个时候的
    ** L->top其实指向了函数的第一个upvalue，那么这里将CClosure对象压入堆栈的话，CClosure
    ** 对象就覆盖了函数的第一个upvalue，但是由于函数的upvalue都保存到了CClosure中，所以没关系。
    ** CClosure对象压入堆栈后，堆栈增加，栈顶+1。
    */
    setclCvalue(L, L->top, cl);
    api_incr_top(L);
    luaC_checkGC(L);
  }
  lua_unlock(L);
}


/* 向栈顶压入一个bool值，b只要不是0，就当做true；否则就是false */
LUA_API void lua_pushboolean (lua_State *L, int b) {
  lua_lock(L);
  setbvalue(L->top, (b != 0));  /* ensure that true is 1 */
  api_incr_top(L);
  lua_unlock(L);
}


/* 向栈顶压入一个指针（即light userdata） */
LUA_API void lua_pushlightuserdata (lua_State *L, void *p) {
  lua_lock(L);
  setpvalue(L->top, p);
  api_incr_top(L);
  lua_unlock(L);
}


/*向栈顶压入lua thread自身，并且如果该线程不是主线程，则返回0，否则返回1. */
LUA_API int lua_pushthread (lua_State *L) {
  lua_lock(L);
  setthvalue(L, L->top, L);
  api_incr_top(L);
  lua_unlock(L);
  return (G(L)->mainthread == L);
}



/*
** get functions (Lua -> stack)
*/


/* 
** auxgetstr()函数用于从table t中取出键值为k的value对象，并将该对象压入堆栈，
** 并更新堆栈指针+1，然后返回该value对象的类型
*/
static int auxgetstr (lua_State *L, const TValue *t, const char *k) {
  const TValue *slot;
  
  /* 根据k来创建table的键值对象 */
  TString *str = luaS_new(L, k);
  /* 
  ** 首先尝试直接从table t中获取键值为k的value对象，如果获取成功且value对象不为nil，
  ** 则存放入slot中，之后将slot中的内容压入堆栈顶部，并更新堆栈指针。如果获取失败或者
  ** 获取到的value对象为nil，则先将键值对象str压入堆栈，并更新堆栈指针，然后由于之前
  ** 获取到的value对象为nil或者获取失败（t不是table类型），这个时候调用luaV_finishget()
  ** 尝试用table的元表中进行获取，结果也会压入堆栈。
  */
  if (luaV_fastget(L, t, str, slot, luaH_getstr)) {
    setobj2s(L, L->top, slot);
    api_incr_top(L);
  }
  else {
    setsvalue2s(L, L->top, str);
    api_incr_top(L);
    luaV_finishget(L, t, L->top - 1, L->top - 1, slot);
  }
  lua_unlock(L);

  /* 返回从table t中获取的然后被压入堆栈的value对象的类型 */
  return ttnov(L->top - 1);
}


/* 从全局环境表_G中获取键值为name的TValue对象，并将该对象压入栈顶，同时返回该对象的类型 */
LUA_API int lua_getglobal (lua_State *L, const char *name) {
  Table *reg = hvalue(&G(L)->l_registry);
  lua_lock(L);
  return auxgetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}


/* 
** 以栈顶部元素为键值从栈索引值为idx的table中获取对应的TValue对象，获取到TValue对象
** 之后，将该对象覆盖写到键值对象所在位置，同时返回该对象的类型。注意栈指针L->top没变。
*/
LUA_API int lua_gettable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  luaV_gettable(L, t, L->top - 1, L->top - 1);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*
** lua_getfield()函数首先调用index2addr()返回索引值idx对应的table的内存地址，
** 然后调用auxgestr()函数从上面这个table中获取键值为k对应的value对象，并将这个
** value对象压入堆栈，并返回这个value对象的类型。执行完lua_getfield()之后，位于
** 堆栈顶部的元素就是从table中获取到的value对象。
*/
LUA_API int lua_getfield (lua_State *L, int idx, const char *k) {
  lua_lock(L);
  return auxgetstr(L, index2addr(L, idx), k);
}


LUA_API int lua_geti (lua_State *L, int idx, lua_Integer n) {
  StkId t;
  const TValue *slot;
  lua_lock(L);
  t = index2addr(L, idx);
  if (luaV_fastget(L, t, n, slot, luaH_getint)) {
    setobj2s(L, L->top, slot);
    api_incr_top(L);
  }
  else {
    setivalue(L->top, n);
    api_incr_top(L);
    luaV_finishget(L, t, L->top - 1, L->top - 1, slot);
  }
  lua_unlock(L);
  return ttnov(L->top - 1);
}


LUA_API int lua_rawget (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top - 1, luaH_get(hvalue(t), L->top - 1));
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/* 
** 从栈索引值（可能是伪索引）为idx处的table中，以n为键值，获取相应的TValue对象，
** 并压入栈顶部，然后返回TValue对象实际的数据类型。
*/
LUA_API int lua_rawgeti (lua_State *L, int idx, lua_Integer n) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setobj2s(L, L->top, luaH_getint(hvalue(t), n));
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


LUA_API int lua_rawgetp (lua_State *L, int idx, const void *p) {
  StkId t;
  TValue k;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  setpvalue(&k, cast(void *, p));
  setobj2s(L, L->top, luaH_get(hvalue(t), &k));
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*
** lua_createtable()函数用于创建一个table，然后压入堆栈，并根据narray和nrec对新创建的table
** 的数组部分或者hash表部分的进行大小更新。
*/
LUA_API void lua_createtable (lua_State *L, int narray, int nrec) {
  Table *t;
  lua_lock(L);
  /* luaH_new()函数会创建一个空的lua table */
  t = luaH_new(L);
  /* 将新创建的表压入堆栈，并更新堆栈指针 */
  sethvalue(L, L->top, t);
  api_incr_top(L);
  /*
  ** narray用于指定table中数组部分的大小，nrec用于指定table中hash表部分的大小，
  ** 并根据这两个值对上面创建的新table进行数组或hash表的大小更新（申请相应内存）。
  */
  if (narray > 0 || nrec > 0)
    luaH_resize(L, t, narray, nrec);
  luaC_checkGC(L);
  lua_unlock(L);
}

/* 
** 获取栈索引值为objindex的对象的元表，table和userdata的每个对象都有自己的元表，
** 而其他类型则只有共有元表，然后将获取到的元表压入栈顶部，并更新栈指针，栈增长。
** 返回值为1，表示找到了元表，并成功压入栈顶部；返回值为0，表示没有找到元表，也没
** 压入栈顶部。
*/
LUA_API int lua_getmetatable (lua_State *L, int objindex) {
  const TValue *obj;
  Table *mt;
  int res = 0;
  lua_lock(L);
  obj = index2addr(L, objindex);
  switch (ttnov(obj)) {
    case LUA_TTABLE:
      mt = hvalue(obj)->metatable;
      break;
    case LUA_TUSERDATA:
      mt = uvalue(obj)->metatable;
      break;
    default:
      mt = G(L)->mt[ttnov(obj)];
      break;
  }
  if (mt != NULL) {
    sethvalue(L, L->top, mt);
    api_incr_top(L);
    res = 1;
  }
  lua_unlock(L);
  return res;
}


/* 
** 从栈索引值为idx处取出一个userdata对象，然后将userdata对象中存放的用户内容及其类型
** 写入栈顶。接着返回用户内容的类型。
*/
LUA_API int lua_getuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  /* 将userdata对象中存放的用户内容及其类型写入栈顶，更新栈指针 */
  getuservalue(L, uvalue(o), L->top);
  api_incr_top(L);
  lua_unlock(L);
  return ttnov(L->top - 1);
}


/*
** set functions (stack -> Lua)
*/

/*
** t[k] = value at the top of the stack (where 'k' is a string)
*/
/*
** t其实是一个table(可以根据auxsetstr()被调用的上下文得知)，这个函数其实就是将
** 位于堆栈顶部的value对象保存到t这个table中，对应的键值为k这个字符串。将位于
** 堆栈顶部的value对象存入table之后，会将该value对象弹出堆栈，也就是L->top会减1。
*/
static void auxsetstr (lua_State *L, const TValue *t, const char *k) {
  const TValue *slot;
  /* 创建一个值为k的字符串对象 */
  TString *str = luaS_new(L, k);
  api_checknelems(L, 1);
  if (luaV_fastset(L, t, str, slot, luaH_getstr, L->top - 1))
    L->top--;  /* pop value */
  else {
    setsvalue2s(L, L->top, str);  /* push 'str' (to make it a TValue) */
    api_incr_top(L);
    luaV_finishset(L, t, L->top - 1, L->top - 2, slot);
    L->top -= 2;  /* pop value and key */
  }
  lua_unlock(L);  /* lock done by caller */
}

/*
** lua_setglobal()函数用于将堆栈最顶部元素以name为键值存放到_G表中，堆栈最顶部元素
** 加入到_G之后就会从堆栈中弹出。
*/
LUA_API void lua_setglobal (lua_State *L, const char *name) {
  /* 获取全局唯一的注册表，注册表就是一个table */
  Table *reg = hvalue(&G(L)->l_registry);
  lua_lock(L);  /* unlock done in 'auxsetstr' */
  /*
  ** luaH_getint(reg, LUA_RIDX_GLOBALS)用于从全局注册表中取出下标为LUA_RIDX_GLOBALS
  ** 的value对象，这个value对象其实是一个表_G，它掌控了整个全局环境，保存了lua语言中
  ** 几乎所有的全局函数和变量。_G在初始情况是只包含lua程序库的函数和变量，lua程序中
  ** 定义的全局函数和变量会自动加入到_G中，而局部函数和变量不会这样做。
  */
  auxsetstr(L, luaH_getint(reg, LUA_RIDX_GLOBALS), name);
}

/*
** 以栈次顶部元素作为键值，将栈顶部元素添加到栈索引值为idx的table中，同时更新栈指针，
** 将栈顶部元素和栈次顶部元素都弹出栈，栈指针L->top指向原键值所在位置。
*/
LUA_API void lua_settable (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  api_checknelems(L, 2);
  t = index2addr(L, idx);
  luaV_settable(L, t, L->top - 2, L->top - 1);
  L->top -= 2;  /* pop index and value */
  lua_unlock(L);
}

/*
** lua_setfield()就是将位于堆栈顶部的value对象，以k这个字符串为键值，
** 存放到idx对应的table中，idx是table在堆栈中的索引，通过idx可以获取到table。
** 当位于堆栈顶部的value对象存入table之后，会被弹出堆栈。
*/
LUA_API void lua_setfield (lua_State *L, int idx, const char *k) {
  lua_lock(L);  /* unlock done in 'auxsetstr' */
  auxsetstr(L, index2addr(L, idx), k);
}


LUA_API void lua_seti (lua_State *L, int idx, lua_Integer n) {
  StkId t;
  const TValue *slot;
  lua_lock(L);
  api_checknelems(L, 1);
  t = index2addr(L, idx);
  if (luaV_fastset(L, t, n, slot, luaH_getint, L->top - 1))
    L->top--;  /* pop value */
  else {
    setivalue(L->top, n);
    api_incr_top(L);
    luaV_finishset(L, t, L->top - 1, L->top - 2, slot);
    L->top -= 2;  /* pop value and key */
  }
  lua_unlock(L);
}


LUA_API void lua_rawset (lua_State *L, int idx) {
  StkId o;
  TValue *slot;
  lua_lock(L);
  api_checknelems(L, 2);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  slot = luaH_set(L, hvalue(o), L->top - 2);
  setobj2t(L, slot, L->top - 1);
  invalidateTMcache(hvalue(o));
  luaC_barrierback(L, hvalue(o), L->top-1);
  L->top -= 2;
  lua_unlock(L);
}


LUA_API void lua_rawseti (lua_State *L, int idx, lua_Integer n) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  luaH_setint(L, hvalue(o), n, L->top - 1);
  luaC_barrierback(L, hvalue(o), L->top-1);
  L->top--;
  lua_unlock(L);
}


LUA_API void lua_rawsetp (lua_State *L, int idx, const void *p) {
  StkId o;
  TValue k, *slot;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttistable(o), "table expected");
  /* 将p设置到TValue中 */
  setpvalue(&k, cast(void *, p));
  slot = luaH_set(L, hvalue(o), &k);
  setobj2t(L, slot, L->top - 1);
  luaC_barrierback(L, hvalue(o), L->top - 1);
  L->top--;
  lua_unlock(L);
}

/* 
** lua_setmetatable()函数会将堆栈顶部的元素（可能是table，可能是nil）作为
** 堆栈索引值是objindex的value对象的元表，设置完成后，将堆栈顶部的元素弹出堆栈。
*/
LUA_API int lua_setmetatable (lua_State *L, int objindex) {
  TValue *obj;
  Table *mt;
  lua_lock(L);
  api_checknelems(L, 1);
  /* 从堆栈索引值objindex中取出对应的value对象 */
  obj = index2addr(L, objindex);
  /*
  ** 判断堆栈顶部的元素是不是nil对象，如果是则先将元表指针设置为NULL，
  ** 如果堆栈顶部的元素不是nil对象，那么断言堆栈顶部的元素是一个table，
  ** 因为从该函数调用的上下文可以知道，此时位于堆栈顶部的元素应该是一个table。
  ** 并将该table作为元表使用。
  */
  if (ttisnil(L->top - 1))
    mt = NULL;
  else {
    api_check(L, ttistable(L->top - 1), "table expected");
    mt = hvalue(L->top - 1);
  }

  /*
  ** 判断从堆栈索引值objindex中取出来的value对象的类型，
  ** 1. 如果该value对象的类型是table，那么就将mt作为该table的元表。
  ** 2. 如果该value对象的类型是userdata，即指针类型，那么也将mt作为该对象的元表。
  ** 3. 如果该value对象的类型是非上述两种类型，那么将mt作为全局状态信息中obj所属类型的元表。
  ** 从这里的实现可以看出，除了table、userdata之外的其他类型，每种类型的所有对象共享一个元表，
  ** 也就是说，所有字符串对象共享一个元表，所有整型共享一个元表，所有浮点型共享一个元表。
  ** 对于table、userdata类型，则每个table对象都有自己的元表，每个userdata对象都有自己的元表。
  */
  switch (ttnov(obj)) {
    case LUA_TTABLE: {
      hvalue(obj)->metatable = mt;
      if (mt) {
        luaC_objbarrier(L, gcvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    case LUA_TUSERDATA: {
      uvalue(obj)->metatable = mt;
      if (mt) {
        luaC_objbarrier(L, uvalue(obj), mt);
        luaC_checkfinalizer(L, gcvalue(obj), mt);
      }
      break;
    }
    default: {
      G(L)->mt[ttnov(obj)] = mt;
      break;
    }
  }
  
  /*
  ** 上述switch语句块中已经将堆栈顶部的元素（可能为nil，可能为一个table）设置为堆栈索引值是
  ** objindex的value对象的元表了，因此可能将其弹出堆栈，节省堆栈空间。
  */
  L->top--;
  lua_unlock(L);
  return 1;
}


LUA_API void lua_setuservalue (lua_State *L, int idx) {
  StkId o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = index2addr(L, idx);
  api_check(L, ttisfulluserdata(o), "full userdata expected");
  setuservalue(L, uvalue(o), L->top - 1);
  luaC_barrier(L, gcvalue(o), L->top - 1);
  L->top--;
  lua_unlock(L);
}


/*
** 'load' and 'call' functions (run Lua code)
*/


#define checkresults(L,na,nr) \
     api_check(L, (nr) == LUA_MULTRET || (L->ci->top - L->top >= (nr) - (na)), \
	"results from function overflow current stack size")

/* lua中要调用某个函数时，就会用这个函数来执行具体的函数调用。n是参数个数，r是返回值个数。 */
LUA_API void lua_callk (lua_State *L, int nargs, int nresults,
                        lua_KContext ctx, lua_KFunction k) {
  StkId func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);
  /* 获取待调用的Closure函数对象 */
  func = L->top - (nargs+1);
  if (k != NULL && L->nny == 0) {  /* need to prepare continuation? */
    /* 
    ** 程序执行到这里表明，在执行函数调用的过程中，可能会被中断，被中断后也需要恢复，
    ** 因此要保存上下文环境及断点
    */
    L->ci->u.c.k = k;  /* save continuation */
    L->ci->u.c.ctx = ctx;  /* save context */
    luaD_call(L, func, nresults);  /* do the call */
  }
  else  /* no continuation or no yieldable */
    /* 程序执行到这里，说明即将执行的函数调用不会被中断，而是一次性执行完毕 */
    luaD_callnoyield(L, func, nresults);  /* just do the call */
  adjustresults(L, nresults);
  lua_unlock(L);
}



/*
** Execute a protected call.
*/
/* 在保护模式下执行函数调用时，CallS保存的真正要执行的函数调用在栈中的地址及其返回值个数 */
struct CallS {  /* data to 'f_call' */
  StkId func;
  int nresults;
};

/* 用来在受保护模式下触发栈中的真正的需要受保护函数调用 */
static void f_call (lua_State *L, void *ud) {
  struct CallS *c = cast(struct CallS *, ud);
  luaD_callnoyield(L, c->func, c->nresults);
}


/*
** 以保护模式来运行栈中的函数调用，其中待执行的函数指针可以由L->top - (nargs+1)得到
*/
LUA_API int lua_pcallk (lua_State *L, int nargs, int nresults, int errfunc,
                        lua_KContext ctx, lua_KFunction k) {
  struct CallS c;
  int status;
  ptrdiff_t func;
  lua_lock(L);
  api_check(L, k == NULL || !isLua(L->ci),
    "cannot use continuations inside hooks");
  api_checknelems(L, nargs+1);
  api_check(L, L->status == LUA_OK, "cannot do calls on non-normal thread");
  checkresults(L, nargs, nresults);

  /* 获取错误处理函数func的索引，相对于整个虚拟栈的起始地址 */
  if (errfunc == 0)
    func = 0;
  else {
    StkId o = index2addr(L, errfunc);
    api_checkstackindex(L, errfunc, o);
    func = savestack(L, o);
  }

  /*
  ** 获取被调用函数的closure对象，这里的nargs是由函数参数传入的。例如在在luaL_dofile中调用
  ** lua_pcall时，这里传入的参数是0，换句话说，这里得到的函数closure对象就是前面
  ** luaY_parser函数中这两句代码放入Lua栈的Closure对象：
  **   setclLvalue(L, L->top, cl);
  **   luaD_inctop(L);
  */
  c.func = L->top - (nargs+1);  /* function to be called */

  /* L->nny大于0，或者k为null，说明线程目前暂时不允许被中断执行。 */
  if (k == NULL || L->nny > 0) {  /* no continuation or no yieldable? */
    c.nresults = nresults;  /* do a 'conventional' protected call */
    /*
    ** luaD_pcall()中会以保护模式来运行f_call()函数，而f_call()中又会触发调用上面的c.func
    ** f_call()用来在受保护模式下触发栈中的真正的需要受保护函数调用，这里就是c.func。
    ** c.func才是真正要在保护模式中运行的函数调用。
    */
    status = luaD_pcall(L, f_call, &c, savestack(L, c.func), func);
  }
  else {  /* prepare continuation (call is already protected by 'resume') */

    /* 程序进入这个分支，说明待执行的函数是可以被中断执行，然后在适当时候进行恢复执行的。 */

    /*
    ** 取当前正在执行的函数的调用信息，往函数调用信息中存入延续函数和上下文信息，以及在extra
    ** 成员中存入被中断函数的函数对象相对于线程栈基址的偏移量，以便在resume的时候进行恢复。
    ** 注意，这里的ci不是即将被调用函数的对应的函数调用信息，被调用函数对应的ci在luaD_call()中通过调
    ** luaD_precall()才生成。这里的ci一般来说就是调用函数lua_pcallk()的函数对应的函数调用信息。举个例子，
    ** lua_pcallk()函数会被luaB_pcall()函数调用，而luaB_pcall()函数正是对应lua接口中的pcall()函数。
    ** 假设我们在lua的main closure中调用pcall()，这种情况下，此处的ci就是main closure对应的ci。假设
    ** 我们在main closure中的某个lua函数xxxx()中调用pcall()函数，那么此处的ci就是xxxx()对应的ci。
    */
    CallInfo *ci = L->ci;
    ci->u.c.k = k;  /* save continuation */
    ci->u.c.ctx = ctx;  /* save context */
    /* save information for error recovery */
    ci->extra = savestack(L, c.func);

    /* 保存线程的错误处理函数，以便执行完函数之后进行恢复。 */
    ci->u.c.old_errfunc = L->errfunc;
    L->errfunc = func;

    /* 给当前的函数调用状态中打上所在线程的允许hook的标记位。 */
    setoah(ci->callstatus, L->allowhook);  /* save value of 'allowhook' */
	
    /* 在当前的函数调用状态中打上返回点恢复（以保护模式运行）的标记 */
    ci->callstatus |= CIST_YPCALL;  /* function can do error recovery */

	/* 通过调用luaD_call()来调用位于栈中的函数。注意此时的当前函数和栈中的函数是调用于被调用关系。 */
    luaD_call(L, c.func, nresults);  /* do the call */

    /* 移除返回点恢复（以保护模式运行）的标记以及线程的错误处理函数。 */
    ci->callstatus &= ~CIST_YPCALL;
    L->errfunc = ci->u.c.old_errfunc;
    status = LUA_OK;  /* if it is here, there were no errors */
  }
  
  adjustresults(L, nresults);
  lua_unlock(L);
  return status;
}

/* 
** 加载lua代码，并进行词法分析和语法分析，分析完成之后，保存了分析结果的LClosure对象
** 已经存放在了栈顶部。
*/
LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  ZIO z;
  int status;
  lua_lock(L);
  if (!chunkname) chunkname = "?";
  
  /* 初始化操作缓冲区的对象 */
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK) {  /* no errors? */
    LClosure *f = clLvalue(L->top - 1);  /* get newly created function */
    if (f->nupvalues >= 1) {  /* does it have an upvalue? */
      /* get global table from registry */
      Table *reg = hvalue(&G(L)->l_registry);
      const TValue *gt = luaH_getint(reg, LUA_RIDX_GLOBALS);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      setobj(L, f->upvals[0]->v, gt);
      luaC_upvalbarrier(L, f->upvals[0]);
    }
  }
  lua_unlock(L);
  return status;
}


LUA_API int lua_dump (lua_State *L, lua_Writer writer, void *data, int strip) {
  int status;
  TValue *o;
  lua_lock(L);
  api_checknelems(L, 1);
  o = L->top - 1;
  if (isLfunction(o))
    status = luaU_dump(L, getproto(o), writer, data, strip);
  else
    status = 1;
  lua_unlock(L);
  return status;
}


/* 返回线程的状态 */
LUA_API int lua_status (lua_State *L) {
  return L->status;
}


/*
** Garbage-collection function
*/

LUA_API int lua_gc (lua_State *L, int what, int data) {
  int res = 0;
  global_State *g;
  lua_lock(L);
  g = G(L);
  switch (what) {
    case LUA_GCSTOP: {
      g->gcrunning = 0;
      break;
    }
    case LUA_GCRESTART: {
      luaE_setdebt(g, 0);
      g->gcrunning = 1;
      break;
    }
    case LUA_GCCOLLECT: {
      luaC_fullgc(L, 0);
      break;
    }
    case LUA_GCCOUNT: {
      /* GC values are expressed in Kbytes: #bytes/2^10 */
      res = cast_int(gettotalbytes(g) >> 10);
      break;
    }
    case LUA_GCCOUNTB: {
      res = cast_int(gettotalbytes(g) & 0x3ff);
      break;
    }
    case LUA_GCSTEP: {
      l_mem debt = 1;  /* =1 to signal that it did an actual step */
      lu_byte oldrunning = g->gcrunning;
      g->gcrunning = 1;  /* allow GC to run */
      if (data == 0) {
        luaE_setdebt(g, -GCSTEPSIZE);  /* to do a "small" step */
        luaC_step(L);
      }
      else {  /* add 'data' to total debt */
        debt = cast(l_mem, data) * 1024 + g->GCdebt;
        luaE_setdebt(g, debt);
        luaC_checkGC(L);
      }
      g->gcrunning = oldrunning;  /* restore previous state */
      if (debt > 0 && g->gcstate == GCSpause)  /* end of cycle? */
        res = 1;  /* signal it */
      break;
    }
    case LUA_GCSETPAUSE: {
      res = g->gcpause;
      g->gcpause = data;
      break;
    }
    case LUA_GCSETSTEPMUL: {
      res = g->gcstepmul;
      if (data < 40) data = 40;  /* avoid ridiculous low values (and 0) */
      g->gcstepmul = data;
      break;
    }
    case LUA_GCISRUNNING: {
      res = g->gcrunning;
      break;
    }
    default: res = -1;  /* invalid option */
  }
  lua_unlock(L);
  return res;
}



/*
** miscellaneous functions
*/


LUA_API int lua_error (lua_State *L) {
  lua_lock(L);
  api_checknelems(L, 1);
  luaG_errormsg(L);
  /* code unreachable; will unlock when control actually leaves the kernel */
  return 0;  /* to avoid warnings */
}


LUA_API int lua_next (lua_State *L, int idx) {
  StkId t;
  int more;
  lua_lock(L);
  t = index2addr(L, idx);
  api_check(L, ttistable(t), "table expected");
  more = luaH_next(L, hvalue(t), L->top - 1);
  if (more) {
    api_incr_top(L);
  }
  else  /* no more elements */
    L->top -= 1;  /* remove key */
  lua_unlock(L);
  return more;
}


LUA_API void lua_concat (lua_State *L, int n) {
  lua_lock(L);
  api_checknelems(L, n);
  if (n >= 2) {
    luaV_concat(L, n);
  }
  else if (n == 0) {  /* push empty string */
    setsvalue2s(L, L->top, luaS_newlstr(L, "", 0));
    api_incr_top(L);
  }
  /* else n == 1; nothing to do */
  luaC_checkGC(L);
  lua_unlock(L);
}


LUA_API void lua_len (lua_State *L, int idx) {
  StkId t;
  lua_lock(L);
  t = index2addr(L, idx);
  luaV_objlen(L, L->top, t);
  api_incr_top(L);
  lua_unlock(L);
}


LUA_API lua_Alloc lua_getallocf (lua_State *L, void **ud) {
  lua_Alloc f;
  lua_lock(L);
  if (ud) *ud = G(L)->ud;
  f = G(L)->frealloc;
  lua_unlock(L);
  return f;
}


LUA_API void lua_setallocf (lua_State *L, lua_Alloc f, void *ud) {
  lua_lock(L);
  G(L)->ud = ud;
  G(L)->frealloc = f;
  lua_unlock(L);
}

/* lua_newuserdata()用于创建一个userdata对象，同时将该对象压入堆栈并返回其内部缓冲区的首地址 */
LUA_API void *lua_newuserdata (lua_State *L, size_t size) {
  Udata *u;
  lua_lock(L);
  /* 创建一个缓冲区大小为size的Udate对象，并将该对象压入堆栈顶部 */
  u = luaS_newudata(L, size);
  setuvalue(L, L->top, u);
  api_incr_top(L);
  
  luaC_checkGC(L);
  lua_unlock(L);
  /* 返回封装在Udata对象内部的缓冲区(块)的首地址 */
  return getudatamem(u);
}



static const char *aux_upvalue (StkId fi, int n, TValue **val,
                                CClosure **owner, UpVal **uv) {
  switch (ttype(fi)) {
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      if (!(1 <= n && n <= f->nupvalues)) return NULL;
      *val = &f->upvalue[n-1];
      if (owner) *owner = f;
      return "";
    }
    case LUA_TLCL: {  /* Lua closure */
      LClosure *f = clLvalue(fi);
      TString *name;
      Proto *p = f->p;
      if (!(1 <= n && n <= p->sizeupvalues)) return NULL;
      *val = f->upvals[n-1]->v;
      if (uv) *uv = f->upvals[n - 1];
      name = p->upvalues[n-1].name;
      return (name == NULL) ? "(*no name)" : getstr(name);
    }
    default: return NULL;  /* not a closure */
  }
}


LUA_API const char *lua_getupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  lua_lock(L);
  name = aux_upvalue(index2addr(L, funcindex), n, &val, NULL, NULL);
  if (name) {
    setobj2s(L, L->top, val);
    api_incr_top(L);
  }
  lua_unlock(L);
  return name;
}


LUA_API const char *lua_setupvalue (lua_State *L, int funcindex, int n) {
  const char *name;
  TValue *val = NULL;  /* to avoid warnings */
  CClosure *owner = NULL;
  UpVal *uv = NULL;
  StkId fi;
  lua_lock(L);
  fi = index2addr(L, funcindex);
  api_checknelems(L, 1);
  name = aux_upvalue(fi, n, &val, &owner, &uv);
  if (name) {
    L->top--;
    setobj(L, val, L->top);
    if (owner) { luaC_barrier(L, owner, L->top); }
    else if (uv) { luaC_upvalbarrier(L, uv); }
  }
  lua_unlock(L);
  return name;
}


static UpVal **getupvalref (lua_State *L, int fidx, int n, LClosure **pf) {
  LClosure *f;
  StkId fi = index2addr(L, fidx);
  api_check(L, ttisLclosure(fi), "Lua function expected");
  f = clLvalue(fi);
  api_check(L, (1 <= n && n <= f->p->sizeupvalues), "invalid upvalue index");
  if (pf) *pf = f;
  return &f->upvals[n - 1];  /* get its upvalue pointer */
}


LUA_API void *lua_upvalueid (lua_State *L, int fidx, int n) {
  StkId fi = index2addr(L, fidx);
  switch (ttype(fi)) {
    case LUA_TLCL: {  /* lua closure */
      return *getupvalref(L, fidx, n, NULL);
    }
    case LUA_TCCL: {  /* C closure */
      CClosure *f = clCvalue(fi);
      api_check(L, 1 <= n && n <= f->nupvalues, "invalid upvalue index");
      return &f->upvalue[n - 1];
    }
    default: {
      api_check(L, 0, "closure expected");
      return NULL;
    }
  }
}


LUA_API void lua_upvaluejoin (lua_State *L, int fidx1, int n1,
                                            int fidx2, int n2) {
  LClosure *f1;
  UpVal **up1 = getupvalref(L, fidx1, n1, &f1);
  UpVal **up2 = getupvalref(L, fidx2, n2, NULL);
  luaC_upvdeccount(L, *up1);
  *up1 = *up2;
  (*up1)->refcount++;
  if (upisopen(*up1)) (*up1)->u.open.touched = 1;
  luaC_upvalbarrier(L, *up1);
}


