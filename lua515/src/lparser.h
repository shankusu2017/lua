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


/*
** Expression descriptor
*/

typedef enum {
  VVOID,		/* no value */
  VNIL,
  VTRUE,
  VFALSE,
  VK,			/* info = index of constant in `k' */
  VKNUM,		/* nval = numerical value */
  VLOCAL,		/* info = local register */
  VUPVAL,   	/* info = index of upvalue in `upvalues' */
  VGLOBAL,		/* info = index of table; aux = index of global name in `k' */
  VINDEXED,		/* info = table register; aux = index register (or `k') */
  VJMP,			/* info = instruction pc */
  VRELOCABLE,	/* info = instruction pc */
  VNONRELOC,	/* info = result register */
  VCALL,		/* info = instruction pc */
  VVARARG		/* info = instruction pc */
} expkind;

typedef struct expdesc {
  expkind k;	/* 表达式类型 */
  union {
    struct { int info, aux; } s;	/* 随着k不同,info,aux表示的意义随之变化,具体看expkind的注释 */
    lua_Number nval;				/* 数值表达式的数值 */
  } u;
  int t;  /* patch list of `exit when true' */
  int f;  /* patch list of `exit when false' */
} expdesc;


typedef struct upvaldesc {
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
  
  Table *h;  			/* table to find (and reuse) elements in `k' */
  struct BlockCnt *bl;  /* chain of current blocks */
  int pc;  				/* next position to code (equivalent to `ncode') */
  int lasttarget;   	/* `pc' of last `jump target' */
  int jpc;  			/* list of pending jumps to `pc' */

  /* 第一个可用的reg的索引，随着locvar的申请和释放，这个值不断变化 */
  int freereg;  		/* first free register */
  
  int nk;  				/* number of elements in `k' */
  int np;  				/* number of elements in `p' */


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
  
  /* number of active local variables：当前激活的var数量
  ** 对于上面的nlocvars第二次声明local时，nactvar:从1->3,因为离开第一个块后，块所属的locvar被释放了（变量的声明周期也结束了）
  */
  lu_byte nactvar;  					

  upvaldesc upvalues[LUAI_MAXUPVALUES];  /* upvalues */
} FuncState;


LUAI_FUNC Proto *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                            const char *name);


#endif
