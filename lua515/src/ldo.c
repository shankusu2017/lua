/*
** $Id: ldo.c,v 2.38.1.4 2012/01/18 02:27:10 roberto Exp $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/


#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define ldo_c
#define LUA_CORE

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"
#include "lzio.h"




/*
** {======================================================
** Error-recovery functions
** =======================================================
*/


/* chain list of long jump buffers */
struct lua_longjmp {
  struct lua_longjmp *previous;
  luai_jmpbuf b;
  volatile int status;  /* error code */
};


void luaD_seterrorobj (lua_State *L, int errcode, StkId oldtop) {
  switch (errcode) {
    case LUA_ERRMEM: {
      setsvalue2s(L, oldtop, luaS_newliteral(L, MEMERRMSG));
      break;
    }
    case LUA_ERRERR: {
      setsvalue2s(L, oldtop, luaS_newliteral(L, "error in error handling"));
      break;
    }
    case LUA_ERRSYNTAX:
    case LUA_ERRRUN: {
      setobjs2s(L, oldtop, L->top - 1);  /* error message on current top */
      break;
    }
  }
  /* 这里结合 luaD_pcall 来一起来看 */
  L->top = oldtop + 1;	/* correct top */
}

/* 空闲的callInfo过多时，尝试压缩其空间 */
static void restore_stack_limit (lua_State *L) {
  lua_assert(L->stack_last - L->stack == L->stacksize - EXTRA_STACK - 1);
  if (L->size_ci > LUAI_MAXCALLS) {  /* there was an overflow? */
    int inuse = cast_int(L->ci - L->base_ci);
    if (inuse + 1 < LUAI_MAXCALLS)  /* can `undo' overflow? */
      luaD_reallocCI(L, LUAI_MAXCALLS);
  }
}

/* 回滚stack到初始状态！！！！ */
static void resetstack (lua_State *L, int status) {
  /* 这一下彻底回滚了 */
  L->ci = L->base_ci;
  L->base = L->ci->base;
  
  luaF_close(L, L->base);  /* close eventual pending closures */
  luaD_seterrorobj(L, status, L->base);
  L->nCcalls = L->baseCcalls;
  L->allowhook = 1;
  restore_stack_limit(L);
  L->errfunc = 0;
  L->errorJmp = NULL;
}

/* 尝试调用异常处理函数 
** 主要在luaG_errormsg中被间接调用
*/
void luaD_throw (lua_State *L, int errcode) {
  if (L->errorJmp) {
    L->errorJmp->status = errcode;  /* !!! 跳出去之前设置status */
    LUAI_THROW(L, L->errorJmp); 	/* 正式跳出 */
  }
  else {	/* 没有设置errHdl，调用panic后退出进程 */
    L->status = cast_byte(errcode);	/* 无jump点了，在这里设置L的状态，有则由上层业务处理 */
    if (G(L)->panic) {
      resetstack(L, errcode);	/* 这里对stack进行收尾 */
      lua_unlock(L);
      G(L)->panic(L);
    }
    exit(EXIT_FAILURE);
  }
}

/* 保护模式下(longjump)调用C函数
** 但发生错误，则调用了L->errfunc后(若设置了)，后走到这里而不是直接退出进程
** 
** RETURN：执行流的执行结果，没有同步到L->status中(由上层调用决定是否同步)
*/
int luaD_rawrunprotected (lua_State *L, Pfunc f, void *ud) {
  struct lua_longjmp lj;
  lj.status = 0;
  lj.previous = L->errorJmp;  /* chain new error handler */
  L->errorJmp = &lj;
  LUAI_TRY(L, &lj,
    (*f)(L, ud);
  );
  L->errorJmp = lj.previous;  /* restore old error handler */
  return lj.status;	/* luaD_throw()中更新了status */
}

/* }====================================================== */

/* stack移动后更新upvalues,ci-list和L->base */
static void correctstack (lua_State *L, TValue *oldstack) {
  CallInfo *ci;
  GCObject *up;
  L->top = (L->top - oldstack) + L->stack;
  for (up = L->openupval; up != NULL; up = up->gch.next)
    gco2uv(up)->v = (gco2uv(up)->v - oldstack) + L->stack;
  for (ci = L->base_ci; ci <= L->ci; ci++) {
    ci->top = (ci->top - oldstack) + L->stack;
    ci->base = (ci->base - oldstack) + L->stack;
    ci->func = (ci->func - oldstack) + L->stack;
  }
  /* L->ci不用调整哈 */
  L->base = (L->base - oldstack) + L->stack;
}

