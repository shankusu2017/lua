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

/* e的类型 expdesc */
#define hasjumps(e)	((e)->t != (e)->f)


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
** OP_JMP sBx PC += sBx
**
** 待回填的跳转链表指向我，而我又指向其它pc，那么将上述链表和我串联在一起即可
*/
int luaK_jump (FuncState *fs) {
  int jpc = fs->jpc;  /* save list of jumps to here */
  int j;
  fs->jpc = NO_JUMP;
  j = luaK_codeAsBx(fs, OP_JMP, 0, NO_JUMP);
  luaK_concat(fs, &j, jpc);  /* keep them on hold */
  return j;
}

/* 从函数返回
** OP_RETURN A B return R(A), ... ,R(A+B-2)
*/
void luaK_ret (FuncState *fs, int first, int nret) {
  luaK_codeABC(fs, OP_RETURN, first, nret+1, 0);	/* 这里可以反推OP_RETURNS中A,B,C的含义了 */
}

/* 有条件跳转 OP_TEST, OP_TESTSET */
static int condjump (FuncState *fs, OpCode op, int A, int B, int C) {
  luaK_codeABC(fs, op, A, B, C);
  return luaK_jump(fs);
}

/* 将待回填的跳转指令pc指向dest */
static void fixjump (FuncState *fs, int pc, int dest) {
  Instruction *jmp = &fs->f->code[pc];
  /* 下面计算跳转指令的跳转目标绝对值时也加了1，和这里是一致的 */
  int offset = dest-(pc+1);		
  lua_assert(dest != NO_JUMP);
  if (abs(offset) > MAXARG_sBx)
    luaX_syntaxerror(fs->ls, "control structure too long");
  SETARG_sBx(*jmp, offset);
}


/*
** returns current `pc' and marks it as a jump target (to avoid wrong
** optimizations(优化) with consecutive(连续) instructions not in the same basic block).
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


static Instruction *getjumpcontrol (FuncState *fs, int pc) {
  Instruction *pi = &fs->f->code[pc];
  if (pc >= 1 && testTMode(GET_OPCODE(*(pi-1))))
    return pi-1;
  else
    return pi;
}


/*
** check whether list has any jump that do not produce a value
** (or produce an inverted(颠,倒) value)
*/
static int need_value (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list)) {
    Instruction i = *getjumpcontrol(fs, list);
    if (GET_OPCODE(i) != OP_TESTSET) return 1;
  }
  return 0;  /* not found */
}


static int patchtestreg (FuncState *fs, int node, int reg) {
  Instruction *i = getjumpcontrol(fs, node);
  if (GET_OPCODE(*i) != OP_TESTSET)
    return 0;  /* cannot patch other instructions */
  if (reg != NO_REG && reg != GETARG_B(*i))
    SETARG_A(*i, reg);
  else  /* no register to put value or register already has the value */
    *i = CREATE_ABC(OP_TEST, GETARG_B(*i), 0, GETARG_C(*i));

  return 1;
}


static void removevalues (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list))
      patchtestreg(fs, list, NO_REG);
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


void luaK_patchlist (FuncState *fs, int list, int target) {
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

/*  l1.sBx = l2 
** 将l2指向的待回填跳转指令/指令链表挂到l1的跳转链表上
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

/* reserve reg:预定 寄存器 实际上是占用n个寄存器的意思
*/
void luaK_reserveregs (FuncState *fs, int n) {
  luaK_checkstack(fs, n);
  fs->freereg += n;	/* 占用n个locvar,释放则n为负值或在其它函数中实现 */
}

/* 重点函数，需要细读 */
static void freereg (FuncState *fs, int reg) {
  if (!ISK(reg) &&            /* 常量的就不用释放了，压根没占用reg */
      reg >= fs->nactvar) {   /* reg从0开始，nactvar从1开始，所以这里reg>=fs->nactvar是可以的
      
  	/* 释放一个reg后,reg==fs->freereg:确保只能释放最新一个被激活的reg(作为exp的临时reg占用？) */
    fs->freereg--;
    lua_assert(reg == fs->freereg);
  }
}

/* 释放被临时占用的reg */
static void freeexp (FuncState *fs, expdesc *e) {
  if (e->k == VNONRELOC)		/* 表达式的值已被CP_XXX到reg中的，才释放 (还没加载到reg，那压根没占用reg，释放个锤子*/
    freereg(fs, e->u.s.info);	/* VNONRELOC info = result register */ 
}

/*
** 将常量加载到fs->f的常量表中
**
** local var = "hello" 则本函数的k,v="hello" 
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
    SETARG_C(getcode(fs, e), nresults+1);
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
    e->k = VNONRELOC;	
    e->u.s.info = GETARG_A(getcode(fs, e));
  }
  else if (e->k == VVARARG) {
    SETARG_B(getcode(fs, e), 2);	/* 2:期待返回一个返回值 */
    e->k = VRELOCABLE;  /* can relocate its simple result */
  }
}


