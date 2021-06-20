/* 参考的BNF地址 shankusu.me/lua/TheCompleteSyntaxOfLua51/ */
/*
** $Id: lparser.c,v 2.42.1.4 2011/10/21 19:31:42 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/


#include <string.h>
#include <stdio.h>

#define lparser_c
#define LUA_CORE

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"


/* 也只有函数调用或变参操作符这两种TOKEN能返回 ... */
#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)

/* i:当前活跃的locvar的索引 */
#define getlocvar(fs, i)	((fs)->f->locvars[(fs)->actvar[i]])

/* 检查value是否超过了limit限制,超过则报错msg */
#define luaY_checklimit(fs,v,l,m)	if ((v)>(l)) errorlimit(fs,l,m)


/*
** nodes for block list (list of active blocks)
** previous:往前跳(eg:查找变量时从now-block往前一级一级的block找)
*/
typedef struct BlockCnt {
  struct BlockCnt *previous;  /* chain */
  int breaklist;  /* list of jumps out of this loop */

  /* 
  ** ！！！！在进入本block的瞬间，外面已经激活的var的数量， ！！！！
  ** 意味着本块内激活的locvar的reg.idx不会低于整个值，
  ** 用于按照便变量的生存期检索变量 
  ** 退出本block后，将fs->reg重置到本次即可清掉本block内激活的actvar
  */
  lu_byte nactvar;  /* # active locals outside the breakable structure */
  
  lu_byte upval;  /* true if some variable in the block is an upvalue(本块中存在某些变量是其它块的upvalues：本块关闭时要做善后处理？) */
  lu_byte isbreakable;  /* true if `block' is a loop, 语法规则：break仅能用于loop的block中 */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void chunk (LexState *ls);
static void expr (LexState *ls, expdesc *v);

/* anchor:锚 
** function f1()
**    function f2()
**    end
** end
** 变量名f2在f1中属于常量，注册变量名f2到f1的常量表中
*/
static void anchor_token (LexState *ls) {
  if (ls->t.token == TK_NAME || ls->t.token == TK_STRING) {
    TString *ts = ls->t.seminfo.ts;
    luaX_newstring(ls, getstr(ts), ts->tsv.len);
  }
}


static void error_expected (LexState *ls, int token) {
  luaX_syntaxerror(ls,
      luaO_pushfstring(ls->L, LUA_QS " expected", luaX_token2str(ls, token)));
}


static void errorlimit (FuncState *fs, int limit, const char *what) {
  const char *msg = (fs->f->linedefined == 0) ?
    luaO_pushfstring(fs->L, "main function has more than %d %s", limit, what) :
    luaO_pushfstring(fs->L, "function at line %d has more than %d %s",
                            fs->f->linedefined, limit, what);
  luaX_lexerror(fs->ls, msg, 0);
}


static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}

/* 检查当前c是否为特定的token'Type */
static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}

static void checknext (LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }


/* 在where(line)这里，who(TK.1)需要一个what(TK.2)匹配
** eg: function 需要一个end来结束函数定义
*/
static void check_match (LexState *ls, int what, int who, int where) {
  if (!testnext(ls, what)) {
    if (where == ls->linenumber)	/* 当前行，那就不需要打印line信息了？ */
      error_expected(ls, what);
    else {
		/* 输出连带line信息的错误信息 */
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
             LUA_QS " expected (to close " LUA_QS " at line %d)",
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
    }
  }
}

/* 
** 强制检查并当前token的type为TK_NAME，返回当前token，
** 读取下一个token 
*/
static TString *str_checkname (LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);		/* 当前token'type必须是TK_NAME的类型 */
  ts = ls->t.seminfo.ts;	/* 提取token的值 */
  luaX_next(ls);			/* 继续读下一个token */
  return ts;
}

/*  KEYCODE: 关键函数 
************************************exp对应的reg已定或是一个参数无需reg*********************************
** VVOID, VKNUM, VNIL, VTRUE, VFALSE,	  i:0 值直接被包含在表达式expdesc中，无需寄存器
** VK									  i:常量表中的索引
** VLOCAL								  i:locvar占用的reg索引
** VGLOBAL								  i:NO_REG->全局变量名的NAME在常量表中的索引
**
**
***********************************需回填指令的RA?**********************************
** VRELOCABLE							  i:？对应指令OP在指令数组中的下标（方便回填指令中的RA？)？
** VCALL, VVARARG						  i:对应指令OP在指令数组中的下标（方便回填指令中的RA？)
**
** VNONRELOC							  i:对应指令OP在指令数组的下标(方便回填指令中的RA?)
*/
static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.s.info = i;
}

/* 用字符串(TK_NAME)s初始化expdesc的e表达式 */
static void codestring (LexState *ls, expdesc *e, TString *s) {
  init_exp(e, VK, luaK_stringK(ls->fs, s));
}

/* 先检查当前t的类型为NAME，后将其携带的string赋值给expdesc, 内部读取一次luaX_next() */
static void checkname(LexState *ls, expdesc *e) {
  codestring(ls, e, str_checkname(ls));
}

/* 
** 填充一个全新的 Locvar信息到 Proto.locvars (供调试用)
*/
static int registerlocalvar (LexState *ls, TString *varname) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;

  /* 原来的总数组f->sizelocvars空间不足则扩大 */
  luaM_growvector(ls->L, f->locvars, fs->nlocvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "too many local variables");
  
  while (oldsize < f->sizelocvars)	/* locvars数组扩大则将新增的slot填NULL */
  	f->locvars[oldsize++].varname = NULL;
  
  /* 更新locvar信息, startPC,endPC暂时还不确定 */
  f->locvars[fs->nlocvars].varname = varname; 
  /* printf("registerlocalvar: idx(%d), name(%p)\n", fs->nlocvars, varname); */
  luaC_objbarrier(ls->L, f, varname);
  return fs->nlocvars++;
}

/* 如果v是不变的string则此宏定义可以利用宏处理阶段提高程序速度 */
#define new_localvarliteral(ls,v,n) \
  new_localvar(ls, luaX_newstring(ls, "" v, (sizeof(v)/sizeof(char))-1), n)

/* KEYCODE
** 注册一个本地变量信息到 Proto.locvars ,
** 填充变量名, startpc,endpc稍后再处理
**
*/
static void new_localvar (LexState *ls, TString *name, int n) {
  FuncState *fs = ls->fs;
  luaY_checklimit(fs, fs->nactvar+n+1, LUAI_MAXVARS, "local variables");
  /* 设置actvar 到 Proto.nlocvars 的映射 */
  /* 这里仅设置了变量的name, 尚未设置startpc,endpc */
  fs->actvar[fs->nactvar+n] = cast(unsigned short, registerlocalvar(ls, name));
  //printf("......... %d->%d", fs->nactvar+n, fs->actvar[fs->nactvar+n]);
}

/* 
** 更新!!! FunState.nactvar 数量，更新 Proto.locvars.startpc 
** 一次性生成多个locvar时，nvars可以告诉本函数方便一次性调整到位
*/
static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  
  /* 更新fs中当前激活的locvar数量 */
  fs->nactvar = cast_byte(fs->nactvar + nvars);	

  /* 更新localvar的startpc 
  ** 对应chunk结束时再更新endpc，也只有那个时候才能确切的知道endpc 
  */
  for (; nvars; nvars--) {
    getlocvar(fs, fs->nactvar - nvars).startpc = fs->pc;
  }
}

/* 确定一批actvar的endpc 
** 仔细看这个函数，很有意思哈（结合 new_localvar adjustlocalvars 一起看 ）
*/
static void removevars (LexState *ls, int tolevel) {
  FuncState *fs = ls->fs;
  while (fs->nactvar > tolevel)	/* 这里tolevel是指block结束时对应的pc.idx */
    getlocvar(fs, --fs->nactvar).endpc = fs->pc;	/* 离开block时,关闭block内actvar */
}

