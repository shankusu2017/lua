/*
** $Id: ltable.c,v 2.117 2015/11/19 19:16:22 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#define ltable_c
#define LUA_CORE

#include "lprefix.h"


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates（候选） to be kept in the array
** part. The actual size of the array is the largest 'n' such that
** more than half the slots between 1 and n are in use.
** Hash uses a mix(混合的) of chained scatter(散列) table with Brent's variation.
** A main invariant(不变) of these tables is that, if an element is not
** in its main position (i.e. the 'original' position that its hash gives
** to it), then the colliding(碰撞) element is in its own main position.
** Hence(因此) even(即使) when the load factor（负载） reaches 100%, performance(性能) remains good.
*/

#include <math.h>
#include <limits.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"


/*
** Maximum size of array part (MAXASIZE) is 2^MAXABITS. MAXABITS is
** the largest integer such that MAXASIZE fits in an unsigned int.
** 下面两个宏定义含有技巧，学着点哦
** MAX_ARRAY_BITS->MAXABITS MAX_ARRAY_SIZE->MAXASIZE
*/
#define MAXABITS	cast_int(sizeof(int) * CHAR_BIT - 1)
#define MAXASIZE	(1u << MAXABITS)

/*
** Maximum size of hash part is 2^MAXHBITS. MAXHBITS is the largest
** integer such that 2^MAXHBITS fits in a signed int. (Note that the
** maximum number of elements in a table, 2^MAXABITS + 2^MAXHBITS, still
** fits comfortably in an unsigned int.)
*/
#define MAXHBITS	(MAXABITS - 1)

/* 计算n在node中的idx */
#define hashpow2(t,n)		(gnode(t, lmod((n), sizenode(t))))
/* 常见类型在node中的索引值 */
#define hashstr(t,str)		hashpow2(t, (str)->hash)
#define hashboolean(t,p)	hashpow2(t, p)
#define hashint(t,i)		hashpow2(t, i)


/*
** for some types, it is better to avoid modulus by power of 2, as
** they tend to have many 2 factors.
**
** 不太明白这里的的(sizenode(t)-1) | 1 的操作
** | 1这一步没必要啊 ps:这里预防node.size=2^0时，除数为0的情况
*/
#define hashmod(t,n)	(gnode(t, ((n) % ((sizenode(t)-1)|1))))

#define hashpointer(t,p)	hashmod(t, point2uint(p))

/* dummy:假的 */
#define dummynode		(&dummynode_)

#define isdummy(n)		((n) == dummynode)

/* 
** table的node.size逻辑上至少为1(2^0)，这里为了减少维护和MEM成本
** 当node逻辑上为nil时，指向这里
*/
static const Node dummynode_ = {
  {NILCONSTANT},  /* value */
  {{NILCONSTANT, 0}}  /* key */
};


/*
** Hash for floating-point numbers.
** The main computation should be just
**     n = frexp(n, &i); return (n * INT_MAX) + i
** but there are some numerical subtleties.
** In a two-complement representation, INT_MAX does not has an exact
** representation as a float, but INT_MIN does; because the absolute
** value of 'frexp' is smaller than 1 (unless 'n' is inf/NaN), the
** absolute value of the product 'frexp * -INT_MIN' is smaller or equal
** to INT_MAX. Next, the use of 'unsigned int' avoids overflows when
** adding 'i'; the use of '~u' (instead of '-u') avoids problems with
** INT_MIN.
*/
#if !defined(l_hashfloat)
/*
**frexp:将给定浮点值x分解为归一化分数和2的整数幂。
** int i = 0,
** local f2 = frexpf(123.45, &i)
** f2 = 0.964453, i = 7  -->123.45=0.964453 * (2^7)
** 仔细看下面这个函数的实现，还蛮有意思的哦
*/
static int l_hashfloat (lua_Number n) 
{
  int i;
  lua_Integer ni;	/* ni的意思应该是number->int */
  n = l_mathop(frexp)(n, &i) * -cast_num(INT_MIN);
  if (!lua_numbertointeger(n, &ni)) {  /* is 'n' inf/-inf/NaN? */
    lua_assert(luai_numisnan(n) || l_mathop(fabs)(n) == cast_num(HUGE_VAL));
    return 0;
  }
  else {  /* normal case */
    unsigned int u = cast(unsigned int, i) + cast(unsigned int, ni);
    return cast_int(u <= cast(unsigned int, INT_MAX) ? u : ~u);
  }
}
#endif


