/*
** $Id: ltable.c,v 2.118.1.4 2018/06/08 16:22:51 roberto Exp $
** Lua tables (hash)
** See Copyright Notice in lua.h
*/

#define ltable_c
#define LUA_CORE

#include "lprefix.h"


/*
** Implementation of tables (aka arrays, objects, or hash tables).
** Tables keep its elements in two parts: an array part and a hash part.
** Non-negative integer keys are all candidates to be kept in the array
** part. The actual size of the array is the largest 'n' such that
** more than half the slots between 1 and n are in use.
** Hash uses a mix of chained scatter table with Brent's variation.
** A main invariant of these tables is that, if an element is not
** in its main position (i.e. the 'original' position that its hash gives
** to it), then the colliding element is in its own main position.
** Hence even when the load factor reaches 100%, performance remains good.
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


/*
** lmod((n), sizenode(t))--用于实现n对散列数组大小取模，即获得n在数组中对应的下标
** 然后gnode()用于获取该下标在散列数组中对应的索引。由于下标是对散列数组大小取模得到
** 的，因此gnode()获得的Node节点肯定是在[1,lsizenode]之间，lsizenode是散列数组大小
** 对2取对数的结果。因此mainposition的范围是[1,lsizenode]，然后处于同一个mainposition
** 的Node通过非常规链表（链表中每个节点存放下一个节点在散列数组中的下标，而不是下一个
** 节点的地址）串接起来。
*/
#define hashpow2(t,n)		(gnode(t, lmod((n), sizenode(t))))

#define hashstr(t,str)		hashpow2(t, (str)->hash)
#define hashboolean(t,p)	hashpow2(t, p)
#define hashint(t,i)		hashpow2(t, i)


/*
** for some types, it is better to avoid modulus by power of 2, as
** they tend to have many 2 factors.
*/
#define hashmod(t,n)	(gnode(t, ((n) % ((sizenode(t)-1)|1))))


#define hashpointer(t,p)	hashmod(t, point2uint(p))