/* 重新调整stack的大小 */
void luaD_reallocstack (lua_State *L, int newsize) {
  TValue *oldstack = L->stack;
  int realsize = newsize + 1 + EXTRA_STACK;
  lua_assert(L->stack_last - L->stack == L->stacksize - EXTRA_STACK - 1);	/* 和stack_init()函数对应 */
  luaM_reallocvector(L, L->stack, L->stacksize, realsize, TValue);
  L->stacksize = realsize;
  L->stack_last = L->stack+newsize;
  correctstack(L, oldstack);
}

/* 调整callInfo链的大小 */
void luaD_reallocCI (lua_State *L, int newsize) {
  CallInfo *oldci = L->base_ci;
  luaM_reallocvector(L, L->base_ci, L->size_ci, newsize, CallInfo);
  L->size_ci = newsize;
  L->ci = (L->ci - oldci) + L->base_ci;
  L->end_ci = L->base_ci + L->size_ci - 1;
}


void luaD_growstack (lua_State *L, int n) {
  if (n <= L->stacksize)  /* double size is enough? */
    luaD_reallocstack(L, 2*L->stacksize);
  else
    luaD_reallocstack(L, L->stacksize + n);
}


static CallInfo *growCI (lua_State *L) {
  if (L->size_ci > LUAI_MAXCALLS)  /* overflow while handling overflow? 嵌套调用层次太深了，直接报错，方便用户检查调用情况 */
    luaD_throw(L, LUA_ERRERR);
  else {
    luaD_reallocCI(L, 2*L->size_ci);	/* 简单粗暴，直接扩大一倍 */
    if (L->size_ci > LUAI_MAXCALLS)
      luaG_runerror(L, "stack overflow");
  }
  return ++L->ci;
}

  static StkId callrethooks (lua_State *L, StkId firstResult) {
  ptrdiff_t fr = savestack(L, firstResult);  /* next call may change stack */
  luaD_callhook(L, LUA_HOOKRET, -1);
  if (f_isLua(L->ci)) {  /* Lua function? */
    while ((L->hookmask & LUA_MASKRET) && L->ci->tailcalls--) /* tail calls */
      luaD_callhook(L, LUA_HOOKTAILRET, -1);
  }
  return restorestack(L, fr);
}


/* 调用钩子函数 */
void luaD_callhook (lua_State *L, int event, int line) {
  lua_Hook hook = L->hook;
  if (hook && L->allowhook) {
    ptrdiff_t top = savestack(L, L->top);
    ptrdiff_t ci_top = savestack(L, L->ci->top);
    lua_Debug ar;
    ar.event = event;
    ar.currentline = line;
    if (event == LUA_HOOKTAILRET)
      ar.i_ci = 0;  /* tail call; no debug information about it */
    else
      ar.i_ci = cast_int(L->ci - L->base_ci);
    luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */
    L->ci->top = L->top + LUA_MINSTACK;
    lua_assert(L->ci->top <= L->stack_last);
    L->allowhook = 0;  /* cannot call hooks inside a hook */
    lua_unlock(L);
    (*hook)(L, &ar);	/* 正式调用钩子函数 */
    lua_lock(L);
    lua_assert(!L->allowhook);
	/* !!!! 现场需恢复，别忘了，亲 */
    L->allowhook = 1;
    L->ci->top = restorestack(L, ci_top);
    L->top = restorestack(L, top);
  }
}