/* 查找一个upvalue,返回其在upval数组中的索引，没有则构建 */
static int indexupvalue (FuncState *fs, TString *name, expdesc *v) {
  int i;
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  /* 当前存在的upvalue中已存在吗? */
  for (i=0; i<f->nups; i++) {
    if (fs->upvalues[i].k == v->k &&			/* 类型为VUPVAL */
		fs->upvalues[i].info == v->u.s.info) {	/* 在proto中的索引一致 */
      lua_assert(f->upvalues[i] == name);		/* 名字就必须一致了 */
      return i;
    }
  }

  /* new one */
  
  /* 数组容量不够则扩大 */
  luaY_checklimit(fs, f->nups + 1, LUAI_MAXUPVALUES, "upvalues");
  luaM_growvector(fs->L, f->upvalues, f->nups, f->sizeupvalues,
                  TString *, MAX_INT, "");
  while (oldsize < f->sizeupvalues)
  	f->upvalues[oldsize++] = NULL;
  
  f->upvalues[f->nups] = name;
  luaC_objbarrier(fs->L, f, name);
  lua_assert(v->k == VLOCAL || v->k == VUPVAL);	/* 这里的v->k==VLOCAL ? */
  /* 更新到fs */
  fs->upvalues[f->nups].k = cast_byte(v->k);
  fs->upvalues[f->nups].info = cast_byte(v->u.s.info);
  return f->nups++;
}

/* 尝试在当前fs中匹配激活状态的locvar */
static int searchvar (FuncState *fs, TString *n) {
  int i;
  for (i=fs->nactvar-1; i >= 0; i--) {
    if (n == getlocvar(fs, i).varname)
      return i;
  }
  return -1;  /* not found */
}

/* fs中的locvar在其它函数中被当作upval引用
** 标记fs中对应的block，你有变量是其它fs的upval
** level:actvar在reg数组中的索引
*/
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  /* 这个标记过程的逻辑蛮有意思的 */
  while (bl && bl->nactvar > level)
  	bl = bl->previous;
  if (bl)
  	bl->upval = 1;
}

/* 查找变量名对应的表达式类型的值类型(VLOCAL还是?)
**
** 仔细看这个函数的逻辑，搞明白关于变量的查找过程
** step1:先在本地fs6激活中的locvar查找，找到则返回VLOCAL
** step2:往前一个fs5中的激活中的locvar查找，找不到，继续下一步step3
** step3:继续往前一个fs1中的激活的locvar查找，一直到fs1->pre为空，则
**     可以确定var是一个VGLOBAL
** step4:在某一个fs3中的激活中的locvar被找到，则标记此fs3中的bl表示你的某个var被其它fsX当作upval了
**        往前退，在fs4中的upval中新增一条信息(此upval在父fs3中是VLOCAL,且在fs3的actvar中的索引是多少)
**        再往前退，在fs5中的upval中新增一条信息(此upval在父fs4中是UPVAL，且在fs4的upvalues的索引是多少)
**        再往前退，直到初始的fs6，在fs6中的upval中新增一条信息（此upval在父fs5中是UPVAL,且在fs5的upvalues的索引是多少)
**
** 理论上可以优化下：在本地locvar找不到时，先不要在父fs中找，而是在本fs的upvales中找下
*/
static int singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  if (fs == NULL) {  /* no more levels? */
  	/* default is global variable, NO_REG:表示此全局变量尚未决定其寄存器的位置
	** 全局变量对应的NAME在p中常量表的索引由singlevar()函数来处理
  	*/
    init_exp(var, VGLOBAL, NO_REG);  
    return VGLOBAL;	/* 往外一层一层都找不到时，则认为它是全局变量 */
  }
  else {
  	/* 在激活的locvar中找到了，则是本地变量 */
    int v = searchvar(fs, n);  /* look up at current level */
    if (v >= 0) {
      init_exp(var, VLOCAL, v);
      if (!base)
        markupval(fs, v);  /* local will be used as an upval */
      return VLOCAL;
    }
    else {  /* not found at current level; try upper one */
      if (singlevaraux(fs->prev, n, var, 0) == VGLOBAL)	/* 都没找到，则是全局变量 */
        return VGLOBAL;
	  /* 父func中找到，在自己的fun中算upval */
      var->u.s.info = indexupvalue(fs, n, var);  /* else was LOCAL or UPVAL */
      var->k = VUPVAL;  /* upvalue in this level */
      return VUPVAL;
    }
  }
}

/* 
** step1: 检查ls->t.token的类型为TK_NAME，读取下一个TOKEN
** step2: 根据上一个token的NAME，确定其变量(VLOCAL,VGLOBAL还是VUPVAL？)类型，
**            后填充expdesc.u.s.info信息
*/
static void singlevar (LexState *ls, expdesc *var) {
  TString *varname = str_checkname(ls);
  FuncState *fs = ls->fs;

  /*
  ** OP_GETGLOBAL A Bx R(A) := Gbl[Kst(Bx)]
  ** OP_SETGLOBAL A Bx Gbl[Kst(Bx)] := R(A)
  ** 全局变量的指令需要知道表示全局变量的NAME在常量表中的idx，
  **     理解这一点就明白了下面var->u.s.info的赋值的意义
  */
  if (singlevaraux(fs, varname, var, 1) == VGLOBAL) {	/* VLOCVAR,VUPVAL在singlevaraux中已被init_exp初始化 */
    var->u.s.info = luaK_stringK(fs, varname);  /* info points to global name */
  }
}

/* 针对 nvars = nexps 赋值进行调整
** 如果右边少了则给左边赋NIL
** 如果右边有call,...则确定期待的返回值个数
**
** ！！！ 如果右边多了，本函数未处理！！！
*/
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  /* extra: 右边除掉fun和...外，表达式的数量少于左边的val的数量的情况下，缺失的数量 */
  int extra = nvars - nexps;	
  if (hasmultret(e->k)) {	/* exp的类型为VARARG或CALL */
    extra++;  /* includes call itself：除开VARARG和CALL本身 */
	
	/*  varCtn ?= expCnt + e.retCnt 分析需要表达式e返回值的个数来平衡左右两边的数量
	** 左边varCnt <= expCnt 无需e返回值
	** 左边varCNT > expCnt, 需要e返回X个值，以便varCnt = expCnt + e.retCnt
    */
    if (extra < 0) extra = 0;
	
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */

	/* 预定上面变参需要的多余的寄存器，预定出来干什么了? 
	** 用于将变参的值从尾到头拷贝给var
	** eg: a, b, c, d = e, f()
	**     右边在explist1处理时f()仅期待返回一个参数，且fs->freereg也是正常的往后移动了1个slot
	**     	   上面luaK_setreturns()函数仅修改了OP_CALL并没有对照移动fs->freereg，此时fs->freereg对应左边变量b。
	**     这里根据需要将其往右边移动2个slot，此时fs->freereg对应左边变量d了，这样就即告诉f需要3个参数，又多预留了
	**         2个参数的位置出来(f原本计划返回1个，故而这里多了2个),逻辑就对上了。明白了吧！！！！！
	*/
    if (extra > 1) luaK_reserveregs(fs, extra-1);	
  }
  else {
    if (e->k != VVOID) luaK_exp2nextreg(fs, e);  /* close last expression */
    if (extra > 0) {	/* nexps：包含右边最后一个exp */
      int reg = fs->freereg;
	  /* 为左边多出来的var申请reg,然后填NIL */
      luaK_reserveregs(fs, extra);
      luaK_nil(fs, reg, extra);
    }
  }
}

/* 进入一个新的block */
static void enterlevel (LexState *ls) {
  if (++ls->L->nCcalls > LUAI_MAXCCALLS)
	luaX_lexerror(ls, "chunk has too many syntax levels", 0);
}


