/*
** $Id: lstate.c,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#define lstate_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


#if !defined(LUAI_GCPAUSE)
#define LUAI_GCPAUSE	200  /* 200% */
#endif

#if !defined(LUAI_GCMUL)
#define LUAI_GCMUL	200 /* GC runs 'twice the speed' of memory allocation */
#endif


/*
** a macro to help the creation of a unique random seed when a state is
** created; the seed is used to randomize hashes.
*/
#if !defined(luai_makeseed)
#include <time.h>
#define luai_makeseed()		cast(unsigned int, time(NULL))
#endif



/*
** thread state + extra space
*/
/* 
** Lua中真正的线程对应的其实应该算是LX，而不是lua_State，但是对外而言Lua中的线程对应的
** 就是lua_State。为什么不直接将extra_数组放在lua_State里面呢？这样可以防止向外部暴露
** 太多的信息。LUA_EXTRASPACE宏的大小就是指针的大小，对应所在机器的地址寄存器的大小（单位是字节）。
** extra_数组可以用来存放一些附加信息。
*/
typedef struct LX {
  lu_byte extra_[LUA_EXTRASPACE];
  lua_State l;
} LX;


/*
** Main thread combines a thread state and the global state
*/
/* 主线程信息，包含了一个主线程自身的lua_State状态信息，还有一个全部thread共享的状态信息 */
/* 
** 创建主线程是申请的对象时LG类型的，创建其他线程时申请的对象时LX，LG比LX就多了个global_State，
** 而global_State是由所有thread共享的，只是是由主线程创建的而已。
*/
typedef struct LG {
  LX l;
  global_State g;
} LG;



#define fromstate(L)	(cast(LX *, cast(lu_byte *, (L)) - offsetof(LX, l)))


/*
** Compute an initial seed as random as possible. Rely on Address Space
** Layout Randomization (if present) to increase randomness..
*/
#define addbuff(b,p,e) \
  { size_t t = cast(size_t, e); \
    memcpy(b + p, &t, sizeof(t)); p += sizeof(t); }

/* 生成hash操作所需要的随机种子 */
static unsigned int makeseed (lua_State *L) {
  char buff[4 * sizeof(size_t)];
  unsigned int h = luai_makeseed();
  int p = 0;
  addbuff(buff, p, L);  /* heap variable */
  addbuff(buff, p, &h);  /* local variable */
  addbuff(buff, p, luaO_nilobject);  /* global variable */
  addbuff(buff, p, &lua_newstate);  /* public function */
  lua_assert(p == sizeof(buff));
  return luaS_hash(buff, p, h);
}


/*
** set GCdebt to a new value keeping the value (totalbytes + GCdebt)
** invariant (and avoiding underflows in 'totalbytes')
*/
void luaE_setdebt (global_State *g, l_mem debt) {
  l_mem tb = gettotalbytes(g);
  lua_assert(tb > 0);
  if (debt < tb - MAX_LMEM)
    debt = tb - MAX_LMEM;  /* will make 'totalbytes == MAX_LMEM' */
  g->totalbytes = tb - debt;
  g->GCdebt = debt;
}


/* 
** 扩展lua_State对象中的ci双向链表，即双向链表的尾部插入一个新的对象，
** 结合宏next_ci()一起看。因此，该双向链表中，处于链表中越深的成员，
** 表示的函数调用层越深。
*/
CallInfo *luaE_extendCI (lua_State *L) {
  /* 创建一个CallInfo对象 */
  CallInfo *ci = luaM_new(L, CallInfo);
  lua_assert(L->ci->next == NULL);
  L->ci->next = ci;
  ci->previous = L->ci;
  ci->next = NULL;
  
  /* ci链表的元素个数递增 */
  L->nci++;
  return ci;
}


/*
** free all CallInfo structures not in use by a thread
*/
/* 释放函数调用链中的所有CallInfo对象 */
void luaE_freeCI (lua_State *L) {
  CallInfo *ci = L->ci;
  CallInfo *next = ci->next;
  ci->next = NULL;
  while ((ci = next) != NULL) {
    next = ci->next;
    luaM_free(L, ci);
    L->nci--;
  }
}


