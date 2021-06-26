/*
** $Id: lobject.h,v 2.20.1.2 2008/08/06 13:29:48 roberto Exp $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/


#ifndef lobject_h
#define lobject_h


#include <stdarg.h>


#include "llimits.h"
#include "lua.h"


/* tags for values visible from Lua */
#define LAST_TAG	LUA_TTHREAD

#define NUM_TAGS	(LAST_TAG+1)


/*
** Extra tags for non-values
*/
#define LUA_TPROTO	(LAST_TAG+1)
#define LUA_TUPVAL	(LAST_TAG+2)
#define LUA_TDEADKEY	(LAST_TAG+3)	/* 表中val为nil的key则为DEADKEY */


/*
** Union of all collectable objects
*/
typedef union GCObject GCObject;


/*
** Common Header for all collectable objects (in macro form, to be
** included in other objects)
*/
#define CommonHeader	GCObject *next; lu_byte tt; lu_byte marked


/*
** Common header in struct form
*/
typedef struct GCheader {
  CommonHeader;
} GCheader;




/*
** Union of all Lua values
*/
typedef union {
  GCObject 	 *gc;
  void 		 *p;	// light userdata 需C自己管理生命周期
  lua_Number n;		// double 可以准确的表示一定范围内(很大)的int
  int 		 b;		/* bool */
} Value;


/*
** Tagged Values
*/

#define TValuefields	Value value; int tt

typedef struct lua_TValue {
  TValuefields;
} TValue;


/* Macros to test type 
* o:是TValue而不是Object类型,这里不是采用CommonHead中的tt来判断类型
*/
#define ttisnil(o)	(ttype(o) == LUA_TNIL)
#define ttisboolean(o)	(ttype(o) == LUA_TBOOLEAN)
#define ttisnumber(o)	(ttype(o) == LUA_TNUMBER)
#define ttislightuserdata(o)	(ttype(o) == LUA_TLIGHTUSERDATA)
#define ttisstring(o)	(ttype(o) == LUA_TSTRING)
#define ttistable(o)	(ttype(o) == LUA_TTABLE)
#define ttisfunction(o)	(ttype(o) == LUA_TFUNCTION)
#define ttisuserdata(o)	(ttype(o) == LUA_TUSERDATA)
#define ttisthread(o)	(ttype(o) == LUA_TTHREAD)

/* Macros to access values */
/*
** 这里的o是TValues类型，而不是Object
**
** !!!!注意这里返回的类型，非gc类型直接返回数值，gc类型，返回对象地址
*/
#define ttype(o)	((o)->tt)
#define gcvalue(o)	check_exp(iscollectable(o), (o)->value.gc)
#define pvalue(o)	check_exp(ttislightuserdata(o), (o)->value.p)
#define bvalue(o)	check_exp(ttisboolean(o), (o)->value.b)
#define nvalue(o)	check_exp(ttisnumber(o), (o)->value.n)

#define rawtsvalue(o)	check_exp(ttisstring(o), &(o)->value.gc->ts)
#define tsvalue(o)	(&rawtsvalue(o)->tsv)
#define rawuvalue(o)	check_exp(ttisuserdata(o), &(o)->value.gc->u)
#define uvalue(o)	(&rawuvalue(o)->uv)
#define clvalue(o)	check_exp(ttisfunction(o), &(o)->value.gc->cl)
#define hvalue(o)	check_exp(ttistable(o), &(o)->value.gc->h)
#define thvalue(o)	check_exp(ttisthread(o), &(o)->value.gc->th)

#define l_isfalse(o)	(ttisnil(o) || (ttisboolean(o) && bvalue(o) == 0))

/*
** for internal debug only
*/
/* 检测类型的一致性 value.type和gc.tt要一致 */
#define checkconsistency(obj) \
  lua_assert(!iscollectable(obj) || (ttype(obj) == (obj)->value.gc->gch.tt))
/* 
** obj若为gc类型则必须可达
** value.tt和gc.gch.tt需一致，obj若不可达，则不可能被赋值
** 
*/
#define checkliveness(g,obj) \
  lua_assert(!iscollectable(obj) || \
  ((ttype(obj) == (obj)->value.gc->gch.tt) && !isdead(g, (obj)->value.gc)))


/* Macros to set values */
#define setnilvalue(obj) ((obj)->tt=LUA_TNIL)

#define setnvalue(obj,x) \
  { TValue *i_o=(obj); i_o->value.n=(x); i_o->tt=LUA_TNUMBER; }

#define setpvalue(obj,x) \
  { TValue *i_o=(obj); i_o->value.p=(x); i_o->tt=LUA_TLIGHTUSERDATA; }

