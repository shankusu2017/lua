/*
** $Id: lcode.c,v 2.25.1.5 2011/01/31 14:53:16 roberto Exp $
** Code generator for Lua
** See Copyright Notice in lua.h
*/


#include <stdlib.h>

#include <stdio.h>

#define lcode_c
#define LUA_CORE

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "ltable.h"

/* 这个判断条件有意哈 */
#define hasjumps(e)	((e)->t != (e)->f)

/* TODOKNOW */
static int isnumeral(expdesc *e) {
  return (e->k == VKNUM &&	/* 仅仅e->k == VKNUM 不够么？ */
		  	e->t == NO_JUMP &&
		  	e->f == NO_JUMP);
}

/* 给连续的变量赋nil
** OP_LOADNIL A B R(A) := ... := R(B) := nil
** 当可以合并前一条OP_LOADNIL时则尝试合并，可以利用fun'stack的默认NIL时，直接用NIL
*/
void luaK_nil (FuncState *fs, int from, int n) {
  Instruction *previous;
  if (fs->pc > fs->lasttarget) {  /* no jumps to current position? */
    if (fs->pc == 0) {  /* function start? */
	  /* 新调用一个fun时，其私有stack默认会被置NULL，这种情况直接使用默认的NULL即可 ldo.c 中的 luaD_precall 函数保证了这一点-
      ** 函数运行过程中stack可能会有残留的数据，不能认为残留下来的数值是NIL（编译器不保证这一点）
	  */
      if (from >= fs->nactvar) 
        return;  /* positions are already clean */
    }
    else {
      previous = &fs->f->code[fs->pc-1];
      if (GET_OPCODE(*previous) == OP_LOADNIL) {	/* 尝试合并前后连续的OP_LOADNIL指令 */
        int pfrom = GETARG_A(*previous);
        int pto = GETARG_B(*previous);
        if (pfrom <= from && from <= pto+1) {  /* can connect both? */
          if (from+n-1 > pto)
            SETARG_B(*previous, from+n-1);
          return;
        }
      }
    }
  }
  luaK_codeABC(fs, OP_LOADNIL, from, from+n-1, 0);  /* else no optimization */
}

/* 
** 构建一条jmp指令，将fs->jpc挂到新构建的jmp指令的尾上
** 组成：  j(new.jmp)->jpc.next->jpc2.next->jpc3.next(NULL) fs->jpc=NULL 
**
** RETURNS j(new.jump)
**
** 待回填的跳转链表指向我，而我又指向其它pc，那么将上述链表和我串联在一起即可
**
** OP_JMP sBx		pc+=sBx
*/
int luaK_jump (FuncState *fs) {
  int jpc = fs->jpc;  	/* save list of jumps to here */
  int j;
  fs->jpc = NO_JUMP;	/* fs->jpc挂到新生成的jmp上，这里需清空fs->jpc */
  j = luaK_codeAsBx(fs, OP_JMP, 0, NO_JUMP);	/* OP_JMP sBx PC += sBx */
  luaK_concat(fs, &j, jpc);  	/* keep them on hold */
  return j;						/* 返回给上层，不然j就丢失了 */
}

/* 从函数返回
** OP_RETURN A B return R(A), ... ,R(A+B-2)
*/
void luaK_ret (FuncState *fs, int first, int nret) {
  luaK_codeABC(fs, OP_RETURN, first, nret+1, 0);	/* 这里可以反推OP_RETURNS中A,B,C的含义了 */
}

/* 条件跳转
** OP_TEST 	 		 A C	if not (R(A) <=> C) then pc++ 
** OP_TESTSET 		 A B C	if (R(B) <=> C) then R(A) := R(B) else pc++
** OP_EQ 			 A B C	if ((RK(B) == RK(C)) ~= A) then pc++
*/
static int condjump (FuncState *fs, OpCode op, int A, int B, int C) {
  luaK_codeABC(fs, op, A, B, C);
  return luaK_jump(fs);
}

/* 将待回填的跳转指令pc指向dest */
static void fixjump (FuncState *fs, int pc, int dest) {
  Instruction *jmp = &fs->f->code[pc];
  /* 
  ** 虚拟机执行过程中执行某条指令i时,pc寄存器已经指向了下一条待执行的指令，故而在计算offset需要
  ** -1, 等效于offset=dest-pc-1
  */
  int offset = dest-(pc+1);		
  lua_assert(dest != NO_JUMP);
  if (abs(offset) > MAXARG_sBx)
    luaX_syntaxerror(fs->ls, "control structure too long");
  SETARG_sBx(*jmp, offset);
}


/*
** returns current `pc' and marks it as a jump target (to avoid wrong
** optimizations(优化) with consecutive(连续) instructions not in the same basic block).
** 有一条跳转指令需要指向下一条待生成的指令，这里返回下一条待生成的指令地址并返回
**   方便回填跳转指令
**   eg WHILE cond DO body END
**     在cond开始的地方构建一个whileinit标记，等待body解析完毕后，生成一条跳转到whileinit
**     的跳转指令，已实现while的循环逻辑
*/
int luaK_getlabel (FuncState *fs) {
  fs->lasttarget = fs->pc;
  return fs->pc;
}

/* 获取跳转指令指向的绝对位置 */
static int getjump (FuncState *fs, int pc) {
  int offset = GETARG_sBx(fs->f->code[pc]);
  if (offset == NO_JUMP)  /* point to itself represents end of list */
    return NO_JUMP;  /* end of list */
  else
    return (pc+1)+offset;  /* turn offset into absolute position */
}

/* 尝试获取跳转指令前的控制指令 eg: OP_EQ, OP_TESTSET
** testTMode测试成立，意味着pc指向的是后面的OP_JUMP指令，这里返回前面的控制指令
*/
static Instruction *getjumpcontrol (FuncState *fs, int pc) {
  Instruction *pi = &fs->f->code[pc];
  /* 参考 jumponcond 函数，这里需往后退一格子 */
  if (pc >= 1 && testTMode(GET_OPCODE(*(pi-1)))) {
  	PrintOneCode(*(pi-1));
    return pi-1;
  } else {
  	PrintOneCode(*pi);
    return pi;
  }
}


/*
** check whether list has any jump that do not produce a value
** (or produce an inverted(颠,倒) value)
*/
static int need_value (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list)) {
    Instruction i = *getjumpcontrol(fs, list);
	/*
	** OP_TESTST : if (R(B) <=> C) then R(A) := R(B) else pc++
	**    指令中的 R(A):=R(B)，已经给reg赋值了
	** OP_TEST(if not (R(A) <=> C) then pc++), OP_LT 等关系指令仅判断了cond，
	** 并没有实际的赋值操作
	**
	** 结合调用的前提(在exp2reg()函数中)，这里判断不等于OP_TESTSET即可
	** 常用在  local a = b >c,   local a = b or c这种只生成了跳转指令没有生存赋值指令
	**     但又需要赋值的上下文中
	*/
    if (GET_OPCODE(i) != OP_TESTSET)
		return 1;
  }
  return 0;  /* not found */
}