#define leavelevel(ls)	((ls)->L->nCcalls--)

/* 进入块时，初始化block信息 */
static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isbreakable) {
  bl->breaklist = NO_JUMP;			/* block中的break待回填的跳转链表为NUL */
  bl->isbreakable = isbreakable;	/* 如果block不支持break,那么不能出现BREAK（eg:if中的body）*/
  bl->nactvar = fs->nactvar;		/* 记录进入block时的nactvar，以便退出block时回滚-重利用reg和确定block中声明的local变量的生命周期 */
  bl->upval = 0;					/* block中暂时还没有upval，后面有了再更新此域 */
  
  /* 这里和ls->fs的切换是类似的逻辑 */
  bl->previous = fs->bl;
  fs->bl = bl;

  /* 判断：进入block时fs对应的下一个freereg的索引和fs->nactvar相匹配 */
  lua_assert(fs->freereg == fs->nactvar);
}


static void leaveblock (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  fs->bl = bl->previous;
  
  /* 确定本block内激活的var的生存周期的endpc
  ** 更新fs->nactvar
  */
  removevars(fs->ls, bl->nactvar);

  /* OP_CLOSE A close all variables in the stack up to (>=) R(A) 
  ** 生成OP_CLOSE指令以便能正确的处理upval
  */
  if (bl->upval) {
    luaK_codeABC(fs, OP_CLOSE, bl->nactvar, 0, 0);
  }
  
  /* a block either controls scope or breaks (never both) */
  lua_assert(!bl->isbreakable || !bl->upval);	/* TODOLOOK 还不是太理解 */
  
  lua_assert(bl->nactvar == fs->nactvar);	/* 这个必须保证 */
  fs->freereg = fs->nactvar;  /* free registers */

  /* 将break语句对应的待回填的跳转链表挂到fs->jpc上，等待回填 */
  luaK_patchtohere(fs, bl->breaklist);
}


static void pushclosure (LexState *ls, FuncState *func, expdesc *v) {
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int oldsize = f->sizep;
  int i;
  luaM_growvector(ls->L, f->p, fs->np, f->sizep, Proto *,
                  MAXARG_Bx, "constant table overflow");
  while (oldsize < f->sizep) f->p[oldsize++] = NULL;
  /* 放入子函数定义数组中，新增对其引用 */
  f->p[fs->np++] = func->f;
  luaC_objbarrier(ls->L, f, func->f);

  /* 生成 OP_CLOSURE指令 */
  init_exp(v, VRELOCABLE, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np-1));
  /* 处理upval */
  for (i=0; i<func->f->nups; i++) {
  	/* 这里终于明白k的不同类型的作用了吧 */
    OpCode o = (func->upvalues[i].k == VLOCAL) ? OP_MOVE : OP_GETUPVAL;
	/* R(A) 被暗含在i上了 */
    luaK_codeABC(fs, o, 0, func->upvalues[i].info, 0);
  }
}

/* 开始编译函数 */
static void open_func (LexState *ls, FuncState *fs) {
  lua_State *L = ls->L;
  
  fs->L = L;	
  Proto *f = luaF_newproto(L);
  fs->ls = ls;
  fs->f = f;	/* funState 在编译哪个Proto */
  
  /* ls指向最新的一个FuncState,这里可以猜测，只有先编译完了子函数才有可能编译父函数 */
  fs->prev = ls->fs;  /* linked list of funcstates */
  ls->fs = fs;
  
  fs->pc = 0;
  fs->lasttarget = -1;
  fs->jpc = NO_JUMP;
  fs->freereg = 0;
  fs->nk = 0;
  fs->np = 0;
  fs->nlocvars = 0;
  fs->nactvar = 0;
  fs->bl = NULL;	/* 这里是NULL */
  f->source = ls->source;
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  fs->h = luaH_new(L, 0, 0);
  
  /* anchor table of constants and prototype (to avoid being collected)
  ** 常量和原型的锚表（避免被收集）
  */
  sethvalue2s(L, L->top, fs->h);
  incr_top(L);	/* 放到堆栈上可避免被gc,如果编译失败stack回缩，则可自动被gc（没有被其它obj引用的话 ） */
  setptvalue2s(L, L->top, f);
  incr_top(L);
  
}


static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;

  /* 关闭还处于激活状态的actvar(设置endpc) */
  removevars(ls, 0);

  /* 自动补一个 OP_RETURN 指令 */
  luaK_ret(fs, 0, 0);  /* final return */

  /* 释放多余的mem */
  luaM_reallocvector(L, f->code, f->sizecode, fs->pc, Instruction);
  f->sizecode = fs->pc;
  
  luaM_reallocvector(L, f->lineinfo, f->sizelineinfo, fs->pc, int);
  f->sizelineinfo = fs->pc;
  
  luaM_reallocvector(L, f->k, f->sizek, fs->nk, TValue);
  f->sizek = fs->nk;
  
  luaM_reallocvector(L, f->p, f->sizep, fs->np, Proto *);
  f->sizep = fs->np;
  
  luaM_reallocvector(L, f->locvars, f->sizelocvars, fs->nlocvars, LocVar);
  f->sizelocvars = fs->nlocvars;
  
  luaM_reallocvector(L, f->upvalues, f->sizeupvalues, f->nups, TString *);
  f->sizeupvalues = f->nups;
  
  lua_assert(luaG_checkcode(f));	/* 检查生成的字节码是否有明显的问题 */
  lua_assert(fs->bl == NULL);
  
  /* 本子函数编译完毕，切换到母函数中去 */
  ls->fs = fs->prev;
  
  /* last token read was anchored(锚定) in defunct function; must reanchor(锚) it 
  ** 本函数的函数名在上一层函数中可能是个常量，这里注册到其常量表中
  */
  if (fs) 
  	anchor_token(ls);

  /* 
  ** 本函数在open_func中新增的两个gc变量占用了堆栈空间，
  ** 编译结束，其被其他gc变量引用不会被gc了，释放其占用的stack空间
  */
  L->top -= 2;  /* remove table and prototype from the stack */
}


