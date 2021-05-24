/*
** $Id: ldo.c,v 2.157.1.1 2017/04/19 17:20:42 roberto Exp $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/

#define ldo_c
#define LUA_CORE

#include "lprefix.h"


#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
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



#define errorstatus(s)	((s) > LUA_YIELD)


/*
** {======================================================
** Error-recovery functions
** =======================================================
*/

/*
** LUAI_THROW/LUAI_TRY define how Lua does exception handling. By
** default, Lua handles errors with exceptions when compiling as
** C++ code, with _longjmp/_setjmp when asked to use them, and with
** longjmp/setjmp otherwise.
*/
#if !defined(LUAI_THROW)				/* { */

#if defined(__cplusplus) && !defined(LUA_USE_LONGJMP)	/* { */

/* C++ exceptions */
#define LUAI_THROW(L,c)		throw(c)
#define LUAI_TRY(L,c,a) \
	try { a } catch(...) { if ((c)->status == 0) (c)->status = -1; }
#define luai_jmpbuf		int  /* dummy variable */

#elif defined(LUA_USE_POSIX)				/* }{ */

/* in POSIX, try _longjmp/_setjmp (more efficient) */
#define LUAI_THROW(L,c)		_longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (_setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#else							/* }{ */

/* ISO C handling with long jumps */
#define LUAI_THROW(L,c)		longjmp((c)->b, 1)
#define LUAI_TRY(L,c,a)		if (setjmp((c)->b) == 0) { a }
#define luai_jmpbuf		jmp_buf

#endif							/* } */

#endif							/* } */



/* chain list of long jump buffers */
/* 包含了返回点信息的链表节点 */
struct lua_longjmp {
  /*
  ** 用于串联该线程中的多个返回点，因为一个线程中可能会运行多个受保护的代码段，这样就需要
  ** 设置多个返回点。注意，往该链表中插入节点是插入到头部的，而且越早加入该链表的节点表示
  ** 越早之前的返回点，因此链表头部节点表示最近一次设置的返回点信息。previous指针指向的
  ** 是链表的下一个节点，表示的是更早之前的返回点。
  */
  struct lua_longjmp *previous;
  /* 异常处理机制相关的东西，如对于C来说，一般是jmp_buf类型 */
  luai_jmpbuf b;
  
  /* 受保护代码运行出错的错误码 */
  volatile int status;  /* error code */
};

/*
** 将errcode对象的错误对象设置到oldtop指向的栈单元中，可以从最后一条语句看到，oldtop会被当做是
** 新的栈顶部单元。
*/
static void seterrorobj (lua_State *L, int errcode, StkId oldtop) {
  switch (errcode) {
    case LUA_ERRMEM: {  /* memory error? */
      setsvalue2s(L, oldtop, G(L)->memerrmsg); /* reuse preregistered msg. */
      break;
    }
    case LUA_ERRERR: {
      setsvalue2s(L, oldtop, luaS_newliteral(L, "error in error handling"));
      break;
    }
    default: {
      setobjs2s(L, oldtop, L->top - 1);  /* error message on current top */
      break;
    }
  }
  /* 将栈指针设置到oldtop指向的栈单元的下一个单元，那么oldtop指向的栈单元就是栈顶部单元了。 */
  L->top = oldtop + 1;
}

/* 
** 在保护模式中执行代码发生了异常，则会通过这个函数来异常，如果使用的C语言机制的话，
** 那就是调用longjmp()，跳转到执行的这段发生了异常的代码之前。结合函数luaD_rawrunprotected()
** 能较好地理解这一流程。
*/
l_noret luaD_throw (lua_State *L, int errcode) {
  /*
  ** 如果L->errorJmp链表成员不为空，那么说明当前线程注册了错误处理函数，其实就是设置了返回点信息。
  ** 且L->errorJmp链表中越靠近头部的节点表示越近的返回点，越靠近尾部的节点对应的是更早之前设置的返回点。
  ** 在函数中会跳转到最近的返回点（最后加入到该链表的返回点）中，并设置本次执行受保护代码出错的错误码
  ** 到包含了返回点信息的节点中。注意，如果线程自己设置了错误处理函数，则此时并不会将错误码errcode对应
  ** 的错误对象压入出错函数所在栈单元中，这点和下面的主线程分支处理不同，而会在调用完
  ** luaD_rawrunprotected()之后再设置，这点可以参考luaD_pcall()。
  */
  if (L->errorJmp) {  /* thread has an error handler? */
    L->errorJmp->status = errcode;  /* set status */
    LUAI_THROW(L, L->errorJmp);  /* jump to it */
  }
  else {  /* thread has no error handler */
    /*
    ** 程序执行到这里，说明当前线程没有设置错误处理函数，即没有设置可用的返回点信息，这个时候由于
    ** 当前线程出现了异常，那么就将错误码设置到该线程对应的lua_State的status成员中。然后检查主线程
    ** 中是否设置了错误处理函数，如果设置了，那么将出错的对象压入主线程的栈顶部，并跳转到主线程中
    ** 最后一次设置的返回点。如果主线程中也没有设置异常处理函数，则先判断全局状态信息中是否设置了
    ** panic()函数，如果设置了的话，还有机会在panic()处理错误并跳出异常；否则进程会调用abort()退出。
    */
    global_State *g = G(L);
    L->status = cast_byte(errcode);  /* mark it as dead */
    if (g->mainthread->errorJmp) {  /* main thread has a handler? */
      setobjs2s(L, g->mainthread->top++, L->top - 1);  /* copy error obj. */
      luaD_throw(g->mainthread, errcode);  /* re-throw in main thread */
    }
    else {  /* no handler at all; abort */
      /* 如果设置了panic()函数，则做如下处理；否则调用abort()退出进程。 */
      if (g->panic) {  /* panic function? */
	  	
        /* 保存错误信息到栈顶部 */
        seterrorobj(L, errcode, L->top);  /* assume EXTRA_STACK */
        if (L->ci->top < L->top)
          L->ci->top = L->top;  /* pushing msg. can break this invariant */
        lua_unlock(L);
		
        /* 调用panic()函数，在该函数中还有一次机会跳出异常。 */
        g->panic(L);  /* call panic function (last chance to jump out) */
      }
	  
	  /* 进程异常退出 */
      abort();
    }
  }
}

