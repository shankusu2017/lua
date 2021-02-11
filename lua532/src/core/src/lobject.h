/*
** $Id: lobject.h,v 2.116 2015/11/03 18:33:10 roberto Exp $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/


#ifndef lobject_h
#define lobject_h


#include <stdarg.h>


#include "llimits.h"
#include "lua.h"


/*
** Extra tags for non-values
*/
#define LUA_TPROTO	LUA_NUMTAGS		/* function prototypes */
#define LUA_TDEADKEY	(LUA_NUMTAGS+1)		/* removed keys in tables */

/*
** number of all possible tags (including LUA_TNONE but excluding DEADKEY)
*/
#define LUA_TOTALTAGS	(LUA_TPROTO + 2)


/*
** tags for Tagged Values have the following use of bits:
** bits 0-3: actual tag (a LUA_T* value)
** bits 4-5: variant bits 类型拓展标记位
** bit    6: whether value is collectable 可回收标记位
*/


/*
** LUA_TFUNCTION variants:
** 0 - Lua function
** 1 - light C function
** 2 - regular C function (closure)
*/
/* Variant tags for functions */
#define LUA_TLCL	(LUA_TFUNCTION | (0 << 4))  /* Lua closure */
#define LUA_TLCF	(LUA_TFUNCTION | (1 << 4))  /* light C function:没有UpVal的C函数？ */
#define LUA_TCCL	(LUA_TFUNCTION | (2 << 4))  /* C closure */


/* Variant tags for strings */
#define LUA_TSHRSTR	(LUA_TSTRING | (0 << 4))  /* short strings */
#define LUA_TLNGSTR	(LUA_TSTRING | (1 << 4))  /* long strings */


/* Variant tags for numbers */
#define LUA_TNUMFLT	(LUA_TNUMBER | (0 << 4))  /* float numbers */
#define LUA_TNUMINT	(LUA_TNUMBER | (1 << 4))  /* integer numbers */


/* Bit mark for collectable types */
#define BIT_ISCOLLECTABLE	(1 << 6)

/* 
 ** mark a tag as collectable
 ** 将一个标记打上gc的标记
*/
#define ctb(t)			((t) | BIT_ISCOLLECTABLE)


/*
** Common type for all collectable objects
** 翻查C手册page.119，这里可以前向typedef定义
*/
typedef struct GCObject GCObject;


/*
** Common Header for all collectable objects 
** (in macro form, to beincluded in other objects)
*/
#define CommonHeader	GCObject *next; lu_byte tt; lu_byte marked


/*
** Common type has only the common header
** 从TValue中的GCObject *gc域看出，凡是gc的类型，必定以GCObject开始
*/
struct GCObject {
  CommonHeader;
};




/*
** Tagged Values. This is the basic representation of values in Lua,
** an actual value plus a tag with its type.
*/

/*
** Union of all Lua values
** integer/float这里可以做了识别
** 
** NOTE:其它的TString,Table等均以GCObject开头时，Value的gc域值就可以用指向GCObject的类型指针来
**      指向TString,Table等实体的地址，因为结构体的第一个域的地址也是结构体的地址
*/
typedef union Value {
  GCObject *gc;    /* collectable objects */
  void *p;         /* light userdata */
  
  int b;           /* booleans 0:false, others:true,用个char就行了 */
  
  lua_CFunction f; /* light C functions */

  /* 这下好，整型和浮点型直接分开存储，直接简单明了 */
  lua_Integer i;   /* integer numbers */
  lua_Number n;    /* float numbers */
} Value;

/* 
 ** 对外接口层的value,type 
 ** 这里有一层type，GCObject中也有自己的一套封装type
*/
#define TValuefields	Value value_; int tt_

/* 这里包装成结构体，方便后续宏定义以及其它地方的参数类型转转  */
typedef struct lua_TValue {
  TValuefields;
} TValue;



/* macro defining a nil value */
#define NILCONSTANT	{NULL}, LUA_TNIL


/* 这里的val_下划线和后面的value_中的下划线对应^_^ */
#define val_(o)		((o)->value_)