Proto *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff, const char *name) {
  struct LexState lexstate;
  struct FuncState funcstate;	/* mainFunc */
  
  lexstate.buff = buff;
  /* 设置input信息，但，buff在上面就设置了，有点意思吧，z和buff对于lexState是有点不同的 */
  luaX_setinput(L, &lexstate, z, luaS_new(L, name));

  /* 一个lua文件，编译模块将其当做一个函数来看待
  ** 函数原型 function (...)
  **          end
  **
  ** BNF funcbody ::= `(´ [parlist] `)´ block end
  */
  open_func(&lexstate, &funcstate);
  funcstate.f->is_vararg = VARARG_ISVARARG;  /* main func. is always vararg，哈哈知道lua文件一般开头的local modName=...的语法支撑了吧 */
  luaX_next(&lexstate);  /* read first token */
  chunk(&lexstate);
  check(&lexstate, TK_EOS);	/* 直到编译到文件EOF才结束编译流程 */
  close_func(&lexstate);
  
  lua_assert(lexstate.fs == NULL);		/* lexstate下不应该还有未编译完的funState了 */
  lua_assert(funcstate.prev == NULL);	/* 已编译完的主函数上面还有其它函数，不可能的嘛 */
  lua_assert(funcstate.f->nups == 0);	/* 编译结束，主函数不应该有nups了 */
  return funcstate.f;
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/

/* A.B, A:B 
** 对前缀生成必要的估值指令，放入free'reg(若有必要)
** 用上述值作为A,再和B一起生成新的VINDEXED表达式
*/
static void field (LexState *ls, expdesc *v) {
  /* field -> ['.' | ':'] NAME */
  FuncState *fs = ls->fs;
  expdesc key;
  
  /* 将前缀(a.b.c中的a.b或a.b中的a)加载到reg中，若前缀已在寄存器中则无需处理(A=VLOCAL(a)) 
  ** OP_GETTABLE 	A B C 	R(A) := R(B)[RK(C)]
  ** OP_SETTABLE   	A B C   R(A)[RK(B)] := RK(C)
  **
  ** prefixexp.key 前缀prefixexp不一定已经被加载到reg数组中了，指令需要先加载到reg中，
  ** 故而在解析key之前先加载到reg中再解析key
  */
  luaK_exp2anyreg(fs, v);	
  
  luaX_next(ls); 		/* skip the dot or colon */
  
  checkname(ls, &key);	/* 读取NAME这个域的常量exp并返回给key */		

  /* 生成新的VINDEXED表达式（求值指令，指令的目标寄存器尚未处理） */
  luaK_indexed(fs, v, &key);	
}


static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  luaX_next(ls);  /* skip the '[' */
  expr(ls, v);
  luaK_exp2val(ls->fs, v);
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

/* 构造表   tbl {a, b, c=val, d.e} */
struct ConsControl {
  expdesc *t;  /* table descriptor 指代本表的expdesc */
  expdesc v;  /* last list item read: 指代正在分析到的哪一个元素eg(b),对于c=val用不上v */
  int nh;  /* total number of `record' elements */
  int na;  /* total number of array elements */
  int tostore;  /* number of array elements pending to be stored */
};

/* 形如 local tbl = { x = y, [a] = b,}
** 中的x=1,这种指定tbl[k]=v的表达式
*/
static void recfield (LexState *ls, struct ConsControl *cc) {
  /* recfield -> (NAME | `['exp1`]') = exp1 */
  FuncState *fs = ls->fs;
  int reg = ls->fs->freereg;
  expdesc key, val;
  int rkkey;

  /* 对key生成加载指令 */
  if (ls->t.token == TK_NAME) {
    luaY_checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
    checkname(ls, &key);
  }
  else  /* ls->t.token == '[' */
    yindex(ls, &key);
  
  cc->nh++;
  checknext(ls, '=');
  /* 回填上述k的加载指令,将表达式的值SET到next'free'reg上 */
  rkkey = luaK_exp2RK(fs, &key);

  /* 初始化表达式val */
  expr(ls, &val);
  /* 先生成对val的LOAD_XXX加载指令，后生成OP_SETTABLE */
  luaK_codeABC(fs, OP_SETTABLE, cc->t->u.s.info, rkkey, luaK_exp2RK(fs, &val));
  
  fs->freereg = reg;  /* free registers 释放表达式占用的临时寄存器 */
}

/* local tbl = {a,b,c,d}
** 解析完毕b,关闭对b的解析
*/
static void closelistfield (FuncState *fs, struct ConsControl *cc) {
  if (cc->v.k == VVOID) return;  /* there is no list item */
  luaK_exp2nextreg(fs, &cc->v);
  cc->v.k = VVOID;	/* 释放表达式 */
  if (cc->tostore == LFIELDS_PER_FLUSH) {
    luaK_setlist(fs, cc->t->u.s.info, cc->na, cc->tostore);  /* flush */
    cc->tostore = 0;  /* no more items pending */
  }
}

/* local tbl = {a, b, c, d}
** 结束d的解析后，调到这里 
** 逻辑独立出来是因为函数调用作为表的最后一个元素和非最后一个元素，其期望对其返回值的个数是不一样的
*/
static void lastlistfield (FuncState *fs, struct ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {	/* 最后一个listfield是变参:作为explist1中的子项，默认仅返回单个参数，这里根据实际需求进行修正 */
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.s.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    if (cc->v.k != VVOID)
      luaK_exp2nextreg(fs, &cc->v);	/* 加载最后一个表达式到next'free'reg */
    luaK_setlist(fs, cc->t->u.s.info, cc->na, cc->tostore);
  }
}

/* tbl = {a,b, c = 100} 数组中单个field eg:a
*/
static void listfield (LexState *ls, struct ConsControl *cc) {
  expr(ls, &cc->v);
  luaY_checklimit(ls->fs, cc->na, MAX_INT, "items in a constructor");
  cc->na++;
  cc->tostore++;
}

static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> ?? */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  
  int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
  struct ConsControl cc;
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  
  /* 初始化table的exp */
  init_exp(t, VRELOCABLE, pc);
  
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  
  luaK_exp2nextreg(ls->fs, t);  /* fix it at stack top (for gc) */
  checknext(ls, '{');
  do {
    lua_assert(cc.v.k == VVOID || cc.tostore > 0);
    if (ls->t.token == '}') break;	/* 表被遍历完毕 */
    closelistfield(fs, &cc);
    switch(ls->t.token) {
      case TK_NAME: {  /* may be listfields or recfields */
        luaX_lookahead(ls);
        if (ls->lookahead.token != '=')  /* expression? */
          listfield(ls, &cc);
        else
          recfield(ls, &cc);
        break;
      }
      case '[': {  /* constructor_item -> recfield */
        recfield(ls, &cc);
        break;
      }
      default: {  /* constructor_part -> listfield */
        listfield(ls, &cc);
        break;
      }
    }
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, '}', '{', line);
  lastlistfield(fs, &cc);

  /* 更新OP_NEWTABLE指令的参数 */
  SETARG_B(fs->f->code[pc], luaO_int2fb(cc.na)); /* set initial array size */
  SETARG_C(fs->f->code[pc], luaO_int2fb(cc.nh));  /* set initial table size */
}

/* }====================================================================== */


/* 
** 解析函数的显式形参列表（对于modName:sub(x,y) 这种隐含的第一个self参数，在外面已被解析完毕
*/
static void parlist (LexState *ls) {
  /* parlist -> [ param { `,' param } ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  int nparams = 0;	/* 固定参数的个数(不包含...变参) */
  f->is_vararg = 0;
  if (ls->t.token != ')') {  /* is `parlist' not empty? */
    do {
      switch (ls->t.token) {
        case TK_NAME: {  /* param -> NAME */
          new_localvar(ls, str_checkname(ls), nparams++);
		  /* adjustlocalvars 在下面调用：一次性调整到位 */
          break;
        }
        case TK_DOTS: {  /* param -> `...' */
          luaX_next(ls);
#if defined(LUA_COMPAT_VARARG)
          /* use `arg' as default name */
          new_localvarliteral(ls, "arg", nparams++);
          f->is_vararg = VARARG_HASARG | VARARG_NEEDSARG;
#endif
          f->is_vararg |= VARARG_ISVARARG;
          break;
        }
        default: luaX_syntaxerror(ls, "<name> or " LUA_QL("...") " expected");
      }
    } while (!f->is_vararg && testnext(ls, ','));	/* 这里看得出来 ... 只能是最后一个形参 */
  }else {
  	// function name() body end 显式形参为空
  }
  
  adjustlocalvars(ls, nparams);
  f->numparams = cast_byte(fs->nactvar - (f->is_vararg & VARARG_HASARG));
  luaK_reserveregs(fs, fs->nactvar);  /* reserve register for parameters */
}

/* 解析函数形参和函数体 */
static void body (LexState *ls, expdesc *e, int needself, int line) {
  /* body ->  `(' parlist `)' chunk END */
  FuncState new_fs;
  
  /* ! 更新ls中的fs变量，完成编译对象fs的切换 */
  open_func(ls, &new_fs);
  
  /* 新函数从哪一行开始定义 */
  new_fs.f->linedefined = line;
  
  /* local name = function () 或者 local function name() 这两种函数定义格式对应的函数都是从‘(’开始，*/
  checknext(ls, '(');

  /* 处理 function modName:sub() body end 这种情况，参考funcname()代码可知 */
  if (needself) {	
    new_localvarliteral(ls, "self", 0);
	/* self当作本fs的第一个locvar，占用一个正常的locvar对应的reg */
    adjustlocalvars(ls, 1);
  }
  /* 解析显式形参 */
  parlist(ls);
  
  checknext(ls, ')');

  /* 解析函数体 */
  chunk(ls);
  
  /* 函数定义结束于哪一行 */
  new_fs.f->lastlinedefined = ls->linenumber;
  
  check_match(ls, TK_END, TK_FUNCTION, line);
  
  close_func(ls);
  
  pushclosure(ls, &new_fs, e);
}