/* 
** LOAD_XXX 生成对间接表达式的求值指令 (VCALL, VARGVAR这里强制返回1个值, 需返回多个值的在上层业务中进行修正) 
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
	  ** a.b.c.d.e... 释放a.b.c.d之前占用的reg,以便重利用reg
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
	
	/* !!!!常量，常量，常量 不需要用到reg，无需更新reg的信息e->k了 */
	case VNIL:
	case VTRUE:
	case VFALSE:
	case VKNUM:
	case VK: {
		break;
	}
	
	/* 还没遇到过，不太理解 */
	case VJMP:
		break;
			
	/* e->k已经确定了寄存器的信息了，直接返回 */
	case VRELOCABLE:
	case VNONRELOC:
		break;
	
    default:
      break;  /* there is one value available (somewhere) */
  }
}


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
      if (reg != e->u.s.info) /* 对于 a = a 这种无效的操作的优化 */
        luaK_codeABC(fs, OP_MOVE, reg, e->u.s.info, 0);
      break;
    }
	/* VJMP尚不理解 */
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
  if (e->k != VNONRELOC) {
    luaK_reserveregs(fs, 1);
    discharge2reg(fs, e, fs->freereg-1);
  }
}

/* dst=src CP_XXX指令，将表达式的值拷贝给指定的寄存器reg, 参考 discharge2reg 函数注释*/
static void exp2reg (FuncState *fs, expdesc *e, int reg) {
  /* 将表达式的src.val赋值给dst(reg) */
  discharge2reg(fs, e, reg);
  
  if (e->k == VJMP)
    luaK_concat(fs, &e->t, e->u.s.info);  /* put this jump in `t' list */
  if (hasjumps(e)) {
    int final;  /* position after whole expression */
    int p_f = NO_JUMP;  /* position of an eventual LOAD false */
    int p_t = NO_JUMP;  /* position of an eventual LOAD true */
    if (need_value(fs, e->t) || need_value(fs, e->f)) {
      int fj = (e->k == VJMP) ? NO_JUMP : luaK_jump(fs);
      p_f = code_label(fs, reg, 0, 1);
      p_t = code_label(fs, reg, 1, 0);
      luaK_patchtohere(fs, fj);
    }
    final = luaK_getlabel(fs);
    patchlistaux(fs, e->f, final, reg, p_f);
    patchlistaux(fs, e->t, final, reg, p_t);
  }
  
  e->f = e->t = NO_JUMP;
  /* 经过dst.(reg) = src.val 后，表达式的值已在寄存器中且地址是reg */
  e->u.s.info = reg;
  e->k = VNONRELOC;
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

  /* 释放特定条件下的被临时占用的reg */
  freeexp(fs, e);
  
  /* 申请一个reg，并将exp赋值到reg上 */
  luaK_reserveregs(fs, 1);
  exp2reg(fs, e, fs->freereg - 1);
}

