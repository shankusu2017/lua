/*
** $Id: lstate.h,v 2.24.1.2 2008/01/03 15:20:39 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"



struct lua_longjmp;  /* defined in ldo.c */


/* table of globals */
#define gt(L)	(&L->l_gt)

/* registry */
#define registry(L)	(&G(L)->l_registry)


/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_CI_SIZE           8

#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* 闭散列，哈希桶算法          ( https://blog.csdn.net/Boring_Wednesday/article/details/80316884) */
typedef struct stringtable {
  GCObject **hash;
  lu_int32 nuse;  	/* number of elements 表中元素总数 */
  int size;			/* 哈希桶的高度 */
} stringtable;


/*
** informations about a call
** 对照lstate.c的stack_init函数看
*/
typedef struct CallInfo {
  StkId top;	/* top for this function,无论Lua/C:本次调用可用栈的栈顶阈值，不可超过它,Lua字节码在编码时已计算出maxstacksize故而能保证，这里更多用于C调用(刚开始给出LUA_MINSTACK)的保护 */
  
  /* base for this function：
  **
  ** 
  ** ForC:本次函数调用中,存放第一个参数的slot,top-base=已压入的调用参数个数,被调用的函数由fun指针指向
  
  ** ForLua:第一个固定参数的位置
  ** 如果lua函数的参数个数固定，普通含义(同C)
  ** 如果lua函数的参数不固定，func-->(top1(base))之间的传入的参数(本函数被调用时实际传入的所有参数)在调用本函数之前其中的部分(用于填充被调函数的固定形参)被复制到了base-->top2
  **     中，原空间回填nil,这样base->top2的空间就是新的固定参数的位置，fun->base可以计算本变参函数被调用时实际接收了多少参数,base又满足统一的含义(指向了第一个固定参数的位置)
  **     只是func->base之间有了空隙而已(因为空出来的slot都被拿去填补了lua函数形参中的固定参数去了)，故而上面说的被复制的个数等于函数形参的个数
  **     eg:定义：funcA(a,b,c, ...)
  **        实际调用：funcA(1,2,3,4,5)
            那么实参1，2，3会从funA->base(top1)移动到base上(此后top1->top2),4,5被保留下来作为funA的...参数
  */
  StkId base;  
  StkId func;  /* function index in the stack,本次函数调用的fun在frame中的位置 */
  
  const Instruction *savedpc;	/* next code,本次母调用被打断时存档,待子调用恢复时拿出来用eg:funA调用funB,开始执行funB时存下funA的next code,待funB结束后，继续funA的next code执行 */

  /* expected number of results from this function 
  ** -1: 期待全部参数, 
  **  0: 期待0个参数，
  **  1：期待1个参数 
  */
  int nresults;  	
  int tailcalls;  	/* number of tail calls lost under this entry */
} CallInfo;



#define curr_func(L)	(clvalue(L->ci->func))
#define ci_func(ci)	(clvalue((ci)->func))
#define f_isLua(ci)	(!ci_func(ci)->c.isC)
#define isLua(ci)	(ttisfunction((ci)->func) && f_isLua(ci))


/*
** `global state', shared by all threads of this state(被本state所属的threads共享,言外之意，还可以有第N+1个state,其是一个完全独立的lua'state(包含独享的global_state和其独享的threads))
*/
typedef struct global_State {
  lua_Alloc frealloc;  	/* function to reallocate memory */
  void *ud;         	/* auxiliary data to `frealloc' */

  stringtable strt;  	/* hash table for strings */
  
  lu_byte currentwhite;	/* atomic() 原子扫描完毕时，切换此值 */
  lu_byte gcstate;  	/* state of garbage collector */
  
  GCObject *rootgc;  	/* list of all collectable objects */
  GCObject *gray;  		/* list of gray objects */
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  GCObject *weak;  		/* list of weak tables (to be cleared)，propagate阶段处理的weak-table被放入此链表(gc过程中weak-attribute还可能发生变化的)，等待最后atomic处理， */
  GCObject *tmudata;  	/* last element of list of userdata to be GC */
  int 		sweepstrgc; /* position of sweep in `strt' */
  GCObject **sweepgc;  	/* position of sweep in `rootgc' */
  
  lu_mem GCthreshold;
  lu_mem totalbytes;  	/* number of bytes currently allocated */
  lu_mem estimate;  	/* an estimate(估计) of number of bytes actually in use */
  lu_mem gcdept; 		/* how much GC is `behind schedule' */
  int gcpause;  		/* size of pause between successive GCs */
  int gcstepmul;  		/* GC `granularity(粒度)' */
  
  lua_CFunction panic;  /* to be called in unprotected errors */
  
  TValue l_registry;
  
  struct lua_State *mainthread;
  
  UpVal uvhead;  /* head of double-linked list of all open upvalues */
  
  struct Table 	*mt[NUM_TAGS];  /* metatables for basic types */
  TString 		*tmname[TM_N];  /* array with tag-method names */
  
  Mbuffer buff;  /* temporary buffer for string concatentation(级联) */
} global_State;