/* 
 ** raw type tag of a TValue 
 ** 读取基础类型bits[0,3],类型拓展bits[4,5],回收标记bit[6,6] 读类型的所有bits
*/
#define rttype(o)	((o)->tt_)

/* 
 ** tag with no variants(变体) (bits 0-3) 
 **
 ** 读类型的基础bit[0,3],
 ** 
 ** 不包含扩展类型bit[4,5],以及回收标志[6,6]
*/
#define novariant(x)	((x) & 0x0F)

/* 
 ** type tag of a TValue (bits- 0-3 for tags + variant bits 4-5) 
 ** 
 ** 读取基础类型bits[0,3],类型拓展[4,5], 
 ** 不包含gc-bit[6]
*/
#define ttype(o)	(rttype(o) & 0x3F)

/* 
 ** type tag of a TValue with no variants (bits 0-3) 
 **
 ** 读类型的基础bit[0,3], 
 ** 不包含扩展类型bit[4,5],以及回收标志[6,6]
*/
#define ttnov(o)	(novariant(rttype(o)))


/* Macros to test type */
/* 判断所有的bits[0,6] */
#define checktag(o,t)		(rttype(o) == (t))
/* 判断基础类型[0,3] */
#define checktype(o,t)		(ttnov(o) == (t))

/* 注意这里分别使用了checktype，checktag两个宏,前面是粗类型判断，后面需要进行精细的拓展类型判断 */
#define ttisnumber(o)		checktype((o), LUA_TNUMBER)
#define ttisfloat(o)		checktag((o), LUA_TNUMFLT)
#define ttisinteger(o)		checktag((o), LUA_TNUMINT)

#define ttisnil(o)		checktag((o), LUA_TNIL)
#define ttisboolean(o)		checktag((o), LUA_TBOOLEAN)
#define ttislightuserdata(o)	checktag((o), LUA_TLIGHTUSERDATA)

#define ttisstring(o)		checktype((o), LUA_TSTRING)
/* 这里先消除bit[6]带来的影响，下同 */
#define ttisshrstring(o)	checktag((o), ctb(LUA_TSHRSTR))
#define ttislngstring(o)	checktag((o), ctb(LUA_TLNGSTR))
/* 这个判断应该被常用 */
#define ttistable(o)		checktag((o), ctb(LUA_TTABLE))

/* bits[0,3] 基础类型判断：是否为函数 */
#define ttisfunction(o)		checktype(o, LUA_TFUNCTION)

/* 
* 上面的宏定义搬到这里，方便理解下面的宏定义
* #define LUA_TLCL	(LUA_TFUNCTION | (0 << 4))   Lua closure 
* #define LUA_TLCF	(LUA_TFUNCTION | (1 << 4))   light C function 
* #define LUA_TCCL	(LUA_TFUNCTION | (2 << 4))   C closure 
*/
#define ttisclosure(o)		((rttype(o) & 0x1F) == LUA_TFUNCTION)
#define ttisCclosure(o)		checktag((o), ctb(LUA_TCCL))
#define ttisLclosure(o)		checktag((o), ctb(LUA_TLCL))
#define ttislcf(o)		checktag((o), LUA_TLCF)

#define ttisfulluserdata(o)	checktag((o), ctb(LUA_TUSERDATA))
#define ttisthread(o)		checktag((o), ctb(LUA_TTHREAD))
#define ttisdeadkey(o)		checktag((o), LUA_TDEADKEY)


/* 
 ** Macros to access values 
 ** SI 定位出来的check_exp宏定义：echek_exp(c,e) (lua_asser(c), (e)) 不太准？
 ** 自己梳理下头文件包含逻辑后，应该是后面的这个：check_exp(c,e) (e)
*/
/* 读整数域 */
#define ivalue(o)	check_exp(ttisinteger(o), val_(o).i)
/* 读浮点域 */
#define fltvalue(o)	check_exp(ttisfloat(o), val_(o).n)
/* 根据类型来决定读整数还是浮点域 */
#define nvalue(o)	check_exp(ttisnumber(o), \
	(ttisinteger(o) ? cast_num(ivalue(o)) : fltvalue(o)))