/* 继续处理 jumponcond 函数生成的 OP_TESTSET 逻辑跳转指令中的R(A)
** OP_TEST,		A C		if not (R(A) <=> C) then pc++			
** OP_TESTSET,	A B C	if (R(B) <=> C) then R(A) := R(B) else pc++
*/
static int patchtestreg (FuncState *fs, int node, int reg) {
  Instruction *i = getjumpcontrol(fs, node);
  if (GET_OPCODE(*i) != OP_TESTSET)
    return 0;  /* cannot patch other instructions */

  /* local a = b and c, 需将判断逻辑的结果赋值给某个reg的情况 */
  if (reg != NO_REG && reg != GETARG_B(*i)) {
    SETARG_A(*i, reg);
  } else { 
    /* no register to put value or register already has the value 
    ** 
  	** 不另外保存R(B)的情况下，对指令进行优化
  	** local a = a and b 
  	** if (a and b) then block end
    ** if Done then block end  Done里面的跳转指令也需要优化
    */
    *i = CREATE_ABC(OP_TEST, GETARG_B(*i), 0, GETARG_C(*i));
  }

  return 1;
}

/* 优化跳转链表中的OP_TESTSET指令的R(A)             */
static void removevalues (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list))
      patchtestreg(fs, list, NO_REG);	//  NO_REG指示函数，优化OP_TESTSET到OP_TEST
}

/* 
** 回填跳转指令链表上的指令到指定目标
**
** 将待回填跳转指令列表list上指令的跳转参数sBx更新到target上 
*/
static void patchlistaux (FuncState *fs, int list, int vtarget, int reg,
                          int dtarget) {
  while (list != NO_JUMP) {
    int next = getjump(fs, list);
    if (patchtestreg(fs, list, reg))
      fixjump(fs, list, vtarget);
    else
      fixjump(fs, list, dtarget);  /* jump to default target */
    list = next;
  }
}

/* 将待回填的跳转到当前指令的跳转链表上的跳转指令的sBx更新为fs->pc */
static void dischargejpc (FuncState *fs) {
  patchlistaux(fs, fs->jpc, fs->pc, NO_REG, fs->pc);
  fs->jpc = NO_JUMP;	/* 置空 */
}

/* 将待回填的跳转指令列表list(可能是单个元素的链表)挂到fs->jpc链表末尾
** 或者将list中的所有待回填的指令指向跳转目标target(已生成对应的指令，不是上面哪种尚未生成的下一条指令)
*/
void luaK_patchlist (FuncState *fs, int list, int target) {
  /* 如果跳转目标target是下一条待生成的指令地址，则直接将待回填的跳转指令链表挂到fs->jpc上 */
  if (target == fs->pc)
    luaK_patchtohere(fs, list);
  else {
    lua_assert(target < fs->pc);
    patchlistaux(fs, list, target, NO_REG, target);
  }
}

/* 将待回填的跳转指令链表list挂到fs->jpc,等生成下一条指令时回填sBx */
void luaK_patchtohere (FuncState *fs, int list) {
  luaK_getlabel(fs);
  luaK_concat(fs, &fs->jpc, list);
}

/*  l1.next->next->next.sBx = l2 
** 将l2指向的待回填跳转指令/指令链表挂到l1的跳转链表尾上
*/
void luaK_concat (FuncState *fs, int *l1, int l2) {
  if (l2 == NO_JUMP) /* l2不是一条跳转指令，直接返回 */
  	return;
  else if (*l1 == NO_JUMP)	/* 当前跳转列表为空 */
    *l1 = l2;	/* l1尚未初始化，直接赋值即可 */
  else {
    int list = *l1;
    int next;
    while ((next = getjump(fs, list)) != NO_JUMP)  /* find last element */
      list = next;
    fixjump(fs, list, l2);	/* 将待回填的跳转指令链表l2挂到l1的末尾 */
  }
}

/* 调整maxstacksize以便匹配locvar的数量 */
void luaK_checkstack (FuncState *fs, int n) {
  int newstack = fs->freereg + n;
  if (newstack > fs->f->maxstacksize) {	/* 这个判断是必须的 */
    if (newstack >= MAXSTACK)
      luaX_syntaxerror(fs->ls, "function or expression too complex");
    fs->f->maxstacksize = cast_byte(newstack);
  }
}

/* reserve reg:预定 寄存器 实际上是占用n个寄存器的意思 */
void luaK_reserveregs (FuncState *fs, int n) {
  luaK_checkstack(fs, n);
  fs->freereg += n;	/* 占用n个locvar,释放则n为负值或在其它函数中实现 */
}

/* 重点函数，需要细读 */
static void freereg (FuncState *fs, int reg) {
  if (!ISK(reg) &&            /* 常量的就不用释放了，压根没占用reg */
      reg >= fs->nactvar) {   /* reg从0开始，nactvar从1开始，所以这里reg>=fs->nactvar是可以的
      
  	/* 释放一个reg后,reg==fs->freereg:确保只能释放最新一个被激活的reg(作为exp的临时reg占用...) */
    fs->freereg--;
    lua_assert(reg == fs->freereg);
  }
}

/* 释放被临时占用的reg
** local a = b + c + d  编译+d之前整合b+c的表达式，释放一个reg
 */
static void freeexp (FuncState *fs, expdesc *e) {
  if (e->k == VNONRELOC)		/* 表达式的值在reg中才释放 (还没加载到reg，那压根没占用reg，释放个锤子*/
    freereg(fs, e->u.s.info);	/* VNONRELOC info = result register */ 
}

/*
** 将常量加载到fs->f的常量表中
**
** local var = "hello
" 则本函数的k,v="hello" 
*/
static int addk (FuncState *fs, TValue *k, TValue *v) {
  lua_State *L = fs->L;
  TValue *idx = luaH_set(L, fs->h, k);
  Proto *f = fs->f;
  int oldsize = f->sizek;
  if (ttisnumber(idx)) {
    lua_assert(luaO_rawequalObj(&fs->f->k[cast_int(nvalue(idx))], v));	
    return cast_int(nvalue(idx));
  }
  else {  /* constant not found; create a new entry */
    setnvalue(idx, cast_num(fs->nk));
    luaM_growvector(L, f->k, fs->nk, f->sizek, TValue,
                    MAXARG_Bx, "constant table overflow");
    while (oldsize < f->sizek) setnilvalue(&f->k[oldsize++]);
    setobj(L, &f->k[fs->nk], v);
    luaC_barrier(L, f, v);
    return fs->nk++;
  }
}

/* 将字符串常量加载到fs->f的常量表中 */
int luaK_stringK (FuncState *fs, TString *s) {
  TValue o;
  setsvalue(fs->L, &o, s);
  return addk(fs, &o, &o);
}


int luaK_numberK (FuncState *fs, lua_Number r) {
  TValue o;
  setnvalue(&o, r);
  return addk(fs, &o, &o);
}


static int boolK (FuncState *fs, int b) {
  TValue o;
  setbvalue(&o, b);
  return addk(fs, &o, &o);
}


static int nilK (FuncState *fs) {
  TValue k, v;
  setnilvalue(&v);
  /* cannot use nil as key; instead use table itself to represent nil */
  sethvalue(fs->L, &k, fs->h);
  return addk(fs, &k, &v);
}