/*
** returns the 'main' position of an element in a table (that is, the index
** of its hash value)
**
** 返回key在node部分对应的pos(又称mainPos)
*/
static Node *mainposition (const Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TNUMINT:
      return hashint(t, ivalue(key));
    case LUA_TNUMFLT:
      return hashmod(t, l_hashfloat(fltvalue(key)));
    case LUA_TSHRSTR:
      return hashstr(t, tsvalue(key));
    case LUA_TLNGSTR:
      return hashpow2(t, luaS_hashlongstr(tsvalue(key)));
    case LUA_TBOOLEAN:
      return hashboolean(t, bvalue(key));
    case LUA_TLIGHTUSERDATA:
      return hashpointer(t, pvalue(key));
    case LUA_TLCF:
      return hashpointer(t, fvalue(key));
    default:
      lua_assert(!ttisdeadkey(key));
      return hashpointer(t, gcvalue(key));
  }
}


/*
** returns the index for 'key' if 'key' is an appropriate（合适的）key to live in
** the array part of the table, 0 otherwise.
** 
** 简约判断下传入的key是否可能落在arrray区间
** key为int类型，且1<=key<=MAXASIZE,则返回key,反之返回0
*/
static unsigned int arrayindex (const TValue *key) {
  if (ttisinteger(key)) {
    lua_Integer k = ivalue(key);
    if (0 < k && (lua_Unsigned)k <= MAXASIZE)
      return cast(unsigned int, k);  /* 'key' is an appropriate array index */
  }
  return 0;  /* 'key' did not match some condition */
}


/*
** returns the index of a 'key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signaled by 0.
**
** 先在array中查找，找到即返回[1,N]，否则进入hash部分查找[N+1,N+M]
** 根据返回值的大小即可确定给出的key到底落在了哪里
** node中的index是array.size+[1,M]:判断下范围即可
** 这里其实是返回key的下一个slot的index
*/
static unsigned int findindex (lua_State *L, Table *t, StkId key) 
{
  unsigned int i;
  /* 
  ** key=nil,意味着从array[0]开始起检索，first iteration就是这个意思
  */
  if (ttisnil(key))
    return 0;  /* first iteration */
  
   /* 尝试转换为数组部分的index */
  i = arrayindex(key);	      
  if (i != 0 && i <= t->sizearray) {  /* is 'key' inside array part?, arrayindex()函数没有将key和t->sizearray比较，所以这里要对比下 */
    return i;  /* yes; that's the index */
  }
  
  /* 没有落入数组部分，则进入node区域查找 */
	int nx;
	Node *n = mainposition(t, key);
	for (;;) {  /* check whether 'key' is somewhere in the chain */
	  /* key may be dead already, but it is ok to use it in 'next' */
	  if (luaV_rawequalobj(gkey(n), key) ||
	        (ttisdeadkey(gkey(n)) && iscollectable(key) && deadvalue(gkey(n)) == gcvalue(key))) {
	    i = cast_int(n - gnode(t, 0));  /* key index in hash table */

	    /* hash elements are numbered after array ones
       ** +1表示返回的是下一个slot的index，记住了哦！！！！ 
       ** 这里 + t->sizearray,可以告诉调用者index落在了node区域
      */
	    return (i + 1) + t->sizearray;	
	  }

	  nx = gnext(n);
    /* 
    ** 结合 luaH_next 函数看，这里的nx==0的判断是必须的，
    ** 确保了next函数的规则,同时也利用了node上的key必须处于相同hash值的链表上这一特性
    ** eg:玩家传入的key在node域找不到则函数返回失败(next函数规则,nil除外)
    */
	  if (nx == 0) {
	    luaG_runerror(L, "invalid key to 'next'");  /* key not found */
	  } else {
      n += nx;
    }
	}
}
int luaH_next (lua_State *L, Table *t, StkId key) {
  /* 
   * 若key=nil,则i=0,那么下面刚好从数组的第一个开始匹配，明白了吧 
   * 若key~=nil,那么i则是key的下一个key的slot的索引，所以下面array部分直接判断，并没有进行先+1，再进行判断，明白了吧
   */
  unsigned int i = findindex(L, t, key);  /* find original element */
  
  for (; i < t->sizearray; i++) {  /* try first array part */
    if (!ttisnil(&t->array[i])) {  /* a non-nil value? */
      setivalue(key, i + 1);	/* 匹配中了数组部分eg:key=3,且key=4的slot不为空，那么就返回4,即next(table,idx=3)-->4 */
      setobj2s(L, key+1, &t->array[i]);	/* 这里可以看到，还返回了值 */
      return 1;
    }
  }

  /* i -= t->sizearray将i转为hash部分的索引 */
  for (i -= t->sizearray; cast_int(i) < sizenode(t); i++) {  /* hash part */
    if (!ttisnil(gval(gnode(t, i)))) {  /* a non-nil value? */
      setobj2s(L, key, gkey(gnode(t, i)));		/* 这里是同上的 */
      setobj2s(L, key+1, gval(gnode(t, i)));
      return 1;
    }
  }
  return 0;  /* no more elements */
}