/*
** 以受保护的方式运行函数f。
** 在调用f之前先调用setjump()函数初始化jmp buff对象，即返回点的相关信息。这样才能在
** 执行f出问题后，让线程返回到调用f之前这个状态来。设置好返回点相关的信息之后，就调用
** 函数f了，这个过程在LUAI_TRY()这个宏中完成的。
*/
int luaD_rawrunprotected (lua_State *L, Pfunc f, void *ud) {
  unsigned short oldnCcalls = L->nCcalls;
  
  /*
  ** 每个线程L中都会保存该线程需要的返回点信息，这个是通过lua_State中的链表成员errorJmp
  ** 实现的。由于每运行一段受保护的代码，都要设置一个返回点，以让线程在运行受保护代码发生
  ** 错误时能返回到执行受保护代码之前。一个线程可能会运行多段受保护的代码，因此也就有多个
  ** 返回点信息，多个返回点信息就是通过lua_State中的errorJmp这个链表成员串起来的。
  ** 在下面的语句中将要以受保护的方式来运行函数f，那么就需要在执行函数f之前设置好返回点
  ** 相关的信息，并将包含了返回点信息的类型为struct lua_longjmp的节点加入到lua_State中的
  ** errorJmp这个链表成员中。执行完f之后，就将该节点信息从lua_State中的errorJmp这个链表成员
  ** 中移除，因为f执行完之后，就不在需要返回点信息了。
  ** 以受保护的方式运行的函数f中肯定包含了longjmp()函数调用，longjmp()函数调用被封装成了
  ** 宏LUAI_THROW()。只有在f中调用了longjmp()才能回到调用了setjump()函数的这里。
  */

  struct lua_longjmp lj;
  lj.status = LUA_OK;
  lj.previous = L->errorJmp;  /* chain new error handler */
  
  /*
  ** 将当前的返回点信息设置到L->errorJmp链表成员的头部，这样如果调用f出错了，在luaD_throw()中
  ** 就会使用L->errorJmp链表头部节点包含的返回点信息执行跳转。这样就回到到了调用f之前的位置。
  */
  L->errorJmp = &lj;
  
  LUAI_TRY(L, &lj,
    (*f)(L, ud);
  );

  /* f已经执行完了，就不再需要预防f出错的返回点信息了，将其从链表中移除。 */
  L->errorJmp = lj.previous;  /* restore old error handler */
  L->nCcalls = oldnCcalls;

  /* 对于运行出错了的情况，结合luaD_thrown()我们知道，错误信息已经压倒栈顶部了。 */

  /*
  ** lj.status在函数开始处被初始化成了LUA_OK，如果执行f的时候没出错，那其值就是LUA_OK，
  ** 如果执行f出错了，由于f中会调用luaD_throw()，所以lj.status会在luaD_throw()中被设置为
  ** 相应的错误码。我们看到luaD_throw()函数中设置的是lua_State对象中的errorJmp成员的status，
  ** 其实就是下面这个lj的status，因为在该函数的上面已经将lj设置到了lua_State对象中的errorJmp
  ** 成员中。
  */
  return lj.status;
}

/* }====================================================== */


/*
** {==================================================================
** Stack reallocation
** ===================================================================
*/
/* 
** 根据旧栈和新栈的地址差来校正线程状态信息lua_State中某些信息的地址。
** oldstack指向的是旧栈的起始地址。
*/
static void correctstack (lua_State *L, TValue *oldstack) {
  CallInfo *ci;
  UpVal *up;
  
  /* 新栈的栈指针 */
  L->top = (L->top - oldstack) + L->stack;
  for (up = L->openupval; up != NULL; up = up->u.open.next)
    up->v = (up->v - oldstack) + L->stack;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    ci->top = (ci->top - oldstack) + L->stack;
    ci->func = (ci->func - oldstack) + L->stack;
    if (isLua(ci))
      ci->u.l.base = (ci->u.l.base - oldstack) + L->stack;
  }
}


/* some space for error handling */
#define ERRORSTACKSIZE	(LUAI_MAXSTACK + 200)


void luaD_reallocstack (lua_State *L, int newsize) {
  /* 保存旧栈地址 */
  TValue *oldstack = L->stack;
  int lim = L->stacksize;
  lua_assert(newsize <= LUAI_MAXSTACK || newsize == ERRORSTACKSIZE);
  lua_assert(L->stack_last - L->stack == L->stacksize - EXTRA_STACK);
  
  /* 申请一个新栈，存放到L->stack中。 */
  luaM_reallocvector(L, L->stack, L->stacksize, newsize, TValue);

  /* 将和旧栈相比多出来的那部分栈单元初始化 */
  for (; lim < newsize; lim++)
    setnilvalue(L->stack + lim); /* erase new segment */

  /* 记录新栈的大小信息和内存上限 */
  L->stacksize = newsize;
  L->stack_last = L->stack + newsize - EXTRA_STACK;

  /* 根据旧栈和新栈的地址差来校正线程状态信息lua_State中某些信息的地址 */
  correctstack(L, oldstack);
}


/* 扩充栈 */
void luaD_growstack (lua_State *L, int n) {
  int size = L->stacksize;
  if (size > LUAI_MAXSTACK)  /* error after extra size? */
    luaD_throw(L, LUA_ERRERR);
  else {
  	/* 计算扩充后栈的大小 */
    int needed = cast_int(L->top - L->stack) + n + EXTRA_STACK;
    int newsize = 2 * size;
    if (newsize > LUAI_MAXSTACK) newsize = LUAI_MAXSTACK;
    if (newsize < needed) newsize = needed;

	/*
	** 如果扩充后的大小超过了，则进行错误处理，也有可能会抛出异常。
	** 如果没有的话，那么就调用luaD_reallocstack()扩充原有栈或者是
	** 申请一个新的栈，然后将旧栈中的数据拷贝到新栈中，并释放旧栈。
	*/
    if (newsize > LUAI_MAXSTACK) {  /* stack overflow? */
      luaD_reallocstack(L, ERRORSTACKSIZE);
      luaG_runerror(L, "stack overflow");
    }
    else
      luaD_reallocstack(L, newsize);
  }
}

/* 返回栈单元使用的个数 */
static int stackinuse (lua_State *L) {
  CallInfo *ci;
  StkId lim = L->top;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {
    if (lim < ci->top) lim = ci->top;
  }
  lua_assert(lim <= L->stack_last);
  return cast_int(lim - L->stack) + 1;  /* part of stack in use */
}


/* 压缩栈 */
void luaD_shrinkstack (lua_State *L) {
  int inuse = stackinuse(L);
  int goodsize = inuse + (inuse / 8) + 2*EXTRA_STACK;
  if (goodsize > LUAI_MAXSTACK)
    goodsize = LUAI_MAXSTACK;  /* respect stack limit */
  if (L->stacksize > LUAI_MAXSTACK)  /* had been handling stack overflow? */
    luaE_freeCI(L);  /* free all CIs (list grew because of an error) */
  else
    luaE_shrinkCI(L);  /* shrink list */
  /* if thread is currently not handling a stack overflow and its
     good size is smaller than current size, shrink its stack */
  if (inuse <= (LUAI_MAXSTACK - EXTRA_STACK) &&
      goodsize < L->stacksize)
    luaD_reallocstack(L, goodsize);
  else  /* don't change stack */
    condmovestack(L,{},{});  /* (change only for debugging) */
}

/* 将栈指针递增1 */
void luaD_inctop (lua_State *L) {
  luaD_checkstack(L, 1);
  L->top++;
}

/* }================================================================== */