/*
**补齐固定形参(若实际传入的参数不够)
**将传给固定形参的值mv到top之上且纠正top
**将剩下(若还有剩下)的参数留给变参...
*/
static StkId adjust_varargs (lua_State *L, Proto *p, int actual) {
  int i;
  int nfixargs = p->numparams;
  Table *htab = NULL;
  StkId base, fixed;
  
  /* 传入的参数数量不够填补fixed参数的，直接补nil：至少得把fixed形参需要的个数补齐 */
  for (; actual < nfixargs; ++actual)	
    setnilvalue(L->top++);
  
#if defined(LUA_COMPAT_VARARG)	/* 将留给...的参数信息打包到额外的arg表中 */
  if (p->is_vararg & VARARG_NEEDSARG) { /* compat. with old-style vararg? */
    int nvar = actual - nfixargs;  /* number of extra arguments */
    lua_assert(p->is_vararg & VARARG_HASARG);
    luaC_checkGC(L);
    luaD_checkstack(L, p->maxstacksize);
    htab = luaH_new(L, nvar, 1);  /* create `arg' table */
    for (i=0; i<nvar; i++)  /* put extra arguments into `arg' table */
      setobj2n(L, luaH_setnum(L, htab, i+1), L->top - nvar + i);
    /* store counter in field `n' */
    setnvalue(luaH_setstr(L, htab, luaS_newliteral(L, "n")), cast_num(nvar));
  }
#endif
  /* move fixed parameters to final position */
  fixed = L->top - actual;  /* first fixed argument */
  base = L->top;  /* final position of first argument */

  /* 从第一个参数开始移动其值到被调函数的fixed‘arg域,直到给所有的fixed'arg赋值为止
  ** 如果还剩下多余的参数，则直接保留下来(留给变参...)，无需移动
  */
  for (i=0; i<nfixargs; i++) {	
    setobjs2s(L, L->top++, fixed+i);	/* !!!!这里移动了top指针 */
    setnilvalue(fixed+i);
  }
  /* add `arg' parameter */
  if (htab) {
    sethvalue(L, L->top++, htab);
    lua_assert(iswhite(obj2gco(htab)));
  }
  return base;
}

/* 直接看代码 */
static StkId tryfuncTM (lua_State *L, StkId func) {
  const TValue *tm = luaT_gettmbyobj(L, func, TM_CALL);
  StkId p;
  ptrdiff_t funcr = savestack(L, func);
  if (!ttisfunction(tm))
    luaG_typeerror(L, func, "call");
  /* Open a hole inside the stack at `func' */
  for (p = L->top; p > func; p--) setobjs2s(L, p, p-1);
  incr_top(L);
  func = restorestack(L, funcr);  /* previous call may change stack */
  setobj2s(L, func, tm);  /* tag method is the new function to be called */
  return func;
}



#define inc_ci(L) \
  ((L->ci == L->end_ci) ? growCI(L) : \
   (condhardstacktests(luaD_reallocCI(L, L->size_ci)), ++L->ci))