/* 
** LOAD_XXX 加载指令 将"常量表达式"，"间接表达式", "VVARARG" 加载到next'free'reg中
**
** 将表达式的值加载到寄存器中(eg:VGLOBAL, VINDEXED)
** 已加载到reg中的则无需此步骤(VNONRELOC)),
**
** RETURNS:寄存器地址 
*/
int luaK_exp2anyreg (FuncState *fs, expdesc *e) {
  /* 对表达式生成估值指令 */
  luaK_dischargevars(fs, e);
  if (e->k == VNONRELOC) {	/* e的src.val已在reg中，则直接返回对应的reg */
    if (!hasjumps(e)) return e->u.s.info;  /* exp is already in a register */
    if (e->u.s.info >= fs->nactvar) {  /* reg. is not a local? */
      exp2reg(fs, e, e->u.s.info);  /* put value on it */
      return e->u.s.info;
    }
  }
  
  /* e的src值还不在reg则将其存入reg */
  luaK_exp2nextreg(fs, e);  /* default */
  return e->u.s.info;
}

/* 类似 LOAD_XXX 生成表达式的加载指令(！！！！不是CP_XXX拷贝一份e的值到reg的拷贝指令) */
void luaK_exp2val (FuncState *fs, expdesc *e) {
  if (hasjumps(e))
    luaK_exp2anyreg(fs, e);	/* 求解表达式的src.val后，将表达式的值放到下一个free.reg中 */
  else
    luaK_dischargevars(fs, e);	/* 对间接表达式（原值不在reg中或不是直接值的）生成求值指令 */
}

/* 
** LOAD_XXX 加载指令 将表达式的值加载到next’free’reg中
** "VK常量表达式" 							 直接返回其常量表索引
** "VLOCAL,VCALL,VNONRELOC寄存器表达式"        直接返回其寄存器地址
** "VINDEXED,VUPVAL,VGLOBAL,VARGVAR"     生成求值指令并加载到next'free'reg中
**
** RETURNS: 表达式最终的RK索引
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
  
  luaK_reserveregs(fs, 2);	/* 预留2个free.reg出来，留给指令OP_SELF使用 */
  luaK_codeABC(fs, OP_SELF, func, e->u.s.info, luaK_exp2RK(fs, key));
  /* 语法要求，key必须是个TK_NAME，是个常量，故而这里不会释放上面2个reg,不明白这句话，看函数实现即可明白 */
  freeexp(fs, key);

  /* 这里对e是间接表达式和VLOCAL表达式这两种分别分析，即可得出结论(e->u.s.info=func这句代码是对的) */
  e->u.s.info = func;
  e->k = VNONRELOC;
}

/* invert:颠倒 */
static void invertjump (FuncState *fs, expdesc *e) {
  Instruction *pc = getjumpcontrol(fs, e->u.s.info);
  lua_assert(testTMode(GET_OPCODE(*pc)) && GET_OPCODE(*pc) != OP_TESTSET &&
                                           GET_OPCODE(*pc) != OP_TEST);
  SETARG_A(*pc, !(GETARG_A(*pc)));
}


static int jumponcond (FuncState *fs, expdesc *e, int cond) {
  if (e->k == VRELOCABLE) {
    Instruction ie = getcode(fs, e);
    if (GET_OPCODE(ie) == OP_NOT) {
      fs->pc--;  /* remove previous OP_NOT */
      return condjump(fs, OP_TEST, GETARG_B(ie), 0, !cond);
    }
    /* else go through */
  }
  discharge2anyreg(fs, e);
  freeexp(fs, e);
  return condjump(fs, OP_TESTSET, NO_REG, e->u.s.info, cond);
}