/*
** Call a hook for the given event. Make sure there is a hook to be
** called. (Both 'L->hook' and 'L->hookmask', which triggers this
** function, can be changed asynchronously by signals.)
*/
/*
** Lua的hook机制，可以在程序触发了某些事件的时候，调用我们注册的一个钩子函数。可以触发
** hook机制的事件目前有以下几个：
** call事件： 调用了一个函数的时候
** return事件：函数返回
** line事件：开始执行新的一行代码
** count事件：执行的指令数达到指定的数量
** 引起钩子函数调用的事件及其掩码在lua.h中定义。
** 为给定的事件调用钩子函数。事件由参数event指定。
*/
void luaD_hook (lua_State *L, int event, int line) {
  /* 获取注册的钩子函数 */
  lua_Hook hook = L->hook;
  
  /* 如果线程中注册了钩子函数，也允许调用钩子函数，那么就准备着手调用钩子函数。 */
  if (hook && L->allowhook) {  /* make sure there is a hook */
    /* 获取当前的函数调用信息 */
    CallInfo *ci = L->ci;

    /* 记录当前栈指针和函数栈上限相对于栈底的偏移量 */
    ptrdiff_t top = savestack(L, L->top);
    ptrdiff_t ci_top = savestack(L, ci->top);

	/* 记录一些和debug相关的信息，如引发本次钩子调用的事件，文件所在行等。 */
    lua_Debug ar;
    ar.event = event;
    ar.currentline = line;
    ar.i_ci = ci;
	
    /* 确保栈的剩余单元个数要大于LUA_MINSTACK */
    luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */

    /* 将当前函数调用的栈的上限更改为L->top + LUA_MINSTACK。确保当前函数调用有足够的栈空间 */
    ci->top = L->top + LUA_MINSTACK;
    lua_assert(ci->top <= L->stack_last);

    /* 
    ** 将lua_State中的allowhook清零，因为钩子函数不允许嵌套，所以在钩子函数里面不能再
    ** 调用钩子函数。
    */
    L->allowhook = 0;  /* cannot call hooks inside a hook */

    /* 将函数调用的状态打上hook标记，表明当前函数调用正在执行一个钩子函数。 */
    ci->callstatus |= CIST_HOOKED;
    lua_unlock(L);

    /* 正式调用钩子函数，因为钩子函数都是C函数，因此直接调用即可。 */
    (*hook)(L, &ar);
    lua_lock(L);
    lua_assert(!L->allowhook);

    /* 钩子函数执行完了，可以将允许标志置位. */
    L->allowhook = 1;

    /* 恢复函数调用之前的栈上限，以及栈指针。并在函数调用状态中去掉hook标记。 */
    ci->top = restorestack(L, ci_top);
    L->top = restorestack(L, top);
    ci->callstatus &= ~CIST_HOOKED;
  }
}


/* 触发函数调用事件（每次进行一个函数时）对应的钩子函数 */
static void callhook (lua_State *L, CallInfo *ci) {
  /* 函数调用事件 */
  int hook = LUA_HOOKCALL;
  ci->u.l.savedpc++;  /* hooks assume 'pc' is already incremented */

  /*
  ** 如果上一层的函数是lua函数，并且是尾调用，那么将引发钩子函数调用的事件改为函数尾调用，
  ** 并将当前的函数调用状态信息中打上尾调用标记，表示当前的函数调用是一个尾调用。
  */
  if (isLua(ci->previous) &&
      GET_OPCODE(*(ci->previous->u.l.savedpc - 1)) == OP_TAILCALL) {
    ci->callstatus |= CIST_TAIL;
    hook = LUA_HOOKTAILCALL;
  }

  /* 根据上面指定的事件调用钩子函数 */
  luaD_hook(L, hook, -1);
  ci->u.l.savedpc--;  /* correct 'pc' */
}


/* 如果函数是可变参数的话，那么需要对参数做一个调整。actual是实际传递了的参数个数。 */
static StkId adjust_varargs (lua_State *L, Proto *p, int actual) {
  int i;
  /* 固定参数的个数 */
  int nfixargs = p->numparams;
  StkId base, fixed;
  /* move fixed parameters to final position */
  fixed = L->top - actual;  /* first fixed argument */
  base = L->top;  /* final position of first argument */
  for (i = 0; i < nfixargs && i < actual; i++) {
    setobjs2s(L, L->top++, fixed + i);
    setnilvalue(fixed + i);  /* erase original copy (for GC) */
  }
  for (; i < nfixargs; i++)
    setnilvalue(L->top++);  /* complete missing arguments */
  return base;
}


/*
** Check whether __call metafield of 'func' is a function. If so, put
** it in stack below original 'func' so that 'luaD_precall' can call
** it. Raise an error if __call metafield is not a function.
*/
/*
** 有些对象是通过元表来驱动函数调用行为的。这个时候需要通过tryfuncTM()来找到真正的
** 调用函数。在lua中，根据元方法进行的函数调用和普通的函数调用会有一定的区别，通过
** 元方法进行的函数调用，需要将拥有该元方法的对象自身作为元方法的第一个参数，这个时候
** 就需要移动栈的内容，将对象插到第一个参数位置处。真正的调用函数就是元方法"__call"的值。
** tryfuncTM()参数中的func就是那个通过元表来驱动函数调用的对象。
*/
static void tryfuncTM (lua_State *L, StkId func) {
  /*
  ** 从func指向的TValue对象的元表中尝试获取TM_CALL对应的值对象。
  ** 意思就是说从func指向的TValue对象的元表中获取键值为"__call"的值对象。
  */
  const TValue *tm = luaT_gettmbyobj(L, func, TM_CALL);
  StkId p;
  /* 判断"__call"对应的值对象是不是一个函数对象，如果不是，那么就报错。 */
  if (!ttisfunction(tm))
    luaG_typeerror(L, func, "call");
  /* Open a hole inside the stack at 'func' */
  
  /*
  ** 将函数调用栈中的参数和func指向的TValue对象都往后挪一个单元，这样TValue对象就
  ** 变成了函数的第一个参数了，下面会在func指向的栈单元中放入真正的调用函数（从元表中找到的）。
  */
  for (p = L->top; p > func; p--)
    setobjs2s(L, p, p-1);
  L->top++;  /* slot ensured by caller */
  
  /* 将func指向的TValue对象改为tm指向的对象，即从元表中获取到的函数对象 */
  setobj2s(L, func, tm);  /* tag method is the new function to be called */
}


