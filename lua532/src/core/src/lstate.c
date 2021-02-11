/*
** $Id: lstate.c,v 2.133 2015/11/13 12:16:51 roberto Exp $
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
**
** 当你需要定义自己的seed时，要修改这里？
*/
#if !defined(luai_makeseed)
#include <time.h>
#define luai_makeseed()		cast(unsigned int, time(NULL))
#endif



/*
** thread state + extra space
*/
typedef struct LX {
  lu_byte extra_[LUA_EXTRASPACE];
  lua_State l;
} LX;


/*
** Main thread combines a thread state and the global state
** 
** 这里看到一个global_State至少有一个lua_State对应
** 后续在同一个global_State中还可以调用lua_newthread新增一个LUA_PTHREAD
** 类似LINUX下一个进程至少包含一个线程，后续还可以pthread_createXX新开另外的线程
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
** 拓展CallInfo双向列表，构造一个新的CallInfo出来
**
** 链表上的cell随着调用栈的变化可以被反复使用，因为调用的层次一般在N层以内
** 那么从N调用到N+1时，看下链表上还有没有下一个free-CallInfo可以，有的话，直接拿来用就可以了
** 从N返回到N-1时，移动下L—>ci指针指到前一个即可(释放了链表上的当前的cell)
** 这样避免频繁的构建调用信息结构CallInfo
*/
CallInfo *luaE_extendCI (lua_State *L)
{
  CallInfo *ci = luaM_new(L, CallInfo);
  /* 决断，有空的就不用调用本函数了-最好放到最前面 */
  lua_assert(L->ci->next == NULL);  

  /* add to list */
  L->ci->next  = ci;
  ci->previous = L->ci;
  ci->next     = NULL;

  L->nci++;
  return ci;
}


/*
** free all CallInfo structures not in use by a thread
*/
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


static void stack_init (lua_State *L1, lua_State *L) {
  int i; 
	CallInfo *ci;
	
  /* initialize stack array */
  L1->stack = luaM_newvector(L, BASIC_STACK_SIZE, TValue);
	//记录堆栈大小
  L1->stacksize = BASIC_STACK_SIZE;
	//RESET堆栈SPACE
  for (i = 0; i < BASIC_STACK_SIZE; i++)
    setnilvalue(L1->stack + i);  /* erase new stack */
	//初始化栈顶指针
  L1->top = L1->stack;

	//设置栈顶指针的阈值，不得达到此阈值，可以看到这里预留了一些SPACE
  L1->stack_last = L1->stack + L1->stacksize - EXTRA_STACK;
	
  /* initialize first ci */
  ci = &L1->base_ci;
  ci->next = ci->previous = NULL;
  ci->callstatus = 0;
  ci->func = L1->top;

  //这里L1->top++自增用的绝妙啊
  setnilvalue(L1->top++);  /* 'function' entry for this 'ci' */

  ci->top = L1->top + LUA_MINSTACK;
  L1->ci = ci;
}


static void freestack (lua_State *L) {
  if (L->stack == NULL)
    return;  /* stack not completely built yet */
  L->ci = &L->base_ci;  /* free the entire 'ci' list */
  luaE_freeCI(L);
  lua_assert(L->nci == 0);
  luaM_freearray(L, L->stack, L->stacksize);  /* free stack array */
}


