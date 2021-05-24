/*
** $Id: lstate.h,v 2.133.1.1 2017/04/19 17:39:34 roberto Exp $
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
**
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray:
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
** The last three lists are used only during the atomic phase.

*/


struct lua_longjmp;  /* defined in ldo.c */


/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/* extra stack space to handle TM calls and some other extras */
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)


/* kinds of Garbage Collection */
#define KGC_NORMAL	0
#define KGC_EMERGENCY	1	/* gc was forced by an allocation failure */


/*
** lua会将系统中所有的字符串存放在一个全局的地方，这个地方就是
** global_State的strt成员
*/
typedef struct stringtable {
  /* 
  ** 存放字符串的hash桶，具有相同hash值的字符串放在同一个桶中，
  ** 同一个桶中的字符串用链表串接起来。当新创建一个字符串的时候，
  ** 首先计算出字符串对应的hash值，然后用这个hash值去索引stringtable中
  ** 的成员hash，得到具体的桶，然后在桶中存放该字符串，插入链表头部。
  ** 采用散列表存放string时，当数据量大的时候需要进行重hash(rehash)，
  ** 通过重新分配桶的数量，来降低每个桶中的字符串数量。
  */
  TString **hash;
  int nuse;  /* number of elements */ /* 字符串的数量 */
  int size;  /* hash桶的个数 */
} stringtable;


