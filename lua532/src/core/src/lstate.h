/*
** $Id: lstate.h,v 2.128 2015/11/13 12:16:51 roberto Exp $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"

#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*

** Some notes about garbage-collected objects: All objects in Lua must
** be kept somehow accessible until being freed, so all objects always
** belong to one (and only one) of these lists, using field 'next' of
** the 'CommonHeader' for the link:
**
** 'allgc': all objects not marked for finalization;
** 'finobj': all objects marked for finalization;
** 'tobefnz': all objects ready to be finalized; 
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).

*/


struct lua_longjmp;  /* defined in ldo.c */



/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */

/*
** 开散列的哈希表
**  array[0]
**       [1]
**		 [2]->next->next->next 
**		 ...
**	     [N]
*/
typedef struct stringtable {
  /* NOTE:这里存二级指针而不是一级指针 */
  TString **hash;  
  int 	    size;  /* 数组的总大小，数组中的元素可自形成链表 */
  int 		nuse;  /* number of elements，当前所有元素的个数 */
} stringtable;


/*
** Information about a call.
** When a thread yields(出让), 'func' is adjusted to pretend(假装) that the
** top function has only the yielded values in its stack; in that
** case, the actual 'func' value is saved in field 'extra'. 
** When a function calls another with a continuation, 'extra' keeps
** the function index so that, in case of errors, the continuation
** function can be called with the correct top.
*/
typedef struct CallInfo {
  /* 以下两个指针是相当重要的指针，一定要弄明白 */
  StkId func;  /* function index in the stack，当前函数指针在堆栈中的位置 */
  
  StkId	top;   /* top for this function */

  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  /* only for Lua functions */
      StkId base;  /* base for this function */
      const Instruction *savedpc; /* 指令 */
    } l;
    struct {  /* only for C functions */
      lua_KFunction k;  /* continuation in case of yields */
      ptrdiff_t old_errfunc;
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;
  ptrdiff_t extra;

  /* expected number of results from this function
  **  local ret1, ret2, ret3 = fun(arg,) nresults:为3，表示调用者期待返回3个值
  */
  short nresults;      

  lu_byte callstatus; /* 这个byte好像有个bit保存了是Lua还是C调用 */
} CallInfo;


/*
** Bits in CallInfo status
** CIST<- CallInfoState
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
#define CIST_LUA	(1<<1)	/* call is running a Lua function */
#define CIST_HOOKED	(1<<2)	/* call is running a debug hook */
#define CIST_FRESH	(1<<3)	/* call is running on a fresh invocation(调用)
                                   of luaV_execute */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
#define CIST_TAIL	(1<<5)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_LEQ	(1<<7)  /* using __lt for __le */

#define isLua(ci)	((ci)->callstatus & CIST_LUA)

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
*/
typedef struct global_State {
  lua_Alloc frealloc;  /* function to reallocate memory */
  void 		*ud;         /* auxiliary data to 'frealloc' */
  
  l_mem totalbytes;  	/* number of bytes currently allocated - GCdebt */
  l_mem GCdebt;  			/* bytes allocated not yet compensated by the collector：收集器尚未补偿的已分配字节 */
  lu_mem GCmemtrav;  	/* memory traversed(贯穿) by the GC */
  lu_mem GCestimate;  /* an estimate(估计) of the non-garbage memory in use */
  
  stringtable strt;  	/* 开散列的哈希表, hash table for strings，only for short string */

  /* 全局的注册表，
  ** array部分
      ** [1] = mainThread, 
      ** [2] = globleTable = {
                              key1 = val1,
                              key2 = val2,
                              ....
                             }
     ** node部分：用于全局的原表
     ** [key1] = name1  --
  
  */
  TValue l_registry;	
  
  unsigned int seed;  /* randomized seed for hashes */
  
  lu_byte currentwhite; /* atomic扫描的最后一步更新此值 */
  
  lu_byte gcstate;  /* state of garbage collector */
  lu_byte gckind;       /* kind of GC running，0:normal,1:emergency(内存不足诱发的？) */
  lu_byte gcrunning;    /* true if GC is (allow?)running */
  
  GCObject *allgc;      /* list of all collectable objects */
  GCObject **sweepgc;   /* current position of sweep(打扫，清除) in list */
  GCObject *finobj;     /* list of collectable objects with finalizers */
  
  GCObject *gray;       /* list of gray objects */
  
  /* 
  ** black对象被barrierback后回到了grayagain
  ** grayagin后面被atomically扫描。避免gray->black->gray->black->gra...消耗性能
  */
  GCObject *grayagain;  /* list of objects to be traversed(穿透) atomically */
  
  GCObject *weak;  /* list of tables with weak values */
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  GCObject *allweak;  /* list of all-weak tables */
  
  GCObject *tobefnz;  /* list of userdata to be GC(to be finalized?) */
  GCObject *fixedgc;  /* list of objects not to be collected(不需要被gc的对象(eg:语言关键字对应的字符串...)) */
  
  struct lua_State *twups;  /* list of threads with open upvalues */
  unsigned int gcfinnum;  /* number of finalizers to call in each GC step */
  int gcpause;          /* size of pause between successive GCs，当前内存(新分配的？)是以前的N%时触发一次全新的GC过程 */
  int gcstepmul;        /* GC 'granularity(颗粒度)',每一小步标记的MEM是新分配MEM的N% */
  lua_CFunction panic;  /* to be called in unprotected errors */
  struct lua_State *mainthread;
  
  const lua_Number *version;  /* pointer to version number，这里是指针 */
  
  TString *memerrmsg;  /* memory-error message */
  TString *tmname[TM_N];  /* array with tag-method names */

  struct Table *mt[LUA_NUMTAGS];  /* metatables for basic types：每种类型的默认原表? */
  
  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
} global_State;