#define setbvalue(obj,x) \
  { TValue *i_o=(obj); i_o->value.b=(x); i_o->tt=LUA_TBOOLEAN; }

#define setsvalue(L,obj,x) \
  { TValue *i_o=(obj); \
    i_o->value.gc=cast(GCObject *, (x)); i_o->tt=LUA_TSTRING; \
    checkliveness(G(L),i_o); }

#define setuvalue(L,obj,x) \
  { TValue *i_o=(obj); \
    i_o->value.gc=cast(GCObject *, (x)); i_o->tt=LUA_TUSERDATA; \
    checkliveness(G(L),i_o); }

#define setthvalue(L,obj,x) \
  { TValue *i_o=(obj); \
    i_o->value.gc=cast(GCObject *, (x)); i_o->tt=LUA_TTHREAD; \
    checkliveness(G(L),i_o); }

#define setclvalue(L,obj,x) \
  { TValue *i_o=(obj); \
    i_o->value.gc=cast(GCObject *, (x)); i_o->tt=LUA_TFUNCTION; \
    checkliveness(G(L),i_o); }

#define sethvalue(L,obj,x) \
  { TValue *i_o=(obj); \
    i_o->value.gc=cast(GCObject *, (x)); i_o->tt=LUA_TTABLE; \
    checkliveness(G(L),i_o); }

#define setptvalue(L,obj,x) \
  { TValue *i_o=(obj); \
    i_o->value.gc=cast(GCObject *, (x)); i_o->tt=LUA_TPROTO; \
    checkliveness(G(L),i_o); }




#define setobj(L,obj1,obj2) \
  { const TValue *o2=(obj2); TValue *o1=(obj1); \
    o1->value = o2->value; o1->tt=o2->tt; \
    checkliveness(G(L),o1); }


/*
** different types of sets, according to destination
*/

/* from stack to (same) stack */
#define setobjs2s	setobj
/* to stack (not from same stack) */
#define setobj2s	setobj
#define setsvalue2s	setsvalue
#define sethvalue2s	sethvalue
#define setptvalue2s	setptvalue
/* from table to same table */
#define setobjt2t	setobj
/* to table */
#define setobj2t	setobj
/* to new object */
#define setobj2n	setobj
#define setsvalue2n	setsvalue
/* 设置TValue的type */
#define setttype(obj, tt) (ttype(obj) = (tt))


#define iscollectable(o)	(ttype(o) >= LUA_TSTRING)



typedef TValue *StkId;  /* index to stack elements */


/*
** String headers for string table
*/
typedef union TString {
  L_Umaxalign dummy;  	/* ensures maximum alignment for strings */
  struct {
    CommonHeader;
    lu_byte reserved;	/* 是否为保留字(eg:语言关键字) ？ */
    unsigned int hash;  /* hash值 */
    size_t len;			/* 不包含lua额外申请的放在数组最后面的\0 */
  } tsv;
} TString;

#define getstr(ts)	cast(const char *, (ts) + 1)
#define svalue(o)       getstr(rawtsvalue(o))


typedef union Udata {
  L_Umaxalign dummy;  /* ensures maximum alignment for `local' udata */
  struct {
    CommonHeader;
    struct Table *metatable;
    struct Table *env;
    size_t len;					/* 负载(load)数据长度 */
  } uv;
} Udata;


/*
** Function Prototypes
** 下面域的排序经过了整理（相关的放在一起），原始代码则是类型相同的放一起(节省MEM考虑)
*/
typedef struct Proto {
  CommonHeader;
  
  TString  *source;		/* 所在源文件的带路径的文件名 */
  int linedefined;		/* 函数起始/结束所在文件行 */
  int lastlinedefined;
  
  Instruction *code;	/* 指向存放指令数组的指针 */
  int sizecode;

  int *lineinfo;  		/* map from opcodes to source lines,   lineinfo[code.idx]->code.fileLine */
  int sizelineinfo;
  
  struct Proto **p;  	/* functions defined inside the function */
  int sizep;  			/* size of `p' */
  
  TValue *k;  			/* constants used by the function */
  int sizek;  			/* size of `k',当前申请的常量数组(*k)的容量, 当前实际使用量在 FuncState.nk                      */

  /* 可参考 FuncState.nlocvars 注释 */
  struct LocVar *locvars;  /* information about local variables */
  int sizelocvars;		   /* 申请的数组的整个大小（once'max?）（当前已被使用的数量在 FuncState nlocvars 域中,当整个数组被使用完毕时，回扩大） */
  
  TString **upvalues;  	/* upvalue names： upvalues的名字的数组 */
  int sizeupvalues;		/* 整个数组的大小(数组内部可能还有部分slot未被使用) */
  lu_byte nups;  		/* number of upvalues:实际使用的数量 */
  
  GCObject *gclist;
  
  lu_byte numparams;	/* 函数原型中定长参数个数 fun(...)->0,funC(a)->1,funA(a,b,...)->2, 如果包含隐式的self参数，那也算一个形参            */
  lu_byte is_vararg;	/* 不定长函数funB(a,...)但凡定义中有...的则是不定长参数，反之则不是 */
  /* 编译过程计算得到：本proto需用到的local'var的数量的最大值
  ** (从第一个型参开始计算(不包含不定参数...因为哪个没法知道确切的数量) 
  ** 实际调用时传给不定参数...的实参在L->func---->L->base之间,数量在OP_VARARG指令中已给出计算公式
  */
  lu_byte maxstacksize;	
} Proto;