/*
** {=============================================================
** Rehash
** ==============================================================
*/

/*
** Compute the optimal(最佳) size for the array part of table 't'. 
** 'nums' is a "count array" where 'nums[i]' is the number of integers in the table
**   between 2^(i - 1) + 1 and 2^i. 
**'pna' enters with the total number of integer keys in the table 
**   and leaves with the number of keys that will go to the array part; 
** return the optimal size.
**
** 确保一个值size使tbl中unsigned int的所有至少一半以上key落在tbl.array[0,size]，中同时tbl.array[0,size]的使用率达到50%以上?
*/
static unsigned int computesizes (unsigned int nums[], unsigned int *pna) {
  int i;
  unsigned int twotoi;        /* 2^i (candidate(候选) for optimal size) */

  unsigned int a        = 0;  /* number of elements smaller than 2^i */
  unsigned int na       = 0;  /* number of elements to go to array part */
  unsigned int optimal  = 0;  /* optimal size for array part */

  /* loop while keys can fill more than half of total size 
  ** 函数要求返回的optimal时，[1,optimal]范围内被使用的key至少超过1/2
  ** for中的*pna > twoto1/2是终止循环的充要条件，且和下面的a>twotoi/2相对应
  ** 理解这两个twoto1/2是理解这个函数的必要条件
  */
  for (i = 0, twotoi = 1; *pna > twotoi/2; i++, twotoi *= 2) {
    if (nums[i] > 0) {
      a += nums[i]; /* 到目前为止[1,twotoi]范围内的key的total

      /* 
      ** more than half elements present?
      ** 对于最新的[1,twotoi]范围，空间利用率超过了50%，则记录下当前的参数
      ** 后续还会尝试再次更新(函数采用了类似贪心算法取满足条件的最大值，而不是一次到位取得最优解
      ** 明白这一点对理解代码有利)
      **
      ** 考虑下key被使用的临界情况之一(下面的数值表示某个区间内用到的key的数量)
      ** [0],[0],[0],[0],[1],[15]
      ** 要满足返回的optimal使用率超过1/2,那么下面的 if (a > twotoi/2) 这个判断情况是必须的
      ** 若没有a>twotoi/2这个条件，那么optimal=16,na=1,显然这不符合作者的本意
      */
      if (a > twotoi/2) {
        /*
         ** optimal size (till(直到) now)
         ** 记录下到目前为止符合要求的最新的数据
         **
         ** 记录当前的twotoi和在[1,twotoi]内key的total
         */
        optimal = twotoi;  
        na = a;            /* all elements up to 'optimal' will go to array part */
      } else {
        /* [1，twotoi] 范围内的key的total没有超过twotoi容量的1/2，key可能分布在[1,max]了后面，继续往后读,这里不急 */
      }
    }
  }
  
  /* 理解了上面的算法，这个lua_assert断言就好理解了 */
  lua_assert((optimal == 0 || optimal / 2 < na) && na <= optimal);
  *pna = na;
  return optimal;
}