/* nresults:-1, C=0，希望返回变参
** nresults: 0, C=1, 希望返回0个参数
** nresults: 1, C=2, 希望返回1个参数
** nresults: 2, C=3, 希望返回2个参数
*/
void luaK_setreturns (FuncState *fs, expdesc *e, int nresults) {
  /* OP_CALL A B C 		R(A), … ,R(A+C-2) := R(A)(R(A+1), … ,R(A+B-1)) */
  if (e->k == VCALL) {  /* expression is an open function call? */
    SETARG_C(getcode(fs, e), nresults+1);	/* Bx是无符号数，[0,+], lparser中的是[-1,+],故而这里要+1 */
  }
  else if (e->k == VVARARG) {
  	/* OP_VARARG A B 	R(A), R(A+1), ..., R(A+B-1) = vararg 
  	** 将变参拷贝到RA指定的寄存器开始的地方，拷贝B个元素，这里仅使用了一个寄存器的编码?
  	*/
    SETARG_B(getcode(fs, e), nresults+1);
    SETARG_A(getcode(fs, e), fs->freereg);
    luaK_reserveregs(fs, 1);
  }
}

/* 对于可能返回变参的表达式，强制其仅返回一个值 */
void luaK_setoneret (FuncState *fs, expdesc *e) {
  if (e->k == VCALL) {  /* expression is an open function call? */
  	/* A B C R(A), … ,R(A+C-2) := R(A)(R(A+1), … ,R(A+B-1)) */
  
  	/* 
  	** 在解析funcarg表达式完毕后，OP_CALL指令已生成，A,B,C三个参数也填充完毕(A参数在解析完函数名后即确定了),
  	** 根据OP_CALL的含义“整个表达式”在寄存器中的reg.addr就是R(A),理解这一点就好立即luaK_setoneret函数了
  	**
  	** 函数调用完毕后，函数调用作为一个表达式整体，返回一个值，占用函数句柄原本占用的reg
  	** 函数调用返回的第一个值占用的reg就是函数指针本身占用的reg
  	** 同时e.u.s.info = R(A)也是基于这个道理
  	*/
    e->k = VNONRELOC;	/* 表达式的reg.addr已确定，故而是VNONRELOC */
    e->u.s.info = GETARG_A(getcode(fs, e));
  } else if (e->k == VVARARG) {
    SETARG_B(getcode(fs, e), 2);	/* 2:期待返回一个返回值 */
    e->k = VRELOCABLE;  /* can relocate its simple result */
  }
}


/* 
** GET_XXX 生成对间接表达式的求值指令 (VCALL, VARGVAR这里强制返回1个值, 需返回多个值的在上层业务中进行修正) 
**
** cond.1 对"直接表达式"(VNIL,VTRUE,VFALSE,VKNUM,VK)，不做处理
**            (直接表达式可以一步生成load_xxx指令到目的寄存器)
**
** cond.2 对"值已在reg数组中的表达式"(VLOCAL, VCALL), e->k = VNONRELOC, 
**             表示其值已经在reg数组中了，后续可以直接用e->u.s.info取其地址
**
** cond.3 对"间接表达式"(VINDEXED, VGLOBAL, VUPVAL)，生成取值指令get_xxx, e->k = VRELOCABLE
**			   表示其值已计算完毕，等待回填其目标寄存器
**
** cond.4 VARGVAR表达式的值已在reg中，等待回填目标寄存器 e->k = VRELOCABLE
*/
void luaK_dischargevars (FuncState *fs, expdesc *e) {
  switch (e->k) {
    case VLOCAL: {	/* exp.src已在reg中，故而这里是VNONRELOC */
      e->k = VNONRELOC;
      break;
    }
    case VUPVAL: {
      e->u.s.info = luaK_codeABC(fs, OP_GETUPVAL, 0, e->u.s.info, 0);
      e->k = VRELOCABLE;
      break;
    }
    case VGLOBAL: {
      e->u.s.info = luaK_codeABx(fs, OP_GETGLOBAL, 0, e->u.s.info);
      e->k = VRELOCABLE;
      break;
    }
    case VINDEXED: {	/* OP_GETTABLE A B C R(A) := R(B)[RK(C)] */
	  /* !!这里是依次释放的
	  ** eg.1：a.b.c.d.e... 释放a.b.c.d之前占用的reg,以便重利用reg
	  ** eg.2: local a = b.c, (b,c作为upval加载到reg时占用了reg，右边作为一个整体，释放b,c占用的reg)
	  */
      freereg(fs, e->u.s.aux);
      freereg(fs, e->u.s.info);
	
	  /* A填 0，配合下面的可重定位VRELOCALBLE */
      e->u.s.info = luaK_codeABC(fs, OP_GETTABLE, 0, e->u.s.info, e->u.s.aux);
      e->k = VRELOCABLE;
      break;
    }
    case VVARARG:
    case VCALL: {
	  /* VCALL 在解析call表达式时，OP_CALL指令已生成，A,B,C三个参数也填充完毕,根据OP_CALL的含义
	  ** 表达式在寄存器中的reg.addr就是R(A),理解这一点就好立即luaK_setoneret函数了
	  */
      luaK_setoneret(fs, e);
      break;
    }
	
	/* !!!!常量，常量，常量 求其值时不需要用到reg，无需更新reg的信息e->k了 */
	case VNIL:
	case VTRUE:
	case VFALSE:
	case VKNUM:
	case VK: {
		break;
	}
	
	/* 
	** 关系表达式，后续如果需要将其值加载到reg，
	** 则是在后面的代码中连续生成2条OP_LOADBOOL指令
	** if (a>b) 常用的业务中无需表达式的值， local a = b > c才需要，等到需要的时候再说
	*/
	case VJMP:
		break;
			
	/* e->k估值信息已生成，这里无需做什么 */
	case VRELOCABLE:
	case VNONRELOC:
		break;
	
    default:
      break;  /* there is one value available (somewhere) */
  }
}

/* 专用函数，检索context, easy */
static int code_label (FuncState *fs, int A, int b, int jump) {
  luaK_getlabel(fs);  /* those instructions may be jump targets */
  return luaK_codeABC(fs, OP_LOADBOOL, A, b, jump);
}

