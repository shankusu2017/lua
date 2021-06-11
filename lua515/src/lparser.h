/*
** $Id: lparser.h,v 1.57.1.1 2007/12/27 13:02:25 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/**************************** 官方的BNF **********************************
chunk ::= {stat [`;´]} [laststat [`;´]]

block ::= chunk

stat ::=  varlist `=´ explist | 
	 functioncall | 
	 do block end | 
	 while exp do block end | 
	 repeat block until exp | 
	 if exp then block {elseif exp then block} [else block] end | 
	 for Name `=´ exp `,´ exp [`,´ exp] do block end | 
	 for namelist in explist do block end | 
	 function funcname funcbody | 
	 local function Name funcbody | 
	 local namelist [`=´ explist] 

laststat ::= return [explist] | break

funcname ::= Name {`.´ Name} [`:´ Name]

varlist ::= var {`,´ var}

var ::=  Name | prefixexp `[´ exp `]´ | prefixexp `.´ Name 

namelist ::= Name {`,´ Name}

explist ::= {exp `,´} exp

exp ::=  nil | false | true | Number | String | `...´ | function | 
	 prefixexp | tableconstructor | exp binop exp | unop exp 

prefixexp ::= var | functioncall | `(´ exp `)´

functioncall ::=  prefixexp args | prefixexp `:´ Name args 

args ::=  `(´ [explist] `)´ | tableconstructor | String 

function ::= function funcbody

funcbody ::= `(´ [parlist] `)´ block end

parlist ::= namelist [`,´ `...´] | `...´

tableconstructor ::= `{´ [fieldlist] `}´

fieldlist ::= field {fieldsep field} [fieldsep]

field ::= `[´ exp `]´ `=´ exp | Name `=´ exp | exp

fieldsep ::= `,´ | `;´

binop ::= `+´ | `-´ | `*´ | `/´ | `^´ | `%´ | `..´ | 
	 `<´ | `<=´ | `>´ | `>=´ | `==´ | `~=´ | 
	 and | or

unop ::= `-´ | not | `#´

**************************************************************************/



/**************************** 自己总结的BNF **********************************
chunk ::= {stat [`;´]}

block ::= chunk

stat ::=  
	 ifstat | 
	 whilestat | 
	 DO block END | 
	 forstat | 
	 repeat | 
	 funcstat | 
	 localstat |
	 retstat |
	 breaksat |
	 exprstat

localstat  	::= local fun | localstat‘
localstat' 	::= LOCAL NAME {`,´ NAME } [`=´ explist1]

explist1   	::= expr { `,' expr }

expr 		::= subexpr

subexpr 	::= (simpleexp | unop subexpr) { binop subexpr }

simpleexp 	::= NUMBER | STRING | NIL | true | false | ... |
                  constructor | FUNCTION body | primaryexp

primaryexp 	::= prefixexp { `.' NAME | `[' expr `]' | `:' NAME funcargs | funcargs }

prefixexp 	::= NAME | '(' expr ')'

funcargs 	::= `(' [ explist1 ] `)' | constructor | STRING

exprstat    := func | assignment

assignment 	::= `,' primaryexp assignment |  `=' explist1 

ifstat 		::= IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END

cond 		::= expr

test_then_block :: [IF | ELSEIF] cond THEN block

whilestat 	::= WHILE cond DO block END

repeatstat 	:: REPEAT block UNTIL cond

forstat 	::= FOR (fornum | forlist) END

fornum 		::= NAME = exp1,exp1[,exp1] forbody

forlist 	::= NAME {,NAME} IN explist1 forbody

forbody 	::= DO block

funcstat 	::= FUNCTION funcname body

funcname 	::= NAME {field} [`:' NAME]

field 		::= ['.' | ':'] NAME 

body 		::=  `(' parlist `)' chunk END

parlist 	:: [ param { `,' param } ] 
param 		::= NAME | ...

constructor ::= {recfield|listfield}

recfield 	::= (NAME | `['exp1`]') = exp1

listfield   ::= exp1

exp1        ::= (exp)   ?


retstat ::= RETURN explist

**************************************************************************/

/*
** Expression descriptor
** 表达式的"类型"
** VNONRELOC, VRELOCABLE表示表达式的reg信息(已被安排到指定的reg或可以重定位到任一reg)
** 其它的类型：表达式的类型 相关函数 luaK_dischargevars
*/