/*
** free half of the CallInfo structures not in use by a thread
*/
/* 压缩函数调用链，从ci链表中移除一半未使用的CallInfo节点，隔一个删一个。 */
void luaE_shrinkCI (lua_State *L) {
  CallInfo *ci = L->ci;
  CallInfo *next2;  /* next's next */
  /* while there are two nexts */
  while (ci->next != NULL && (next2 = ci->next->next) != NULL) {
    luaM_free(L, ci->next);  /* free next */
    L->nci--;
    ci->next = next2;  /* remove 'next' from the list */
    next2->previous = ci;
    ci = next2;  /* keep next's next */
  }
}

/* 初始化lua_State中的虚拟栈，一个lua_State代表的是一个thread的状态信息 */
static void stack_init (lua_State *L1, lua_State *L) {
  int i; CallInfo *ci;

  /* initialize stack array */
  /*
  ** 为虚拟栈申请内存，虚拟栈中的一个栈单元存放的是TValue，因为TValue中可以
  ** 存放Lua中所有可能的值，因此栈中可以存放任何值。虚拟栈其实就是一个元素类型
  ** 为TValue的数组，除了和栈一样支持“先进后出”的特性之外，还可以像数组一样通过
  ** 索引来访问和修改。
  */
  L1->stack = luaM_newvector(L, BASIC_STACK_SIZE, TValue);
  L1->stacksize = BASIC_STACK_SIZE;
  
  /* 对申请了内存的虚拟栈进行初始化，即每个单元都写入nil值。 */
  for (i = 0; i < BASIC_STACK_SIZE; i++)
    setnilvalue(L1->stack + i);  /* erase new stack */

  /* 
  ** 由于一开始虚拟栈中没有写入任何的值，因此栈指针就等于栈起始地址，表示栈中
  ** 下一个即将放入值得地方就是虚拟栈的起始地址。
  */
  L1->top = L1->stack;

  /* 计算整个虚拟栈的内存上限，要除去额外的5个栈单元 */
  L1->stack_last = L1->stack + L1->stacksize - EXTRA_STACK;
  
  /* initialize first ci */
  /* 初始化该线程中第一个函数调用对应的CallInfo信息 */
  ci = &L1->base_ci;
  ci->next = ci->previous = NULL;
  ci->callstatus = 0;
  
  /*
  ** 第一个函数调用信息中的函数指针指向虚拟栈的内存起始单元。
  */
  ci->func = L1->top;
  
  /*
  ** 将lua_State中的base_ci函数调用信息对应的Closure对象设置成nil对象，同时增长栈指针。
  ** 为什么这么做呢？因为base_ci并不对应一个实际的函数调用。该线程后续执行函数调用的
  ** 时候，会在luaD_precall()中申请一个新的CallInfo对象，来保存新函数调用的信息。
  */
  setnilvalue(L1->top++);  /* 'function' entry for this 'ci' */

  /* 默认栈至少LUA_MINSTACK个空闲的槽 */
  ci->top = L1->top + LUA_MINSTACK;

  /*
  ** 写入函数调用链的头部，即CallInfo链表的第一个元素是lua_State中的base_ci。从main函数中可以看到，
  ** 下面即将进行的函数调用就是pmain()。
  */
  L1->ci = ci;
}

/* 释放虚拟栈。 */
static void freestack (lua_State *L) {
  if (L->stack == NULL)
    return;  /* stack not completely built yet */
  L->ci = &L->base_ci;  /* free the entire 'ci' list */
  luaE_freeCI(L);
  lua_assert(L->nci == 0);
  luaM_freearray(L, L->stack, L->stacksize);  /* free stack array */
}


/*
** Create registry table and its predefined values
*/
/* 创建全局注册表，以及其一些预定义的内容，如_G表，主线程对应的状态信息 */
static void init_registry (lua_State *L, global_State *g) {
  TValue temp;
  /* create registry */
  /* 创建一个表，会被当做全局注册表，存放在global_State对象的l_registry成员中 */
  Table *registry = luaH_new(L);
  sethvalue(L, &g->l_registry, registry);

  /*
  ** 对全局注册表的数组部分进行扩容，将其数组部分的大小设置为2，其中第一个元素（索引
  ** 为LUA_RIDX_MAINTHREAD）用于存放主线程状态信息，即lua_State对象L；第二个元素（索引
  ** 为LUA_RIDX_GLOBALS）用于存放全局表_G，_G表也是在倒数第二条语句中调用luaH_new()创建的。
  */
  luaH_resize(L, registry, LUA_RIDX_LAST, 0);
  /* registry[LUA_RIDX_MAINTHREAD] = L */
  setthvalue(L, &temp, L);  /* temp = L */
  luaH_setint(L, registry, LUA_RIDX_MAINTHREAD, &temp);
  /* registry[LUA_RIDX_GLOBALS] = table of globals */
  sethvalue(L, &temp, luaH_new(L));  /* temp = new table (global table) */
  luaH_setint(L, registry, LUA_RIDX_GLOBALS, &temp);
}