/*
** Given 'nres' results at 'firstResult', move 'wanted' of them to 'res'.
** Handle most typical cases (zero results for commands, one result for
** expressions, multiple results for tail calls/single parameters)
** separated.
*/
/*
** res指向为当前函数指针的位置，在moveresults的时候，会用来存放函数的
** 第一个返回值，其余返回值依次往后，这样做是为了节省栈空间，因为函数调用已经
** 执行完了，那么那些参数和函数指针本身就没用了，只要保存函数调用的返回值就可以
** 了，因此将函数返回值移动到函数指针所在位置开始依次往后存放。在该函数最后，
** 会更新栈指针L->top，使其指向挪动后的最后一个返回值的下一个栈单元。
*/
static int moveresults (lua_State *L, const TValue *firstResult, StkId res,
                                      int nres, int wanted) {
  switch (wanted) {  /* handle typical cases separately */
    case 0: break;  /* nothing to move */
    case 1: {  /* one result needed */
      /*
      ** 如果函数预期返回一个值，而实际返回0个值，那么就将nil对象作为其最后返回值。
      */
      if (nres == 0)   /* no results? */
        firstResult = luaO_nilobject;  /* adjust with nil */

	  /* 将函数执行结果设置到res指向的栈单元。 */
      setobjs2s(L, res, firstResult);  /* move it to proper place */
      break;
    }
    case LUA_MULTRET: {
      int i;
      /* 如果有多个返回值，则从res指向的位置开始，依次往后面存放，同时更新栈指针L->top */
      for (i = 0; i < nres; i++)  /* move all results to correct place */
        setobjs2s(L, res + i, firstResult + i);
      L->top = res + nres;
      return 0;  /* wanted == LUA_MULTRET */
    }
    default: {
      int i;
      /*
      ** 如果实际返回值个数多于期望的返回值个数，那么只将期望的返回值挪到指定位置，
      ** 实际返回值中多余的那部分就直接丢弃了；如果实际的返回值个数少于期望的返回值
      ** 个数，那么将实际返回值先挪到指定位置，缺少的那部分返回值都用nil对象来填充。
      ** 最后，更新栈指针L->top，使其指向最后一个返回值的下一个栈单元。
      */
      if (wanted <= nres) {  /* enough results? */
        for (i = 0; i < wanted; i++)  /* move wanted results to correct place */
          setobjs2s(L, res + i, firstResult + i);
      }
      else {  /* not enough results; use all of them plus nils */
        for (i = 0; i < nres; i++)  /* move all results to correct place */
          setobjs2s(L, res + i, firstResult + i);
        for (; i < wanted; i++)  /* complete wanted number of results */
          setnilvalue(res + i);
      }
      break;
    }
  }

  /* 更新栈指针L->top，使其指向最后一个返回值的下一个栈单元 */
  L->top = res + wanted;  /* top points after the last result */
  return 1;
}


/*
** Finishes a function call: calls hook if necessary, removes CallInfo,
** moves current number of results to proper place; returns 0 iff call
** wanted multiple (variable number of) results.
*/
/* 对当前函数调用做一些收尾工作，比如将函数返回值挪到适当位置，并退回到上一层函数调用中去。 */
int luaD_poscall (lua_State *L, CallInfo *ci, StkId firstResult, int nres) {
  StkId res;
  /* 取出在函数调用之前保存在CallInfo对象中预期的函数返回值个数。 */
  int wanted = ci->nresults;
  if (L->hookmask & (LUA_MASKRET | LUA_MASKLINE)) {
  	
    /* 如果有注册函数调用返回时对应的事件，那么要执行相应的钩子函数。 */
    if (L->hookmask & LUA_MASKRET) {
      ptrdiff_t fr = savestack(L, firstResult);  /* hook may change stack */
      luaD_hook(L, LUA_HOOKRET, -1);
      firstResult = restorestack(L, fr);
    }
    L->oldpc = ci->previous->u.l.savedpc;  /* 'oldpc' for caller function */
  }

  /*
  ** 将res指向为当前函数指针的位置，在下面的moveresults的时候，会用来存放函数的
  ** 第一个返回值，其余返回值依次往后，这样做是为了节省栈空间，因为函数调用已经
  ** 执行完了，那么那些参数和函数指针本身就没用了，只要保存函数调用的返回值就可以
  ** 了，因此将函数返回值移动到函数指针所在位置开始依次往后存放。
  */
  res = ci->func;  /* res == final position of 1st result */
  /* 
  ** 由于当前函数调用已经执行完了，那么就需要返回上一层的函数调用栈了，因此将
  ** L->ci设置为上一层的函数调用。
  */
  L->ci = ci->previous;  /* back to caller */
  /* move results to proper place */
  /*
  ** 将res指向为当前函数指针的位置，在下面的moveresults的时候，会用来存放函数的
  ** 第一个返回值，其余返回值依次往后，这样做是为了节省栈空间，因为函数调用已经
  ** 执行完了，那么那些参数和函数指针本身就没用了，只要保存函数调用的返回值就可以
  ** 了，因此将函数返回值移动到函数指针所在位置开始依次往后存放。在该函数最后，
  ** 会更新栈指针L->top，使其指向挪动后的最后一个返回值的下一个栈单元。
  */
  return moveresults(L, firstResult, res, nres, wanted);
}


/* next_ci()宏用于进入一个新的函数调用栈，函数调用层次变深。 */
#define next_ci(L) (L->ci = (L->ci->next ? L->ci->next : luaE_extendCI(L)))


/* macro to check stack size, preserving 'p' */
#define checkstackp(L,n,p)  \
  luaD_checkstackaux(L, n, \
    ptrdiff_t t__ = savestack(L, p);  /* save 'p' */ \
    luaC_checkGC(L),  /* stack grow uses memory */ \
    p = restorestack(L, t__))  /* 'pos' part: restore 'p' */