typedef enum {
  VVOID,		/* no value:表示表达式尚未进行评估，也可能表达式就是空 */
  	
  VNIL,
  VTRUE,		/* true */
  VFALSE,		/* fales */
  VK,			/* info = index of constant in `k' 常量表达式 */
  VKNUM,		/* nval = numerical value */

  
  VLOCAL,		/* info = local register */
  VUPVAL,   	/* info = index of upvalue in `upvalues' */
  VGLOBAL,		/* info = index of table; aux = index of global name in `k' */

  /* 索引表达式 eg: tbl(info).aux(aux) */
  VINDEXED,		/* info = table register; aux = index register (or `k') */

  /* 跳转表达式，常用于关系表达式 */
  VJMP,			/* info = instruction pc */

  /* 表达式尚未加载到reg（目标reg尚未确定，可以放在栈的任意位置，只要能访问到）
  ** info:本指令在指令数组中的索引，方便后面回填本指令的目标地址寄存器(R(A))
  */
  VRELOCABLE,	/* info = instruction pc */

  /* 表达式的值已被加载到reg中了，info:对应寄存器的idx */
  VNONRELOC,	/* info = result register */
  
  VCALL,		/* info = instruction pc: info表示exp对应的指令在指令f的指令数组中的下标，下同 */
  VVARARG		/* info = instruction pc   ... 变参操作符 */
} expkind;

typedef struct expdesc {
  expkind k;	/* 表达式类型，后面会更新为表达式占用寄存器的类型 */
  union {
    struct { int info, aux; } s;	/* 随着k不同,info,aux表示的意义随之变化,具体看expkind的注释 */
    lua_Number nval;				/* 数值表达式的数值 */
  } u;
  int t;  /* patch list of `exit when true' */
  int f;  /* patch list of `exit when false' */
} expdesc;


typedef struct upvaldesc {
  /* VLOCAL则表示在父closure的actvar中找到，可以用OP_MOVE指令
  ** VUPVAL 则需OP_GETUPVAL指令
  */
  lu_byte k;
  
  lu_byte info;
} upvaldesc;


struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function 
** 编译阶段的func状态机，成品则是Proto
*/
typedef struct FuncState {
  struct lua_State *L;  /* copy of the Lua state */
  
  struct FuncState *prev;  /* enclosing function,先编译完子函数，才能编译父函数 */

  struct LexState *ls;  /* lexical state */

  Proto *f;  			/* current function header */
  
  struct BlockCnt *bl;  /* chain of current blocks */
  int pc;  				/* 指向:下一个待生成的指令 next position to code (equivalent to `ncode') */
  int lasttarget;   	/* `pc' of last `jump target' */
  int jpc;  			/* list of pending jumps to `pc'：指向下一个待生成的指令的待回填的跳转链表 */


  /* 存储常量kvar在对应的 Proto.k 常量数组中的下标的映射表 
  **
  ** local var= "hello" , 常量 "hello" 存在Proto.k常量数组中的第0个位置处
  ** h["hello"] = 0
  ** 查找常量过程如下 k="hello", 进入h表查找,找到v(0), 再用v到Proto.k中取值
  ** 参考函数 lcode.c addk
  */
  Table *h;  			/* table to find (and reuse) elements in `k' */
  int nk;  				/* number of elements in `k' */
  
  int np;  				/* number of elements in `p' */


  /* 第一个可用的reg的索引，随着locvar的申请和释放，这个值不断变化
  ** 随着编译过程中临时占用寄存器的申请和释放，这个值也在不断变化
  **  local a = b + c 计算完b+c的结果要存放到一个临时寄存器中，赋值给a后，这个寄存器要释放
  ** 由于locvar的存在需要"始终"占用一个reg，所以freereg>=nactvar
  */
  int freereg;  		/* first free register */

  /* 
  ** 当前函数已使用的本地变量总和(下面的总和为6)，整个数组大小的定义在 Proto 的sizelocvars域中 
  **
  ** do
  **   	local v1, v2, v3
  ** end
  ** do
  **	local v1, v2, v3
  ** end
  ** nlocvars:从1->6, 这样每一个locvar都有一个唯一的 Proto.locvars中对应的信息，
  **    至于运行到某一行时，v1到底指代哪一个v1,可以从startpc,endpc中推断出来（调试库也是靠pc推断的哦？）
  */
  short nlocvars;  		/* number of elements in `locvars' */

  /*
  ** declared-variable stack
  ** 当前激活的var的idx到f.locvars的映射 
  */
  unsigned short actvar[LUAI_MAXVARS];
  
  /* number of active local variables：当前激活中的locvar数量
  ** 对于上面的nlocvars第二次声明local时，nactvar:从1->3,因为离开第一个块后，块所属的locvar被释放了（变量的声明周期也结束了）
  */
  lu_byte nactvar;  					

  upvaldesc upvalues[LUAI_MAXUPVALUES];  /* upvalues */
} FuncState;


LUAI_FUNC Proto *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                            const char *name);


#endif
