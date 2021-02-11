/*
** $Id: lgc.h,v 2.90 2015/10/21 18:15:15 roberto Exp $
** Garbage Collector
** See Copyright Notice in lua.h
*/

/*
** 网上其它介绍： https://blog.codingnow.com/2011/03/lua_gc_1.html
*/

#ifndef lgc_h
#define lgc_h


#include "lobject.h"
#include "lstate.h"

/*
** Collectable objects may have one of three colors: white, which
** means the object is not marked; gray, which means the
** object is marked, but its references may be not marked; and
** black, which means that the object and all its references are marked.
**
** The main invariant(不变) of the garbage collector, while marking objects,
** is that a black object can never point to a white one. Moreover(此外),
** any gray object must be in a "gray list" (gray, grayagain, weak,
** allweak, ephemeron) so that it can be visited again before finishing
** the collection cycle. These lists have no meaning when the invariant
** is not being enforced(强制执行) (e.g., sweep phase).
*/



/* how much to allocate before next GC step */
#if !defined(GCSTEPSIZE)
/* ~100 small strings */
#define GCSTEPSIZE	(cast_int(100 * sizeof(TString)))
#endif

/*
** 三色标记法
** Object为white,gray,black三色之一，并有如下两条不变(Invariants)规则
** 1:被根集直接引用的对象要么是gray，要么是black
** 2:black：自己"直接"引用的object要么处于待扫描的gray或已扫描完毕的black中(不可能是white)
*/

/*
** finalization:对于具有gc元方法的GCObject执行对应的方法？https://blog.codingnow.com/2018/10/lua_gc.html
** barrier     :边界，屏障 
**
*/

/*
** Possible states of the Garbage Collector
*/
#define GCSpropagate	0		/* 传播 */
#define GCSatomic	1           /* gray为NULL后，进入atomic,再开始处理grayAgain... */

#define GCSswpallgc	2
#define GCSswpfinobj	3
#define GCSswptobefnz	4		/* to be finalization ? */
#define GCSswpend	5

#define GCScallfin	6

#define GCSpause	7   /* gc一轮工作的起始阶段：仅用来标记rootlink? */

/* phase:阶段 */
#define issweepphase(g)  \
	(GCSswpallgc <= (g)->gcstate && (g)->gcstate <= GCSswpend)


/*
** macro to tell when main invariant (white objects cannot point to black
** ones) must be kept. During a collection, the sweep
** phase may break the invariant, as objects turned white may point to
** still-black objects. The invariant is restored when sweep ends and
** all objects are white again.
*/

#define keepinvariant(g)	((g)->gcstate <= GCSatomic)


/*
** some useful bit tricks（技巧）
*/
/* eg:m = 1<<2, 则此宏相当于将x中的某一位bits置0(resetbits)和1(setbits),或测试某一位bits是否为1
** 
** 和后面的bitmask宏配合起来用相当方便
*/
#define resetbits(x,m)		((x) &= cast(lu_byte, ~(m)))
#define setbits(x,m)		((x) |= (m))
#define testbits(x,m)		((x) & (m))

#define bitmask(b)		(1<<(b))
#define bit2mask(b1,b2)		(bitmask(b1) | bitmask(b2))

/* 对上面宏定义的封装,方便其它模块调用 */
#define l_setbit(x,b)		setbits(x, bitmask(b))
#define resetbit(x,b)		resetbits(x, bitmask(b))
#define testbit(x,b)		testbits(x, bitmask(b))


/* Layout for bit use in 'marked' field: */
#define WHITE0BIT	0  			/* object is white (type 0) */
#define WHITE1BIT	1  			/* object is white (type 1) */
#define BLACKBIT	2  			/* object is black */
#define FINALIZEDBIT	3  	    /* object has been marked for finalization */
/* bit 7 is currently used by tests (luaL_checkmemory) */

/* 看到了么，这里用了复数形式哦,虽然无需如此严谨，但编程还是严谨为好 */
#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)

/* 是否拥有任何白色标记？ */
#define iswhite(x)      testbits((x)->marked, WHITEBITS)
/* 是否拥有黑色bits? */
#define isblack(x)      testbit((x)->marked, BLACKBIT)
/* 
 ** 这里用了互斥属性的特点进行反向判断
 ** 这里可以看出，gray状态本身没有安排特定的bits来主动标记它
 */