/*
** Information about a call.
** When a thread yields, 'func' is adjusted to pretend that the
** top function has only the yielded values in its stack; in that
** case, the actual 'func' value is saved in field 'extra'.
** When a function calls another with a continuation, 'extra' keeps
** the function index so that, in case of errors, the continuation
** function can be called with the correct top.
*/
/*
** CallInfo结构体用于存放一个函数调用过程相关的信息。
** CallInfo是一个标准的双向链表结构，这个双向链表结构表示一个逻辑上的函数调用链。
** 在运行过程中，并不是每次调入更深层次的函数，就立刻构造出一个CallInfo节点。
** 整个CallInfo链表会在运行中被反复复用。直到GC的时候才清理那些比当前调用层次
** 更深的无用节点。
*/
typedef struct CallInfo {
  /* func指向的是该函数调用对应的Closure对象 */
  StkId func;  /* function index in the stack */

  /*
  ** top指向函数的最后一个栈单元在栈中的位置，意思就是说func和top之间的
  ** 这部分内容就是函数栈的全部内容，包括函数指针、参数值、函数内部定义的本地变量对应的栈单元、
  ** 函数调用结果等。
  */
  StkId	top;  /* top for this function */
  
  /* 用于存放函数调用链。函数调用链用一个双向链表来链接起来。 */
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    struct {  /* only for Lua functions */
      /* 
      ** 每个函数都有自己的栈，base存放的是函数栈的基地址，[base, top)就是该函数内部的栈地址范围。
      ** 在函数栈中，函数的形参和内部定义的本地变量对应函数栈中的哪个栈单元都是在指令解析过程中
      ** 就确定好了的。函数的实参是在调用函数之前需要先在函数栈中设置好。可以参考handle_script()。
      */
      StkId base;  /* base for this function */

      /* 
      ** savedpc保存的是该函数调用目前已经执行到的地方，即指向的是当前函数调用中下一条将要执行的
      ** 指令的地址。如果在本函数（或闭包）中再次调用别的函数(或闭包), 那么该值就是本函数的运行断点，
      ** 等被调用函数返回后，就从savedpc指向的地方继续执行。
      ** 存放函数对应指令的内存（savedpc指向的内存）是不在虚拟栈中的，而是通过堆动态申请的。
      */
      const Instruction *savedpc;
    } l;
    struct {  /* only for C functions */
      /* 延续函数，用于完成被yield操作中断的函数中未完成的操作 */
      lua_KFunction k;  /* continuation in case of yields */
      /* 
      ** ptrdiff_t是C/C++标准库中定义的一个与机器相关的数据类型。ptrdiff_t类型变量
      ** 通常用来保存两个指针减法操作的结果。
      ** old_errfunc用来保存被中断函数对应的错误处理函数。
      */
      ptrdiff_t old_errfunc;
	  
      /* 保存被中断函数的上下文信息，作为延续函数的参数 */
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;

  /* 用于在yield操作时保存被中断函数相对于栈基址的偏移量，用以在resume操作时找到被中断的函数对象 */
  ptrdiff_t extra;

  /* 函数调用返回值的数量 */
  short nresults;  /* expected number of results from this function */

  /* 函数调用的状态 */
  unsigned short callstatus;
} CallInfo;


/*
** Bits in CallInfo status
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
#define CIST_LUA	(1<<1)	/* call is running a Lua function */
#define CIST_HOOKED	(1<<2)	/* call is running a debug hook */
#define CIST_FRESH	(1<<3)	/* call is running on a fresh invocation
                                   of luaV_execute */
#define CIST_YPCALL	(1<<4)	/* call is a yieldable protected call */
#define CIST_TAIL	(1<<5)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_LEQ	(1<<7)  /* using __lt for __le */
#define CIST_FIN	(1<<8)  /* call is running a finalizer */

#define isLua(ci)	((ci)->callstatus & CIST_LUA)

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
*/
/* Lua虚拟机中所有thread共享的全局状态信息 */
typedef struct global_State {
  /* frealloc指定lua中用于申请内存的函数 */
  lua_Alloc frealloc;  /* function to reallocate memory */
  void *ud;         /* auxiliary data to 'frealloc' */
  l_mem totalbytes;  /* number of bytes currently allocated - GCdebt */
  l_mem GCdebt;  /* bytes allocated not yet compensated by the collector */
  lu_mem GCmemtrav;  /* memory traversed by the GC */
  lu_mem GCestimate;  /* an estimate of the non-garbage memory in use */

  /* 
  ** 存放系统中所有短字符串的hash表，相同的短字符串在lua中只会存在一份，但
  ** 长字符串时独立存放的，因此相同的长字符串可能会有多份。
  */
  stringtable strt;  /* hash table for strings */
  
  /* 
  ** 保存全局的注册表，注册表就是一个全局的table（即整个虚拟机中只有一个注册表），
  ** 它只能被C代码访问，通常，它用来保存那些需要在几个模块中共享的数据
  */
  TValue l_registry;

  /* lua中进行hash操作时的随机种子，例如给字符串对象进行hash时，会使用该成员的值。 */
  unsigned int seed;  /* randomized seed for hashes */
  lu_byte currentwhite;
  lu_byte gcstate;  /* state of garbage collector */
  lu_byte gckind;  /* kind of GC running */
  /* 开启GC的标志位 */
  lu_byte gcrunning;  /* true if GC is running */
  /* allgc用来链接所有需要进行内存回收（GC）的对象。 */
  GCObject *allgc;  /* list of all collectable objects */
  GCObject **sweepgc;  /* current position of sweep in list */
  GCObject *finobj;  /* list of collectable objects with finalizers */
  GCObject *gray;  /* list of gray objects */
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  GCObject *weak;  /* list of tables with weak values */
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  GCObject *allweak;  /* list of all-weak tables */
  GCObject *tobefnz;  /* list of userdata to be GC */
  
  /* 用于保存不被GC回收的对象，如lua中关键字对应的TString对象，元方法对应的TString对象等等 */
  GCObject *fixedgc;  /* list of objects not to be collected */
  struct lua_State *twups;  /* list of threads with open upvalues */
  unsigned int gcfinnum;  /* number of finalizers to call in each GC step */
  int gcpause;  /* size of pause between successive GCs */
  int gcstepmul;  /* GC 'granularity' */
  lua_CFunction panic;  /* to be called in unprotected errors */

  /* 主线程Mainthread对应的状态信息 */
  struct lua_State *mainthread;
  /* 存放版本号 */
  const lua_Number *version;  /* pointer to version number */
  TString *memerrmsg;  /* memory-error message */

  /* 存放lua中支持的event对应的名字(字符串) */
  TString *tmname[TM_N];  /* array with tag-method names */

  /* 基本类型的元表。除了userdata和table之外，其余类型的每种类型的所有对象共享同一个元表。 */
  struct Table *mt[LUA_NUMTAGS];  /* metatables for basic types */
  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
} global_State;


/*
** 'per thread' state
*/
/*
** lua_State结构体用于存放Lua虚拟机中单个线程（thread）的全局状态信息。Lua官方文档该
** 类型的对象用于指代Lua中的一个线程，每个线程拥有自己独立的数据栈和函数调用链。因此
** 该类型的对象的内容会随着线程的执行而改变，因此是一个动态数据的集合，描述了线程的
** 执行状态机信息。
*/
struct lua_State {
  CommonHeader;

  /*
  ** nci表示链表成员ci中包含的CallInfo对象的个数，即ci链表的元素个数。
  */
  unsigned short nci;  /* number of items in 'ci' list */

  /* status存放的是thread的执行状态 */
  lu_byte status;

  /* 指向整个栈的栈顶位置（未存入有效数据） */
  StkId top;  /* first free slot in the stack */
  global_State *l_G;  /* l_G指向的是由所有thread共享的全局虚拟机信息 */

  /*
  ** L->ci指向的始终是函数调用链中当前正在执行的函数调用对应的CallInfo节点。
  ** 链表ci存放的是当前函数的调用链信息，调用链中的CallInfo由一个双向链表串起来。
  ** 通过遍历这个ci链表，就可以获取到完成的调用链信息。在这个双向链表中，ci只是
  ** 用于遍历这个双向链表，ci是一个指针，是没有分配具体的CallInfo对象所需内存的，
  ** 而双向链表中的其他CallInfo节点都是分配了内存的，于是这个ci就用来根据需要指向
  ** 其中的某一个CallInfo节点，指向当前正在进行的函数调用所对应的CallInfo节点。
  ** ci->previous表示的是当前正在被调用的函数的调用者所对应的CallInfo对象，这一点从
  ** luaD_poscall()函数中可以看出。举例如下：下面的cix就是指CallInfo对象
  **    ----------    ----------    -----------
  **    |  ci1   | -> |  ci2   | -> |   ci3   |
  **    |(func1) | <- |(func2) | <- |(func3)  |
  **    ----------    ----------    -----------
  **      L->ci
  ** func1()调用了func2()，func2()中调用了func3()。
  ** 假设当前正在执行func1()这个函数，L->ci就会指向ci1，且此时ci链表中只会有最左边那个ci1，
  ** 当func1执行到调用func2()的地方时，就会创建一个ci2，此时L->ci就会指向ci2，当func2执行到
  ** 调用func3()的时候，将会创建一个ci3，相应的L->ci就会指向ci3，当func3()执行完了之后，在
  ** luaD_poscall()中进行函数调用收尾工作时，就会将L->ci指向ci2(即ci3->previous)，表示当前正在
  ** 进行的函数调用是func2。此时并不会立刻就是放ci3节点，因为func2()中可能还会调用其他函数，比如
  ** func4()，那么这个时候就可以复用ci3这个CallInfo节点。func2()执行完毕之后，和上面func3()类似，
  ** L->ci就会指向ci1（ci2->previous），这个时候ci2，ci3都不会被释放，因为可能func1中还会调用其他
  ** 函数，被调用的函数中可能又会调用其他函数，那么ci2和ci3都可以复用。
  ** 因此ci指向的双向链表的每一个CallInfo节点代表的就是一层函数调用。但有一点不变，就是L->ci指向的
  ** 始终是函数调用链中当前正在执行的函数调用对应的CallInfo节点。
  */
  CallInfo *ci;  /* call info for current function */

  /*
  ** 保存最后一次跟踪的函数调用的执行断点，即该函数下一条将要执行的指令的地址。用于hook。
  */
  const Instruction *oldpc;  /* last pc traced */

  /* stack_last存放整个栈的内存上限 */
  StkId stack_last;  /* last free slot in the stack */

  /* 栈的起始地址 */
  StkId stack;  /* stack base */
  UpVal *openupval;  /* list of open upvalues in this stack */
  GCObject *gclist;
  struct lua_State *twups;  /* list of threads with open upvalues */

  /*
  ** 每个线程L中都会保存当前longjump的返回点errorJmp，这个成员的类型是struct lua_longjmp *，
  ** 这个结构体是一个链表，节点里面包含了setjump()所需要的jmp buffer成员。每运行一段受保护的
  ** 代码，都会生成一个新的返回点，并插入到这个链表中。受保护的代码通常容易引起错误，为了在
  ** 引起错误时能够返回到运行这段代码之前，因此需要设置一个返回点，这样才能跳转回来。
  */
  struct lua_longjmp *errorJmp;  /* current error recover point */

  /*
  ** 该线程执行的第一个函数调用信息(其对应的函数对象其实是nil。)，即base_ci记录的是调用栈的
  ** 栈底（最外层的CallInfo）, base_ci对应的函数一定是从C函数发起的调用。
  */
  CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */

  /* 用于注册钩子函数到线程中 */
  volatile lua_Hook hook;

  /* 错误处理函数的在栈中的索引 */
  ptrdiff_t errfunc;  /* current error handling function (stack index) */

  /* stacksize存放的是函数调用栈的大小 */
  int stacksize;

  /*
  ** basehookcount是用户设置了钩子函数的计数，当虚拟机运行的指令数达到basehookcount时，
  ** 如果注册了对应LUA_HOOKCOUNT事件的钩子函数，那么此时钩子函数就会被执行。
  */
  int basehookcount;

  /* hookcount用于记录当前距离执行对应LUA_HOOKCOUNT事件的钩子函数还剩的还没有执行的指令数。 */
  int hookcount;

  /* 线程中不可中断的函数调用数 */
  unsigned short nny;  /* number of non-yieldable calls in stack */

  /* 嵌套调用的函数的层数 */
  unsigned short nCcalls;  /* number of nested C calls */

  /* 存放触发钩子函数调用的事件对应的掩码 */
  l_signalT hookmask;

  /* 线程允许执行钩子函数的标志位 */
  lu_byte allowhook;
};


/* 获取L中存放的全局状态信息指针 */
#define G(L)	(L->l_G)


/*
** Union of all collectable objects (only for conversions)
*/
/*
** 用于存放所有需要GC的数据类型的联合体，这个联合体主要用于做类型转换使用。
** lua是怎么利用下面这个联合体实现将GCObject转换为具体的某一种需要GC的类型的呢？
** 我们知道，所有需要进行GC的类型在其对应的结构体开始都会包含一个CommonHeader。
** union GCUnion中列出了所有类型都是需要进行GC的，都包含CommonHeader。实际上GCObject
** 这个类型其实就只包含了CommonHeader，因此对于一个类型为union GCUnion的值，该值的
** 内存中最开始都包含了CommonHeader，而不管该值是union GCUnion中哪一种具体的类型。
** 因此可以利用这一特性来做类型转换。
** lua中为什么这么做呢？我的理解是为了实现对需要进行GC的类型进行统一管理，比如我们要申请
** 一个需要进行GC的类型的对象，由于需要进行GC的类型很多，我们不可能为每中类型都创建申请接口，
** 这样就会有很多冗余代码。因为在申请一个需要进行GC的类型的对象时，我们统一转换为申请一个GCObject
** 类型的对象，申请的同时传入具体的某个类型以及所需要的内存大小。这样在申请完GCObject对象时，
** 我们就可以按需转换为我们所需要的某个类型。因为大家的头部都一样，多出来的就是某个类型自己私有
** 的东西了。
** GCUnion联合体所有可能的类型的对象的地址都和类型为GCObject的gc的地址一样。
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

/* 将value对象o进行强制类型转换，转换成union GCUnion* 类型 */
#define cast_u(o)	cast(union GCUnion *, (o))

/* macros to convert a GCObject into a specific value */
/* 下面的宏用于将一个GCObject转换为某一个具体的value对象 */

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
/* 
** obj2gco用于将某个需要进行垃圾回收的对象强转为GCObject对象，之所以可以这样转，
** 是因为所有需要进行垃圾回收的对象都和GCObject有一样的头部，则类型转换后怎么使用
** 程序可以通过上下文得知。
*/
#define obj2gco(v) \
	check_exp(novariant((v)->tt) < LUA_TDEADKEY, (&(cast_u(v)->gc)))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_freeCI (lua_State *L);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);


#endif