/*
** Create registry table and its predefined(预定义) values
*/
static void init_registry (lua_State *L, global_State *g) {
  TValue temp;

  /* create registry */
  Table *registry = luaH_new(L);
  sethvalue(L, &g->l_registry, registry);
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
static void f_luaopen (lua_State *L, void *ud) {
  global_State *g = G(L);
  UNUSED(ud);	//避免编译警告

	//初始化堆栈
  stack_init(L, L);  /* init stack */
	
  //初始化全局的"寄存器"
  init_registry(L, g);

  luaS_init(L);
  luaT_init(L);
  luaX_init(L);
  g->gcrunning = 1;  /* allow gc */
  g->version = lua_version(NULL);
  luai_userstateopen(L);
}


/*
** preinitialize a thread with consistent values without allocating
** any memory (to avoid errors)
** 
** 仔细看上面的注释，to avoide errors的考虑是严谨性的体现
*/
static void preinit_thread (lua_State *L, global_State *g) {
	//这一句话超级重要
  G(L) = g;
	
  L->stack = NULL;
  L->ci  	 = NULL;
  L->nci 	 = 0;
  L->stacksize = 0;
	
  L->twups 		= L;  /* thread has no upvalues */
  L->errorJmp = NULL;
  L->nCcalls  = 0;
  L->hook 		= NULL;
  L->hookmask = 0;
  L->basehookcount = 0;
  L->allowhook = 1;
  resethookcount(L);
  L->openupval = NULL;
  L->nny 			= 1;
  L->status 	= LUA_OK;
  L->errfunc 	= 0;
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

/* 在global_State下新开一个LUA_TTHREAD */
LUA_API lua_State *lua_newthread (lua_State *L) {
  global_State *g = G(L);
  lua_State *L1;
  lua_lock(L);
  luaC_checkGC(L);
  
  /* create new thread */
  L1 = &cast(LX *, luaM_newobject(L, LUA_TTHREAD, sizeof(LX)))->l;
  L1->marked = luaC_white(g);
  L1->tt = LUA_TTHREAD;
  
  /* link it on list 'allgc' */
  L1->next = g->allgc;
  g->allgc = obj2gco(L1);
  
  /* anchor it on L stack */
  setthvalue(L, L->top, L1);
  api_incr_top(L);

  preinit_thread(L1, g);

  L1->hookmask = L->hookmask;
  L1->basehookcount = L->basehookcount;
  L1->hook = L->hook;
  resethookcount(L1);
  
  /* initialize L1 extra space */
  memcpy(lua_getextraspace(L1), lua_getextraspace(g->mainthread),
         LUA_EXTRASPACE);
  
  luai_userstatethread(L, L1);	/* 调用预留给用户的接口 */
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

/* 创建一台虚拟机 */
LUA_API lua_State *lua_newstate (lua_Alloc f, void *ud) 
{
  int i;
  lua_State *L;
  global_State *g;
  LG *l = cast(LG *, (*f)(ud, NULL, LUA_TTHREAD, sizeof(LG)));
  if (l == NULL)
		return NULL;
	
  L = &l->l.l;	// main thread
  g = &l->g;    // global_state

	//设置gc相关域
  L->next = NULL;
  L->tt 	= LUA_TTHREAD;

	/* 这里两步有先后顺序 */
  g->currentwhite = bitmask(WHITE0BIT);
  L->marked 		  = luaC_white(g);
	
	//设置其它的域，重要的是有个类似的L->G=g
  preinit_thread(L, g);
	
  g->frealloc 	= f;
  g->ud 				= ud;
  g->mainthread = L;	/* 这一句KEY */
  g->seed 			= makeseed(L);	/* 设置seed */
  g->gcrunning 	= 0;  /* no GC while building state */
  g->GCestimate = 0;

	//全局的字符串hash结构
  g->strt.size = g->strt.nuse = 0;
  g->strt.hash = NULL;

	/* 全局的注册表 */
  setnilvalue(&g->l_registry);
	
  g->panic 		= NULL;
  g->version 	= NULL;

	/* GC相关域 */
  g->gcstate 	= GCSpause;
  g->gckind 	= KGC_NORMAL;
  g->allgc 		= g->finobj = g->tobefnz = g->fixedgc = NULL;
  g->sweepgc 	= NULL;
  g->gray 		= g->grayagain = NULL;
  g->weak 		= g->ephemeron = g->allweak = NULL;
	
  g->twups 			= NULL;
  g->totalbytes = sizeof(LG);
  g->GCdebt 		= 0;
  g->gcfinnum 	= 0;
  g->gcpause 		= LUAI_GCPAUSE;	/* 设置默认GCPAUSE */
  g->gcstepmul 	= LUAI_GCMUL;

	/* 初始化元表 */
  for (i=0; i < LUA_NUMTAGS; i++)
		g->mt[i] = NULL;

	/* f_luaopen可能会失败，这里放到最后一步 */
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