#define isgray(x)  /* neither white nor black */  \
	(!testbits((x)->marked, WHITEBITS | bitmask(BLACKBIT)))

/* 处于fnz状态吗？ */
#define tofinalize(x)	testbit((x)->marked, FINALIZEDBIT)

/* 另一种white,拿笔出来画bits分布图 */
#define otherwhite(g)	((g)->currentwhite ^ WHITEBITS)

/*
** ow:other.white m:mark
** 如果自己仅是另一种white:则isdeadm为true,反之则为false
** 若为gray或black则不是isdeadm
**
**
** atomic函数的最后阶段设置了currentwhite=otherwhite,
** 假设扫描阶段currentWhite=0,那么在进入sweep阶段时curentWhite便被set为1了。
** 在sweep阶段otherWhite=0的GCObject便是需要在本次GC中被清除了
** 在sweep新增加的GCObject设置为1即可(这就是双白色的作用了)
*/
#define isdeadm(ow,m)	(!(((m) ^ WHITEBITS) & (ow)))
#define isdead(g,v)	isdeadm(otherwhite(g), (v)->marked)

/* white1<->white2 */
#define changewhite(x)	((x)->marked ^= WHITEBITS)
#define gray2black(x)	l_setbit((x)->marked, BLACKBIT)
//提取当前的white对应的bit
#define luaC_white(g)	cast(lu_byte, (g)->currentwhite & WHITEBITS)

/*
** Does one step of collection when debt(负债) becomes positive(正数).
** 'pre'/'pos' allows some adjustments to be done only when needed. 
** macro'condchangemem' is used only for heavy(满载) tests (forcing a full
** GC cycle on every opportunity(时机))
*/
#define luaC_condGC(L,pre,pos) \
	{ if (G(L)->GCdebt > 0) { pre; luaC_step(L); pos;}; \
	  condchangemem(L,pre,pos); }

/* more often than not, 'pre'/'pos' are empty */
#define luaC_checkGC(L)		luaC_condGC(L,,)


/* barrier:屏障
**
** 三色标记法有个重要的规则：black不能引用white对象
** 这里barrier就是保证这个条规则的意思。
**
*/
#define luaC_barrier(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ?  \
	luaC_barrier_(L,obj2gco(p),gcvalue(v)) : cast_void(0))

/* 
** 后退操作，基本上用于处理table,因为table和其引用的对象一般是1：N
** 修改一次表后，仅需要标记一下改表即可，无需修改一次表就改一次该引用对象
** p:table?
*/
#define luaC_barrierback(L,p,v) (  \
	(iscollectable(v) && isblack(p) && iswhite(gcvalue(v))) ? \
	luaC_barrierback_(L,p) : cast_void(0))

#define luaC_objbarrier(L,p,o) (  \
	(isblack(p) && iswhite(o)) ? \
	luaC_barrier_(L,obj2gco(p),obj2gco(o)) : cast_void(0))

#define luaC_upvalbarrier(L,uv) ( \
	(iscollectable((uv)->v) && !upisopen(uv)) ? \
         luaC_upvalbarrier_(L,uv) : cast_void(0))

LUAI_FUNC void luaC_fix (lua_State *L, GCObject *o);
LUAI_FUNC void luaC_freeallobjects (lua_State *L);
LUAI_FUNC void luaC_step (lua_State *L);
LUAI_FUNC void luaC_runtilstate (lua_State *L, int statesmask);
LUAI_FUNC void luaC_fullgc (lua_State *L, int isemergency);
LUAI_FUNC GCObject *luaC_newobj (lua_State *L, int tt, size_t sz);
LUAI_FUNC void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v);
LUAI_FUNC void luaC_barrierback_ (lua_State *L, Table *o);
LUAI_FUNC void luaC_upvalbarrier_ (lua_State *L, UpVal *uv);
LUAI_FUNC void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt);
LUAI_FUNC void luaC_upvdeccount (lua_State *L, UpVal *uv);


#endif