/*
** `per thread' state
*/
struct lua_State {
  CommonHeader;
  lu_byte status;

  global_State *l_G;

   /* 
   ** 对于C.frame:first free slot in the stack, 当前指向的addr是可用的！！！
   ** 对于Lua.frame 一般情况下L->top=L->ci->top(lua编译阶段即可知道Lua函数栈需要的最大值)
   **     在执行有关动态参数的指令时，用于指示最后一个参数的位置,用于计算参数的具体数量
   */
  StkId top; 
  StkId base;  					/* base of current function, 当前调用frame中，第一个形参的addr，具体解释看CallInfo */
  								/* 此处没定义func，func定义在ci(CallInfo)中 */
  CallInfo *ci;  				/* call info for current function */
  const Instruction *savedpc;  	/* `savedpc' of current function */

  /* 
  ** stacksize =(stack_last - stack) + (1 + EXTRA_STACK) 
  ** stack_last到真实的stack->mem.top之间还有一层缓冲区
  */
  StkId stack_last;  		/* last free slot in the stack */
  StkId stack;  			/* stack base */
  int 	stacksize;

  CallInfo 	*base_ci;  		/* array of CallInfo's */
  CallInfo 	*end_ci;  		/* points after end of ci array*/
  int 		size_ci;  		/* size of array `base_ci' */
  unsigned short nCcalls;  	/* number of nested C calls */
  /* nested C calls when resuming coroutine
  **（在进入resume之前嵌套的C调用，以便判断从Cyiled返回时resume中是否夹杂了新的C调用） ???
  */
  unsigned short baseCcalls;  
  
  lu_byte hookmask;
  lu_byte allowhook;
  int basehookcount;	/* 参考debug.sethook,虚拟机执行N个pc后调用指定的钩子函数 */
  int hookcount;		/* 当前还需要执行N个pc才能触发上面提到的钩子函数 */
  lua_Hook hook;		/*  调试用的hook函数句柄, 参考 debug.sethook           */
  
  TValue l_gt;  		/* table of globals: 每次生成一个closure时，env从此继承而不是从上层函数继承环境变量 */
  
  TValue env;  			/* temporary place for environments */
  GCObject *openupval;  /* list of open upvalues in this stack */
  
  GCObject *gclist;
  
  struct lua_longjmp *errorJmp;  /* current error recover point */
  ptrdiff_t errfunc;  			 /* current error handling function (stack index) */
};


#define G(L)	(L->l_G)


/*
** Union of all collectable objects
*/
union GCObject {
  GCheader gch;
  union TString ts;
  union Udata u;
  union Closure cl;
  struct Table h;	
  struct Proto p;
  struct UpVal uv;
  struct lua_State th;  /* thread */
};


/* macros to convert a GCObject into a specific value */
#define rawgco2ts(o)	check_exp((o)->gch.tt == LUA_TSTRING, &((o)->ts))
#define gco2ts(o)	(&rawgco2ts(o)->tsv)
#define rawgco2u(o)	check_exp((o)->gch.tt == LUA_TUSERDATA, &((o)->u))
#define gco2u(o)	(&rawgco2u(o)->uv)
#define gco2cl(o)	check_exp((o)->gch.tt == LUA_TFUNCTION, &((o)->cl))
#define gco2h(o)	check_exp((o)->gch.tt == LUA_TTABLE, &((o)->h))
#define gco2p(o)	check_exp((o)->gch.tt == LUA_TPROTO, &((o)->p))
#define gco2uv(o)	check_exp((o)->gch.tt == LUA_TUPVAL, &((o)->uv))
#define ngcotouv(o) \
	check_exp((o) == NULL || (o)->gch.tt == LUA_TUPVAL, &((o)->uv))
#define gco2th(o)	check_exp((o)->gch.tt == LUA_TTHREAD, &((o)->th))

/* macro to convert any Lua object into a GCObject */
#define obj2gco(v)	(cast(GCObject *, (v)))


LUAI_FUNC lua_State *luaE_newthread (lua_State *L);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);

#endif