/* 
** key若落在[1,INT32]，则nums对应的域++ ,返回1
** 反之返回0
*/
static int countint (const TValue *key, unsigned int *nums) {
  unsigned int k = arrayindex(key);
  if (k != 0) {  /* is 'key' an appropriate array index? */
    nums[luaO_ceillog2(k)]++;  /* count as such */
    return 1;
  } else {
    return 0;
  }
}


/*
** Count keys in array part of table 't': Fill 'nums[i]' with
** number of keys that will go into corresponding（相应） slice and return
** total number of non-nil keys.
** a:统计数组部分总数
** b:数组中key在[2^0,2^1),[2^1,2^2),[2^2,2^3),[2^3,2^4)...等区间内的元素个数
*/
static unsigned int numusearray (const Table *t, unsigned int *nums) {
  int lg;
  unsigned int ttlg;        /* 2^lg */
  unsigned int ause = 0;    /* summation of 'nums' */
  unsigned int i = 1;       /* count to traverse all array keys */
  /* traverse each slice */
  for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg *= 2) {
    unsigned int lc  = 0;    /* counter */
    unsigned int lim = ttlg;
    if (lim > t->sizearray) {
      lim = t->sizearray;   /* adjust upper limit */
	  
      if (i > lim)	/* 这里用于终止循环，因为后续已经没有元素了 */
        break;      /* no more elements to count */
    }
	
    /* 
    ** count elements in range (2^(lg - 1), 2^lg] 
    ** 统计这个区间内的element数量
	*/
    for (; i <= lim; i++) {
      if (!ttisnil(&t->array[i-1])) 
        lc++;
    }
	
    nums[lg] += lc;
    ause += lc;
  }
  return ause;
}

/*
** 统计tbl中node部分的信息
** nums:收集[1,uint32Max]的key到对应的区间
** pna: 累加[1,uint32Max]的key的总数
** totaluse：所有类型的key的总数
** 
** 相对上面的numusehash()这里还多了pna,因为array中只可能存在key[1,uint32Max],
**   而node中的key有可能是字符串"abcKey"等非上述范围的key
** pna:param count array ?
*/
static int numusehash (const Table *t, unsigned int *nums, unsigned int *pna)
{
  int totaluse = 0;  /* total number of elements */
  int ause     = 0;  /* elements added to 'nums' (can go to array part) */
  int i        = sizenode(t);
  while (i--) {
    Node *n = &t->node[i];
    if (!ttisnil(gval(n))) {
      ause += countint(gkey(n), nums);
      totaluse++;
    }
  }
  *pna += ause;
  return totaluse;
}

/* 
** 调整array的大小到size
** 新slot填nil
*/
static void setarrayvector (lua_State *L, Table *t, unsigned int size) {
  unsigned int i;
  luaM_reallocvector(L, t->array, t->sizearray, size, TValue);
  for (i=t->sizearray; i<size; i++)
     setnilvalue(&t->array[i]);	    /* 若有空的slot则填nil */
  t->sizearray = size;
}

/* 
** 申请全新的Node,大小为size
** 如果要保留table的旧node则应该在本函数被调用前保存
*/
static void setnodevector (lua_State *L, Table *t, unsigned int size) {
  int lsize;
  if (size == 0) {  /* no elements to hash part? */
    t->node = cast(Node *, dummynode);  /* use common 'dummynode' */
    lsize 	= 0;
  } else {
    int i;

  	/* 整理成特殊要求的size */
    lsize = luaO_ceillog2(size);	/* size对2的对数 */
    if (lsize > MAXHBITS)
      luaG_runerror(L, "table overflow");
    size = twoto(lsize);			/* 还原成 1024这样的普通数 */
	/* 申请全新的MEM.Node */
    t->node = luaM_newvector(L, size, Node);
    for (i = 0; i < (int)size; i++) {	/* 对新的Node填nil */
      Node *n       = gnode(t, i);
      gnext(n)      = 0;
      setnilvalue(wgkey(n));
      setnilvalue(gval(n));
    }
  }
  
  t->lsizenode = cast_byte(lsize);
  t->lastfree  = gnode(t, size);  /* all positions are free */
}