/*
** open parts of the state that may cause memory-allocation errors.
** ('g->version' != NULL flags that the state was completely build)
*/
/* 初始化lua_State中可能会引起内存申请错误的部分 */
static void f_luaopen (lua_State *L, void *ud) {
  /* 获取由所有thread共享的状态信息 */
  global_State *g = G(L);
  UNUSED(ud);
  /* 初始化lua_State中的虚拟栈 */
  stack_init(L, L);  /* init stack */

  /* 初始化全局注册表 */
  init_registry(L, g);

  /* 初始化string table及string cache */
  luaS_init(L);

  /* 初始化元方法，主要是初始化event对应的名字，同时设置不让GC来回收这些字符串。*/
  luaT_init(L);

  /* 
  ** 初始化词法分析器，这里主要是将lua中的关键字保存到由所有thread共享的状态信息
  ** global_State的fixedgc链表成员中。
  */
  luaX_init(L);
  g->gcrunning = 1;  /* allow gc */
  g->version = lua_version(NULL);
  luai_userstateopen(L);
}


/*
** preinitialize a thread with consistent values without allocating
** any memory (to avoid errors)
*/
/* 初始化lua_State状态信息中那些不涉及内存申请和分配的部分。 */
static void preinit_thread (lua_State *L, global_State *g) {
  G(L) = g;
  L->stack = NULL;
  L->ci = NULL;
  L->nci = 0;
  L->stacksize = 0;
  L->twups = L;  /* thread has no upvalues */
  L->errorJmp = NULL;
  L->nCcalls = 0;
  L->hook = NULL;
  L->hookmask = 0;
  L->basehookcount = 0;
  L->allowhook = 1;
  resethookcount(L);
  L->openupval = NULL;
  L->nny = 1;
  L->status = LUA_OK;
  L->errfunc = 0;
}


static void close_state (lua_State *L) {
  global_State *g = G(L);
  luaF_close(L, L->stack);  /* close all upvalues for this thread */
  luaC_freeallobjects(L);  /* collect all objects */
  if (g->version)  /* closing a fully built state? */
    luai_userstateclose(L);
  luaM_freearray(L, G(L)->strt.hash, G(L)->strt.size);
  freestack(L);
  lua_assert(gettotalbytes(g) == sizeof(LG));
  (*g->frealloc)(g->ud, fromstate(L), sizeof(LG), 0);  /* free main block */
}


/* 创建一个新的thread，并将其压入栈顶部 */
LUA_API lua_State *lua_newthread (lua_State *L) {
  /* 获取由所有thread共享的全局状态信息 */
  global_State *g = G(L);
  lua_State *L1;
  lua_lock(L);
  luaC_checkGC(L);
  
  /* create new thread */
  /* 创建一个新的thread对象，这里对应的是LX类型的对象，Lua中真正的线程对应的
  ** 其实应该算是LX，而不是lua_State，但是对外而言Lua中的线程对应的就是lua_State。
  */
  L1 = &cast(LX *, luaM_newobject(L, LUA_TTHREAD, sizeof(LX)))->l;
  L1->marked = luaC_white(g);
  
  /* 
  ** 设置lua_State对象中的类型tt为thread，因为对外而言，lua_State代表的就是一个线程的
  ** 执行状态信息。
  */
  L1->tt = LUA_TTHREAD;
  
  /* link it on list 'allgc' */
  /*
  ** 由于lua_State类型的对象也是要GC的，因此要将这个对象挂载到global_State对象的allgc成员中，
  ** 这样Lua的GC模块就会在适当的时候对lua_State对象进行回收。下面的操作时将L1添加到allgc链表
  ** 的头部。
  */
  L1->next = g->allgc;
  g->allgc = obj2gco(L1);
  
  /* anchor it on L stack */
  /* 将线程lua_State对象压入栈顶部，并更新栈指针。 */
  setthvalue(L, L->top, L1);
  api_incr_top(L);

  /* 初始化lua_State状态信息中那些不涉及内存申请和分配的部分。 */
  preinit_thread(L1, g);
  L1->hookmask = L->hookmask;
  L1->basehookcount = L->basehookcount;
  L1->hook = L->hook;
  resethookcount(L1);
  
  /* initialize L1 extra space */
  /* 用主线程中的extra_数组内容来初始化本次新创建线程的extra_数组。 */
  memcpy(lua_getextraspace(L1), lua_getextraspace(g->mainthread),
         LUA_EXTRASPACE);
  luai_userstatethread(L, L1);
  
  /* 初始化lua_State中的虚拟栈。 */
  stack_init(L1, L);  /* init stack */
  lua_unlock(L);
  return L1;
}