/*
** Prepares a function call: checks the stack, creates a new CallInfo
** entry, fills in the relevant information, calls hook if needed.
** If function is a C function, does the call, too. (Otherwise, leave
** the execution ('luaV_execute') to the caller, to allow stackless
** calls.) Returns true iff function has been executed (C function).
*/
/* 
** luaD_precall()函数用于为一个函数调用做前期准备，如果是C函数（包括C闭包）的话，
** 就直接在这里执行了。返回值为0，表示待执行函数是一个lua函数，此时需要跳转到
** lua虚拟机中执行；返回值为1，表示待执行函数是一个C函数，且该函数已经执行完了，
** 收尾工作也结束了。
*/
int luaD_precall (lua_State *L, StkId func, int nresults) {
  lua_CFunction f;
  CallInfo *ci;
  switch (ttype(func)) {
    case LUA_TCCL:  /* C closure */
      /* 获取待调用的函数指针 */
      f = clCvalue(func)->f;
      goto Cfunc;
    case LUA_TLCF:  /* light C function */
      /* 获取待调用的函数指针 */
      f = fvalue(func);
     Cfunc: {
      int n;  /* number of returns */
	  
      /* checkstackp()用于确保当前函数调用栈有至少LUA_MINSTACK个单元 */
      checkstackp(L, LUA_MINSTACK, func);  /* ensure minimum stack size */
	  
      /* 从lua_State的ci链表中获取一个CallInfo节点，用于存放当前函数调用栈的信息 */
      ci = next_ci(L);  /* now 'enter' new function */
      ci->nresults = nresults;
      ci->func = func;
      /*
      ** ci->top指向的是当前函数栈的最后一个栈单元在数据栈中的位置，这里将其设置为L->top +  LUA_MINSTACK 
      ** 那么[func, L->top +  LUA_MINSTACK]都分配给了该函数调用过程。[func,L->top)之间是函数指针及其
      ** 形参的范围。注意到L->top并没有更新，指向的仍然是当前调用函数最后一个参数的下一个位置。
      */
      /*
      ** 注意到，此时没有修改L->top指针，即在准备C函数调用的时不会修改栈指针，只会设置C函数调用的
      ** 栈范围的ci->top。但在下面真正执行C函数调用的时候，C函数里面会将结果压入栈顶部中，即此时的
      ** C函数参数之后，栈指针也会被修改。执行完C函数之后，会调用luaD_poscall()将函数结果从函数参数
      ** 之后的位置挪到从函数队形所在位置开始往后依次存放。最后挪动结果这一步和lua函数是一样的，在
      ** 虚拟机luaV_execute()中，lua函数最后一步的return的也会调用luaD_poscall()将函数结果挪到lua函数
      ** 对象所在位置开始往后依次存放。
      */
      ci->top = L->top + LUA_MINSTACK;
      lua_assert(ci->top <= L->stack_last);
      ci->callstatus = 0;
	  
      /*
      ** 因为这里即将进行一个新的函数调用，因此检查用户是否注册了函数调用的事件，如果注册了对应事件，
      ** 则以该事件来调用用户注册的钩子函数。
      */
      if (L->hookmask & LUA_MASKCALL)
        luaD_hook(L, LUA_HOOKCALL, -1);
      lua_unlock(L);
	  
      /*
      ** 执行该C函数调用，C函数的调用结果会直接压入到栈中，从L->top指向的位置开始。函数执行过程
      ** 可以结合math库的某个函数来看，以math_abs()为例，该函数会根据自己的需要从func后面的栈单元
      ** 取出所需个数的参数，执行计算过程，将结果又压入到栈顶中，然后更新栈指针。函数调用的结果在
      ** 栈中是紧跟在函数参数之后的。在函数调用过程中，由于会往栈中压入调用结果，因此会更新L->top，
      ** 因此如果函数调用结果有n个，那么L->top = L->top + n，这点从math_abs()可以看出。那么函数的
      ** 第一个返回值在栈中的地址就是：L->top - n。
      */
      /*
      ** 注意，所有注册到lua中的C函数的返回值表示C函数执行结果的个数，即压入栈中的结果的个数，从
      ** 函数最后一个参数的下一个位置开始依次存放，在下面的luaD_poscall()中又会调用moveresults()
      ** 将结果挪到从函数对象所在栈单元开始依次存放。函数的参数是调用该函数的地方负责压入到函数
      ** 后面的，负责准备好的。
      */
      n = (*f)(L);  /* do the actual call */
      lua_lock(L);
      api_checknelems(L, n);
	  
      /*
      ** 对该函数调用做一些收尾工作，比如将函数返回值挪到适当位置，并退回到上一层函数调用中去。
      ** (L->top -n)是执行完函数调用后第一个函数调用结果的地址。
      */
      luaD_poscall(L, ci, L->top - n, n);

      /* 返回1表示是C函数调用 */
      return 1;
    }
    case LUA_TLCL: {  /* Lua function: prepare its call */
      /*
      ** 程序进入这个分支，说明lua中执行了那个函数调用时一个lua函数，这个时候需要做一些前期准备工作，
      ** lua函数执行是在虚拟机中的，即luaV_execute()。
      */
      
      StkId base;
	  
      /* 获取待执行函数对应的原型信息 */
      Proto *p = clLvalue(func)->p;
	
      /* 获取实际传递了的函数参数的个数 */
      int n = cast_int(L->top - func) - 1;  /* number of real arguments */
	  
      /* 
      ** 获取函数内部栈的大小，并做检查。在函数栈中，函数的形参和内部定义的本地变量对应函数栈
      ** 中的哪个栈单元都是在指令解析过程中就确定好了的。函数的实参是在调用函数之前需要先在
      ** 函数栈中设置好。可以参考handle_script()。
      */
      int fsize = p->maxstacksize;  /* frame size */
      checkstackp(L, fsize, func);

      /* 判断函数是不是可变参函数 */
      if (p->is_vararg)
        base = adjust_varargs(L, p, n);
      else {  /* non vararg function */

        /* 将此次函数调用过程中没有传递实参的参数，赋予nil值 */
        for (; n < p->numparams; n++)
          setnilvalue(L->top++);  /* complete missing arguments */

        /* base指向了第一个参数 */
        base = func + 1;
      }

      /* 从lua_State的ci链表中获取一个CallInfo节点，用于存放当前函数调用的信息 */
      ci = next_ci(L);  /* now 'enter' new function */
      ci->nresults = nresults;
      ci->func = func;
      ci->u.l.base = base;
	  
      /* 注意到此时整个虚拟栈的栈指针和当前函数调用的栈指针是一样的。 */
      L->top = ci->top = base + fsize;

      /*
      ** 在准备工作中，[base, ci->top)这部分栈单元就是函数内的栈。在函数栈中，函数的形参和
      ** 内部定义的本地变量对应函数栈中的哪个栈单元都是在指令解析过程中就确定好了的。函数的
      ** 实参是在调用函数之前需要先在函数栈中设置好。可以参考handle_script()。
      */
	  
      lua_assert(ci->top <= L->stack_last);
      ci->u.l.savedpc = p->code;  /* starting point */
	  /* 标记是lua函数 */
      ci->callstatus = CIST_LUA;

      /* 如果注册了函数调用事件对应的钩子函数，那么就触发钩子函数的调用 */
      if (L->hookmask & LUA_MASKCALL)
        callhook(L, ci);

      /* 返回0表示是lua函数调用 */
      return 0;
    }
    default: {  /* not a function */
      /*
      ** 有些对象是通过元表来驱动函数调用行为的。这个时候需要通过tryfuncTM()来找到真正的
      ** 调用函数。在lua中，根据元方法进行的函数调用和普通的函数调用会有一定的区别，通过
      ** 元方法进行的函数调用，需要将拥有该元方法的对象自身作为元方法的第一个参数，这个时候
      ** 就需要移动栈的内容，将对象插到第一个参数位置处。真正的调用函数就是元方法"__get"的值。
	  */
      checkstackp(L, 1, func);  /* ensure space for metamethod */
	  
      tryfuncTM(L, func);  /* try to get '__call' metamethod */
      /*
      ** 执行完tryfuncTM()之后，位于函数调用栈中的函数对象和参数都已经设置好了，因此可以通过
      ** 调用luaD_precall()来触发函数调用操作了。
      */
      return luaD_precall(L, func, nresults);  /* now it must be a function */
    }
  }
}