/*
** 控制性函数
** 根据最新的nasize和nhsize来rehash所有的element
** 看不懂的话，拿笔和纸出来画图
*/
void luaH_resize (lua_State *L, Table *t, unsigned int nasize, unsigned int nhsize)
{
  int j;
  unsigned int i;
  unsigned int oldasize = t->sizearray;
  
  /* 记录下Node部分旧的mem,len信息 */
  int oldhsize  = t->lsizenode;
  Node *nold    = t->node;  /* save old hash ... */

  /* create new hash part with appropriate(适当) size */
  setnodevector(L, t, nhsize);

  /* table.array的各种可能的size都是2的幂，所以这里若size是增长，则array中原有的elemnt就不用动了
  ** eg:old.size=4,old.key=3,pos1=3%4 new.size=8,则pos2=3%8==pos1
  **
  ** 若size是收缩，那么被保留的部分也不用动了，因为array.key满足[1,array.size]
  **   不理解的话，拿笔出来画一下就明白了
  */
  if (nasize > oldasize) {  /* array part must grow? */
    setarrayvector(L, t, nasize);
  } else if (nasize < oldasize) {  /* array part must shrink(收缩)? */
    /* step.1,这一步必须在step.2的前面*/
    t->sizearray = nasize;

    /* step.2
    ** re-insert elements from vanishing(消失的) slice 
    ** 保留下来的那一部分不用动，原理看上面注释
    */
    for (i=nasize; i<oldasize; i++) {
      if (!ttisnil(&t->array[i])) {
        luaH_setint(L, t, i + 1, &t->array[i]);
      }
    }
    /* shrink array */
    luaM_reallocvector(L, t->array, oldasize, nasize, TValue);
  }
  
  /* re-insert elements from hash part */
  for (j = twoto(oldhsize) - 1; j >= 0; j--) {
    Node *old = nold + j;
    if (!ttisnil(gval(old))) {
      /* doesn't need barrier/invalidate cache, as entry was
         already present in the table */
      setobjt2t(L, luaH_set(L, t, gkey(old)), gval(old));
    }
  }
  
  if (!isdummy(nold)) {
    luaM_freearray(L, nold, cast(size_t, twoto(oldhsize))); /* free old hash */
  }
}

/* 仅调整数组部分大小 */
void luaH_resizearray (lua_State *L, Table *t, unsigned int nasize) {
  int nsize = isdummy(t->node) ? 0 : sizenode(t);
  luaH_resize(L, t, nasize, nsize);
}

/*
** nums[i] = number of keys 'k' where 2^(i - 1) < k <= 2^i
** 
** 控制性函数
** 新加一个key=ek的情况下，调整下array和node的大小
** 一般在table的slot满的情况下被调用
*/
static void rehash (lua_State *L, Table *t, const TValue *ek) 
{
  unsigned int asize;  	/* optimal(最佳) size for array part */
  unsigned int na;  	/* number of keys in the array part */
  unsigned int nums[MAXABITS + 1];
  int totaluse;
  
  for (int i = 0; i <= MAXABITS; i++)
  	nums[i] = 0;  /* reset counts */
  
  na = numusearray(t, nums);  /* count keys in array part */
  totaluse = na;  /* all those keys are integer keys */

  /* 
  ** 计算node中元素总数并叠加到totaluse上
  ** 将node中的key==int[1,uint32Max]的信息更新到nums和na上
  */
  totaluse += numusehash(t, nums, &na);  /* count keys in hash part */
  
  /* count extra key 
  ** 同上步处理ek
  */
  na += countint(ek, nums);
  totaluse++;

    /*
    **到目前为止相关的数据域含义为
    ** totaluse:所有元素个数的总和
    ** na      :key==int[1,uint32Max]总和
    ** nums    :数组中key在[2^0,2^1),[2^1,2^2),[2^2,2^3),[2^3,2^4)...等区间内的元素个数
    */


  /* compute new size for array part */
  asize = computesizes(nums, &na);
  /* 
  ** 调用computesizes后
  ** asize:最优解的新数组大小
  ** na:被调整为落在新数组中的元素个数
  */
  
  /* resize the table to new computed sizes
  ** totaluse+asize-na>= totaluse,因为na<=asize
  */
  luaH_resize(L, t, asize, totaluse - na);
}