/* 先做调用前的准备工作，后进入函数调用(for C,not Lua)
** nresults:-1返回所有的返回值
** 0：不要返回值
** 1：期待一个返回值
*/
int luaD_precall (lua_State *L, StkId func, int nresults) {
  LClosure *cl;
  ptrdiff_t funcr;	/* 当前调用函数的pc距离stack栈底的偏移量 */
  if (!ttisfunction(func)) /* `func' is not a function? */
    func = tryfuncTM(L, func);  /* check the `function' tag method */
  
  /* 随着新的调用产生,ci链/stack可能因为增长而移动位置
  ** 故不能记住绝地位置而记住相对位置，后面根据此值最终确定ci->func 
  */
  funcr = savestack(L, func);	
  cl = &clvalue(func)->l;
  L->ci->savedpc = L->savedpc;	/* 正式调用前，存档L->savedpc至L->ci->savedpc */
  if (!cl->isC) {  /* Lua function? prepare its call */
    CallInfo *ci;
    StkId st, base;
    Proto *p = cl->p;
    luaD_checkstack(L, p->maxstacksize);
    func = restorestack(L, funcr);
    if (!p->is_vararg) {  /* no varargs?(不是变参函数?即函数参数数量固定) */
      base = func + 1;
      if (L->top > base + p->numparams)	/* 删除栈上多余的传入参数 */
        L->top = base + p->numparams;
    }
    else {  /* vararg function */
      int nargs = cast_int(L->top - func) - 1;	/* 计算实际传入的参数个数 */
      base = adjust_varargs(L, p, nargs);
      func = restorestack(L, funcr);  /* previous call may change the stack */
    }
    ci = inc_ci(L);  /* now `enter' new function */
    ci->func = func;
    L->base = ci->base = base;
	/* 这里可以推导出L->base---->L->top之间的区域都是ci的私有栈空间(lua,c均如此) */
    ci->top = L->base + p->maxstacksize;
    lua_assert(ci->top <= L->stack_last);
    L->savedpc = p->code;  /* starting point */
    ci->tailcalls = 0;
    ci->nresults = nresults;
	
	/* 新的函数的私有栈空间直接补nil(参数的区域除外),                    
	** 编译模块中的 lcode.c 中的 luaK_nil 函数默认这一点！！！！！！！！ 
	*/
    for (st = L->top; st < ci->top; st++)
      setnilvalue(st);	

	/* 最后调整L->top使其指向本次ci的栈顶,对于Lua函数而言L->Base---->(L->Base+L->maxstacksize)之间都是我私有的了，且是有效的
	** C由于L->top是动态变化的，故而L->top的值被设置为传入参数后栈顶的位置，后面会因为push等函数而动态变化-
	*/
    L->top = ci->top;
	
    if (L->hookmask & LUA_MASKCALL) {
      L->savedpc++;  /* hooks assume 'pc' is already incremented */
      luaD_callhook(L, LUA_HOOKCALL, -1);
      L->savedpc--;  /* correct 'pc' */
    }
    return PCRLUA;
  }
  else {  /* if is a C function, call it */
    CallInfo *ci;
    int n;
    luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */
	/* 填充新的CallInfo */
    ci = inc_ci(L);  /* now `enter' new function */
    ci->func = restorestack(L, funcr);
    L->base = ci->base = ci->func + 1;	/* C函数没有Lua函数的变参问题，所以这里无需adjust_varargs() */
	/* "OP_CALL指令"已经将L->top指向了最后一个传入参数的上方 */
    ci->top = L->top + LUA_MINSTACK;	/* 这里和上面luaD_checkstack呼应 */
    lua_assert(ci->top <= L->stack_last);
    ci->nresults = nresults;
    if (L->hookmask & LUA_MASKCALL)
      luaD_callhook(L, LUA_HOOKCALL, -1);
    lua_unlock(L);
	// L->top已经在lvm中准备好了(call和vararg指令)
    n = (*curr_func(L)->c.f)(L);  /* do the actual call */
    lua_lock(L);
    if (n < 0)  /* yielding, co调用yield，co.yeild运行完毕了,co.yeild还不能释放ci-list信息，需等到母thread调用resume，将控制权转移到co，再在co.resume中luaD_poscall()才释放 */
      return PCRYIELD;
    else {
      luaD_poscall(L, L->top - n);	/* 调整子C函数的返回值到指定位置并适配母函数的wanted(results) */
      return PCRC;
    }
  }
}

/* 函数调用结束后，处理实际返回值和期待返回值的匹配问题
** 也处理ci链的嵌套逻辑（本层ci结束往后退一层)
**
** 即处理C函数调用,也处理Lua函数执行结束即将返回这两种情况
** 没有检测C函数说返回了n个参数，当实际上没有返回那么多参数的情况
** RETURNS: wanted.cnt: 0:返回多个参数，1：返回0个，2：返回1个。。。
*/
int luaD_poscall (lua_State *L, StkId firstResult) {
  StkId res;
  int wanted, i;
  CallInfo *ci;
  if (L->hookmask & LUA_MASKRET)
    firstResult = callrethooks(L, firstResult);
  ci = L->ci--;
  res = ci->func;  /* res == final position of 1st result */
  wanted = ci->nresults;
  L->base = (ci - 1)->base;  /* restore base */
  L->savedpc = (ci - 1)->savedpc;  /* restore savedpc */
  
  /* move results to correct place */
  for (i = wanted; i != 0 && firstResult < L->top; i--)	/* 这个判断即处理非尾调用，又处理了尾调用 */
    setobjs2s(L, res++, firstResult++);	/* wanted根据实际返回数量赋值 */
  while (i-- > 0)
    setnilvalue(res++);	/* local a, b, c = funcA(...), 针对 funcA的返回值不够则补nil */

  /*
  ** L->top恢复到最后一个返回参数在stack的位置，这里和调用函数之前，
  ** 将L->top设置到最后一个传入参数在stack的位置相呼应了！！！
  ** 
  ** 最终将L->top恢复到ci->top是由“OP_CALL”指令负责
  */
  L->top = res;	
  return (wanted - LUA_MULTRET);  /* 0 iff wanted == LUA_MULTRET */
}