/*
** Check appropriate error for stack overflow ("regular" overflow or
** overflow while handling stack overflow). If 'nCalls' is larger than
** LUAI_MAXCCALLS (which means it is handling a "regular" overflow) but
** smaller than 9/8 of LUAI_MAXCCALLS, does not report an error (to
** allow overflow handling to work)
*/
/* 处理栈错误 */
static void stackerror (lua_State *L) {
  if (L->nCcalls == LUAI_MAXCCALLS)
    luaG_runerror(L, "C stack overflow");
  else if (L->nCcalls >= (LUAI_MAXCCALLS + (LUAI_MAXCCALLS>>3)))
    luaD_throw(L, LUA_ERRERR);  /* error while handing stack error */
}


/*
** Call a function (C or Lua). The function to be called is at *func.
** The arguments are on the stack, right after the function.
** When returns, all the results are on the stack, starting at the original
** function position.
*/
/*
** lua中调用一个函数时都会执行这个函数。被调用的函数其地址为func，函数的参数也存放在栈中，
** 紧随在函数之后。函数调用返回时，所有的结果也会存放在栈中，从函数在栈中的原始位置出开始存放。
*/
void luaD_call (lua_State *L, StkId func, int nResults) {

  /* 如果函数嵌套调用的层数超过了限制，那么就返回错误 */
  if (++L->nCcalls >= LUAI_MAXCCALLS)
    stackerror(L);
  /* 
  ** luaD_precall()函数用于为一个函数调用做前期准备，如果是C函数（包括C闭包）的话，
  ** 就直接在这里执行了。返回值为0，表示待执行函数是一个lua函数，此时需要跳转到
  ** lua虚拟机中执行；返回值为1，表示待执行函数是一个C函数，且该函数已经执行完了，
  ** 收尾工作也结束了。
  */
  if (!luaD_precall(L, func, nResults))  /* is a Lua function? */
    luaV_execute(L);  /* call it */
  L->nCcalls--;
}


/*
** Similar to 'luaD_call', but does not allow yields during the call
*/
/* lua函数调用过程中不允许暂停时走的就是这个函数 */
void luaD_callnoyield (lua_State *L, StkId func, int nResults) {
  L->nny++;
  luaD_call(L, func, nResults);
  L->nny--;
}


/*
** Completes the execution of an interrupted C function, calling its
** continuation function.
*/
/* 用于调用被中断函数的延续函数来完成被中断函数未完成的操作。 */
static void finishCcall (lua_State *L, int status) {

  /* 获取当前被中断的函数对应的函数调用信息 */
  CallInfo *ci = L->ci;
  int n;
  /* must have a continuation and must be able to call it */
  lua_assert(ci->u.c.k != NULL && L->nny == 0);
  /* error status can only happen in a protected call */
  lua_assert((ci->callstatus & CIST_YPCALL) || status == LUA_YIELD);
  if (ci->callstatus & CIST_YPCALL) {  /* was inside a pcall? */
    ci->callstatus &= ~CIST_YPCALL;  /* continuation is also inside it */
    L->errfunc = ci->u.c.old_errfunc;  /* with the same error function */
  }
  /* finish 'lua_callk'/'lua_pcall'; CIST_YPCALL and 'errfunc' already
     handled */
  adjustresults(L, ci->nresults);
  lua_unlock(L);

  /* 调用延续函数，以完成被中断函数中未完成的操作 */
  n = (*ci->u.c.k)(L, status, ci->u.c.ctx);  /* call continuation function */
  lua_lock(L);
  api_checknelems(L, n);

  /* 调用luaD_poscall()做一些收尾工作。 */
  luaD_poscall(L, ci, L->top - n, n);  /* finish 'luaD_precall' */
}


/*
** Executes "full continuation" (everything in the stack) of a
** previously interrupted coroutine until the stack is empty (or another
** interruption long-jumps out of the loop). If the coroutine is
** recovering from an error, 'ud' points to the error status, which must
** be passed to the first continuation function (otherwise the default
** status is LUA_YIELD).
*/
/* 完成被中断协程（本次resume的协程）的调用链中未完成的其他函数调用 */
static void unroll (lua_State *L, void *ud) {
  if (ud != NULL)  /* error status? */
    finishCcall(L, *(int *)ud);  /* finish 'lua_pcallk' callee */

  /*
  ** 如果L->ci!=&L->basse_ci说明当前协程的调用链中还有其他未完成的函数调用，那么此处会执行
  ** 这些函数调用的中未完成的操作，C函数是通过调用被中断函数的延续函数来完成，而Lua函数则直接
  ** 到虚拟机中去执行未完成的指令。
  ** 在这个while循环中，L->ci的改变是在finishCall()中通过调用luaD_poscall()函数完成的，或者
  ** 在luaV_execute()执行函数的最后一条指令return时调用luaD_poscall()完成的。
  */
  while (L->ci != &L->base_ci) {  /* something in the stack */
    if (!isLua(L->ci))  /* C function? */
      finishCcall(L, LUA_YIELD);  /* complete its execution */
    else {  /* Lua function */
      /* 
      ** 对于Lua函数，分两步执行未完成的函数操作，首先是执行被中断的那条指令，然后才是执行
      ** 被中断指令的后续指令。
      */
      luaV_finishOp(L);  /* finish interrupted instruction */
      luaV_execute(L);  /* execute down to higher C 'boundary' */
    }
  }
}


/*
** Try to find a suspended protected call (a "recover point") for the
** given thread.
*/
/* 
** 遍历当前线程的函数调用链，从当前线程的函数调用链中找到一个处于挂起状态的在保护模式下
** 执行的函数调用，即函数调用的状态中包含了CIST_YPCALL标志位的第一个函数调用信息。
*/
static CallInfo *findpcall (lua_State *L) {
  CallInfo *ci;
  for (ci = L->ci; ci != NULL; ci = ci->previous) {  /* search for a pcall */
    if (ci->callstatus & CIST_YPCALL)
      return ci;
  }
  return NULL;  /* no pending pcall */
}


/*
** Recovers from an error in a coroutine. Finds a recover point (if
** there is one) and completes the execution of the interrupted
** 'luaD_pcall'. If there is no recover point, returns zero.
*/
/* 从协程的错误中尝试进行恢复的函数，恢复是通过 */
static int recover (lua_State *L, int status) {
  StkId oldtop;

  /* 
  ** 从当前协程的函数调用链中找到一个处于挂起状态的在保护模式下执行的函数调用，
  ** 如果没有找到对应的函数调用，那就说明没有返回点信息，也就不能进行恢复。
  */
  CallInfo *ci = findpcall(L);
  if (ci == NULL) return 0;  /* no recovery point */
  /* "finish" luaD_pcall */
  oldtop = restorestack(L, ci->extra);
  luaF_close(L, oldtop);
  seterrorobj(L, status, oldtop);
  L->ci = ci;
  L->allowhook = getoah(ci->callstatus);  /* restore original 'allowhook' */
  L->nny = 0;  /* should be zero to be yieldable */
  luaD_shrinkstack(L);
  L->errfunc = ci->u.c.old_errfunc;
  return 1;  /* continue running the coroutine */
}