/*
** }=============================================================
*/

/* 构造一张表 */
Table *luaH_new (lua_State *L) {
  GCObject *o 	= luaC_newobj(L, LUA_TTABLE, sizeof(Table));
  Table *t 		= gco2t(o);

  /* 处理原表 */	
  t->metatable 	= NULL;
  t->flags 		= cast_byte(~0);

  /* 处理数组部分 */
  t->array 		= NULL;
  t->sizearray  = 0;
	
  /* 处理node部分 */
  setnodevector(L, t, 0);
  return t;
}

/* 
** 释放表占用的MEM
** 看得出Table由三部分组成了吧
*/
void luaH_free (lua_State *L, Table *t) {
	/* 
	** 必须进行isdummy判断，因为空的node部分指向了共用的mem
	** 非空时才指向自己私有的mem
	*/
  if (!isdummy(t->node)) {	
    luaM_freearray(L, t->node, cast(size_t, sizenode(t)));
  }
	
  luaM_freearray(L, t->array, t->sizearray);
  luaM_free(L, t);
}

/* 从这里可以看到freepos是倒序来的 */
static Node *getfreepos (Table *t) {
  while (t->lastfree > t->node) {
    t->lastfree--;
    if (ttisnil(gkey(t->lastfree)))
      return t->lastfree;
  }
  return NULL;  /* could not find a free place */
}



/*
** inserts a new key into a hash table;
**
** first, check whether key's main
** position is free. If not, check whether colliding（碰撞的） node is in its main
** position or not: if it is not, move colliding node to an empty place and
** put new key in its main position; otherwise (colliding node is in its main
** position), new key goes to an empty position.
*/
TValue *luaH_newkey (lua_State *L, Table *t, const TValue *key) {
  Node   *mp;
  TValue aux;
  
  /* 对key值进行必要的非空和范围判断 */
  if (ttisnil(key))	{	/* 看到了么，tbl不支持nil的key */
  	luaG_runerror(L, "table index is nil");
   } else if (ttisfloat(key)) {
    lua_Integer k;
    if (luaV_tointeger(key, &k, 0)) {  /* index is int? */
      setivalue(&aux, k);
      key = &aux;  /* insert it as an integer */
    } else if (luai_numisnan(fltvalue(key))) {
      luaG_runerror(L, "table index is NaN");
   	}
  }
   
  mp = mainposition(t, key);
  if (!ttisnil(gval(mp)) || isdummy(mp)) {  /* main position is taken? */
    Node *othern;
    Node *f = getfreepos(t);  /* get a free place */
    /* slot全满，则只能重新rehash扩容了 */
    if (f == NULL) {  /* cannot find a free place? */
      rehash(L, t, key);  /* grow table */
      /* whatever called 'newkey' takes care of TM cache */
      return luaH_set(L, t, key);  /* insert key into grown table */
    }
	
    lua_assert(!isdummy(f));

    /* 当前占领我的mainPos的元素，计算下它原本的mainPos */
    othern = mainposition(t, gkey(mp));

    /* 
    ** 它和我不是亲戚，那么我的mainPos优先给我用，它必须腾出来
    ** 上面的优先原则就是node部分的核心原则，理解这一点，即一个pos优先被属于它的mainPos的元素占用,就理解了下面的代码
    */
    if (othern != mp) {  /* is colliding node out of its main position? */
        
      /* yes; move colliding node into free position 
       ** 在它的亲戚链表中找到他的上一级
      */
      while (othern + gnext(othern) != mp) {  /* find previous */
        othern += gnext(othern);
      }
      /* 调整上面的他的上一个的元素使其指向新的free
      ** 并将它CP到新的free中
      */
      gnext(othern) = cast_int(f - othern);  /* rechain to point to 'f' */
      *f = *mp;  /* copy colliding node into free pos. (mp->next also goes) */
      /* 处理下他们一家族的链表 */
      if (gnext(mp) != 0) {
        gnext(f) += cast_int(mp - f);  /* correct 'next' */
        gnext(mp) = 0;  /* now 'mp' is free */
      }

      /* 将它腾出来的，原本属于我的位置的value域填入nil值(擦除残留的值) */
      setnilvalue(gval(mp));
    } else {  /* colliding node is in its own main position */
      /* new node will go into free position 
      ** 设置新free的next域，value不用设置，因为是全新的
      */
      if (gnext(mp) != 0)
        gnext(f) = cast_int((mp + gnext(mp)) - f);  /* chain new position */
      else
        lua_assert(gnext(f) == 0);  /* 链表的要求，这里必须gnext(f) == 0 */
      gnext(mp) = cast_int(f - mp);
      
      /* 方面下面setnodekey调用中mp的统一含义 */
      mp = f;   
    }
  }

  setnodekey(L, &mp->i_key, key);

  /* 进行GC的barrierback操作，确保black不会指向white */
  luaC_barrierback(L, t, key);

  /* 函数名为newkey，所以这里判断下val==nil，确保上面将对应的pos的val置空了*/
  lua_assert(ttisnil(gval(mp)));
  
  return gval(mp);
}