/*
** 'per thread' state
** 蛮多域还看不明白，尤其是虚拟机执行相关的，待后续再回过头来看
*/
struct lua_State {
  CommonHeader;

  /* 调用层次不能太深 */
  unsigned short nci;  /* number of items in 'ci' list */
  
  lu_byte status;		/* 线程的状态 */
  global_State *l_G;	/* 指向root'global_State的指针 */

  /* call info for current function 
  ** 函数调用可以嵌套，这里标记当前被调用的函数
  */
  CallInfo *ci; 
  
  const Instruction *oldpc;  	/* last pc traced */

	/*
	** 由于栈space预留了一些extra所以这里stack_last + extra == stack + stacksize
	** extra.size == EXTRA_STACK
	** NOTE:
	**    top和statck_last之间可能要求有个LUA_MINSTACK(调用C函数之前)，也就是说Lua仅保证默认的堆栈容量是这么大，若C
	**        想扩容则需要C自己主动调用相关接口
    ** extra区域：可能是避免爆栈时破坏其它数据结构，这里做一个缓存？(https://manistein.github.io/blog/post/program/build-a-lua-interpreter/%E6%9E%84%E5%BB%BAlua%E8%A7%A3%E9%87%8A%E5%99%A8part1/) 
	** >--------statck-------top---------statck_last----[<-extra->]-----statcksize----
	*/
  StkId stack;  /* stack base */
  int stacksize;
  
  StkId top;  /* first free slot in the stack,当前栈指针 */
  StkId stack_last;  /* last free slot in the stack,看初始化赋值得知，此值不可达到,就像数组最后一个元素的下一个元素地址一样 */


  
  UpVal *openupval;  /* list of open upvalues in this stack */
  GCObject *gclist;
  struct lua_State *twups;  /* list of threads with open upvalues */
  
  struct lua_longjmp *errorJmp;  /* current error recover point */
  
  CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */
  lua_Hook hook;
  
  ptrdiff_t errfunc;  /* current error handling function (stack index) */
  
  int basehookcount;
  int hookcount;
  unsigned short nny;  /* number of non-yieldable calls in stack */
  unsigned short nCcalls;  /* number of nested(嵌套)， C calls？(现在来看不是)，不能超过某个阈值LUAI_MAXCCALLS？否则认为有错 */
  lu_byte hookmask;
  lu_byte allowhook;        /* 是否允许运行debug用的hook? */
};


/* 提取lua_State中root'global_State的指针的宏定义 */
#define G(L)	(L->l_G)


/*
** Union of all collectable objects (only for conversions)
**
** only for conversions：这句话是重点，也是这结构体的作用
** 这里要求联合的所有域均以GCObject类型开头
*/
union GCUnion {
  GCObject gc;  /* common header */
  struct TString ts;
  struct Udata u;
  union Closure cl;
  struct Table h;
  struct Proto p;
  struct lua_State th;  /* thread */
};

/* 将 GCObject类型的指针o转换为GCUnio，后再转为对应的如TString、Table类型 */
#define cast_u(o)	cast(union GCUnion *, (o))

/* macros to convert a GCObject into a specific value */
/* 由gc指针转换为TString指针 */
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
	
#define gco2u(o)  check_exp((o)->tt == LUA_TUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LUA_TLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LUA_TCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
#define gco2t(o)  check_exp((o)->tt == LUA_TTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LUA_TPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LUA_TTHREAD, &((cast_u(o))->th))


/* macro to convert a Lua object into a GCObject */
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

/* 对外部模块提供的接口 */
LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);
/* shrink:收缩   */
LUAI_FUNC void luaE_shrinkCI (lua_State *L);


#endif