/*
** Call a function (C or Lua). The function to be called is at *func.
** The arguments are on the stack, right after the function.
** When returns, all the results are on the stack, starting at the original
** function position.
*/ 
void luaD_call (lua_State *L, StkId func, int nResults) {
  if (++L->nCcalls >= LUAI_MAXCCALLS) {	/* 调用层次太深，进入抛出异常 */
    if (L->nCcalls == LUAI_MAXCCALLS)
      luaG_runerror(L, "C stack overflow");
    else if (L->nCcalls >= (LUAI_MAXCCALLS + (LUAI_MAXCCALLS>>3)))
      luaD_throw(L, LUA_ERRERR);  /* error while handing stack error */
  }
  if (luaD_precall(L, func, nResults) == PCRLUA)  /* is a Lua function? */
    luaV_execute(L, 1);  /* call it, 这里的1是真的妙啊 */
  L->nCcalls--;
  luaC_checkGC(L);
}

/* 协程co开始执行co.resume 母thread在lbaselib.auxresume()中交出CPU，等待子co返回 */
static void resume (lua_State *L, void *ud) {
  StkId firstArg = cast(StkId, ud);	/* 没有传参时firstArg指向top,下面的firstArg>L->base还是成立 */
  CallInfo *ci = L->ci;
  if (L->status == 0) {  /* start coroutine? */
    lua_assert(ci == L->base_ci);	/* 尚未有任何调用链ci生成(或co已运行完毕) */
	  lua_assert(firstArg > L->base);	/* 至少还有个参数(是co.fun),意味着不是co运行完毕的状态，运行完毕后不能调用本函数了，co.fun都没有了，ci也是空的，ro不知道该怎么运行了不是 */
    /* 若是崭新的co第一次开始运行resume,则会生成相应的ci（co.initFun),再运行起来和普通的c.main中构建一个thread后第一次运行是一样的 */
    if (luaD_precall(L, firstArg - 1, LUA_MULTRET) != PCRLUA)
        return;
  } else {  /* resuming from previous yield */
    lua_assert(L->status == LUA_YIELD);	/* 非YEILD状态，不能调用resume */
    L->status = 0;	/* switch back status */
    if (!f_isLua(ci)) {  /* `common' yield? ci这里指向的是baselib.yield */
      /* finish interrupted execution of `OP_CALL' */
      lua_assert(GET_OPCODE(*((ci-1)->savedpc - 1)) == OP_CALL ||
                 GET_OPCODE(*((ci-1)->savedpc - 1)) == OP_TAILCALL);
      if (luaD_poscall(L, firstArg))  /* complete it... 结束上述说的baselib.yield的调用流程 */
        L->top = L->ci->top;  /* and correct top if not multiple results,如果是 multiple results则由跟在后面的vararg或者setlist来调整L->top(他们还需要用到L->top来确定传入参数的个数呢,所以这里不能将其恢复到L->ci->top，) */
    }
    else  /* yielded inside a hook: just continue its execution */
      L->base = L->ci->base;
  }
  
  luaV_execute(L, cast_int(L->ci - L->base_ci));	/* 这里的nexeccalls值得好好推导一下 */
}


static int resume_error (lua_State *L, const char *msg) {
  L->top = L->ci->base;
  setsvalue2s(L, L->top, luaS_new(L, msg));
  incr_top(L);
  lua_unlock(L);
  return LUA_ERRRUN;
}