/* 
** CP_XXX拷贝指令(reg = e) 拷贝表达式的值到指定的目的寄存器(reg(dst) = exp(src),
**    						最后 e.k = NONRELOC
**							     e.u.s.info = reg
**
** cond.1 对"直接表达式"(VNIL,VTRUE,VFALSE,VKNUM,VK)直接按照reg的目的地生成求值指令(load_xx)
**
** cond.2 对"值已在reg数组中的表达式"(VLOCAL, VCALL)按照reg的目的地生成op_move指令
**        对"VARGVAR"直接回填其目的寄存器
**
** cond.3 对"间接表达式"(VINDEXED, VGLOBAL, VUPVAL)先生成计算其值到free'reg的指令(get_xxx)
**		  	再回填上述get_xxx指令的目的reg为指定的reg
**
** 参考init_exp 和 luaK_dischargevars函数来理解本函数
*/
static void discharge2reg (FuncState *fs, expdesc *e, int reg) {
  /* 
  ** cond.1 对"间接表达式"(VINDEXED, VGLOBAL, VUPVAL)生成计算其值到free'reg的指令(get_xxx)，同时e->k = VRELOCABLE
  **            表示表达式原值的计算指令以生成，需回填目标地址
  ** cond.2 对已在reg数组中的"reg表达式"(VLOCAL,VCALL)更新e->k = VNONRELOC(VCALL还需e->u.s.info=RA)
  */
  luaK_dischargevars(fs, e);
  
  /*
  ** cond.1 对"直接表达式"(VNIL,VTRUE,VFALSE,VKNUM,VK)直接按照reg的目的地生成求值指令(load_xx)
  ** cond.2 对"reg表达式"(VNONRELOC)按照reg的目的地生成赋值指令(op_move)
  ** cond.3 对"间接表达式"(VRELOCABLE)回填其目的寄存器参数RA
  */
  
  switch (e->k) {
  	/* 表达式的值是常值, 这里生成指令并回填R(A) */
    case VNIL: {
      luaK_nil(fs, reg, 1);
      break;
    }
    case VFALSE:  case VTRUE: {
      luaK_codeABC(fs, OP_LOADBOOL, reg, e->k == VTRUE, 0);
      break;
    }

	/* 表达式的值在e->u.s.info：常量表中，这里提出来，生成指令并回填R(A)                       */
    case VK: {
	  /* reg：指令的目标寄存器RA, e->u.s.info:指令中常量exp在常量表中的索引 */
      luaK_codeABx(fs, OP_LOADK, reg, e->u.s.info);
      break;
    }
	/* 同上VK，只是nval在常量中的索引延迟到这里确定 */
    case VKNUM: {
      luaK_codeABx(fs, OP_LOADK, reg, luaK_numberK(fs, e->u.nval));
      break;
    }
	
	/* 指令，表达式的值都已确定，这里回填指令的目的地R(A)即可 */
    case VRELOCABLE: {
      Instruction *pc = &getcode(fs, e);
      SETARG_A(*pc, reg);
      break;
    }
	/* 表达式e的值已确定且在reg中了，生成OP_MOVE指令，完成R(A)=R(B)逻辑 */
    case VNONRELOC: {
      if (reg != e->u.s.info) /* 对于 a = a 这种操作的优化 */
        luaK_codeABC(fs, OP_MOVE, reg, e->u.s.info, 0);
      break;
    }
    default: {
      lua_assert(e->k == VVOID || e->k == VJMP);
      return;  /* nothing to do... */
    }
  }
  
  /* 表达式的目的寄存器R(A)已确定 */
  e->u.s.info = reg;
  e->k = VNONRELOC;
}

/* 对 非已经CP_XXX到寄存器上的表达式(即：不是VNONRELOC表达式），生成CP_XXX指令到next.free.reg */
static void discharge2anyreg (FuncState *fs, expdesc *e) {
  /* 表达式的结果值已经在reg上则无需重复加载到reg上 */
  if (e->k != VNONRELOC) {
    luaK_reserveregs(fs, 1);
    discharge2reg(fs, e, fs->freereg-1);
  }
}

/* dst=src CP_XXX指令，将表达式的值拷贝给指定的寄存器reg, 参考 discharge2reg 函数注释
** 函数内部处理了e的所有悬而未决的事务
**   包括给VJMP生成必要的LOADBOOL指令,给OP_TESTSET回填赋值的目标寄存器或优化成OP_TEST
** 函数处理完毕后，整个表达式就全部处理完毕了。
** 编译原理中的“总结者”的操作了 
** 编译原理中的“总结者”的操作了
** 编译原理中的“总结者”的操作了
*/
static void exp2reg (FuncState *fs, expdesc *e, int reg) {
  /* 将表达式的src.val赋值给dst(reg) */
  discharge2reg(fs, e, reg);

  /* 
  ** local a = b > c
  ** 关系指令的e.u.s.info指向的jmp要跳转到true.list
  ** 故而这里是e.t
  */
  if (e->k == VJMP)
    luaK_concat(fs, &e->t, e->u.s.info);  /* put this jump in `t' list */

  /*
  ** 处理表达式中剩余的JMP逻辑
  ** 	local a, z = b and c, d
  **    或者 local a = b > c
  */
  if (hasjumps(e)) {
    int final;  /* position after whole expression */
    int p_f = NO_JUMP;  /* position of an eventual LOAD false */
    int p_t = NO_JUMP;  /* position of an eventual LOAD true */
    if (need_value(fs, e->t) || need_value(fs, e->f)) {
		
	  /* 
	  ** OP_EQ, 		A B C	if ((RK(B) == RK(C)) ~= A) then pc++		
	  ** OP_TEST,	 	A C	  if not (R(A) <=> C) then pc++ 		  
	  ** OP_TESTSET, 	A B C   if (R(B) <=> C) then R(A) := R(B) else pc++ 
	  */
      int fj = (e->k == VJMP) ? NO_JUMP : luaK_jump(fs);	/* TODOKNOW */

	  /* R(A) := (Bool)B; if (C) pc++ 
	  ** p_f,p_t的执行是互斥的且执行完毕后pc均指向p_t+1
	  */
      p_f = code_label(fs, reg, 0, 1);	/* path.false */
      p_t = code_label(fs, reg, 1, 0);
	  
      luaK_patchtohere(fs, fj);
    }
	
    final = luaK_getlabel(fs);
	/* 
	** 对于OP_TESTSET指令，指令内部实现了赋值操作，
	**   跳过OP_LOADBOOL指令，直接跳转到final 
	** 对于其他的TEST指令则挂到p_f/p_t上 
	*/
    patchlistaux(fs, e->f, final, reg, p_f);	
    patchlistaux(fs, e->t, final, reg, p_t);	/* 回填OP_TESTSET中的跳转逻辑 */
  }

  /* 整个表达式解析完毕，编译原理中的“总结者”的作用了 */
  /* 整个表达式解析完毕，编译原理中的“总结者”的作用了 */
  /* 整个表达式解析完毕，编译原理中的“总结者”的作用了 */
   {
	  e->f = e->t = NO_JUMP;
	  /* 经过dst.(reg) = src.val 后，表达式的值已在寄存器中且地址是reg */
	  e->u.s.info = reg;
	  e->k = VNONRELOC;
   }
}

/* 
** LOAD_XXX 加载指令 将"常量表达式"，"间接表达式", "VVARARG" 加载到next'free'reg中
**
** 将表达式的值加载到寄存器中(eg:VGLOBAL, VINDEXED)
** 已加载到reg中的则无需此步骤(VNONRELOC)),
**
** RETURNS:返回表达式的值的寄存器地址 
*/
int luaK_exp2anyreg (FuncState *fs, expdesc *e) {
  /* 对表达式生成估值指令 */
  luaK_dischargevars(fs, e);
  
  if (e->k == VNONRELOC) {	/* e的src.val已在reg中，则直接返回对应的reg */
    if (!hasjumps(e)) return e->u.s.info;  /* exp is already in a register */
    if (e->u.s.info >= fs->nactvar) {  /* reg. is not a local? */
      exp2reg(fs, e, e->u.s.info);  /* put value on it */
	  assert(0);	// 不知道这块代码如何才能被调用，这里打一个死亡点
      return e->u.s.info;
    }
  }
  
  /* e的src值还不在reg则将其加载到reg */
  luaK_exp2nextreg(fs, e);  /* default */
  return e->u.s.info;
}