#define dummynode		(&dummynode_)

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
static int l_hashfloat (lua_Number n) {
  int i;
  lua_Integer ni;
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
*/
/*
** lua表中散列表部分的组织是，首先计算数据的key所在的桶数组的位置，这个位置称为
** mainposition，通过查看hashint()的实现我们可以看到lua表中散列表的具体实现，如下：
** 由于lua表中既有数组部分，又有散列表部分，为了对外实现统一的操作接口，lua表中的
** 散列表实现也是通过一个数组来实现的，暂且称之为散列数组。假设其大小为n（一定要为2的
** 整数次幂），那么lsizenode中存放的就是n取对数得到的结果。从hashint()的实现中我们可以
** 看到，散列桶的大小是lsizenode，即散列数组中[1,lsizenode]之间的这几个元素就是散列桶
** “链表”的首元素，然后桶中的每个元素都会保存落在同一个桶中的下一个元素在整个散列数组中
** 的下标，从而将落在同一个散列桶的元素串接起来。mainposition()函数返回的值是[1,lsizenode]
** 之间的某个值，即散列桶的首个Node节点的地址。
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
** returns the index for 'key' if 'key' is an appropriate key to live in
** the array part of the table, 0 otherwise.
*/
/*
** 如果key在table的数组部分，那么arrayindex()会返回这个key在数组的下标（即索引）。
** 在该函数中，如果key并不是一个整数，那么直接返回0表示未在数组中找到；如果key是
** 一个范围在(0, MAXASIZE]之间的整数，那么就返回该整数，亦即数组的索引。但如果key
** 虽然是一个整数，但是不在上述范围之内，也是一个不合法的下标，故此时也会返回0.
*/
static unsigned int arrayindex (const TValue *key) {
  /* 判断key对象中的数据部分是不是一个整数 */
  if (ttisinteger(key)) {
    lua_Integer k = ivalue(key);

    /* 判断该整数是否合法，合法则返回该整数 */
    if (0 < k && (lua_Unsigned)k <= MAXASIZE)
      return cast(unsigned int, k);  /* 'key' is an appropriate array index */
  }
  return 0;  /* 'key' did not match some condition */
}


/*
** returns the index of a 'key' for table traversals. First goes all
** elements in the array part, then elements in the hash part. The
** beginning of a traversal is signaled by 0.
*/
/*
** 返回key对应的用于遍历lua表的下标（即索引），在该函数中，首先遍历table的
** 数组部分，如果找到了那么就返回该索引，如果没有找到，那么就遍历table的散列表
** 部分，如果还是没有找到，那么就返回0。注：lua的数组索引是从1开始的，0是一个
** 不合法的索引，所以可以通过返回一个0来表示没有找到对应的key对应的索引。
*/
static unsigned int findindex (lua_State *L, Table *t, StkId key) {
  unsigned int i;
  /* 如果key是一个nil对象，那么就返回0。 */
  if (ttisnil(key)) return 0;  /* first iteration */

  /*
  ** 首先用key去遍历table的数组部分，如果arrayindex()返回的索引值不为0，且小于等于
  ** 数组的大小，那说明这个索引就落在了数组部分，而不是散列表部分；如果arrayindex()
  ** 返回的索引超过了数组的大小，那说明这个索引虽然是一个整数，但是会存放在散列表中
  */
  i = arrayindex(key);
  if (i != 0 && i <= t->sizearray)  /* is 'key' inside array part? */
    /* 落在了数组部分 */
    return i;  /* yes; that's the index */
  else {
    /*
    ** 进入此分支表明key落在了散列表部分，这个时候首先利用key计算出其在散列表的
    ** mainposition。由于具有相同mainposition的Node节点会以“链表”的形式组织起来，
    ** 所以找到mainposition之后需要遍历这个“链表”（非常规链表，下面有解释）找到
    ** key对应的节点。
    */
    int nx;
    Node *n = mainposition(t, key);
    for (;;) {  /* check whether 'key' is somewhere in the chain */
      /* key may be dead already, but it is ok to use it in 'next' */
      /*
      ** 依次比较“链表”中每个Node的key节点和参数中指定的key节点是否相等，如果相等，
      ** 那么就找到了对应的节点，然后计算其下标；如果不想等，那么需要进一步看链表
      ** 中对应的Node节点是否处于待GC回收的状态，并且判断key节点是否是需要GC的类型
      ** 如果以上条件均满足，那么进一步比较key的内容和Node节点中key的内容是否相等，
      ** 如果相等，那么也当作是找到了对应的节点，也会计算出下标，并返回。
      */
      if (luaV_rawequalobj(gkey(n), key) ||
            (ttisdeadkey(gkey(n)) && iscollectable(key) &&
             deadvalue(gkey(n)) == gcvalue(key))) {
        /*
        ** 我们知道，lua表中的散列表是用一个元素类型为Node *数组实现的，那么它是怎么
        ** 实现散列功能的呢？在Node的key节点中，有一个next成员，这个next成员是用于
        ** 保存落在同一个mainposition中的Node节点在整个散列数组中的下标。因此，lua表中的
        ** 散列表实现和lua字符串中的散列表实现有所不同，lua字符串的散列表中落在同一个
        ** 桶中的元素直接包含了下一个元素的地址（显示指定的地址），而lua表的散列表
        ** 中落在同一个桶（mainposition）的元素会存放也落在该桶中的元素在整个散列数组的
        ** 下标，这样通过访问桶中某个元素中存放的下一个元素的下标，就可以找到下一个元素，
        ** 进而也实现了类似链表的功能。故计算下标的时候可以直接用所获得的Node节点地址
        ** 减去Node数组的起始地址来获得。lua表中的散列表是用一个Node*类型的数组实现的，
        ** 因此元素在散列表的索引就是在Node*数组中的下标。
        */
        i = cast_int(n - gnode(t, 0));  /* key index in hash table */
        /* hash elements are numbered after array ones */
        /*
        ** 处于hash表中的元素索引值是在数组部分之后，故这里需要加上数组的大小，由于
        ** 索引值从1开始，所以这里还需要加上一个1。
				*/
        return (i + 1) + t->sizearray;
      }

      /* 获取下一个处于同一个mainposition的Node节点 */
      nx = gnext(n);
      if (nx == 0)
        luaG_runerror(L, "invalid key to 'next'");  /* key not found */
      else n += nx;
    }
  }
}

/*
** 外层通过调用luaH_next()接口就可以得到当前key对应的元素值和下一个key对应的索引。
** 在lunH_next()实现中，之所以将key对应的value保存在了紧跟在key之后的内存中，是因为
** 外层在调用luaH_next()的时候，就已经为其在key对象之后预留了一个TValue大小的内存
** 用于存放获取到的具体内容。外层调用可以参考lua_next()。
*/
int luaH_next (lua_State *L, Table *t, StkId key) {

  /* 找出key对象在lua表中的索引，详细见findindex()的注解 */
  unsigned int i = findindex(L, t, key);  /* find original element */

  /*
  ** 首先尝试从lua表中的数组部分查找，如果没有找到，就尝试从散列表部分查找。
  ** 如果key落在了数组部分，并且key对应的内容也是合法的（不为nil），那么就
  ** 修改key中的索引，改为原索引的下一个索引，由于是数组，故下一个索引即为
  ** 原索引+1。同时将原索引对应的value对象拷贝到key对象之后。这样外层通过调用luaH_next()
  ** 接口就可以得到下一个元素的索引和当前索引对应的元素值。
  */
  for (; i < t->sizearray; i++) {  /* try first array part */
    if (!ttisnil(&t->array[i])) {  /* a non-nil value? */
      setivalue(key, i + 1); /* 保存下一个元素的索引 */
      setobj2s(L, key+1, &t->array[i]); /* 保存获取到的value对象 */
      return 1; /* 返回1表示迭代成功，找到了内容 */
    }
  }
  for (i -= t->sizearray; cast_int(i) < sizenode(t); i++) {  /* hash part */
    if (!ttisnil(gval(gnode(t, i)))) {  /* a non-nil value? */

      /* 这个地方也会保存下一个元素的索引，因为这里将原索引对应的key对象直接保存
      ** 在了参数指定的key中，而我们知道lua表中的散列表中会在每个Node节点的key对象
      ** 中保存下一个Node节点在整个散列数组中的下标，因为这里只需要保存参数key对应
      ** 的Node节点的key对象就可以知道下一个对象的索引值了。
      */
      setobj2s(L, key, gkey(gnode(t, i)));
      setobj2s(L, key+1, gval(gnode(t, i))); /* 保存获取到的value对象 */
      return 1; /* 返回1表示迭代成功，找到了内容 */
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
** Compute the optimal size for the array part of table 't'. 'nums' is a
** "count array" where 'nums[i]' is the number of integers in the table
** between 2^(i - 1) + 1 and 2^i. 'pna' enters with the total number of
** integer keys in the table and leaves with the number of keys that
** will go to the array part; return the optimal size.
*/
static unsigned int computesizes (unsigned int nums[], unsigned int *pna) {
  int i;
  unsigned int twotoi;  /* 2^i (candidate for optimal size) */
  unsigned int a = 0;  /* number of elements smaller than 2^i */
  unsigned int na = 0;  /* number of elements to go to array part */
  unsigned int optimal = 0;  /* optimal size for array part */
  /* loop while keys can fill more than half of total size */
  for (i = 0, twotoi = 1;
       twotoi > 0 && *pna > twotoi / 2;
       i++, twotoi *= 2) {
    if (nums[i] > 0) {
      a += nums[i];
      if (a > twotoi/2) {  /* more than half elements present? */
        optimal = twotoi;  /* optimal size (till now) */
        na = a;  /* all elements up to 'optimal' will go to array part */
      }
    }
  }
  lua_assert((optimal == 0 || optimal / 2 < na) && na <= optimal);
  *pna = na;
  return optimal;
}


static int countint (const TValue *key, unsigned int *nums) {
  /* 返回key在lua表的数组部分的下标（即索引） */
  unsigned int k = arrayindex(key);

  /* k不等于0表明是一个有效的下标 */
  if (k != 0) {  /* is 'key' an appropriate array index? */
    nums[luaO_ceillog2(k)]++;  /* count as such */
    return 1;
  }
  else
    return 0;
}


/*
** Count keys in array part of table 't': Fill 'nums[i]' with
** number of keys that will go into corresponding slice and return
** total number of non-nil keys.
*/
static unsigned int numusearray (const Table *t, unsigned int *nums) {
  int lg;
  unsigned int ttlg;  /* 2^lg */
  unsigned int ause = 0;  /* summation of 'nums' */
  unsigned int i = 1;  /* count to traverse all array keys */
  /* traverse each slice */
  for (lg = 0, ttlg = 1; lg <= MAXABITS; lg++, ttlg *= 2) {
    unsigned int lc = 0;  /* counter */
    unsigned int lim = ttlg;
    if (lim > t->sizearray) {
      lim = t->sizearray;  /* adjust upper limit */
      if (i > lim)
        break;  /* no more elements to count */
    }
    /* count elements in range (2^(lg - 1), 2^lg] */
    for (; i <= lim; i++) {
      if (!ttisnil(&t->array[i-1]))
        lc++;
    }
    nums[lg] += lc;
    ause += lc;
  }
  return ause;
}


static int numusehash (const Table *t, unsigned int *nums, unsigned int *pna) {
  int totaluse = 0;  /* total number of elements */
  int ause = 0;  /* elements added to 'nums' (can go to array part) */
  int i = sizenode(t);
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
** setarrayvector()用于对lua表中的数组部分进行扩容或者初始化，
** 并对新增加的部分做一个简单的初始化，即设置为nil。并更新数组部分的大小。
*/
static void setarrayvector (lua_State *L, Table *t, unsigned int size) {
  unsigned int i;
  luaM_reallocvector(L, t->array, t->sizearray, size, TValue);
  for (i=t->sizearray; i<size; i++)
     setnilvalue(&t->array[i]);
  t->sizearray = size;
}


/*
** setnodevector()用于对lua表中的散列表部分进行初始化，即根据参数指定的大小申请散列数组
** 所需要的内存，并对散列数组做一个初始化，即在初始情况下，散列数组中的每个Node都没有后继
** 的Node，并将Node中的key和value信息设置为nil。
*/
static void setnodevector (lua_State *L, Table *t, unsigned int size) {
  if (size == 0) {  /* no elements to hash part? */
    t->node = cast(Node *, dummynode);  /* use common 'dummynode' */
    t->lsizenode = 0;
    t->lastfree = NULL;  /* signal that it is using dummy node */
  }
  else {
    int i;

    /* 求size对2取对数的值 */
    int lsize = luaO_ceillog2(size);
    if (lsize > MAXHBITS)
      luaG_runerror(L, "table overflow");

    /* lsize是将size对2取对数后的结果，故通过lsize求size时
    ** 只要将1左移lsize个位就可以了
    */
    size = twoto(lsize);

    /* 为散列数组申请内存 */
    t->node = luaM_newvector(L, size, Node);

    /* 初始化散列数组 */
    for (i = 0; i < (int)size; i++) {
      Node *n = gnode(t, i);
      gnext(n) = 0;
      setnilvalue(wgkey(n));
      setnilvalue(gval(n));
    }

    /* 设置lua表中的lsizenode和lastfree */
    t->lsizenode = cast_byte(lsize);
    t->lastfree = gnode(t, size);  /* all positions are free */
  }
}


typedef struct {
  Table *t;
  unsigned int nhsize;
} AuxsetnodeT;


/* 初始化ud中的table的散列表部分，包括设置一些管理信息及申请内存 */
static void auxsetnode (lua_State *L, void *ud) {
  AuxsetnodeT *asn = cast(AuxsetnodeT *, ud);
  setnodevector(L, asn->t, asn->nhsize);
}

/* 根据参数指定的数组部分大小和散列表部分的大小为table重新设置数组部分和散列表部分 */
void luaH_resize (lua_State *L, Table *t, unsigned int nasize,
                                          unsigned int nhsize) {
  unsigned int i;
  int j;
  AuxsetnodeT asn;

  /* 保存表中数组的原始大小 */
  unsigned int oldasize = t->sizearray;

  /* 保存lua表中散列表部分的散列数组的原始大小 */
  int oldhsize = allocsizenode(t);

  /* 保存旧的散列表数组 */
  Node *nold = t->node;  /* save old hash ... */

  /* 如果参数指定的数组大小大于数组的原始大小，那么就对数组部分进行扩容 */
  if (nasize > oldasize)  /* array part must grow? */
    setarrayvector(L, t, nasize);
  /* create new hash part with appropriate size */
  asn.t = t; asn.nhsize = nhsize;

  /* 重新为lua表中申请散列数组 */
  if (luaD_rawrunprotected(L, auxsetnode, &asn) != LUA_OK) {  /* mem. error? */
    setarrayvector(L, t, oldasize);  /* array back to its original size */
    luaD_throw(L, LUA_ERRMEM);  /* rethrow memory error */
  }

	/* 如果参数指定的数组大小小于数组的原始大小，那么就对数组部分进行缩容 */
  if (nasize < oldasize) {  /* array part must shrink? */
    t->sizearray = nasize;
    /* re-insert elements from vanishing slice */
    for (i=nasize; i<oldasize; i++) {
      if (!ttisnil(&t->array[i]))
        luaH_setint(L, t, i + 1, &t->array[i]);
    }
    /* shrink array */
    /* 对原始数组中超过nasize的那部分所占用的内存释放 */
    luaM_reallocvector(L, t->array, oldasize, nasize, TValue);
  }
  /* re-insert elements from hash part */
  /* 将旧的散列表中的内容插入到新的散列表中 */
  for (j = oldhsize - 1; j >= 0; j--) {
    Node *old = nold + j;
    if (!ttisnil(gval(old))) {
      /* doesn't need barrier/invalidate cache, as entry was
         already present in the table */
      setobjt2t(L, luaH_set(L, t, gkey(old)), gval(old));
    }
  }

  /* 如果旧的散列表非空，那么需要释放散列数组的内存 */
  if (oldhsize > 0)  /* not the dummy node? */
    luaM_freearray(L, nold, cast(size_t, oldhsize)); /* free old hash */
}

/* 根据参数指定的大小对table中的数组部分进行调整 */
void luaH_resizearray (lua_State *L, Table *t, unsigned int nasize) {
  int nsize = allocsizenode(t);
  luaH_resize(L, t, nasize, nsize);
}

/*
** nums[i] = number of keys 'k' where 2^(i - 1) < k <= 2^i
*/
/*
** 分配一个位图nums ，将其中的所有位置0。这个位图的意义在于：nums数组中第i个元素存
** 放的是key在2^(i-l)和2^i之间的元素数量。
** (2) 遍历Lua表中的数组部分，计算其中的元素数量，更新对应的nums 数组中的元素数量
** (numusearray函数）。
** (3) 遍历lua表中的散列桶部分，因为其中也可能存放了正整数，需要根据这里的正整数数量
** 更新对应的nums数组元素数量（numusehash 函数）。
** (4) 此时nums数组已经有了当前这个Table中所有正整数的分配统计，逐个遍历nu ms 数组，获
** 得其范围区间内所包含的整数数量大于50% 的最大索引，作为重新散列之后的数组大小，
** 超过这个范围的正整数，就分配到散列桶部分了（ computesizes 函数） 。
** (5) 根据上面计算得到的调整后的数组和散列桶大小调整表（ resize 函数）。
*/
static void rehash (lua_State *L, Table *t, const TValue *ek) {
  unsigned int asize;  /* optimal size for array part */
  unsigned int na;  /* number of keys in the array part */
  unsigned int nums[MAXABITS + 1];
  int i;
  int totaluse;
  for (i = 0; i <= MAXABITS; i++) nums[i] = 0;  /* reset counts */
  na = numusearray(t, nums);  /* count keys in array part */
  totaluse = na;  /* all those keys are integer keys */
  totaluse += numusehash(t, nums, &na);  /* count keys in hash part */
  /* count extra key */
  na += countint(ek, nums);
  totaluse++;
  /* compute new size for array part */
  asize = computesizes(nums, &na);
  /* resize the table to new computed sizes */
  luaH_resize(L, t, asize, totaluse - na);
}



/*
** }=============================================================
*/


/* luaH_new()函数用于创建一个新的lua表 */
Table *luaH_new (lua_State *L) {
  GCObject *o = luaC_newobj(L, LUA_TTABLE, sizeof(Table));
  Table *t = gco2t(o);
  t->metatable = NULL;
  t->flags = cast_byte(~0);
  t->array = NULL;
  t->sizearray = 0;
  setnodevector(L, t, 0);
  return t;
}

/*
** luaH_free()用于释放一个lua表，包括释放lua表中数组部分的内存，
** 以及散列表部分中散列数组的内存，最后释放lua表本身占用的内存。
*/
void luaH_free (lua_State *L, Table *t) {
  if (!isdummy(t))
    luaM_freearray(L, t->node, cast(size_t, sizenode(t)));
  luaM_freearray(L, t->array, t->sizearray);
  luaM_free(L, t);
}


/* getfreepos()会从后往前遍历散列数组，找到一个key信息为空的Node节点，然后返回 */
static Node *getfreepos (Table *t) {
  if (!isdummy(t)) {
    while (t->lastfree > t->node) {
      t->lastfree--;
      if (ttisnil(gkey(t->lastfree)))
        return t->lastfree;
    }
  }
  return NULL;  /* could not find a free place */
}



/*
** inserts a new key into a hash table; first, check whether key's main
** position is free. If not, check whether colliding node is in its main
** position or not: if it is not, move colliding node to an empty place and
** put new key in its main position; otherwise (colliding node is in its main
** position), new key goes to an empty position.
*/
/*
** 为key对象分配一个新的Node节点，并将key作为该Node节点的key信息，
** 然后返回该Node节点的value对象的指针
*/
TValue *luaH_newkey (lua_State *L, Table *t, const TValue *key) {
  Node *mp;
  TValue aux;

  /* 判断key是不是nil，如果是nil的，直接报错 */
  if (ttisnil(key)) luaG_runerror(L, "table index is nil");
  else if (ttisfloat(key)) {

    /*
    ** 如果进入这个分支，说明key对象是一个float类型的值，以mode=0来调用
    ** luaV_tointeger()判断key对象是不是一个整数，如果是一个整数的话，那么
    ** 获得这个整数后，生成一个新的key对象；如果不是一个整数的话，那么就会报错。
	  */
    lua_Integer k;
    if (luaV_tointeger(key, &k, 0)) {  /* does index fit in an integer? */
      setivalue(&aux, k); /* 取出原来key对象中的整数之后生成一个新的key对象 */
      key = &aux;  /* insert it as an integer */
    }
    else if (luai_numisnan(fltvalue(key)))
      luaG_runerror(L, "table index is NaN");
  }

  /*
  ** 根据key对象找出该key所落在的mainposition，并返回所在的
  ** mainposition中的第一个Node节点。
  */
  mp = mainposition(t, key);

  /*
  ** 如果key对象所在的mainposition已经有数据了， 那么需要为这个新的key对象
  ** 分配新的Node节点，
  */
  if (!ttisnil(gval(mp)) || isdummy(t)) {  /* main position is taken? */
    Node *othern;

    /* 从散列数组中选取一个空闲的Node节点 */
    Node *f = getfreepos(t);  /* get a free place */

		/*
		** 如果没有找到合适的Node节点，那么就对散列表做一个重hash，比如做一些扩容等。
		** 做了重hash之后，一般会有空闲的Node节点，这个时候将key信息作为这个Node节点
		** 的key对象，然后返回该Node节点value指针。
		*/
    if (f == NULL) {  /* cannot find a free place? */
      rehash(L, t, key);  /* grow table */
      /* whatever called 'newkey' takes care of TM cache */
      return luaH_set(L, t, key);  /* insert key into grown table */
    }
    lua_assert(!isdummy(t));

    /*
    ** 利用上面得到的所在mainposition的首个Node节点的key信息再计算一次mainposition，
    ** 看看两次计算得到的Node节点是不是同一个（通过比较地址是不是相等）。
    */
    othern = mainposition(t, gkey(mp));

    /* ***这部分还需要回过头来看*** */
    if (othern != mp) {  /* is colliding node out of its main position? */
      /* yes; move colliding node into free position */
      while (othern + gnext(othern) != mp)  /* find previous */
        othern += gnext(othern);
      gnext(othern) = cast_int(f - othern);  /* rechain to point to 'f' */
      *f = *mp;  /* copy colliding node into free pos. (mp->next also goes) */
      if (gnext(mp) != 0) {
        gnext(f) += cast_int(mp - f);  /* correct 'next' */
        gnext(mp) = 0;  /* now 'mp' is free */
      }
      setnilvalue(gval(mp));
    }
    else {  /* colliding node is in its own main position */
      /* new node will go into free position */

			/*
			** gnext(mp) != 0 说明所在mainposition除了首个Node节点之外还有其他Node节点，
			** 由于新分配的节点会被当作第二个Node节点，所以这里需要当新分配的Node节点的
			** next值设置为原来的第二个Node节点在散列数组中的位置。这样才能保证链表的
			** 一致性。
			*/
      if (gnext(mp) != 0)
        gnext(f) = cast_int((mp + gnext(mp)) - f);  /* chain new position */
      else lua_assert(gnext(f) == 0);
			/*
			** 将mp的下一个Node节点设置为f，即将新分配的Node节点插入到mainposition的
			** 首个Node节点之后（其实新分配的Node节点会当作mainposition中的第二个Node节点）
			*/
      gnext(mp) = cast_int(f - mp);
      mp = f;
    }
  }

	/* 将key对象作为Node节点的key信息，然后返回Node的value指针 */
  setnodekey(L, &mp->i_key, key);
  luaC_barrierback(L, t, key);
  lua_assert(ttisnil(gval(mp)));
  return gval(mp);
}


/*
** search function for integers
*/
/*
** 获取键值为整数的value对象。
** 获取key对应的value对象，首先从lua表的数组部分查找，如果没有落在数组部分，则
** 从散列表部分进行查找，在散列表中查找时，首先计算key对应的mainposition，然后
** 遍历处于该mainposition中的所有Node，返回符合条件的Node中的value对象。
*/
const TValue *luaH_getint (Table *t, lua_Integer key) {
  /* (1 <= key && key <= t->sizearray) */
  if (l_castS2U(key) - 1 < t->sizearray)
    return &t->array[key - 1];
  else {
    Node *n = hashint(t, key);
    for (;;) {  /* check whether 'key' is somewhere in the chain */
      if (ttisinteger(gkey(n)) && ivalue(gkey(n)) == key)
        return gval(n);  /* that's it */
      else {
        int nx = gnext(n);
        if (nx == 0) break;
        n += nx;
      }
    }
    return luaO_nilobject;
  }
}


/*
** search function for short strings
*/
/* 从lua表中获取键值为短字符串类型的value信息 */
const TValue *luaH_getshortstr (Table *t, TString *key) {

  /*
  ** 根据字符串的hash值从lua表的散列表部分获取所在的
  ** mainposition中的首个Node节点
  */
  Node *n = hashstr(t, key);
  lua_assert(key->tt == LUA_TSHRSTR);

  /*
  ** 遍历mainposition中所有的Node节点，找到键值等于参数指定的key的Node节点，
  ** 然后取出Node节点中的value信息。如果没有找到，则返回nil
  */
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
/*
** 根据参数指定的key信息从lua表的散列表部分获取对应的value信息。
** 流程是先计算出key信息对应的mainposition，然后遍历落在这个mainposition
** 中的所有Node节点，找到键值等于参数指定的key的Node节点，然后返回该节点的
** value信息。
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


/* 根据字符串类型的key从lua表中获取对应的value对象 */
const TValue *luaH_getstr (Table *t, TString *key) {
  /* 短字符串和长字符串分开处理。 */
  if (key->tt == LUA_TSHRSTR)
    /* 从lua表中获取键值为短字符串类型的value信息 */
    return luaH_getshortstr(t, key);
  else {  /* for long strings, use generic case */
    /*
    ** 如果是长字符串，则根据TString类型的key生成一个Node节点的key对象，
    ** 然后调用getgeneric()获取对应的vlaue信息。
    */
    TValue ko;
    setsvalue(cast(lua_State *, NULL), &ko, key);
    return getgeneric(t, &ko);
  }
}


/*
** main search function
*/
/*
** 根据key信息从lua表中获取对应的value信息。可以看到，在luaH_get()函数中根据
** key的不同类型分别进行了不同的处理，可以提高效率。
*/
const TValue *luaH_get (Table *t, const TValue *key) {
  switch (ttype(key)) {
    case LUA_TSHRSTR: return luaH_getshortstr(t, tsvalue(key));
    case LUA_TNUMINT: return luaH_getint(t, ivalue(key));
    case LUA_TNIL: return luaO_nilobject;
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
*/
/*
** 将key信息保存到lua表中，并返回key对应的value对象的指针，
** 上层调用者可以在这个指针指向的内存处存放value信息
*/
TValue *luaH_set (lua_State *L, Table *t, const TValue *key) {
  const TValue *p = luaH_get(t, key);
  if (p != luaO_nilobject)
    return cast(TValue *, p);
  else return luaH_newkey(L, t, key);
}

/*
** 存放一个键值为整数的value对象。
** 将参数中指定的value对象设置到key对应的节点（可能是数组部分，也可能是散列表部分）中，
** 设置的时候，首先尝试从lua表中查找，如果找到了对应的非空value，那么就将参数指定的
** value对象替换掉原来的value对象；如果lua表中不存在这个key对应的value对象，那么就在
** lua表中的散列表部分中插入一个新的Node节点，Node节点的key对象由参数中的key生成，
** Node节点的value对象即为参数指定的value对象。
*/
void luaH_setint (lua_State *L, Table *t, lua_Integer key, TValue *value) {
  const TValue *p = luaH_getint(t, key);
  TValue *cell;
  if (p != luaO_nilobject)
    cell = cast(TValue *, p);
  else {
    /* 根据参数指定的key在散列表中合适的位置创建一个Node节点，并
    ** 返回对应的value对象指针
    */
    TValue k;
    setivalue(&k, key);
    cell = luaH_newkey(L, t, &k);
  }

  /* 将参数指定的value对象设置为key对应的value对象 */
  setobj2t(L, cell, value);
}


static lua_Unsigned unbound_search (Table *t, lua_Unsigned j) {
  lua_Unsigned i = j;  /* i is zero or a present index */
  j++;
  /* find 'i' and 'j' such that i is present and j is not */
  while (!ttisnil(luaH_getint(t, j))) {
    i = j;
    if (j > l_castS2U(LUA_MAXINTEGER) / 2) {  /* overflow? */
      /* table was built with bad purposes: resort to linear search */
      i = 1;
      while (!ttisnil(luaH_getint(t, i))) i++;
      return i - 1;
    }
    j *= 2;
  }
  /* now do a binary search between them */
  while (j - i > 1) {
    lua_Unsigned m = (i+j)/2;
    if (ttisnil(luaH_getint(t, m))) j = m;
    else i = m;
  }
  return i;
}


/*
** Try to find a boundary in table 't'. A 'boundary' is an integer index
** such that t[i] is non-nil and t[i+1] is nil (and 0 if t[1] is nil).
*/
/* lua表中的去长度操作方法，即#。流程如下：
** 如果表存在数组部分：在数组部分二分查找返回位置i，其中i是满足条件
** t[i] != nil 且t[i + 1] = nil 的最大值。
** 否则前面的数组部分查不到满足条件的数据，则进入散列表部分查找：
** 在散列表部分二分查找返回位置i ，其中i是满足条件t[i] ! = nil 且t[i + 1] = nil 的最大值。
*/
lua_Unsigned luaH_getn (Table *t) {
  unsigned int j = t->sizearray;
  if (j > 0 && ttisnil(&t->array[j - 1])) {
    /* there is a boundary in the array part: (binary) search for it */
    unsigned int i = 0;
    while (j - i > 1) {
      unsigned int m = (i+j)/2;
      if (ttisnil(&t->array[m - 1])) j = m;
      else i = m;
    }
    return i;
  }
  /* else must find a boundary in hash part */
  else if (isdummy(t))  /* hash part is empty? */
    return j;  /* that is easy... */
  else return unbound_search(t, j);
}



#if defined(LUA_DEBUG)

Node *luaH_mainposition (const Table *t, const TValue *key) {
  return mainposition(t, key);
}

int luaH_isdummy (const Table *t) { return isdummy(t); }

#endif