/* 解析表达式，返回表达式中的项的数量
** 边解析表达式，边生成对应的CP_XXX指令(最后一个表达式可能是变参表达式，故留给上层业务处理)
**
** NOTE: 一个表达式的项，仅占用一个reg(对于a.b.c.d这种解析过程中临时用到寄存器的，
**        会释放占用临时寄存器，最后仅保留xxx.d这个最终结果占用的哪一个寄存器)
**        不明白注释，用ChunkySpy运行一下func(a.b.c.d.f)就明白了
**
** 对(VCALL,VARGARG)暂定仅期待返回一个参数，这就是函数名中1的由来吧
**   真正期待的返回参数个数的修正交由上层业务处理(eg:luaK_setmultret,  )
*/
static int explist1 (LexState *ls, expdesc *v) {
  /* explist1 -> expr { `,' expr } */
  int n = 1;  /* at least one expression */
  expr(ls, v);
  while (testnext(ls, ',')) {
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}

/* 
** !!!!!!!! KEYFUNCTION
** 多多多多多读几遍代码，一定要测定弄懂本函数的设计思路和逻辑
**
** funcargs -> `(' [ explist1 ] `)' | constructor | STRING 
** 
** eg: fun(a,b,c,d)
**   首先解析的是函数参数的exp,最后才是将整个函数调用表达式作为一个整体VCALL表达式的exp赋值给f
**   (f对上是一个VCALL的exp来看待的),f作为exp的最终reg地址也就是函数名表达式的reg的地址(也就是传入的f->u.s.info的值)
*/
static void funcargs (LexState *ls, expdesc *f) {
  FuncState *fs = ls->fs;
  expdesc args;
  int base, nparams;
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> `(' [ explist1 ] `)' */
      if (line != ls->lastline)
        luaX_syntaxerror(ls,"ambiguous syntax (function call x new statement)");
      luaX_next(ls);
      if (ls->t.token == ')')  /* arg list is empty? */
        args.k = VVOID;
      else {
        explist1(ls, &args);
        luaK_setmultret(fs, &args);	/* 修正变参表达式的返回值个数：变参作为函数调用传入的最后一个参数时，期望返回全部参数 */
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '{': {  /* funcargs -> constructor */
      constructor(ls, &args);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      codestring(ls, &args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use `seminfo' before `next' */
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
      return;
    }
  }

  /* 函数名必须已经解析完毕，已经在reg中了 */
  lua_assert(f->k == VNONRELOC);
  
  base = f->u.s.info;  /* base register for call */
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    nparams = fs->freereg - (base+1);
  }
  
  /* C固定为2：表示期待单个返回值！,  如果不是单个，则需在上层逻辑中予以修正 
  ** ！这里得知先生成的准备参数的指令，再生成的OP_CALL指令
  */
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  
  luaK_fixline(fs, line);
  
  /* chunkstat 在解析完一个stat后会根据ls->fs->nactvar再一次调整fs->freereg 
  **
  ** funcargs:作为explist1的子域，这里默认要求返回一个返回值，和上面生成OP_CALL指令中的参数也是对应的
  **   eg1:(请求返回单个值) a, b, c = fun(), g.h, i  这里+1，可以让g.h间接表达式的CP指令能放入到后一个free'reg中
  ** 
  **
  **   eg2:(请求不要返回值) 如果是funA()
  **          local b
  **            这种不要任何返回值的情况，chunk()函数在编译本exp所在的stat后会再次调整fs->freereg（编译local b之前），
  **            funA()被编译结束后随即回收了这里空出来的一个free'reg
  **
  **
  **   eg3:(请求返回多个值) 如果要求funB返回多个，那么funcargs必然是作为explist1的最后一个参数,
  **           后面不再跟随任何exp了，随着本exp所在的stat后chunk函数会 将freereg变量修正回来(也就不存在浪费)，
  **           这一点eg2是一样的
  **            	local tbl = {a, b, funM()}
  **            	funA(b, funC())
  **				funD(t,y,funI(funP()))
  **          		local g
  */
  fs->freereg = base+1;  /* call remove function and arguments and leaves
                            (unless changed) one result */
  //printf("######### reg.idx(%d)\n", fs->freereg);							
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/


static void prefixexp (LexState *ls, expdesc *v) {
  /* prefixexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      expr(ls, v);
      check_match(ls, ')', '(', line);
      luaK_dischargevars(ls->fs, v);
      return;
    }
    case TK_NAME: {
      /* 确定当前ls->t.token的变量类型(VLOCAL,VGLOBAL还是VUPVAL？)
      **     填充expdesc.u.s.info信息
      ** 读取下一个Token
      */
      singlevar(ls, v); 
      return;
    }
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
      return;
    }
  }
}

/* primary：基本的 */
static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp -> prefixexp { `.' NAME | `[' exp `]' | `:' NAME funcargs | funcargs } */
  FuncState *fs = ls->fs;
  
  prefixexp(ls, v);
  
  for (;;) {
    switch (ls->t.token) {
      case '.': {  /* field */
        field(ls, v);
        break;
      }
      case '[': {  /* `[' exp1 `]' */
        expdesc key;
		/* 
		** OP_GETTABLE A B C R(A) := R(B)[RK(C)]
		** exp = v[key] 前缀v不一定是VNONRELOC，在解析exp时，
		** 需加载到reg中再解析后面的key
		*/
        luaK_exp2anyreg(fs, v);	/*  */
        yindex(ls, &key);
        luaK_indexed(fs, v, &key);
        break;
      }
      case ':': {  /* `:' NAME funcargs */
        expdesc key;
        luaX_next(ls);
        checkname(ls, &key);
        luaK_self(fs, v, &key);
        funcargs(ls, v);
        break;
      }
      case '(': case TK_STRING: case '{': {  /* funcargs 函数调用 */
	  	/* expr(arglist) 参数列表可能需要占用reg，这里需要将expr先存到reg中 */
        luaK_exp2nextreg(fs, v);
        funcargs(ls, v);
        break;
      }
      default: return;
    }
  }
}

/* 对表达式进行初始化，间接表达式则生成求src.val的指令, 等待回填dst.reg */
static void simpleexp (LexState *ls, expdesc *v) {
  /* simpleexp -> NUMBER | STRING | NIL | true | false | ... |
                  constructor | FUNCTION body | primaryexp */
  switch (ls->t.token) {
    case TK_NUMBER: {
      init_exp(v, VKNUM, 0);
      v->u.nval = ls->t.seminfo.r;	/* 直接赋值NUMBER */
      break;
    }
    case TK_STRING: {
      codestring(ls, v, ls->t.seminfo.ts);
      break;
    }
    case TK_NIL: {
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      init_exp(v, VFALSE, 0);
      break;
    }
    case TK_DOTS: {  /* vararg */
      FuncState *fs = ls->fs;
	  /* 在非变参函数内使用"变参"，明显是个错误 */
      check_condition(ls, fs->f->is_vararg,
                      "cannot use " LUA_QL("...") " outside a vararg function");
      fs->f->is_vararg &= ~VARARG_NEEDSARG;  /* don't need 'arg' */
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, 1, 0));	/* 这里B=0，表示期待一个参数和funcall仅返回一个参数是一样的 */
      break;
    }
    case '{': {  /* constructor */
      constructor(ls, v);
      return;
    }
    case TK_FUNCTION: {
      luaX_next(ls);
      body(ls, v, 0, ls->linenumber);
      return;
    }
    default: {
      primaryexp(ls, v);
      return;
    }
  }
  luaX_next(ls);
}