/* 
** CP_XXX 拷贝指令 next'free.reg = exp 
** 确定next'free'reg前尝试释放空闲的reg，注释参考 exp2reg , discharge2reg
*/
void luaK_exp2nextreg (FuncState *fs, expdesc *e) {
  /* 
  ** 更新exp的reg或者op信息
  ** 不能确定exp对应指令的则e->u.info中填入指令地址，方便回填,同时e->k:更新为VRELOCABLE，表示需要回填RA?
  */
  luaK_dischargevars(fs, e);

  /* 释放被临时占用的reg
  ** eg: local a,b,c = funA()(), d, e
  ** funA()()整个exp作为一个整理占用一个reg，
  ** 解析第二个()之前funA()已经完全解析完毕故而可以释放其占用的reg了。
  */
  freeexp(fs, e);
  
  /* 申请一个reg，并将exp赋值到reg上 */
  luaK_reserveregs(fs, 1);
  exp2reg(fs, e, fs->freereg - 1);
}

/* 类似 LOAD_XXX 生成表达式的加载指令(！！！！不是CP_XXX拷贝一份e的值到reg的拷贝指令)
** 要处理诸如 tbl{[a>b] = 3},所以这里要区分hasjumps这种情况
*/
void luaK_exp2val (FuncState *fs, expdesc *e) {
  if (hasjumps(e)) {			
  	/* 
  	** tbl{[a>b] = 3} , print("abc" .. a>b), c > (a > b)
  	** 诸如上面这些需要取得表达式a>b的情况而a>b本身还是个VJMP,没有生成取值指令，
  	** 那么要补充取值的指令上来
    */
    luaK_exp2anyreg(fs, e);	/* 求解表达式的src.val后，将表达式的值放到下一个free.reg中 */
   } else {
    /* 反之，对于 VNONRELOC, VRELOCABLE, VINDEXED... 等看情况生成必要的取值指令即可 */
    luaK_dischargevars(fs, e);
   }
}

/* 
** LOAD_XXX 加载指令 将表达式的值加载到next’free’reg中
** "VK常量表达式" 							 直接返回其常量表索引
** "VLOCAL,VCALL,VNONRELOC寄存器表达式"        直接返回其寄存器地址
** "VINDEXED,VUPVAL,VGLOBAL,VARGVAR"     生成求值指令并加载到next'free'reg中
**
** RETURNS: 表达式最终的RK索引
**
** 对表达式进行"终结"收尾处理，
**  OP_EQ:生成待生成的LOAD_BOOL,回填对应的JMP
**  OP_TESTSET: 回填悬挂的JMP，更新待赋值的R(A)，（优化成OP_TEST，这里好像没有进行此项操作）
*/
int luaK_exp2RK (FuncState *fs, expdesc *e) {
  /* 对[间接]表达式e生成求值指令 */
  luaK_exp2val(fs, e);

  /* e是常量表达式，无需生成求值指令，直接返回常量表中对应的索引即可 */
  switch (e->k) {
    case VKNUM:
    case VTRUE:
    case VFALSE:
    case VNIL: {
      if (fs->nk <= MAXINDEXRK) {  /* constant fit in RK operand? */
        e->u.s.info = (e->k == VNIL)  ? nilK(fs) :
                      (e->k == VKNUM) ? luaK_numberK(fs, e->u.nval) :
                                        boolK(fs, (e->k == VTRUE));
        e->k = VK;
        return RKASK(e->u.s.info);
      }
      else break;
    }
    case VK: {
      if (e->u.s.info <= MAXINDEXRK)  /* constant fit in argC? */
        return RKASK(e->u.s.info);
      else break;
    }
    default: break;
  }
  
  /* not a constant in the right range: put it in a register 
  **
  ** 间接表达式(非常量表达式)，将其src.val赋值到下一个free.reg中
  */
  return luaK_exp2anyreg(fs, e);
}

/* var = ex
** 先 LOAD_XXX (ex) 后 SET_XXX(var=ex) 的"赋值组合业务"
*/
void luaK_storevar (FuncState *fs, expdesc *var, expdesc *ex) {
  switch (var->k) {
    case VLOCAL: {
      freeexp(fs, ex);
	  /* var是VLOCAL, 那么直接将ex的值拷贝到var对应的reg即可*/
      exp2reg(fs, ex, var->u.s.info);	/* var = ex */
      return;
    }
    case VUPVAL: {		/* UpValue[B] := R(A) */
	  /* 现将表达式加载到reg,再赋值给UPVAL */
      int e = luaK_exp2anyreg(fs, ex);	/* var = ex */
      luaK_codeABC(fs, OP_SETUPVAL, e, var->u.s.info, 0);
      break;
    }
    case VGLOBAL: {		/* Gbl[Kst(Bx)] := R(A) */
      int e = luaK_exp2anyreg(fs, ex);	/* var = ex */
      luaK_codeABx(fs, OP_SETGLOBAL, e, var->u.s.info);
      break;
    }
    case VINDEXED: {	/* R(A)[RK(B)] := RK(C) */
      int e = luaK_exp2RK(fs, ex);	/* var = ex */
      luaK_codeABC(fs, OP_SETTABLE, var->u.s.info, var->u.s.aux, e);
      break;
    }
    default: {
      lua_assert(0);  /* invalid var kind to store */
      break;
    }
  }
  
  /* 释放求ex表达式的值的过程中产生的临时寄存器 */
  freeexp(fs, ex);
}

/* OP_SELF A B C R(A+1) := R(B); R(A) := R(B)[RK(C)] */
void luaK_self (FuncState *fs, expdesc *e, expdesc *key) {
  int func;
  
  /* function tbl.sub()
  ** end
  ** tbl可能不是localvar，有可能是个global,upval等，所以求其值
  ** 在
  */
  
  luaK_exp2anyreg(fs, e);
  freeexp(fs, e);
  func = fs->freereg;
  
  luaK_reserveregs(fs, 2);	/* 预留2个reg给OP_SELF */
  luaK_codeABC(fs, OP_SELF, func, e->u.s.info, luaK_exp2RK(fs, key));
  /* 语法要求，key必须是个TK_NAME，是个常量，故而这里不会释放上面2个reg,不明白这句话，看函数实现即可明白 */
  freeexp(fs, key);

  /* 这里对e是间接表达式和VLOCAL表达式这两种分别分析，即可得出结论(e->u.s.info=func这句代码是对的) */
  e->u.s.info = func;
  e->k = VNONRELOC;
}