/* and */
void luaK_goiftrue (FuncState *fs, expdesc *e) {
  int pc;  /* pc of last jump */
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VK: case VKNUM: case VTRUE: {	
      pc = NO_JUMP;  /* always true; do nothing， keep go throught? */
      break;
    }
    case VJMP: {
      invertjump(fs, e);
      pc = e->u.s.info;
      break;
    }
    default: {
      pc = jumponcond(fs, e, 0);
      break;
    }
  }
  luaK_concat(fs, &e->f, pc);  /* insert last jump in `f' list */
  luaK_patchtohere(fs, e->t);
  e->t = NO_JUMP;
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
  luaK_concat(fs, &e->t, pc);  /* insert last jump in `t' list */
  luaK_patchtohere(fs, e->f);
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
    case VJMP: {
      invertjump(fs, e);
      break;
    }
    case VRELOCABLE:
    case VNONRELOC: {
      discharge2anyreg(fs, e);
      freeexp(fs, e);
      e->u.s.info = luaK_codeABC(fs, OP_NOT, 0, e->u.s.info, 0);
      e->k = VRELOCABLE;
      break;
    }
    default: {
      lua_assert(0);  /* cannot happen */
      break;
    }
  }
  
  /* interchange true and false lists */
  { int temp = e->f; e->f = e->t; e->t = temp; }
  
  removevalues(fs, e->f);
  removevalues(fs, e->t);
}

/* 索引表达式t.k 
** info = table register; aux = index register (or `k') 
** eg: tbl(info).aux(aux) 
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
	/* 这里R(A)的值尚未确定，e->=VRELOCABLE:表示需要重定位？ */
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
  e1->u.s.info = condjump(fs, op, cond, o1, o2);
  e1->k = VJMP;
}


void luaK_prefix (FuncState *fs, UnOpr op, expdesc *e) {
  expdesc e2;
  e2.t = e2.f = NO_JUMP;
  e2.k = VKNUM; e2.u.nval = 0;
  switch (op) {
    case OPR_MINUS: {
      if (!isnumeral(e))
        luaK_exp2anyreg(fs, e);  /* cannot operate on non-numeric constants */
      codearith(fs, OP_UNM, e, &e2);
      break;
    }
    case OPR_NOT: codenot(fs, e); break;
    case OPR_LEN: {
      luaK_exp2anyreg(fs, e);  /* cannot operate on constants */
      codearith(fs, OP_LEN, e, &e2);
      break;
    }
    default: lua_assert(0);
  }
}

/* infix: 中缀 */
void luaK_infix (FuncState *fs, BinOpr op, expdesc *v) {
  switch (op) {
    case OPR_AND: {
      luaK_goiftrue(fs, v);
      break;
    }
    case OPR_OR: {
      luaK_goiffalse(fs, v);
      break;
    }
    case OPR_CONCAT: {
      luaK_exp2nextreg(fs, v);  /* operand must be on the `stack' */
      break;
    }
    case OPR_ADD: case OPR_SUB: case OPR_MUL: case OPR_DIV:
    case OPR_MOD: case OPR_POW: {
      if (!isnumeral(v)) luaK_exp2RK(fs, v);
      break;
    }
    default: {
      luaK_exp2RK(fs, v);
      break;
    }
  }
}


void luaK_posfix (FuncState *fs, BinOpr op, expdesc *e1, expdesc *e2) {
  switch (op) {
    case OPR_AND: {
      lua_assert(e1->t == NO_JUMP);  /* list must be closed */
      luaK_dischargevars(fs, e2);
      luaK_concat(fs, &e2->f, e1->f);
      *e1 = *e2;
      break;
    }
    case OPR_OR: {
      lua_assert(e1->f == NO_JUMP);  /* list must be closed */
      luaK_dischargevars(fs, e2);
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

/* 更新上一个生成的pc对应的行信息 */
void luaK_fixline (FuncState *fs, int line) {
  fs->f->lineinfo[fs->pc - 1] = line;
}


static int luaK_code (FuncState *fs, Instruction i, int line) {
  Proto *f = fs->f;
  dischargejpc(fs);  /* `pc' will change */
  /* put new instruction in code array */
  luaM_growvector(fs->L, f->code, fs->pc, f->sizecode, Instruction,
                  MAX_INT, "code size overflow");
  f->code[fs->pc] = i;
  /* save corresponding line information */
  luaM_growvector(fs->L, f->lineinfo, fs->pc, f->sizelineinfo, int,
                  MAX_INT, "code size overflow");
  f->lineinfo[fs->pc] = line;
  return fs->pc++;
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