/* 返回TK可能的一元操作符TK */
static UnOpr getunopr (int op) {
  switch (op) {
    case TK_NOT: return OPR_NOT;
    case '-': return OPR_MINUS;
    case '#': return OPR_LEN;
    default: return OPR_NOUNOPR;
  }
}


static BinOpr getbinopr (int op) {
  switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '/': return OPR_DIV;
    case '%': return OPR_MOD;
    case '^': return OPR_POW;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
  }
}

/* 操作符的优先级分左右：用于处理结合性？ */
static const struct {
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {6, 6}, {6, 6}, {7, 7}, {7, 7}, {7, 7},  /* `+' `-' `/' `%' */
   {10, 9}, {5, 4},                 /* power and concat (right associative) */
   {3, 3}, {3, 3},                  /* equality and inequality */
   {3, 3}, {3, 3}, {3, 3}, {3, 3},  /* order */
   {2, 2}, {1, 1}                   /* logical (and/or) */
};

#define UNARY_PRIORITY	8  /* priority for unary operators,一元操作符的优先级 */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where `binop' is any binary operator with a priority higher than `limit'
** 操作符表达式
*/
static BinOpr subexpr (LexState *ls, expdesc *v, unsigned int limit) {
  BinOpr op;
  UnOpr uop;
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  /* 表达式前有一元操作符 - not # */
  if (uop != OPR_NOUNOPR) {
    luaX_next(ls);
    subexpr(ls, v, UNARY_PRIORITY);
    luaK_prefix(ls->fs, uop, v);
  } else {
  	simpleexp(ls, v);
  }
  
  /* expand while operators have priorities higher than `limit' */
  op = getbinopr(ls->t.token);
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    expdesc v2;
    BinOpr nextop;
    luaX_next(ls);
	
	/* a = b + c * d 解析操作符左边的表达式b
	** 进入解析c表达式的操作前，将b表达式存一个档
	*/
    luaK_infix(ls->fs, op, v);
	
    /* read sub-expression with higher priority 
    ** a = b + c * d
    ** 对于c表达式而言， 有两个操作符在同一时间试图将其组合到自己麾下，一个是左边的加号，另外一个是右边的乘号
    ** 对于加号而言，c在加号的右边，这里是.right的优先级，进入递归后,乘号是左边的.left优先级， 
	*/
    nextop = subexpr(ls, &v2, priority[op].right);

	/* 将b和(c*d)组合起来作为一个整体表达式返回给上层逻辑 */
    luaK_posfix(ls->fs, op, v, &v2);
    op = nextop;
  }
  leavelevel(ls);
  return op;  /* return first untreated operator */
}

/* 
** eg.1 local a = b * (c+d+e)
**       解析到'('时进入此函数，并且在')'后才结束，函数将c+d+e作为一个整体返回给上层
** 
*/
static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, 0);	/* 0的传入意味着将表达式前面的操作符的优先级视为0，那么c会和'+'结合而不是前面的'*'结合了 */
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/

/* 紧接着出现的block是一个崭新的block吗 */
static int block_follow (int token) {
  switch (token) {	
    case TK_ELSE: case TK_ELSEIF: /* else, elseif 是一个新block的开始 */
	case TK_UNTIL: 	/* REPEAT block UNTIL cond cond是一个新block的开始(和block没关系了) */
	case TK_END:   	/* end表示块结束，后面当然是新block的开始 */
    case TK_EOS:	/* 文件末尾了，当前块肯定结束了 */
      return 1;
    default: return 0;
  }
}


static void block (LexState *ls) {
  /* block -> chunk */
  FuncState *fs = ls->fs;
  BlockCnt bl;
  
  /* isbreakable==0，意味着不可以挂bl.breaklist.jmp 
  ** 如果需要like WHILE(cond) DO
  **                  DO
  **					break
  **                  END
  **              END
  ** 要实现跳出WHILE.STAT中的break功能，在block函数之前调用enterblock(,,isbreakable==1)
  ** 示例直接看whilestat即可
  **
  ** 这里之所以不准许挂bl.breaklist.jmp可能是因为 这个block函数是最内层的 DO stat END 的实现了
  */
  enterblock(fs, &bl, 0);	
  chunk(ls);
  
  /* 看上面注释 */
  lua_assert(bl.breaklist == NO_JUMP);
  /* 有进有出 */
  leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign {
  struct LHS_assign *prev;
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to a local variable, the local variable
** is needed in a previous assignment (to a table). If so, save original
** local value in a safe place and use this safe copy in the previous
** assignment.
**
** local a, b = {}, 10
** a[b], b = 20, 30
** print(a[10], a[30])
*/
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  int extra = fs->freereg;  /* eventual position to save local variable */
  int conflict = 0;
  for (; lh; lh = lh->prev) {
    if (lh->v.k == VINDEXED) {
      if (lh->v.u.s.info == v->u.s.info) {  /* conflict? */
        conflict = 1;
        lh->v.u.s.info = extra;  /* previous assignment will use safe copy */
      }
      if (lh->v.u.s.aux == v->u.s.info) {  /* conflict? */
        conflict = 1;
        lh->v.u.s.aux = extra;  /* previous assignment will use safe copy */
      }
    }
  }
  /* 将冲突的域单独拷贝一份出来 */
  if (conflict) {
    luaK_codeABC(fs, OP_MOVE, fs->freereg, v->u.s.info, 0);  /* make copy */
    luaK_reserveregs(fs, 1);
  }
}

/*
** KEYCODE 
** 多多多读几遍，一定要搞懂
*/
static void assignment (LexState *ls, struct LHS_assign *lh, int nvars) {
  expdesc e;
  /* 对=左边的exp还是又类型要求的 */
  check_condition(ls, VLOCAL <= lh->v.k && lh->v.k <= VINDEXED,
                      "syntax error");
  if (testnext(ls, ',')) {  /* assignment -> `,' primaryexp assignment */
    struct LHS_assign nv;
    nv.prev = lh;
    primaryexp(ls, &nv.v);
    if (nv.v.k == VLOCAL)
      check_conflict(ls, lh, &nv.v);
    luaY_checklimit(ls->fs, nvars, LUAI_MAXCCALLS - ls->L->nCcalls,
                    "variables in assignment");
    assignment(ls, &nv, nvars+1);
  }
  else {  /* assignment -> `=' explist1 */
    int nexps;
    checknext(ls, '=');
    nexps = explist1(ls, &e);
    if (nexps != nvars) {
      adjust_assign(ls, nvars, nexps, &e);
      if (nexps > nvars)
        ls->fs->freereg -= nexps - nvars;  /* remove extra values：右边的参数数量多余左边的，直接忽略 */
    }
    else {	/* eg: a = b, a = b(),=号左右两边相等且只有一个项的情况*/
	  /* 变量和表达式数量相等，那么如果最后一个表达式是变参表达式，
	  ** 则强制更新其期待的返回值个数为(多了也没用,少了也不行) 
	  */
      luaK_setoneret(ls->fs, &e);  /* close last expression */
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }

  /* default assignment: 这句话的意思是：构造一个默认的用于赋值的stat中exp(表达式对应的reg已准备妥当！) */
  init_exp(&e, VNONRELOC, ls->fs->freereg-1);  
  
  /* 
  ** !!!! 函数内部会自动缩减 --ls->fs->freereg  
  ** 	  赋值表达式left=right中的right代表的reg.addr随着自动更新(从右边往左边一步一步移动)
  */
  luaK_storevar(ls->fs, &lh->v, &e);	
}

/* WHILE-cond REPEAT-cond IF-cond ELSEIF-cond */
static int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  expr(ls, &v);  /* read condition */
  
  if (v.k == VNIL) v.k = VFALSE;  /* `falses' are all equal here */
  
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}

static void breakstat (LexState *ls) {
  FuncState *fs = ls->fs;
  BlockCnt *bl = fs->bl;
  int upval = 0;

  /* 往前一层一层的查找，找到第一个breakable的block即可 */
  while (bl && !bl->isbreakable) {
    upval |= bl->upval;	/* 这个 |= 用的妙 */
    bl = bl->previous;
  }
  
  if (!bl)
    luaX_syntaxerror(ls, "no loop to break");
  
  /* break跳转出去时需要处理前面的upval */
  if (upval)
    luaK_codeABC(fs, OP_CLOSE, bl->nactvar, 0, 0);
  
  /* 
  ** 这里生成JMP指令并挂到待回填的bl->breaklist链表上
  ** leaveblock函数中会回填bl->breaklist，完成整个JMP流程
  */
  luaK_concat(fs, &bl->breaklist, luaK_jump(fs));
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  luaX_next(ls);  /* skip WHILE */
  
  /* 提取循环的第一条指令 */
  whileinit = luaK_getlabel(fs);
  
  condexit = cond(ls);
  
  enterblock(fs, &bl, 1);
  checknext(ls, TK_DO);
  block(ls);

  /* 生成一条指向whileinit的跳转指令，从而实现while循环 */
  luaK_patchlist(fs, luaK_jump(fs), whileinit);
  
  check_match(ls, TK_END, TK_WHILE, line);	/* 关键字匹配检查 */
  /*
  ** 处理block中locvar的生命周期，回滚freereg指针，根据upval的有无生成OP_CLOSE指令
  ** 处理待回填的block->breaklist跳转指令链表
  */
  leaveblock(fs);
  
  /* 将待回填的condexit链表挂到fs->jpc上，等待生成下一条指令时，将其回填 
  ** 从而实现cond为假时跳过body的逻辑
  */
  luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}

/*
repeat
   statements
until( condition )
*/


static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL cond */
  int condexit;
  FuncState *fs = ls->fs;
  /* 循环的入口 */
  int repeat_init = luaK_getlabel(fs);
  BlockCnt bl1, bl2;
  enterblock(fs, &bl1, 1);  /* loop block */
  enterblock(fs, &bl2, 0);  /* scope block */
  luaX_next(ls);  /* skip REPEAT */
  chunk(ls);
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  condexit = cond(ls);  /* read condition (inside scope block) */
  if (!bl2.upval) {  /* no upvalues? */
    leaveblock(fs);  /* finish scope */
    luaK_patchlist(ls->fs, condexit, repeat_init);  /* close the loop */
  }
  else {  /* complete semantics when there are upvalues */
  	/* 函数内部建立一条con.true的跳转通道，跳转到REPEAT STAT之外
  	** 不然会陷入REPEAT死循环
  	*/
    breakstat(ls);  /* if condition then break */
	
    luaK_patchtohere(ls->fs, condexit);  /* else... */
    leaveblock(fs);  /* finish scope... */
    luaK_patchlist(ls->fs, luaK_jump(fs), repeat_init);  /* and repeat */
  }
  leaveblock(fs);  /* finish loop */
}