/* invert:颠倒 
** OP_EQ等关系指令默认后面跟随false.path，
** 如果某个表达式后面不是false.path(eg ifstat())
** 或者是求反操作符 NOT 则需将关系指令的参数A进行反转以实现正确的语义
*/
static void invertjump (FuncState *fs, expdesc *e) {
  Instruction *pc = getjumpcontrol(fs, e->u.s.info);
  /* 测试指令仅有 OP_LE.., OP_TEST, OP_TESTSET, OP_TFORLOOP
  ** 能用在exp中单独求值的，去掉OP_TFORLOOP, 
  ** 这里为何还要不能是OP_TEST/OP_TESTSET呢，因为函数外面的调用环境限制了其VTYPE=VJUMP，只能是关系指令
  */
  lua_assert(testTMode(GET_OPCODE(*pc)) && GET_OPCODE(*pc) != OP_TESTSET &&
                                           GET_OPCODE(*pc) != OP_TEST);
  
  /* OP_EQ, A B C   if ((RK(B) == RK(C)) ~= A) then pc++ */
  SETARG_A(*pc, !(GETARG_A(*pc)));
}


static int jumponcond (FuncState *fs, expdesc *e, int cond) {
  /* WHILE (NOT a) DO
  **   body
  ** END
  ** 对着走一遍流程就理解这里if代码了
  */
  if (e->k == VRELOCABLE) {	/* 表达式的结果尚未保存到reg */
    Instruction ie = getcode(fs, e);
  	/* 表达式还是一个OP_NOT的，那就无需保存结果，只需要用到R(B)参数即可，
  	** 不用先 R(A) = not R(B)， 再去判断R(A)和cond的关系，而是一步到位
  	** 直接用R(B)和!cond判断即可 
  	*/
  	/* OP_NOT  B	  R(A) := not R(B) */
    if (GET_OPCODE(ie) == OP_NOT) {	
      fs->pc--;  /* remove previous OP_NOT */
	  /* OP_TEST A C if not (R(A) <=> C) then pc++ */ 
      return condjump(fs, OP_TEST, GETARG_B(ie), 0, !cond);	
    }
    /* else go through */
  }

  /* 将表达式的值加载到reg中，以便后面的OP_TESTEST指令对其进行评估 */
  discharge2anyreg(fs, e);
  freeexp(fs, e);
  
  /* if (R(B) <=> C) then R(A) := R(B) else pc++ 
  ** 这里RA尚未填写，回填这个跳转指令链表时会处理 patchtestreg
  */
  return condjump(fs, OP_TESTSET, NO_REG, e->u.s.info, cond);	
}

/* 这里的go是goon的意思 */
void luaK_goiftrue (FuncState *fs, expdesc *e) {
  int pc;  /* pc of last jump */
  luaK_dischargevars(fs, e);
  
  switch (e->k) {
    case VK: case VKNUM: case VTRUE: {	
      pc = NO_JUMP;  /* always true; do nothing， keep go throught? */
      break;
    }
    case VJMP: {
	  /* 关系指令(IF (a>b) THEN statment END)
	  ** eg: EQ, JMP,  OP_EQ A B C   if ((RK(B) == RK(C)) ~= A) then pc++ 
	  ** 紧接着的是false.path，和这里的true,相反，需要翻转条件 
	  */
      invertjump(fs, e);
      pc = e->u.s.info;	/* 原本上面指令接着的是JMP->true.path，这里反转后就成了false.path了 */
      break;
    }
    default: {
      /* 生成(OP_TESTSE   cond)和无条件跳转指令
      ** eg : WHILE (a) DO
      **        body
      **      END
      */
      /* 这里cond==0   
      ** OP_TESTSET,	/*	A B C	if (R(B) <=> C) then R(A) := R(B) else pc++	
      ** jumponcond 函数在生成 OP_TESTSET指令后接着生成OP_JUMP
      ** 一起来看就实现了 if (cond==0) then
      **                       R(A) = R(B)
      **                       OP_JUMP--->fail.out
      **                  end
      **    即条件判断失败则跳转的逻辑
      */
      pc = jumponcond(fs, e, 0); 	/* 这里的0结合OP_TESTSET指令一起看 */
      break;
    }
  }

  /* if (a and b) 
  ** 这里 cond为假时，false.jmp的跳转地址还尚未确定
  */
  luaK_concat(fs, &e->f, pc);  /* insert last jump in `f' list */
  

  /* 条件为true,则go thorught 
  ** 对于 luaK_infix 中的OP_TESTSET这里无用(哪个指令没有true.jmp.list-为true继续判断后面的reg即可不用额外的跳转)
  ** 对于 if (cond) 这里起到了作用
  */
  luaK_patchtohere(fs, e->t);	
  e->t = NO_JUMP;	/* 已经将待回填的truelist挂到fs->jpc上了，这里置空 */
}

/* or */
static void luaK_goiffalse (FuncState *fs, expdesc *e) {
  int pc;  /* pc of last jump */
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VNIL: case VFALSE: {
      pc = NO_JUMP;  /* always false; do nothing */
      break;
    }
    case VJMP: {
      pc = e->u.s.info;
      break;
    }
    default: {
      pc = jumponcond(fs, e, 1);
      break;
    }
  }

  
  /* 
  ** eg WHILE(a OR b) THEN
  **      body
  **    END
  **    将上面的TRUE.JUMP(pc)链接到a的truelist，等待解析完毕整个cond后回填到body的第一条指令上
  ** 	将a->falselist更新到下一条语句,如果表达式a为假，就继续执行表达式b
  **    
  */
  luaK_concat(fs, &e->t, pc);  /* insert last jump in `t' list */
  luaK_patchtohere(fs, e->f);	/* */
  e->f = NO_JUMP;
}

/* not  A B R(A) := not R(B) 
** not的stat的左边必须有左值,否则就是语法错误
*/
static void codenot (FuncState *fs, expdesc *e) {
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VNIL: case VFALSE: {
      e->k = VTRUE;
      break;
    }
    case VK: case VKNUM: case VTRUE: {
      e->k = VFALSE;
      break;
    }
    case VJMP: {	/* IF (not (a > b)) THEN
    				**    STATMENT
    				** END
    				*/
      invertjump(fs, e);
      break;
    }
    case VRELOCABLE:
    case VNONRELOC: {
      discharge2anyreg(fs, e);
      freeexp(fs, e);
	  /* R(A) := not R(B) */
      e->u.s.info = luaK_codeABC(fs, OP_NOT, 0, e->u.s.info, 0);	/* 这里R(A) 还不知道 */
      e->k = VRELOCABLE;
      break;
    }
    default: {
      lua_assert(0);  /* cannot happen */
      break;
    }
  }
  
  /* interchange true and false lists 
  ** VJMP:f/t均为空
  ** VNONRELOC/VRELOCABLE 交换t/f eg: local a = not (b and c)
  */
  {lua_assert((e->k != VJMP) || !hasjumps(e));}
  { int temp = e->f; e->f = e->t; e->t = temp; }

  /* 
  ** 如果有悬挂的OP_TESTSET指令那么尝试将其优化 
  ** 因为不需要将用OP_TESTSET指令将R(B)保持到R(A)中了
  ** 这里只取值判断即可，后续若要将其保存到reg中eg(local a                    = not(a and b)),上面的OP_NOT指令
  ** 实现了这个功能，如果不用将其保存到reg中(if (not (a and b)) THEN END则OP_NOT指令都会被优化为OP_TEST指令
  */
  removevalues(fs, e->f);	
  removevalues(fs, e->t);
}