/* 读取gc域,iscollectable的检查还是必须要有的 */
#define gcvalue(o)	check_exp(iscollectable(o), val_(o).gc)
/* 读取 void *类型的 p域 */
#define pvalue(o)	check_exp(ttislightuserdata(o), val_(o).p)
/* 读String */
#define tsvalue(o)	check_exp(ttisstring(o), gco2ts(val_(o).gc))
/* 读 Udata */
#define uvalue(o)	check_exp(ttisfulluserdata(o), gco2u(val_(o).gc))

/* 读 CLosure, CClosure, LClosure */
#define clvalue(o)	check_exp(ttisclosure(o), gco2cl(val_(o).gc))
#define clLvalue(o)	check_exp(ttisLclosure(o), gco2lcl(val_(o).gc))
#define clCvalue(o)	check_exp(ttisCclosure(o), gco2ccl(val_(o).gc))
/* 读Light C function */
#define fvalue(o)	check_exp(ttislcf(o), val_(o).f)

/* 读Table */
#define hvalue(o)	check_exp(ttistable(o), gco2t(val_(o).gc))
/* 读 booleans */
#define bvalue(o)	check_exp(ttisboolean(o), val_(o).b)
/* 读 thread */
#define thvalue(o)	check_exp(ttisthread(o), gco2th(val_(o).gc))

/* a dead value may get the 'gc' field, but cannot access its contents */
#define deadvalue(o)	check_exp(ttisdeadkey(o), cast(void *, val_(o).gc))


/* 是否为FALSE，nil或者booleans.false均被认为FALSE */
#define l_isfalse(o)	(ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))

/* 标记是否具有gc标记？ */
#define iscollectable(o)	(rttype(o) & BIT_ISCOLLECTABLE)


/* 
 ** Macros for internal（内置） tests
 ** 判断类型
 ** obj是TValue,这里校验下外壳和内部的object中的type是否“一致 ”
*/
#define righttt(obj)		(ttype(obj) == gcvalue(obj)->tt)

/* 检测生命周期？还不太明白 */
#define checkliveness(L,obj) \
	lua_longassert(!iscollectable(obj) || \
		(righttt(obj) && (L == NULL || !isdead(G(L),gcvalue(obj)))))


/* 
 ** Macros to set values
 ** 设置TValue中的类型tt_，OBJect中的类型域tt没处理
*/
#define settt_(o,t)	((o)->tt_=(t))

/*
 ** 简单的赋值操作，gcobject的在后面
 ** 直接设置TValue的的value_和tt_域
 **
 **
*/
/* 将对象赋float值，且设置为对应的type LUA_TNUMFLT == LUA_T + NUM + FLT */
#define setfltvalue(obj,x) \
  { TValue *io=(obj); val_(io).n=(x); settt_(io, LUA_TNUMFLT); }
/* 先检测原来的type是否为float,若是，才赋新的float值，下同 */
#define chgfltvalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisfloat(io)); val_(io).n=(x); }

#define setivalue(obj,x) \
  { TValue *io=(obj); val_(io).i=(x); settt_(io, LUA_TNUMINT); }
#define chgivalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisinteger(io)); val_(io).i=(x); }

/* 设置object为Nil */
#define setnilvalue(obj) settt_(obj, LUA_TNIL)

/* 将对象设置为 Light C Function */
#define setfvalue(obj,x) \
  { TValue *io=(obj); val_(io).f=(x); settt_(io, LUA_TLCF); }

/* 设置对象为 light userdata值 */
#define setpvalue(obj,x) \
  { TValue *io=(obj); val_(io).p=(x); settt_(io, LUA_TLIGHTUSERDATA); }

/* 设置对象为bool值 */
#define setbvalue(obj,x) \
  { TValue *io=(obj); val_(io).b=(x); settt_(io, LUA_TBOOLEAN); }

/*
 ** GCObject类型的变量赋值，一定要看懂setgcovalue，setsvalue这两个例子，其它原理相同
 **
 **
*/
/* 将x中指向object的指针赋值一份给obj,且设置obj的type */
#define setgcovalue(L,obj,x) \
  { TValue *io = (obj); GCObject *i_g=(x); \
    val_(io).gc = i_g; settt_(io, ctb(i_g->tt)); }