/*
** search function for integers
** 获取t[key]的值的地址，注意这里是返回值的地址
*/
const TValue *luaH_getint (Table *t, lua_Integer key) {
  /* (1 <= key && key <= t->sizearray) */
  if (l_castS2U(key) - 1 < t->sizearray) {
    return &t->array[key - 1];
  } else {
    Node *n = hashint(t, key);
    for (;;) {  /* check whether 'key' is somewhere in the chain */
      if (ttisinteger(gkey(n)) && ivalue(gkey(n)) == key)
        return gval(n);  /* that's it */
      else {
        int nx = gnext(n);
        if (nx == 0)
          break;
        n += nx;
      }
    }
    return luaO_nilobject;
  }
}


/*
** search function for short strings
*/
const TValue *luaH_getshortstr (Table *t, TString *key) {
  Node *n = hashstr(t, key);
  lua_assert(key->tt == LUA_TSHRSTR);
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    const TValue *k = gkey(n);
    if (ttisshrstring(k) && eqshrstr(tsvalue(k), key))
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);
      if (nx == 0)
        return luaO_nilobject;  /* not found */
      n += nx;
    }
  }
}


/*
** "Generic" get version. (Not that generic: not valid for integers,
** which may be in array part, nor for floats with integral values.)
*/
static const TValue *getgeneric (Table *t, const TValue *key) {
  Node *n = mainposition(t, key);
  for (;;) {  /* check whether 'key' is somewhere in the chain */
    if (luaV_rawequalobj(gkey(n), key))
      return gval(n);  /* that's it */
    else {
      int nx = gnext(n);
      if (nx == 0)
        return luaO_nilobject;  /* not found */
      n += nx;
    }
  }
}


const TValue *luaH_getstr (Table *t, TString *key) {
  if (key->tt == LUA_TSHRSTR)
    return luaH_getshortstr(t, key);
  else {  /* for long strings, use generic case */
    TValue ko;
    setsvalue(cast(lua_State *, NULL), &ko, key);
    return getgeneric(t, &ko);
  }
}