/* 索引表达式t.k 
** info = table register; aux = index register (or `k') 
** eg: tbl(info).aux(aux) 
** 
** 先将k的exp加载到reg中(对于VJMP这种情况下的k(tbl[a>b]=c中的k=a>b),还需要补充相关的LOADBOOL指令
** 后t->K--->VINDEXED 
*/
void luaK_indexed (FuncState *fs, expdesc *t, expdesc *k) {
  t->u.s.aux = luaK_exp2RK(fs, k);
  t->k = VINDEXED;
}

/* 尝试合并二元操作符以及左右两边的表达式(编译优化) */
static int constfolding (OpCode op, expdesc *e1, expdesc *e2) {
  lua_Number v1, v2, r;
  
  /* 两个操作数都得是numeral */
  if (!isnumeral(e1) || !isnumeral(e2)) return 0;
  
  v1 = e1->u.nval;
  v2 = e2->u.nval;
  switch (op) {
    case OP_ADD: r = luai_numadd(v1, v2); break;
    case OP_SUB: r = luai_numsub(v1, v2); break;
    case OP_MUL: r = luai_nummul(v1, v2); break;
    case OP_DIV:
      if (v2 == 0) return 0;  /* do not attempt to divide by 0 */
      r = luai_numdiv(v1, v2); break;
    case OP_MOD:
      if (v2 == 0) return 0;  /* do not attempt to divide by 0 */
      r = luai_nummod(v1, v2); break;
    case OP_POW: r = luai_numpow(v1, v2); break;
    case OP_UNM: r = luai_numunm(v1); break;
    case OP_LEN: return 0;  /* no constant folding for 'len' */
    default: lua_assert(0); r = 0; break;
  }
  if (luai_numisnan(r)) return 0;  /* do not attempt to produce NaN */
  e1->u.nval = r;
  return 1;
}

/* 
** local a = b + c 
** 表达式运行完毕后，b,c占用的临时的reg就可以被释放了，故而这一行编译完成后b,c占用的reg也可以释放了
*/
static void codearith (FuncState *fs, OpCode op, expdesc *e1, expdesc *e2) {
  if (constfolding(op, e1, e2))
    return;
  else {
  	/* 
  	** OP_NOT 有专用函数，不走这里, 故而下面无需判断OP_NOT 
  	** luaK_exp2RK将终结悬挂在e1, e2上的可能t/f jump(eg: a + (b > c))收尾处理
    */
    int o2 = (op != OP_UNM && op != OP_LEN) ? luaK_exp2RK(fs, e2) : 0;	
    int o1 = luaK_exp2RK(fs, e1);
    /* 释放exp的规则是从后往前free */
    if (o1 > o2) {
      freeexp(fs, e1);
      freeexp(fs, e2);
    }
    else {
      freeexp(fs, e2);
      freeexp(fs, e1);
    }
	
	/* var = exp1 op exp2
	** 运算符的操作数(exp1,exp2)的估值指令已生成，运算符的指令也已生成
	** 但目的寄存器(var)尚未确定下来,有可能需要var有可能不需要(eg: if (exp1 op exp2) 或
	**   locac var = exp1 op exp2)
	*/
    e1->u.s.info = luaK_codeABC(fs, op, 0, o1, o2);
    e1->k = VRELOCABLE;
  }
}

/* 关系表达式 */
static void codecomp (FuncState *fs, OpCode op, int cond, expdesc *e1,
                                                          expdesc *e2) {
  int o1 = luaK_exp2RK(fs, e1);
  int o2 = luaK_exp2RK(fs, e2);
  freeexp(fs, e2);
  freeexp(fs, e1);
  if (cond == 0 && op != OP_EQ) {
    int temp;  /* exchange args to replace by `<' or `<=' */
    temp = o1; o1 = o2; o2 = temp;  /* o1 <==> o2 */
    cond = 1;
  }
  
  /* OP_EQ,	  A B C   if ((RK(B) == RK(C)) ~= A) then pc++		  */
  /* OP_LT,   A B C   if ((RK(B) <	RK(C)) ~= A) then pc++		  */
  /* OP_LE,	  A B C   if ((RK(B) <= RK(C)) ~= A) then pc++		  */
  e1->u.s.info = condjump(fs, op, cond, o1, o2);	
  e1->k = VJMP;
}

/* local a = not b 
** 处理前缀操作符
OP_UNM, 	A B		R(A) := -R(B)
OP_NOT, 	A B		R(A) := not R(B)
OP_LEN, 	A B		R(A) := length of R(B)
*/
void luaK_prefix (FuncState *fs, UnOpr op, expdesc *e) {
  expdesc e2;
  e2.t = e2.f = NO_JUMP;
  e2.k = VKNUM; e2.u.nval = 0;
  switch (op) {
    case OPR_MINUS: {	/* OP_UNM, A B 	R(A) := -R(B) */
      if (!isnumeral(e))
        luaK_exp2anyreg(fs, e);  /* cannot operate on non-numeric constants */
      codearith(fs, OP_UNM, e, &e2);
      break;
    }
    case OPR_NOT: codenot(fs, e); 	/* OP_NOT, 	A B 	R(A) := not R(B) */
		break;
    case OPR_LEN: {					/* OP_LEN, 	A B		R(A) := length of R(B) */
      luaK_exp2anyreg(fs, e);  /* cannot operate on constants */
      codearith(fs, OP_LEN, e, &e2);
      break;
    }
    default: lua_assert(0);
  }
}

/* infix: 中缀 a = exp1 + exp2 解析exp2前，根据操作符来处理exp1 */
void luaK_infix (FuncState *fs, BinOpr op, expdesc *v) {
  switch (op) {
  	/*  
  	** 将v加载到寄存器,生成OP_TESTSET指令， OP_TESTSET   	A B C	if (R(B) <=> C) then R(A) := R(B) else pc++ 
  	**
  	** 1. 由于此时false.out未知，故而生成false.jmp后，待跳转的位置待定，等待后续回填
  	** 2. 而若(R(B)==false)则指令将值赋给了R(A)，R(A)尚为确定地址(if (a and b) 
  	**        这种就不需要R(A)的地址，所以函数尚未明确R(A)的地址，
  	**        因为这里还不确定时候需要RA的地址(a = b and c 这种情况才需要))
  	** 综合上述2点luaK_goiftrue已实现了OPR_AND的语义：
  	**   R(B)为假时，赋值假值给R(A), 同时提前结束后续的指令，
  	**   R(B)为真则继续接着处理后面的表达式
  	*/
    case OPR_AND: {
      luaK_goiftrue(fs, v);
      break;
    }
    case OPR_OR: {
      luaK_goiffalse(fs, v);
      break;
    }
	/* 由于OP_CONCAT的实际实现方式，这里必须将exp复制一份到stack上
	** 理论上来说 local a = b .. c .. d 知道了b,c,d的地址就可以将其合并
	**   但是，如果将他们复制到statck且地址是连续的，那么实际实现起来，会容易些，同时
	**   也能照顾到c= 1>2这种还有悬挂的jmp的情况
	**   所以这里就这样重复COPY一份到stack上了
	*/
    case OPR_CONCAT: {
      luaK_exp2nextreg(fs, v);  /* operand must be on the `stack' */
      break;
    }
    case OPR_ADD: case OPR_SUB: case OPR_MUL: case OPR_DIV:
    case OPR_MOD: case OPR_POW: {
	  /* 纯数值则考虑表达式合并 eg: local a = 10 + 5直接将exp1(10) 和exp2(5)合并成15 */
      if (!isnumeral(v))
	  	luaK_exp2RK(fs, v);	
      break;
    }
    default: {	/* 关系运算符 */
      luaK_exp2RK(fs, v);	/* 将v"加载"到reg */
      break;
    }
  }
}