/* 将x的String域赋值为obj */
#define setsvalue(L,obj,x) \
  { TValue *io = (obj); TString *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(x_->tt)); \
    checkliveness(L,io); }

#define setuvalue(L,obj,x) \
  { TValue *io = (obj); Udata *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TUSERDATA)); \
    checkliveness(L,io); }

/* 将x的Lua_State域赋值给obj */
#define setthvalue(L,obj,x) \
  { TValue *io = (obj); lua_State *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTHREAD)); \
    checkliveness(L,io); }

#define setclLvalue(L,obj,x) \
  { TValue *io = (obj); LClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TLCL)); \
    checkliveness(L,io); }

#define setclCvalue(L,obj,x) \
  { TValue *io = (obj); CClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TCCL)); \
    checkliveness(L,io); }

/* 将x的Table域赋值为obj */
#define sethvalue(L,obj,x) \
  { TValue *io = (obj); Table *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_TTABLE)); \
    checkliveness(L,io); }

#define setdeadvalue(obj)	settt_(obj, LUA_TDEADKEY)


/* obj1 = obj2 */
#define setobj(L,obj1,obj2) \
	{ TValue *io1=(obj1); *io1 = *(obj2); \
	  (void)L; checkliveness(L,io1); }

/*
** different types of assignments, according to destination
*/

/* 学学下面两个宏定义名字 */
/* from stack to (same) stack */
#define setobjs2s	setobj
/* to stack (not from same stack) */
#define setobj2s	setobj

/* 浅拷贝，下同 TString */
#define setsvalue2s	setsvalue
/* 拷贝 Table */
#define sethvalue2s	sethvalue

/* setptvalue尚未找到实现 */
#define setptvalue2s	setptvalue

/* from table to same table */
#define setobjt2t	setobj

/* to new object */
#define setobj2n	setobj
#define setsvalue2n	setsvalue

/* 
 ** to table (define it as an expression to be used in macros) 
 ** 深拷贝？
*/
#define setobj2t(L,o1,o2)  ((void)L, *(o1)=*(o2), checkliveness(L,(o1)))




/*
** {======================================================
** types and prototypes
** =======================================================
*/


typedef TValue *StkId;  /* index to stack elements */




/*
** Header for string value;
** string bytes follow the end of this structure
** (aligned according to 'UTString'; see next).
**
** 字符串可以记录'\0',故而需要主动记录其长度
*/
typedef struct TString {
  CommonHeader;
  
  /*
  ** 短字符串: 是否为保留字-llex.c中的luaX_init函数会标记此域，其它地方是否有标记，暂不清楚
  ** ltm.c 中没有处理.通读代码可以，short'string中的extra仅lex子模块用
  ** 长字符串: 是否以计算过hash值
  */
  lu_byte extra;  			/* reserved words for short strings; "has hash" for longs */
  
  lu_byte shrlen;  			/* length for short strings */
  
  /* 
  ** 短字符串:字符串对应的hash值
  ** 长字符串:分情况，若extra！=0，则为其hash值，若==0,则是global_state的seed,
  **          等到需要使用自己的hash时才计算(节省CPU)
  */
  unsigned int hash;
  union {
    size_t lnglen;  		/* length for long strings */
    struct TString *hnext;  /* linked list for hash table */
  } u;
} TString;


/*
** Ensures that address after this type is always fully aligned.
**
** 请仔细查阅C语言中联合部分对齐要求的手册，这个dummy真是妙
** UTString中的dummy的作用:接下来的数据域的开头地址满足最严格的对齐要求，故而可以放任意数据类型到TString的后面
** 下同
** sizeof(UTString)返回的值中考虑到了后面接一个UTString的情况下的单个UTString所需要的地址空间
** 后面接的UTString可能是dummy,这里dummy有最严格的对齐要求，意味着，后面可以跟任何类型的数据类型
** TString后面只跟char *冒失没有必要弄这么严格的要求，但是后续的gcobject（eg:Udata)都是
** 这样做的，所以这里就统一这样处理了，无需纠结这个小细节
*/
typedef union UTString {
  /* dummy放到tsv后面冒失更容易理解？ */
  L_Umaxalign dummy;	/* ensures maximum alignment for strings */
  TString tsv;
} UTString;


