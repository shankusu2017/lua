/*
** $Id: lparser.h,v 1.74 2014/10/25 11:50:46 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/*
** Expression descriptor 表达式描述符
*/

typedef enum {
	/* no value
	** local var1, var2  这种声明就是这种情况，因为后面没有给变量赋值，类似于C的void的了
  ** local a = function() end  这种，函数参数列表也为空
	*/
  VVOID,			
		
  VNIL,
  VTRUE,
  VFALSE,
  
  VK,				  /* info = index of constant in 'k' */

  VKFLT,			/* nval = numerical float value */
  VKINT,			/* nval = numerical integer value */
  
  /* 表达式已经生成了指令，并且已经分配了寄存器保存这个值 */
  VNONRELOC,	/* info = result register */
  
  VLOCAL,			/* info = local register */

  /* 需要产生指令OP_GETUPVALl来获取其值 */
  VUPVAL,     /* info = index of upvalue in 'upvalues' */

  /* 根据vt的不同，需要产生OP_GETTABLE或者OP_GETTABUP指令来获取其值 */
  VINDEXED,		/* t = table register/upvalue; idx = index R/K */
  
  VJMP,				/* info = instruction pc */

  /* VUPVAL和VINDEXED都被转化为VRELOCABLE类型，表示获取指令已经生成，
  ** 但是指令的目标寄存器(A)还没有确定，等待回填。回填后，VRELOCABLE类型会转化成VNONRELOC类型。
  */
  VRELOCABLE, /* info = instruction pc */

  VCALL,			/* info = instruction pc */
  VVARARG			/* info = instruction pc */
} expkind;


#define vkisvar(k)	(VLOCAL <= (k) && (k) <= VINDEXED)
#define vkisinreg(k)	((k) == VNONRELOC || (k) == VLOCAL)

typedef struct expdesc {
  /* 表达式的类型 eg: VNIL, VK ... 
  ** 类型在变，指令可能跟着变(指令对应的操作数的属性(位置...)也不断在变)
  */
  expkind k;  

  union {
    struct {        /* for indexed variables (VINDEXED) */
      short idx;  	/* index (R/K) */
      lu_byte t;  	/* table (register or upvalue) */
      lu_byte vt;  	/* whether 't' is register (VLOCAL) or upvalue (VUPVAL) */
    } ind;	/* ind->index */
		
    /*
    ** k==VK时，表示Proto.k中的索引
    */
    int info;  					/* for generic use：意如前面注释 */

    lua_Number nval; 		/* for VKFLT，存储表达式的浮点值或整数值 */
    lua_Integer ival;   /* for VKINT */
  } u;
	
  /*
  ** 这两个patch list代表本表达式被评估为true或者false时的跳出指令列表。
  ** 通过将一个地址回填给patch list，就将对这个表达式的评估直接引导到对应的执行分支。
  ** 任何类型的表达式都可能带有patch list。如果有patch list，说明这个表达式本身或者子表达式使用了关系或者逻辑表达式。
  */
  int t;  /* patch list of 'exit when true' */
  int f;  /* patch list of 'exit when false' */
} expdesc;


/* description of active local variable */
typedef struct Vardesc {
  short idx;  /* variable index in stack */
} Vardesc;


/* description of pending goto statements and label statements */
typedef struct Labeldesc {
  TString *name;  		/* label identifier */
  int pc;  			    /* position in code */
  int line;  		    /* line where it appeared */
  lu_byte nactvar;  	/* local level where it appears in current block */
} Labeldesc;

/* list of labels or gotos */
typedef struct Labellist {
  Labeldesc *arr;  	/* array */
  int size;         /* array size */
  int n; 			/* number of entries(条目) in use */
} Labellist;


/* dynamic structures used by the parser */
typedef struct Dyndata {
  struct {  /* list of active local variables */
    Vardesc *arr;	/* buf地址 */		
    int size;			/* 能容纳的element总长 */
    int n;				/* 当前已使用个数 */
  } actvar;
  Labellist gt;  		/* list of pending gotos */
  Labellist label;  /* list of active labels */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate(生成) code for a given function(给定功能) */
typedef struct FuncState {
  Proto *f;         /* current function header */

  /*
  ** local function a(...)
  **    local function b(...)
  **        local function c(...)
  **        end
  **    end
  ** end
  ** c->prev=b
  ** b->prev=a
  ** a->prev=mainF
  */
  struct FuncState 	*prev;   /* enclosing(封闭) function */

  /* lexical state 全局的分析器 */
  struct LexState 	*ls;

  /*
  ** chain of current blocks
  ** 嵌套的作用域(代码块)
  */
  struct BlockCnt 	*bl;      

  /* Program Counter */
  int pc;           /* next position to code (equivalent to 'ncode') */
  
  int lasttarget;   /* 'label' of last 'jump label' */

  /* 待修改的跳转指令的List */
  int jpc;          /* list of pending jumps to 'pc' */

  int nk;           /* number of elements in 'k' f->k */
  
  int np;           /* number of elements in 'p' */

  int firstlocal;   /* index of first local var (in Dyndata array) */
  short nlocvars;   /* number of elements in 'f->locvars' */

  lu_byte nactvar;  /* number of active local variables */
  
  lu_byte nups;     /* number of upvalues */
	
	/* 当前空闲寄存器的起始id */
  lu_byte freereg;  /* first free register */
} FuncState;


LUAI_FUNC LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                 Dyndata *dyd, const char *name, int firstchar);


#endif