LUA_API int lua_resume (lua_State *L, int nargs) {
  int status;
  lua_lock(L);
  if (L->status != LUA_YIELD && (L->status != 0 || L->ci != L->base_ci))
      return resume_error(L, "cannot resume non-suspended coroutine");
  if (L->nCcalls >= LUAI_MAXCCALLS)
    return resume_error(L, "C stack overflow");
  luai_userstateresume(L, nargs);
  lua_assert(L->errfunc == 0);
  L->baseCcalls = ++L->nCcalls;
  /* 必须protected状态下call，不然协程出错，整个进程都会被关闭 
  ** 本函数还没有为co生成ci链,resume中会生成co的ci调用链（如果是第一次resume）
  */
  status = luaD_rawrunprotected(L, resume, L->top - nargs);	
  if (status != 0) {  /* error? */
    L->status = cast_byte(status);  /* mark thread as `dead' */
    luaD_seterrorobj(L, status, L->top);
    L->ci->top = L->top;	/* 上面压入了errMsg这里更新下top */
  }
  else {
    lua_assert(L->nCcalls == L->baseCcalls);
    status = L->status;	/* coroutinue运行中出让则为 LUA_YIELD */
  }
  --L->nCcalls;
  lua_unlock(L);
  return status;
}


LUA_API int lua_yield (lua_State *L, int nresults) {
  luai_userstateyield(L, nresults);
  lua_lock(L);
  if (L->nCcalls > L->baseCcalls)
    luaG_runerror(L, "attempt to yield across metamethod/C-call boundary");
  L->base = L->top - nresults;  /* protect stack slots below */
  L->status = LUA_YIELD;
  lua_unlock(L);
  return -1;	/* note:这是一个特殊的值，用于标识从yield返回 */
}

/* 
** old_top 指向被调用函数slot 
*/
int luaD_pcall (lua_State *L, Pfunc func, void *u,
                ptrdiff_t old_top, ptrdiff_t ef) {
  int status;
  unsigned short oldnCcalls = L->nCcalls;
  
  /* 存档当前的ci,以便发生错误恢复时使用 */
  ptrdiff_t old_ci = saveci(L, L->ci);	/* 这里只能记住offset而不是绝对地址(call过程中ci可能会调整!) */
  
  lu_byte old_allowhooks = L->allowhook;
  ptrdiff_t old_errfunc = L->errfunc;
  L->errfunc = ef;
  status = luaD_rawrunprotected(L, func, u);

  /* 发生了错误，回滚到存档时刻 */
  if (status != 0) {  /* an error occurred? */
    StkId oldtop = restorestack(L, old_top);
    luaF_close(L, oldtop);  /* close eventual pending closures */
    luaD_seterrorobj(L, status, oldtop);	/* 顺带correct了top */
    L->nCcalls = oldnCcalls;
    L->ci = restoreci(L, old_ci);
    L->base = L->ci->base;
    L->savedpc = L->ci->savedpc;
    L->allowhook = old_allowhooks;
    restore_stack_limit(L);
  }
  L->errfunc = old_errfunc;
  return status;
}



/*
** Execute a protected parser.
** 此结构体鸡肋，为满足luaD_pcall函数调用的原型要求而增
** 现实和理想的妥协才是真正的生活，才是工业级的代码的常态，不要只有学院派的圆满哦，现实是复杂多变的。
*/
struct SParser {  /* data to `f_parser' */
  ZIO *z;			/* 底层的io句柄(C++称为对象) */
  Mbuffer buff;  	/* buffer to be used by the scanner */
  const char *name;	/* chunk name */
};

static void f_parser (lua_State *L, void *ud) {
  int i;
  Proto *tf;
  Closure *cl;
  struct SParser *p = cast(struct SParser *, ud);
  int c = luaZ_lookahead(p->z);
  luaC_checkGC(L);
  tf = ((c == LUA_SIGNATURE[0]) ? luaU_undump : luaY_parser)(L, p->z,
                                                             &p->buff, p->name);
  cl = luaF_newLclosure(L, tf->nups, hvalue(gt(L)));	/* 新生成的clouse的env直接来自gobal'table而不是上层函数的env */
  cl->l.p = tf;
  for (i = 0; i < tf->nups; i++)  /* initialize eventual upvalues */
    cl->l.upvals[i] = luaF_newupval(L);
  setclvalue(L, L->top, cl);
  incr_top(L);
}


int luaD_protectedparser (lua_State *L, ZIO *z, const char *name) {
  struct SParser p;
  int status;
  p.z = z; p.name = name;
  luaZ_initbuffer(L, &p.buff);
  status = luaD_pcall(L, f_parser, &p, savestack(L, L->top), L->errfunc);
  luaZ_freebuffer(L, &p.buff);
  return status;
}