/*
** Get the actual string (array of bytes) from a 'TString'.
**
** (Access to 'extra' ensures that value is really a 'TString'.)
** 
** 提取TString中携带的数据域的头地址, 
*/
#define getstr(ts)  \
  check_exp(sizeof((ts)->extra), cast(char *, (ts)) + sizeof(UTString))


/* 
** get the actual string (array of bytes) from a Lua value
** 提取数据域地址前核查下是否为字符串
*/
#define svalue(o)       getstr(tsvalue(o))

/* 
 ** get string length from 'TString *s'
 ** 字符串的长度
*/
#define tsslen(s)	((s)->tt == LUA_TSHRSTR ? (s)->shrlen : (s)->u.lnglen)

/* 
** get string length from 'TValue *o' 
*/
#define vslen(o)	tsslen(tsvalue(o))


/*
** Header for userdata; memory area follows the end of this structure
** (aligned according to 'UUdata'; see next).
** UserData数据结构
*/
typedef struct Udata {
  CommonHeader;
  
  lu_byte ttuv_;  /* user value's tag */
  struct Table *metatable;
  size_t len;  /* number of bytes */
  union Value user_;  /* user value */
} Udata;


/*
** Ensures that address after this type is always fully aligned.
** dummy:虚拟的
*/
typedef union UUdata {
  L_Umaxalign dummy;  /* ensures maximum alignment for 'local' udata */
  Udata uv;
} UUdata;


/*
**  Get the address of memory block inside 'Udata'.
** (Access to 'ttuv_' ensures that value is really a 'Udata'.)
** 读UData的数据域
*/
#define getudatamem(u)  \
  check_exp(sizeof((u)->ttuv_), (cast(char*, (u)) + sizeof(UUdata)))

/* 
** 将o赋值给u
** !!!! 注意这里o,u所使用的各自域却不一样，一个是user_,另外一个是value_,下同
** 这里还用了const语法，兼容性更好点吧
*/
#define setuservalue(L,u,o) \
	{ const TValue *io=(o); Udata *iu = (u); \
	  iu->user_ = io->value_; iu->ttuv_ = rttype(io); \
	  checkliveness(L,io); }

/* 提取u中的user_数据域给o的value_域 */
#define getuservalue(L,u,o) \
	{ TValue *io=(o); const Udata *iu = (u); \
	  io->value_ = iu->user_; settt_(io, iu->ttuv_); \
	  checkliveness(L,io); }


/*
** Description of an upvalue for function prototypes
** 看得出，upval可能存在于堆栈，也可能存在于list中
*/
typedef struct Upvaldesc {
  /* name域为NULL，是否意味着本值无效？ */
  TString *name;    /* upvalue name (for debug information) */

  lu_byte instack;  /* whether it is in stack (register) */
  lu_byte idx;      /* index of upvalue (in stack or in outer function's list) */
} Upvaldesc;


/*
** Description of a local variable for function prototypes
** (used for debug information)
** 本地变量？若是，那全局变量是否有相关定义，若有则具体结构？和loc的差异？
*/
typedef struct LocVar {
  TString *varname;	/* 初始是为NULL */
  /* 这里就决定了变量的作用域？ */
  int startpc;  		/* first point where variable is active */
  int endpc;    		/* first point where variable is dead */
} LocVar;