static int exp1 (LexState *ls) {
  expdesc e;
  int k;
  expr(ls, &e);
  k = e.k;
  luaK_exp2nextreg(ls->fs, &e);
  return k;
}


static void forbody (LexState *ls, int base, int line, int nvars, int isnum) {
  /* forbody -> DO block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  adjustlocalvars(ls, 3);  /* control variables */
  checknext(ls, TK_DO);
  prep = isnum ? luaK_codeAsBx(fs, OP_FORPREP, base, NO_JUMP) : luaK_jump(fs);
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  block(ls);
  leaveblock(fs);  /* end of scope for declared variables */
  luaK_patchtohere(fs, prep);
  endfor = (isnum) ? luaK_codeAsBx(fs, OP_FORLOOP, base, NO_JUMP) :
                     luaK_codeABC(fs, OP_TFORLOOP, base, 0, nvars);
  luaK_fixline(fs, line);  /* pretend that `OP_FOR' starts the loop */
  luaK_patchlist(fs, (isnum ? endfor : luaK_jump(fs)), prep + 1);
}

/* 
---- fornum ----
for a = exp, b, [exp] do
end
*/
static void fornum (LexState *ls, TString *varname, int line) {
  /* fornum -> NAME = exp1,exp1[,exp1] forbody */
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  new_localvarliteral(ls, "(for index)", 0);
  new_localvarliteral(ls, "(for limit)", 1);
  new_localvarliteral(ls, "(for step)", 2);
  new_localvar(ls, varname, 3);
  checknext(ls, '=');
  exp1(ls);  /* initial value */
  checknext(ls, ',');
  exp1(ls);  /* limit */
  if (testnext(ls, ','))
    exp1(ls);  /* optional step */
  else {  /* default step = 1 */
    luaK_codeABx(fs, OP_LOADK, fs->freereg, luaK_numberK(fs, 1));
    luaK_reserveregs(fs, 1);
  }
  forbody(ls, base, line, 1, 1);
}

/*
---- forlist ----
for k [, v] in ipairs|pair(tbl) do
end
*/
static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME {,NAME} IN explist1 forbody */
  FuncState *fs = ls->fs;
  expdesc e;
  int nvars = 0;
  int line;
  int base = fs->freereg;
  /* create control variables 
  ** 前面已经调用了enterblock，所以这里创建的是block.locvar代码
  ** 这里隐式的创建了3个locvar,且不可被lua代码访问
  */
  new_localvarliteral(ls, "(for generator)", nvars++);
  new_localvarliteral(ls, "(for state)", nvars++);
  new_localvarliteral(ls, "(for control)", nvars++);
  /* create declared variables */
  new_localvar(ls, indexname, nvars++);
  while (testnext(ls, ','))
    new_localvar(ls, str_checkname(ls), nvars++);
  checknext(ls, TK_IN);
  line = ls->linenumber;
  adjust_assign(ls, 3, explist1(ls, &e), &e);
  luaK_checkstack(fs, 3);  /* extra space to call generator */
  forbody(ls, base, line, nvars - 3, 0);
}

/*
---- forlist ----
for k [, v] in ipairs|pair(tbl) do
end

---- fornum ----
for a = exp, b, [exp] do
end

*/
static void forstat (LexState *ls, int line) {
  /* forstat -> FOR (fornum | forlist) END */
  FuncState *fs = ls->fs;
  TString *varname;
  BlockCnt bl;
  enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  luaX_next(ls);  /* skip `for' */
  varname = str_checkname(ls);  /* first variable name */
  switch (ls->t.token) {
    case '=': fornum(ls, varname, line); break;
    case ',': case TK_IN: forlist(ls, varname); break;
    default: luaX_syntaxerror(ls, LUA_QL("=") " or " LUA_QL("in") " expected");
  }
  check_match(ls, TK_END, TK_FOR, line);
  leaveblock(fs);  /* loop scope (`break' jumps to this point) */
}


static int test_then_block (LexState *ls) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  int condexit;
  luaX_next(ls);  /* skip IF or ELSEIF */
  condexit = cond(ls);
  checknext(ls, TK_THEN);
  block(ls);  /* `then' part */
  return condexit;	/* 返回待回填的falselist跳转链表，等待上层函数回填 */
}