/* a = b + c 解析完c之后，将b+c合并作为一个exp 
** TODOREAD KEYFUN
*/
void luaK_posfix (FuncState *fs, BinOpr op, expdesc *e1, expdesc *e2) {
  switch (op) {
    case OPR_AND: {
	  /* b and c, b为ture则指令流自动顺延到解析c来，不用跳转，故而如果这里出现了跳转，
	  ** 那就是解析b后，收尾的处理出现了Fatal err
	  */
      lua_assert(e1->t == NO_JUMP);  /* list must be closed */

	  /* 
	  ** 生成估值指令，由于还不知道是 local var = exp1 and exp2
	  **   还是if (exp1 and exp2),那种情况故而无需生成赋值指令(第一种情况)和判断跳转指令(第二种情况)
	  **   这里将上述待完成的逻辑交给上层处理(本函数完成接口要求中的求值exp的功能即可)
	  */
      luaK_dischargevars(fs, e2);
	
	  /* 合并false.jmp.list 
	  ** WHILE(a AND b AND c...) DO
	  **  body
	  ** END
	  ** 所有的表达式a,b,c..的false.jump的目的地都是一样的(whilestat结束后的第一条指令)
	  */
      luaK_concat(fs, &e2->f, e1->f);
	  /* 
	  ** 拷贝了e1->f到e2->f之后，e1的作用域也就此为止了，下面将e2覆盖到e1上也就顺理成章
	  ** 同时也完成了(a and b)中将a和b合并成一个表达式的功能要求
	  */
      *e1 = *e2;
      break;
    }
    case OPR_OR: {
      lua_assert(e1->f == NO_JUMP);  /* list must be closed */
      luaK_dischargevars(fs, e2);
	
	  /* 合并true.jmp.list 
	  ** WHILE(a OR b OR c...) DO
	  **  body
	  ** END
	  ** 所有的表达式a,b,c..的true.jump的目的地都是一样的(body中的第一条指令)
	  */
      luaK_concat(fs, &e2->t, e1->t);
      *e1 = *e2;
      break;
    }
    case OPR_CONCAT: {
      luaK_exp2val(fs, e2);
      if (e2->k == VRELOCABLE && GET_OPCODE(getcode(fs, e2)) == OP_CONCAT) {
        lua_assert(e1->u.s.info == GETARG_B(getcode(fs, e2))-1);
        freeexp(fs, e1);
        SETARG_B(getcode(fs, e2), e1->u.s.info);
        e1->k = VRELOCABLE; e1->u.s.info = e2->u.s.info;
      }
      else {
        luaK_exp2nextreg(fs, e2);  /* operand must be on the 'stack' */
        codearith(fs, OP_CONCAT, e1, e2);
      }
      break;
    }
    case OPR_ADD: codearith(fs, OP_ADD, e1, e2); break;
    case OPR_SUB: codearith(fs, OP_SUB, e1, e2); break;
    case OPR_MUL: codearith(fs, OP_MUL, e1, e2); break;
    case OPR_DIV: codearith(fs, OP_DIV, e1, e2); break;
    case OPR_MOD: codearith(fs, OP_MOD, e1, e2); break;
    case OPR_POW: codearith(fs, OP_POW, e1, e2); break;
    case OPR_EQ: codecomp(fs, OP_EQ, 1, e1, e2); break;
    case OPR_NE: codecomp(fs, OP_EQ, 0, e1, e2); break;
    case OPR_LT: codecomp(fs, OP_LT, 1, e1, e2); break;
    case OPR_LE: codecomp(fs, OP_LE, 1, e1, e2); break;
    case OPR_GT: codecomp(fs, OP_LT, 0, e1, e2); break;
    case OPR_GE: codecomp(fs, OP_LE, 0, e1, e2); break;
    default: lua_assert(0);
  }
}

/* 更新上一个生成的pc对应的行信息
** 对着forstat看，一下就明白了
** 某些情况下，指令的生成顺序和代码中的出现的顺序不一定相等哦
*/
void luaK_fixline (FuncState *fs, int line) {
  fs->f->lineinfo[fs->pc - 1] = line;
}


static int luaK_code (FuncState *fs, Instruction i, int line) {
  Proto *f = fs->f;
  
  /* 如果有待回填的指向下一条待生成指令的链表，则回填 */
  dischargejpc(fs);  /* `pc' will change */
  
  /* put new instruction in code array */
  luaM_growvector(fs->L, f->code, fs->pc, f->sizecode, Instruction,
                  MAX_INT, "code size overflow");
  f->code[fs->pc] = i;
  
  /* save corresponding line information 更新指令的line.map信息 */
  luaM_growvector(fs->L, f->lineinfo, fs->pc, f->sizelineinfo, int,
                  MAX_INT, "code size overflow");
  f->lineinfo[fs->pc] = line;

  PrintOneCode(i);
  
  return fs->pc++;				/* pc指向下一个待生成指令的idx,记住这一点对理解跳转逻辑是必要的 */
}	


int luaK_codeABC (FuncState *fs, OpCode o, int a, int b, int c) {
  lua_assert(getOpMode(o) == iABC);
  lua_assert(getBMode(o) != OpArgN || b == 0);
  lua_assert(getCMode(o) != OpArgN || c == 0);
  return luaK_code(fs, CREATE_ABC(o, a, b, c), fs->ls->lastline);
}


int luaK_codeABx (FuncState *fs, OpCode o, int a, unsigned int bc) {
  lua_assert(getOpMode(o) == iABx || getOpMode(o) == iAsBx);
  lua_assert(getCMode(o) == OpArgN);
  return luaK_code(fs, CREATE_ABx(o, a, bc), fs->ls->lastline);
}


void luaK_setlist (FuncState *fs, int base, int nelems, int tostore) {
  int c =  (nelems - 1)/LFIELDS_PER_FLUSH + 1;
  int b = (tostore == LUA_MULTRET) ? 0 : tostore;	/* tostore中最后一个是变参，则tostore==LUA_MULTRET */
  lua_assert(tostore != 0);
  if (c <= MAXARG_C)
    luaK_codeABC(fs, OP_SETLIST, base, b, c);
  else {
  	/* c过大，将其放到下一条指令中 */
    luaK_codeABC(fs, OP_SETLIST, base, b, 0);
    luaK_code(fs, cast(Instruction, c), fs->ls->lastline);
  }
  /* 这里可以回收空闲出来的寄存器了，有意思吧, base+1：保留tbl占用的一个reg,构造表过程中其它临时寄存器被释放 */
  fs->freereg = base + 1;  /* free registers with list values */
}