/*
** Function Prototypes
*/
typedef struct Proto {
  CommonHeader;			      /* Proto也有GCOBJET的性质 */

  lu_byte is_vararg;      /* 0:固定参数，1: fun(...)，2: (arg1, argN, ...) ? */

  lu_byte numparams;      /* number of fixed parameters，形参中固定参数数量(当输入参数不足这个数量时，lua要补nil) 
                            包括两种可能的形式 fun(arg1, arg2), fun(arg1, arg2, arg3, ...)*/
  
  lu_byte maxstacksize;   /* number of registers needed by this function */

  int linedefined;  	  /* debug information  */
  int lastlinedefined;    /* debug information  */
  
  /* constants used by the function，本闭包用到的常量表
  ** 函数中有引用常量的地方，直接从此表取值即可
  ** 数组大小为sizek,实际使用的数量在FunState的nk域
  */
  TValue *k;              /* local reference to function's constant table */
  int sizek;              /* size of 'k' */          

  /* 
  ** 指向第一个指令的地址配合fs->pc使用
  */
  Instruction *code;      /* opcodes */
  int sizecode;           /* 指令数组大小 */

  /* 这里说明函数内还可以定义函数！和C大不同且不止一个，这里是个2级指针！！！！！ */
  struct Proto **p;       /* functions defined inside the function */
  int sizep;              /* size of 'p' */

  /* 每一条指令对应的行数信息 */
  int *lineinfo;          /* map from opcodes to source lines (debug information) */
  int sizelineinfo;

  /* 下面两个是个重点域！但都是一级指针,二不是二级指针 */
  /* locvars:sizelocvars匹配 */
  LocVar *locvars;        /* information about local variables (debug information) */
  int sizelocvars;				/* 已分配的array的大小 */
  /* 真正被使用的大小在FunState的nlocvars中 */
	
  /* 实际被使用的大小在FunState的nups中 */
  Upvaldesc *upvalues;    /* upvalue information */
  int sizeupvalues;       /* size of 'upvalues' */

  /* 若函数无闭包则可以N份closure共享一份Proto? */
  struct LClosure *cache; /* last-created closure with this prototype */
	
  /* 被编译的文件名 */
  TString  *source;       /* used for debug information */
  
  /* 
  ** 前面不是有一个CommonHeader么：那个是gc内存管理用的
  ** 
  ** 这里是Proto自己形式的链表需要用到的指针
  */
  GCObject *gclist;
} Proto;



/*
** Lua Upvalues
*/
typedef struct UpVal UpVal;


/*
** Closures
**
** nupvalues:上值个数
** L和C闭包共用一个结构体的头
*/
#define ClosureHeader \
	CommonHeader; lu_byte nupvalues; GCObject *gclist

/* C闭包 */
typedef struct CClosure {
  ClosureHeader;
  lua_CFunction f;	/* 这个用来干啥？指向C的XXXX函数？ */
  TValue upvalue[1];  /* list of upvalues ，用来干啥？用来存放所有的upvalue？ */
} CClosure;

/* Lua闭包 */
typedef struct LClosure {
  ClosureHeader;
  struct Proto *p;		/* 闭包对应的函数 */
  
  /* list of upvalues,数量在nupvalues中定义 
  ** 每个LClosure都有一个_ENV闭包数据，所以这里大小至少为1？
  */
  UpVal *upvals[1];  	
} LClosure;

/* 上述统一起来 */
typedef union Closure {
  CClosure c;
  LClosure l;
} Closure;


#define isLfunction(o)	ttisLclosure(o)

/* o->gc->l(LClosure)->p */
#define getproto(o)	(clLvalue(o)->p)


/*
** Tables
*/
/* 
** 这里组合了2种类型的key
** nk:用于表的散列部分
** tvk:仅仅让定义更完整而已，array部分直接用数组下标做key，这里tvk没有被使用到(可以在取key的val的直接.key.val)
*/
typedef union TKey {
  struct {
    TValuefields;
    int next;  	/* for chaining (offset for next node),是否能改为unsigned? */
  } nk;			/* node-key的简写？ */
  TValue tvk;
} TKey;


/* 
** copy a value into a key without messing up field 'next' 
** 赋值nk的值域和类型域，相当于一个简单的浅拷贝
** (void)L是消除编译警告，暂无其它意思
** NOTE:next域没有处理
** 
** checkliveness:作用尚不明确(可能和GC相关)
*/
#define setnodekey(L,key,obj) \
	{ TKey *k_=(key); const TValue *io_=(obj); \
	  k_->nk.value_ = io_->value_; k_->nk.tt_ = io_->tt_; \
	  (void)L; checkliveness(L,io_); }

/* 
 ** Table的一个Node
 ** 
 ** 下面的array部分是否没有用到TKey,那么是否意味着Tkey中的tvk是多余的呢？
 ** 从内存分布来看，也没啥多余的,毕竟tvk完全是nk的子集，不额外占用一丝一毫的内存。
 ** TKey中tvk,nk的两个声明使TKey的定义更为完整。
*/
typedef struct Node {
  TValue i_val;		/* node'value */
  TKey i_key;			/* node'key */
} Node;