static void ifstat (LexState *ls, int line) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  int flist;	/* false'list */
  int escapelist = NO_JUMP;	/* 块结束的addr */
  flist = test_then_block(ls);  /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF) {
    luaK_concat(fs, &escapelist, luaK_jump(fs));	/* 前面一个if/elseif执行完毕后，跳转到整个语句结束处 */
    luaK_patchtohere(fs, flist);	/* 前面一个if/elseif判断失败则跳转到下一个elseif哪里，尝试判断下一个 */
    flist = test_then_block(ls);  /* ELSEIF cond THEN block */
  }
  if (ls->t.token == TK_ELSE) {
    luaK_concat(fs, &escapelist, luaK_jump(fs));
    luaK_patchtohere(fs, flist);
    luaX_next(ls);  /* skip ELSE (after patch, for correct line info) */
    block(ls);  /* `else' part */
  }
  else
    luaK_concat(fs, &escapelist, flist);
  /* 整个语句的结束出 */
  luaK_patchtohere(fs, escapelist);
  check_match(ls, TK_END, TK_IF, line);
}

/* local function funName body */
static void localfunc (LexState *ls) {
  expdesc v, b;
  FuncState *fs = ls->fs;

/* step.1 处理local属性的函数名表达式 */
  /* 注册函数名，意味着在funName函数中funName这个变量已经生效了
  ** local function funName()
  **	 	print(type(funName))  函数名funName在这里已可见，是一个UPVAL
  ** end
  */
  new_localvar(ls, str_checkname(ls), 0);
  /* 初始化函数名表达式 */
  init_exp(&v, VLOCAL, fs->freereg);
  /* 上面新增了一个locvar，更新reg(消耗了一个reg) */
  luaK_reserveregs(fs, 1);
  /* 更新fs->nactvar, 更新上述actvar的startpc */
  adjustlocalvars(ls, 1);


/* step.2 处理local函数的定义业务 */
  body(ls, &b, 0, ls->linenumber);

/* step.3 实现 local function funName body 中的业务逻辑：将函数定义赋值给funName */
  luaK_storevar(fs, &v, &b);

  
  /* debug information will only see the variable after this point! 这里更新了funName的startpc 
  ** 上面设置funName.startpc以便函数名对于函数内部是可见的
  ** 这里更新一次，以便调整调试器的可见域么？还不是太明白其用途
  */
  getlocvar(fs, fs->nactvar - 1).startpc = fs->pc;
  //luaG_printString("locavar.name:%s", (getlocvar(fs, fs->nactvar - 1).varname));
}

static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME {`,' NAME} [`=' explist1] */
  int nvars = 0;
  int nexps;
  expdesc e;
  
  do {	
  	/* 登记左边的变量名到 Proto.locvars,
  	** 但尚未更新fs->nactvar变量，意味则未激活表达式左边的变量
  	** eg local a = "hello"
  	**    local a = a
  	**    print(a)---->hello
  	**    这里第二行查找右边的a时，左边的a尚未生效，那么往前找，找到了第一行的a,所以打印的是"hello"
  	**    这一点和C语言不一样！
  	*/
    new_localvar(ls, str_checkname(ls), nvars++);
  } while (testnext(ls, ','));
  
  if (testnext(ls, '='))
    nexps = explist1(ls, &e);	/* 解析表达式 */
  else {
    e.k = VVOID;	/* local a, b 这种情况 */
    nexps = 0;
  }
  
  /* 
  ** local a,b = c
  ** 左边多出来的变量 adjust_assign函数给右边自动补nil
  ** local a, b = c.d, e.g, h.i
  **    右边多占用的reg,在本stat编译结束后chunk()函数会自动回收，所以(stack上有数据残留哦！)
  **    不过残留的数据对后续不会有影响(编译器不会假设stack上的slot默认是nil,这边从本函数给左边的变量补nil值就能知道了)
  **    再回过头看 luaK_nil 函数，知道为什么有 fs->pc == 0那个条件判断了吧(有一点点理解作者的用意了吧！！)
  **
  */
  adjust_assign(ls, nvars, nexps, &e);
  adjustlocalvars(ls, nvars);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {field} [`:' NAME] */
  int needself = 0;
  singlevar(ls, v);
  while (ls->t.token == '.')
    field(ls, v);
  if (ls->t.token == ':') {
    needself = 1;	/* 需要给函数添加一个self参数 eg:           function modName:sub () body end */
    field(ls, v);
  }
  return needself;
}

/* 全局函数定义的stat, 对照 localfunc函数看 */
static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int needself;
  expdesc v, b;
  
  luaX_next(ls);  /* skip FUNCTION */
  /* 处理变量名是一个全局值(不是VLOCAL）表达式，故这里没有localfunc中特有的注册localvar的逻辑 */
  needself = funcname(ls, &v);
  
  body(ls, &b, needself, line);

  /* 将函数值拷贝到变量名中 */
  luaK_storevar(ls->fs, &v, &b);
  
  luaK_fixline(ls->fs, line);  /* definition `happens' in the first line */
}

/* 处理表达式stat */
static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  struct LHS_assign v;
  primaryexp(ls, &v.v);
  if (v.v.k == VCALL)  /* stat -> func */
    SETARG_C(getcode(fs, &v.v), 1);  /* call statement uses no results:直接忽略返回值 */
  else {  /* stat -> assignment */
    v.prev = NULL;
    assignment(ls, &v, 1);
  }
}


static void retstat (LexState *ls) {
  /* stat -> RETURN explist */
  FuncState *fs = ls->fs;
  expdesc e;
  int first, nret;  /* registers with returned values */
  luaX_next(ls);  /* skip RETURN */
  if (block_follow(ls->t.token) || ls->t.token == ';')
    first = nret = 0;  /* return no values */
  else {
    nret = explist1(ls, &e);  /* optional return values */
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1) {  /* tail call? */
        SET_OPCODE(getcode(fs,&e), OP_TAILCALL);
        lua_assert(GETARG_A(getcode(fs,&e)) == fs->nactvar);
      }
      first = fs->nactvar;
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);
      else {
        luaK_exp2nextreg(fs, &e);  /* values must go to the `stack' */
        first = fs->nactvar;  /* return all `active' values */
        lua_assert(nret == fs->freereg - first);
      }
    }
  }
  luaK_ret(fs, first, nret);
}


static int statement (LexState *ls) {
  int line = ls->linenumber;  /* may be needed for error messages */
  switch (ls->t.token) {
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line);
      return 0;
    }
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      return 0;
    }
    case TK_DO: {  /* stat -> DO block END */
      luaX_next(ls);  /* skip DO */
      block(ls);
      check_match(ls, TK_END, TK_DO, line);
      return 0;
    }
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      return 0;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls, line);
      return 0;
    }
    case TK_FUNCTION: {
      funcstat(ls, line);  /* stat -> funcstat */
      return 0;
    }
    case TK_LOCAL: {  /* stat -> localstat */
      luaX_next(ls);  /* skip LOCAL */
      if (testnext(ls, TK_FUNCTION))  /* local function? */
        localfunc(ls);	
      else
        localstat(ls);
      return 0;
    }
    case TK_RETURN: {  /* stat -> retstat */
      retstat(ls);
      return 1;  /* must be last statement */
    }
    case TK_BREAK: {  /* stat -> breakstat */
      luaX_next(ls);  /* skip BREAK */
      breakstat(ls);
      return 1;  /* must be last statement */
    }
    default: {
      exprstat(ls);
      return 0;  /* to avoid warnings */
    }
  }
}


static void chunk (LexState *ls) {
  /* chunk -> { stat [`;'] } */
  int islast = 0;	/* break和return 只能是chunk的最后一个op */
  enterlevel(ls);
  while (!islast && !block_follow(ls->t.token)) {
    islast = statement(ls);
	/* statement后面的';'是可选的 */
    testnext(ls, ';');
    lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
               ls->fs->freereg >= ls->fs->nactvar);
	
	/* 释放上一个块占用的临时寄存器 */
    ls->fs->freereg = ls->fs->nactvar;  /* free registers */
  }
  leavelevel(ls);
}

/* }====================================================================== */