/*
** Signal an error in the call to 'lua_resume', not in the execution
** of the coroutine itself. (Such errors should not be handled by any
** coroutine error handler and should not kill the coroutine.)
*/
static int resume_error (lua_State *L, const char *msg, int narg) {
  L->top -= narg;  /* remove args from the stack */
  setsvalue2s(L, L->top, luaS_new(L, msg));  /* push error message */
  api_incr_top(L);
  lua_unlock(L);
  return LUA_ERRRUN;
}


/*
** Do the work for 'lua_resume' in protected mode. Most of the work
** depends on the status of the coroutine: initial state, suspended
** inside a hook, or regularly suspended (optionally with a continuation
** function), plus erroneous cases: non-suspended coroutine or dead
** coroutine.
*/
/* 协程的resume操作实现 */
static void resume (lua_State *L, void *ud) {
  /* 传递给resume()函数的参数个数。 */
  int n = *(cast(int*, ud));  /* number of arguments */

  /* 获取第一个参数 */
  StkId firstArg = L->top - n;  /* first argument */

  /* 获取协程中当前正在运行的函数调用信息 */
  CallInfo *ci = L->ci;
  
  /*
  ** 如果此时协程的状态为LUA_OK，说明是第一次在该协程上执行resume操作，此时调用luaD_precall()
  ** 做调用函数之前准备，如果是C函数的话，在luaD_precall()就会执行，如果是lua函数，准备工作完了
  ** 之后还要在luaV_execute()虚拟机中具体执行。
  */
  if (L->status == LUA_OK) {  /* starting a coroutine? */
    if (!luaD_precall(L, firstArg - 1, LUA_MULTRET))  /* Lua function? */
      luaV_execute(L);  /* call it */
  }
  else {  /* resuming from previous yield */
    /* 程序进入这个分支，说明该协程并非首次执行resume，而是从之前的中断中继续执行未完成的操作。 */
  
    lua_assert(L->status == LUA_YIELD);
    L->status = LUA_OK;  /* mark that it is running (again) */

    /* 
    ** 因为在lua_yieldk()中进行yield操作的时候，会将被中断的函数对象存放到函数调用信息的
    ** ci_extra成员中保存起来，注意保存的是被中断的函数对象所在栈单元与栈基址的偏移量。
    ** 因此这里从ci->extra成员中恢复出被中断的函数对象。
    */
    ci->func = restorestack(L, ci->extra);

    /* 如果被中断的函数调用是一个lua函数，则在虚拟机中继续执行该函数调用 */
    if (isLua(ci))  /* yielded inside a hook? */
      luaV_execute(L);  /* just continue running Lua code */
    else {  /* 'common' yield */

      /*
      ** 如果被中断的函数调用没有注册延续函数，那么就调用luaD_poscall()做被中断函数的收尾工作。
      ** 相反，如果被中断的函数调用注册了延续函数，那么就调用延续函数来完成被中断函数未完成的
      ** 操作。最后再调用luaD_poscall()来做一些收尾工作。
      */
      if (ci->u.c.k != NULL) {  /* does it have a continuation function? */
        lua_unlock(L);
        n = (*ci->u.c.k)(L, LUA_YIELD, ci->u.c.ctx); /* call continuation */
        lua_lock(L);
        api_checknelems(L, n);
        firstArg = L->top - n;  /* yield results come from continuation */
      }
      luaD_poscall(L, ci, firstArg, n);  /* finish 'luaD_precall' */
    }

	/* 完成当前协程的调用链中未完成的其他函数调用。 */
    unroll(L, NULL);  /* run continuation */
  }
}

/* resume操作的辅助函数，L是协程，from是调用协程的线程 */
LUA_API int lua_resume (lua_State *L, lua_State *from, int nargs) {
  int status;

  /* 
  ** 保存当前线程中记录的不允许中断的函数调用计数，因为在执行resume操作时要将L->nny清零，
  ** 待resume操作结束后，需要恢复L->nny的值，所以这里将保存方便后面进行恢复。
  */
  unsigned short oldnny = L->nny;  /* save "number of non-yieldable" calls */
  lua_lock(L);

  if (L->status == LUA_OK) {  /* may be starting a coroutine */
    if (L->ci != &L->base_ci)  /* not in base level? */
      return resume_error(L, "cannot resume non-suspended coroutine", nargs);
  }
  else if (L->status != LUA_YIELD)
    /* 
    ** 如果协程的状态不是LUA_OK，也不是LUA_YIELD的话，说明这个协程状态不对，此时会报错，
    ** 并将错误信息压入协程的栈顶部。
    */
    return resume_error(L, "cannot resume dead coroutine", nargs);

  /* 程序执行到这里，协程的状态要么是LUA_OK或者LUA_YIELD。 */

  L->nCcalls = (from) ? from->nCcalls + 1 : 1;
  if (L->nCcalls >= LUAI_MAXCCALLS)
    return resume_error(L, "C stack overflow", nargs);
  luai_userstateresume(L, nargs);
  L->nny = 0;  /* allow yields */
  api_checknelems(L, (L->status == LUA_OK) ? nargs + 1 : nargs);

  /*
  ** 以保护模式来调用resume()执行协程的resume操作。nargs是传递给resume()函数的参数个数，这个时候
  ** 这些参数位于协程栈顶部。
  */
  status = luaD_rawrunprotected(L, resume, &nargs);
  if (status == -1)  /* error calling 'lua_resume'? */
    status = LUA_ERRRUN;
  else {  /* continue running after recoverable errors */
    while (errorstatus(status) && recover(L, status)) {
      /* unroll continuation */
      status = luaD_rawrunprotected(L, unroll, &status);
    }
    if (errorstatus(status)) {  /* unrecoverable error? */
      L->status = cast_byte(status);  /* mark thread as 'dead' */
      seterrorobj(L, status, L->top);  /* push error message */
      L->ci->top = L->top;
    }
    else lua_assert(status == L->status);  /* normal end or yield */
  }

  /* 恢复环境 */
  L->nny = oldnny;  /* restore 'nny' */
  L->nCcalls--;
  lua_assert(L->nCcalls == ((from) ? from->nCcalls : 0));
  lua_unlock(L);
  return status;
}

/* 判断当前线程中不可中断的函数调用数量是不是为0，为0，则表示当前线程是可中断的。 */
LUA_API int lua_isyieldable (lua_State *L) {
  return (L->nny == 0);
}

