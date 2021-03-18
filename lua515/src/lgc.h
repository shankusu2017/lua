/*
** $Id: lgc.h,v 2.15.1.1 2007/12/27 13:02:25 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#ifndef lgc_h
#define lgc_h


#include "lobject.h"


/*
** Possible states of the Garbage Collector
*/
#define GCSpause	0		// 暂停状态，也是初始状态
#define GCSpropagate	1	// 遍历阶段，扫描gray NOTE:gc开始后从pause->propagate整个"gc过程"是原子的
#define GCSsweepstring	2	// 回收空闲的string
#define GCSsweep	3		// 回收其它类型的数据
#define GCSfinalize	4		// 处理带有mt且mt有gc的所有userData的阶段？ */


/*
** some userful bit tricks
*/
#define resetbits(x,m)	((x) &= cast(lu_byte, ~(m)))	/* 清除特定的bit位 */
#define setbits(x,m)	((x) |= (m))					/* 标记特定的bit位 */
#define testbits(x,m)	((x) & (m))						/* 测试特定的bit位 */
#define bitmask(b)	(1<<(b))							/* 获取某个左移N位的bit值 */
#define bit2mask(b1,b2)	(bitmask(b1) | bitmask(b2))		/* 将1左移N位的bit值 | 将1左移M位的bit值 */
#define l_setbit(x,b)	setbits(x, bitmask(b))			/* 将x的左移N位的bit设置为1 */
#define resetbit(x,b)	resetbits(x, bitmask(b))	·	/* 将X的左移N位的bit设置位0 */
#define testbit(x,b)	testbits(x, bitmask(b))			/* 测试X的左移N位是否为 1 */
#define set2bits(x,b1,b2)	setbits(x, (bit2mask(b1, b2)))	/* 将X的左移N位，M位设置为 1 */
#define reset2bits(x,b1,b2)	resetbits(x, (bit2mask(b1, b2)))	/* 清除X的左移N位，M位 */
#define test2bits(x,b1,b2)	testbits(x, (bit2mask(b1, b2)))		/* 测试X的左移N位，M位是否至少有一个为1 */



/*
** Layout for bit use in `marked' field:
** bit 0 - object is white (type 0)
** bit 1 - object is white (type 1)
** bit 2 - object is black
** bit 3 - for userdata: has been finalized
** bit 3 - for tables: has weak keys
** bit 4 - for tables: has weak values
** bit 5 - object is fixed (should not be collected)
** bit 6 - object is "super" fixed (only the main thread)
*/


#define WHITE0BIT	0
#define WHITE1BIT	1
#define BLACKBIT	2

#define FINALIZEDBIT	3		/* 需被回收(本轮或下一轮)*/
#define KEYWEAKBIT	3			/* 拥有弱key */
#define VALUEWEAKBIT	4		/* 拥有弱val */
#define FIXEDBIT	5			/* 保留数据，不能被GC,eg:语言关键字 */
#define SFIXEDBIT	6			/* 保留数据，only used for mainThread */
#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)


#define iswhite(x)      test2bits((x)->gch.marked, WHITE0BIT, WHITE1BIT)	/* 是任何一种白色吗 */
#define isblack(x)      testbit((x)->gch.marked, BLACKBIT)	/* 是黑色吗 */
#define isgray(x)	(!isblack(x) && !iswhite(x))	/* 不是黑，同时也不是任何一种白，则是gray */
/* 保留 bit[7,2]位的数据，将bit[1,0]翻转 */
#define otherwhite(g)	(g->currentwhite ^ WHITEBITS)	/* 返回另一种白的值 */
/* 忽略bit[7,2] 若curWhite[1,1]->isdead[x,x]=false, curWhite[1,0]->isdead[x,1]=true,curWhite[0,1]->isdead[1,x]=true*/
#define isdead(g,v)	((v)->gch.marked & otherwhite(g) & WHITEBITS)	/* 是dead(另一种白)吗 */
/* 翻转bit[1,0],保留bit[7,2] */
#define changewhite(x)	((x)->gch.marked ^= WHITEBITS)
/* 标记bit[2]为1 */
#define gray2black(x)	l_setbit((x)->gch.marked, BLACKBIT)	/* 看了isgray，应该理解这里 */
/* gc且bit[1,0]任意一位为1即可 */
#define valiswhite(x)	(iscollectable(x) && iswhite(gcvalue(x)))

#define luaC_white(g)	cast(lu_byte, (g)->currentwhite & WHITEBITS)	/* 当前white的bits值 */


#define luaC_checkGC(L) { \
  condhardstacktests(luaD_reallocstack(L, L->stacksize - EXTRA_STACK - 1)); \
  if (G(L)->totalbytes >= G(L)->GCthreshold) \
	luaC_step(L); }

/* f: forward */ 
#define luaC_barrier(L,p,v) { if (valiswhite(v) && isblack(obj2gco(p)))  \
	luaC_barrierf(L,obj2gco(p),gcvalue(v)); }
/* for table back */
#define luaC_barriert(L,t,v) { if (valiswhite(v) && isblack(obj2gco(t)))  \
	luaC_barrierback(L,t); }

#define luaC_objbarrier(L,p,o)  \
	{ if (iswhite(obj2gco(o)) && isblack(obj2gco(p))) \
		luaC_barrierf(L,obj2gco(p),obj2gco(o)); }

#define luaC_objbarriert(L,t,o)  \
   { if (iswhite(obj2gco(o)) && isblack(obj2gco(t))) luaC_barrierback(L,t); }

LUAI_FUNC size_t luaC_separateudata (lua_State *L, int all);
LUAI_FUNC void luaC_callGCTM (lua_State *L);
LUAI_FUNC void luaC_freeall (lua_State *L);
LUAI_FUNC void luaC_step (lua_State *L);
LUAI_FUNC void luaC_fullgc (lua_State *L);
LUAI_FUNC void luaC_link (lua_State *L, GCObject *o, lu_byte tt);
LUAI_FUNC void luaC_linkupval (lua_State *L, UpVal *uv);
LUAI_FUNC void luaC_barrierf (lua_State *L, GCObject *o, GCObject *v);
LUAI_FUNC void luaC_barrierback (lua_State *L, Table *t);


#endif