void luaE_freethread (lua_State *L, lua_State *L1) {
  LX *l = fromstate(L1);
  luaF_close(L1, L1->stack);  /* close all upvalues for this thread */
  lua_assert(L1->openupval == NULL);
  luai_userstatefree(L, L1);
  freestack(L1);
  luaM_free(L, l);
}

/* 初始化lua_State对象，参数中的f是内存申请函数 */
LUA_API lua_State *lua_newstate (lua_Alloc f, void *ud) {
  int i;
  lua_State *L;
  global_State *g;
  
  /* 
  ** 创建LG对象，LG对象是主线程信息，包含了一个主线程自身的lua_State状态信息，
  ** 还有一个由全部thread共享的状态信息。
  */
  LG *l = cast(LG *, (*f)(ud, NULL, LUA_TTHREAD, sizeof(LG)));
  if (l == NULL) return NULL;
  
  /* 获取主线程对应的lua_State状态信息 */
  L = &l->l.l;
  
  /* 获取全部线程共享的global_State状态信息 */
  g = &l->g;
  L->next = NULL;
  
  /* 设置类型 */
  L->tt = LUA_TTHREAD;
  g->currentwhite = bitmask(WHITE0BIT);
  L->marked = luaC_white(g);

  /* 
  ** 初始化lua_State状态信息中那些不涉及内存申请和分配的部分。涉及到内存操作的
  ** 部分由下面的f_luaopen()来完成。
  */
  preinit_thread(L, g);

  /* 初始化全部线程共享的global_State状态信息。 */
  g->frealloc = f;
  g->ud = ud;

  /* 保存主线程状态信息。 */
  g->mainthread = L;

  /* 生成hash操作所需要的随机种子 */
  g->seed = makeseed(L);
  g->gcrunning = 0;  /* no GC while building state */
  g->GCestimate = 0;
  g->strt.size = g->strt.nuse = 0;
  g->strt.hash = NULL;
  setnilvalue(&g->l_registry);
  g->panic = NULL;
  g->version = NULL;
  g->gcstate = GCSpause;
  g->gckind = KGC_NORMAL;
  g->allgc = g->finobj = g->tobefnz = g->fixedgc = NULL;
  g->sweepgc = NULL;
  g->gray = g->grayagain = NULL;
  g->weak = g->ephemeron = g->allweak = NULL;
  g->twups = NULL;
  g->totalbytes = sizeof(LG);
  g->GCdebt = 0;
  g->gcfinnum = 0;
  g->gcpause = LUAI_GCPAUSE;
  g->gcstepmul = LUAI_GCMUL;
  for (i=0; i < LUA_NUMTAGS; i++) g->mt[i] = NULL;
  /* 
  ** 以保护模式来调用f_luaopen()函数，该函数主要功能是初始化lua_State中可能会
  ** 引起内存申请错误的部分。因此采用保护模式来运行。
  */
  if (luaD_rawrunprotected(L, f_luaopen, NULL) != LUA_OK) {
    /* memory allocation error: free partial state */
    close_state(L);
    L = NULL;
  }
  return L;
}


LUA_API void lua_close (lua_State *L) {
  L = G(L)->mainthread;  /* only the main thread can be closed */
  lua_lock(L);
  close_state(L);
}