/* 执行yield操作，中断当前正在执行的函数调用 */
LUA_API int lua_yieldk (lua_State *L, int nresults, lua_KContext ctx,
                        lua_KFunction k) {
  CallInfo *ci = L->ci;
  luai_userstateyield(L, nresults);
  lua_lock(L);
  api_checknelems(L, nresults);

  /* 如果L->nny > 0，说明被中断的协程中有不可中断的函数调用，这时会报错。 */
  if (L->nny > 0) {
    if (L != G(L)->mainthread)
      luaG_runerror(L, "attempt to yield across a C-call boundary");
    else
      luaG_runerror(L, "attempt to yield from outside a coroutine");
  }

  /* 将当前的协程状态设置为LUA_YIELD，表示协程被中断 */
  L->status = LUA_YIELD;

  /* 
  ** 将当前正在执行的即将被中断的函数对象保存到函数调用信息的extra成员中，
  ** 注意保存的不是具体的函数对象，而是函数对象相对于协程栈基址的偏移量。
  */
  ci->extra = savestack(L, ci->func);  /* save current 'func' */
  if (isLua(ci)) {  /* inside a hook? */
    api_check(L, k == NULL, "hooks cannot continue after yielding");
  }
  else {
    /*
    ** 程序进入这个分支，说明被中断的函数调用不是一个Lua函数，此时需要保存被中断函数的
    ** 上下文信息，以及用于在resume时完成被中断函数未完成操作的延续函数。
    */
    if ((ci->u.c.k = k) != NULL)  /* is there a continuation? */
      ci->u.c.ctx = ctx;  /* save context */
    ci->func = L->top - nresults - 1;  /* protect stack below results */

    /* 抛出LUA_YIELD的异常。这个时候会返回到相应的返回点，而不会执行else之后的语句。 */
    luaD_throw(L, LUA_YIELD);
  }
  
  lua_assert(ci->callstatus & CIST_HOOKED);  /* must be inside a hook */
  lua_unlock(L);
  return 0;  /* return to 'luaD_hook' */
}

/*
** 如果需要在保护模式下运行栈中的函数调用，那么lua就会调用该函数来处理。old_top表示的是
** 下面即将在函数func中执行的函数调用在栈中的位置（是相对于整个虚拟栈起始地址的下标，不是地址）（结合
** lua_pcallk()来看），ef是本次执行受保护代码出错后的错误处理函数。参数u存放的则是在func中
** 运行的函数调用相关的信息，如该函数调用的返回值个数以及函数指针在栈中的地址。
** 受保护模式的代码调用流程：luaD_pcall以受保护模式调用func，func则会从栈中取出真正要执行的函数调用。
** 参数u存放的就是真正要执行的函数调用的信息。
*/
int luaD_pcall (lua_State *L, Pfunc func, void *u,
                ptrdiff_t old_top, ptrdiff_t ef) {
  int status;
  /*
  ** 我们知道L->ci保存的是当前正在执行的函数调用对应的CallInfo信息，由于下面要
  ** 开启一个新的函数调用，因此这里先将当前的函数调用信息保存下来，以便执行完
  ** 新的函数调用之后进行恢复，继续上次的执行。
  */
  CallInfo *old_ci = L->ci;
  
  /* 同时也保存当前lua线程中一些需要保存的东西 */
  lu_byte old_allowhooks = L->allowhook;
  unsigned short old_nny = L->nny;
  ptrdiff_t old_errfunc = L->errfunc;

  /* 为本次的函数调用设置新的错误处理函数，当前这个值是错误处理函数的栈索引 */
  L->errfunc = ef;
  
  /* 以保护模式来执行该函数调用，如果返回值不等于LUA_OK，那么说明在执行代码过程中出现了错误 */
  status = luaD_rawrunprotected(L, func, u);
  if (status != LUA_OK) {  /* an error occurred? */
    /*
    ** 恢复在保护模式下运行的函数在栈中的地址，注意这个函数不是func，而是func中触发执行的函数调用。
    ** func可以参考f_call()
    */
    StkId oldtop = restorestack(L, old_top);
    luaF_close(L, oldtop);  /* close possible pending closures */
  
    /* 将错误码status对应的错误信息设置到出错函数所在的栈单元中。 */
    seterrorobj(L, status, oldtop);

    /* 恢复上一次函数调用的信息。 */
    L->ci = old_ci;
    L->allowhook = old_allowhooks;
    L->nny = old_nny;
    luaD_shrinkstack(L);
  }

  /* 恢复错误处理函数。 */
  L->errfunc = old_errfunc;
  return status;
}



/*
** Execute a protected parser.
*/
struct SParser {  /* data to 'f_parser' */
  ZIO *z;
  Mbuffer buff;  /* dynamic structure used by the scanner */
  Dyndata dyd;  /* dynamic structures used by the parser */
  const char *mode;  /* 代码文件的类型，是二进制文件还是文本文件 */
  const char *name;
};


static void checkmode (lua_State *L, const char *mode, const char *x) {
  if (mode && strchr(mode, x[0]) == NULL) {
    luaO_pushfstring(L,
       "attempt to load a %s chunk (mode is '%s')", x, mode);
    luaD_throw(L, LUA_ERRSYNTAX);
  }
}

/* f_parser()是对代码分析的入口函数，分析之后生成的对应的LClosure对象再栈顶部。 */
static void f_parser (lua_State *L, void *ud) {
  /*
  ** cl是一个LClosure类型的指针，其指向的LClosure对象在luaY_parser()或者luaU_undump()
  ** 中创建，并压入了栈顶部。因此代码分析完之后生成的对应的LClosure对象就在栈顶部了。
  */
  LClosure *cl;
  struct SParser *p = cast(struct SParser *, ud);
  int c = zgetc(p->z);  /* read first character */

  /* 判断代码文件是二进制文件还是文本文件，并进行区分处理 */
  if (c == LUA_SIGNATURE[0]) {
    checkmode(L, p->mode, "binary");
    cl = luaU_undump(L, p->z, p->name);
  }
  else {
    /* 对于文本文件形式的lua代码文件，调用luaY_parser()进行代码分析 */
    checkmode(L, p->mode, "text");
    cl = luaY_parser(L, p->z, &p->buff, &p->dyd, p->name, c);
  }
  lua_assert(cl->nupvalues == cl->p->sizeupvalues);

  /* 申请用于存放闭包中将使用到的自由变量所需要的内存 */
  luaF_initupvals(L, cl);
}


int luaD_protectedparser (lua_State *L, ZIO *z, const char *name,
                                        const char *mode) {
  struct SParser p;
  int status;
  /* 由于下面的f_parser()在执行过程中不可中断，因此L->nny需要加1。 */
  L->nny++;  /* cannot yield during parsing */
  p.z = z; p.name = name; p.mode = mode;
  p.dyd.actvar.arr = NULL; p.dyd.actvar.size = 0;
  p.dyd.gt.arr = NULL; p.dyd.gt.size = 0;
  p.dyd.label.arr = NULL; p.dyd.label.size = 0;

  /* 初始化缓冲区对象 */
  luaZ_initbuffer(L, &p.buff);
  /* f_parser()是对代码分析的入口函数，分析之后生成的对应的LClosure对象再栈顶部。 */
  status = luaD_pcall(L, f_parser, &p, savestack(L, L->top), L->errfunc);
  luaZ_freebuffer(L, &p.buff);
  luaM_freearray(L, p.dyd.actvar.arr, p.dyd.actvar.size);
  luaM_freearray(L, p.dyd.gt.arr, p.dyd.gt.size);
  luaM_freearray(L, p.dyd.label.arr, p.dyd.label.size);
  L->nny--;
  return status;
}