/* masks for new-style vararg */
#define VARARG_HASARG		1	/* 兼容旧版本的函数形参...转为名为arg表的格式？ */
#define VARARG_ISVARARG		2	/* 是变参函数 */
#define VARARG_NEEDSARG		4	/* 兼容旧版本的函数形参...转为名为arg表的格式？ */


typedef struct LocVar {
  TString 	*varname;	/* 变量名 */
  int 		startpc;  	/* first point where variable is active */
  int 		endpc;    	/* first point where variable is dead */
} LocVar;



/*
** Upvalues
*/

typedef struct UpVal {
  CommonHeader;
  TValue *v;  		/* points to stack or to its own value */
  union {
    TValue value;  	/* the value (when closed) */
    struct {  		/* double linked list (when open) */
      struct UpVal *prev;
      struct UpVal *next;
    } l;			/* l:list */
  } u;
} UpVal;


/*
** Closures
** env：环境变量(全局环境？）的指针
** isC: 1：C函数， 0：Lua函数
*/

#define ClosureHeader \
	CommonHeader; lu_byte isC; lu_byte nupvalues; GCObject *gclist; \
	struct Table *env

typedef struct CClosure {
  ClosureHeader;
  lua_CFunction f;
  TValue 		upvalue[1];
} CClosure;


typedef struct LClosure {
  ClosureHeader;
  struct Proto 	*p;
  UpVal 		*upvals[1];
} LClosure;


typedef union Closure {
  CClosure c;
  LClosure l;
} Closure;


#define iscfunction(o)	(ttype(o) == LUA_TFUNCTION && clvalue(o)->c.isC)
#define isLfunction(o)	(ttype(o) == LUA_TFUNCTION && !clvalue(o)->c.isC)


/*
** Tables
*/

typedef union TKey {
  struct {
    TValuefields;		/* 这里不能简单的用TValue替代，因为TValue已经是最顶层的Value表现形式了。不能再和XXX混合形成更高层次的Value */
    struct Node *next;  /* for chaining */
  } nk;
  TValue tvk;			/* tvk域起到了上面nk.val的作用,方便某些情况下的代码编写，算是理想和现实的折中吧 */
} TKey;


typedef struct Node {
  TValue i_val;
  TKey 	 i_key;
} Node;


typedef struct Table {
  CommonHeader;
  struct Table 	*metatable;
  lu_byte 		flags;  		/* 1<<p means tagmethod(p) is not present */ 
  Node 			*node;
  lu_byte 		lsizenode;  	/* log2 of size of `node' array */
  Node 			*lastfree; 		/* any free position is before this position */
  TValue 		*array;  		/* array part */
  int 			sizearray;  	/* size of `array' array */
  GCObject 		*gclist;		/* gc过程中用到，比如当前自己在gray链表中，则指向下一个gray'obj */
} Table;



/*
** `module' operation for hashing (size is always a power of 2)
*/
#define lmod(s,size) \
	(check_exp((size&(size-1))==0, (cast(int, (s) & ((size)-1)))))


#define twoto(x)	(1<<(x))
#define sizenode(t)	(twoto((t)->lsizenode))


#define luaO_nilobject		(&luaO_nilobject_)

LUAI_DATA const TValue luaO_nilobject_;

#define ceillog2(x)	(luaO_log2((x)-1) + 1)

LUAI_FUNC int luaO_log2 (unsigned int x);
LUAI_FUNC int luaO_int2fb (unsigned int x);
LUAI_FUNC int luaO_fb2int (int x);
LUAI_FUNC int luaO_rawequalObj (const TValue *t1, const TValue *t2);
LUAI_FUNC int luaO_str2d (const char *s, lua_Number *result);
LUAI_FUNC const char *luaO_pushvfstring (lua_State *L, const char *fmt,
                                                       va_list argp);
LUAI_FUNC const char *luaO_pushfstring (lua_State *L, const char *fmt, ...);
LUAI_FUNC void luaO_chunkid (char *out, const char *source, size_t len);


#endif