typedef struct Table {
  CommonHeader;
  lu_byte 			flags;  /* 1<<p means tagmethod(p) is not present，这里和一般的==0表示否定意思有点不一样哦 */

  /* 散列Node相关域，多过几遍 */
  Node 					*node;
  lu_byte 			lsizenode;  /* log2 of size of 'node' array,内部函数会强制调整输入的nodeSize，使其满足2的幂的要求 */

  /* any free position is before this position
  ** 
  ** 看注释和代码可知lastfree被初始化为node.last+1，即一个哨兵值
  ** C语言规定：数组最后一个元素的下一个元素的地址仍然是有意义的，
  ** 但是不能进行解析地址操作,所以这里这样做没问题（不知是利用了语言的特点还是作者的编程习惯）。
  */
  Node 					*lastfree;  

  /* 数组部分，这里的数据cell的结构比Node的要简单的多也易懂 */
  TValue 				*array;     /* array part */  
  unsigned int 	sizearray;  /* size of 'array' array,构造时可以指定size且不一定是2的幂，所以这里用了自然数 */
  
  struct Table 	*metatable;	/* 本表的元表的指针 */

	/* 
	** CommonHeader中的GCObject *next用于global_State中的allgc链表中
	** 此域被用于global_State中的gray,black的链表中
	** 阅读完GC相关原理后会发现两者的差异
	*/
  GCObject 		 	*gclist;
} Table;



/*
** 'module' operation for hashing (size is always a power(幂) of 2)
** size&(size-1)==0,必然可以推出size=2的正整数N次方的结果
** (s & ((size) - 1)))这结果，将size={2,4,8,16....}代入进去就知道是求余运算
** 综合来看就是求s对size的余，且size必须是2的正整数次方
*/
#define lmod(s,size) \
	(check_exp((size&(size-1))==0, (cast(int, (s) & ((size)-1)))))

/* C的移位运算，2的N次方 */
#define twoto(x)	(1<<(x))

/* 
** 这里根据lsizenode来反向求Table中node部分大小的常规表达式的数值 
** 根据Table中lsizenode的定义，若node的实际大小为32，则lsizenode==5
** 现在知道了lsizenode==5,那么Node的大小到底是多少呢？用这个宏来计算是32，直接用C语言左移一位相当于*=2的效果来计算
** 1*=2,*=2,*=2,*=2,*=2,----->32
*/
#define sizenode(t)	(twoto((t)->lsizenode))


/*
** (address of) a fixed nil value
** 仔细看这luaO_nilobject的实现
*/
#define luaO_nilobject		(&luaO_nilobject_)

/* 注意这里的const修饰 */
LUAI_DDEC const TValue luaO_nilobject_;

/* size of buffer for 'luaO_utf8esc' function */
#define UTF8BUFFSZ	8

/* 这两个函数不知道啥意思 */
LUAI_FUNC int luaO_int2fb (unsigned int x);
LUAI_FUNC int luaO_fb2int (int x);

LUAI_FUNC int luaO_utf8esc (char *buff, unsigned long x);

/* 同函数名，求2的对数，结果向上取整 */
LUAI_FUNC int luaO_ceillog2 (unsigned int x);

LUAI_FUNC void luaO_arith (lua_State *L, int op, const TValue *p1,
                           const TValue *p2, TValue *res);
/* 字符转数字？ */
LUAI_FUNC size_t luaO_str2num (const char *s, TValue *o);

/* 16进制字符对应的数值 eg: '5' == 5, 'f'== 15 */
LUAI_FUNC int luaO_hexavalue (int c);

/* obj指向的对象转为String */
LUAI_FUNC void luaO_tostring (lua_State *L, StkId obj);

/* 按格式要求压入字符串 ？ */
LUAI_FUNC const char *luaO_pushvfstring (lua_State *L, const char *fmt,
                                                       va_list argp);
LUAI_FUNC const char *luaO_pushfstring (lua_State *L, const char *fmt, ...);

/* 不太明白函数作用 */
LUAI_FUNC void luaO_chunkid (char *out, const char *source, size_t len);


#endif