/*
** main search function
** 提取表t[key]的值的地址
*/
const TValue *luaH_get (Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TSHRSTR: 
      return luaH_getshortstr(t, tsvalue(key));
    case LUA_TNUMINT: 
      return luaH_getint(t, ivalue(key));
    case LUA_TNIL:
      return luaO_nilobject;
    case LUA_TNUMFLT: {
      lua_Integer k;
      if (luaV_tointeger(key, &k, 0)) /* index is int? */
        return luaH_getint(t, k);  /* use specialized version */
      /* else... */
    }  /* FALLTHROUGH */
    default:
      return getgeneric(t, key);
  }
}


/*
** beware: when using this function you probably need to check a GC
** barrier and invalidate the TM cache.
** 提取key对应的val的地址
**
** 若key在table中还不存在，则构建key，并返回其val的地址
** 若存在，直接返回其val的地址
*/
TValue *luaH_set (lua_State *L, Table *t, const TValue *key) {
  const TValue *p = luaH_get(t, key);
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else
  	return luaH_newkey(L, t, key);
}

void luaH_setint (lua_State *L, Table *t, lua_Integer key, TValue *value) {
  const TValue *p = luaH_getint(t, key);
  TValue *cell;
  if (p != luaO_nilobject) {
    cell = cast(TValue *, p);
  } else {
    TValue k;
    setivalue(&k, key);
    cell = luaH_newkey(L, t, &k);
  }
  setobj2t(L, cell, value);
}

/* 
** 可能情况的列举 
** tbl.a = 1            tbl.a   = 1
                        tbl.[5] = 1
** tbl.array is empty   tbl.array[1,1,nil,1]
*/
static int unbound_search (Table *t, unsigned int j) {
  unsigned int i = j;  /* i is zero or a present index */
  j++;
  /* find 'i' and 'j' such that i is present and j is not */
  while (!ttisnil(luaH_getint(t, j))) {
    i = j;
    if (j > cast(unsigned int, MAX_INT)/2) {  /* overflow? */
      /* table was built with bad purposes: resort to linear search */
      i = 1;
      while (!ttisnil(luaH_getint(t, i)))
        i++;
      return i - 1;
    }
    j *= 2;
  }
  /* now do a binary search between them */
  while (j - i > 1) {
    unsigned int m = (i+j)/2;
    if (ttisnil(luaH_getint(t, m)))
     j = m;
    else
     i = m;
  }
  return i;
}


/*
** Try to find a boundary in table 't'. A 'boundary' is an integer index
** such that t[i] is non-nil and t[i+1] is nil (and 0 if t[1] is nil).
** 
** 控制性函数
** 
** 这个函数仅对全是table.insert()生成的表做保证，
**   其它情况它不保证啥，因为那种情况下，也没有办法给一个明确的定义
** 
*/
int luaH_getn (Table *t) {
  unsigned int j = t->sizearray;
  
  /* 
  ** Lua仅保证对于array中的key是[1,N]连续状态时（N不需要满足N==array.size,可以比array.size小）,
  ** getn返回N，其它情况的返回值暂无明确定义
  */
  if (j > 0 && ttisnil(&t->array[j - 1])) 
  { 
    /* there is a boundary in the array part: (binary) search for it */
    unsigned int i = 0;
    while (j - i > 1) {
      unsigned int m = (i+j)/2;
      if (ttisnil(&t->array[m - 1]))
	  	  j = m;
      else
	  	  i = m;    /* 这里利用上面的假设:key在[1,N]是连续的
    }
    return i;

    /* else must find a boundary in hash part */
  } else if (isdummy(t->node)) {  /* hash part is empty? */
    return j;  /* that is easy... */
  }  
  /* 运行到这里，Lua保证的情况均已快速处理了 */
  /* 运行到这里，Lua保证的情况均已快速处理了 */
  /* 运行到这里，Lua保证的情况均已快速处理了 */
  else {
    /* 几种可能的情况 
    ** tbl.a = 1            tbl.a = 1
    ** tbl.array is empty   array[1,1,nil,1]
    */
    return unbound_search(t, j);
  }
}



#if defined(LUA_DEBUG)

Node *luaH_mainposition (const Table *t, const TValue *key) {
  return mainposition(t, key);
}

int luaH_isdummy (Node *n) { return isdummy(n); }

#endif
