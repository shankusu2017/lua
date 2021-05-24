/*
** $Id: lcorolib.c,v 1.10.1.1 2017/04/19 17:20:42 roberto Exp $
** Coroutine Library
** See Copyright Notice in lua.h
*/

#define lcorolib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdlib.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

/* 从当前线程中获取创建的协程对象，协程对象是当前线程正在执行的函数调用的栈中的首位元素 */
static lua_State *getco (lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  luaL_argcheck(L, co, 1, "thread expected");
  return co;
}

/* 唤醒协程co */
/*
** 调用辅助函数auxresume()来执行resume操作，当resume操作出错时，auxresume()函数的返回值小于0，
** 那么在auxresume()函数中会将错误信息压入栈顶；当resume操作成功时，auxresume()函数的返回值大于0，
** 在auxresume()函数中也会将resume操作的返回值（可能有多个）依次压入栈顶部。
*/
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status;
  
  /*
  ** 确保新线程的栈中有足够的空闲栈单元，没有的话，会延长栈空间。同时将新线程
  ** 中的当前活跃函数调用信息（即L->ci）中的ci->top设置为co->top + narg。
  ** 如果检查栈出错，那么将对应的错误信息压入栈顶部。同时该函数返回-1表示出错。
  */
  if (!lua_checkstack(co, narg)) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }

  /*
  ** 判断将要resume的协程的状态是不是dead，如果是的话，那么resume协程出错，此时会将
  ** 错误信息压入栈顶部。同时该函数返回-1表示出错。
  */
  if (lua_status(co) == LUA_OK && lua_gettop(co) == 0) {
    lua_pushliteral(L, "cannot resume dead coroutine");
    return -1;  /* error flag */
  }

  /* 将当前线程的栈中的最顶部narg个元素移到新线程中的栈中去，作为新线程中待执行函数的参数。 */
  lua_xmove(L, co, narg);

  /*
  ** 调用lua_resume()函数来触发resume操作。lua_resume()会将resume操作的返回值或者错误信息压入协程
  ** 压入协程的栈中。
  */
  status = lua_resume(co, L, narg);
  if (status == LUA_OK || status == LUA_YIELD) {

    /*
    ** 程序进入这个分支，说明resume操作执行成功，且resume操作的返回值也已经压入协程的栈顶部了。
    */
    int nres = lua_gettop(co);

    /*
    ** 如果resume操作返回值太多会而调用协程的线程的空闲栈单元不够，则会报错，同时丢弃协程栈中的
    ** resume操作的返回值，同时往调用协程的线程的栈顶部压入错误信息。同时该函数返回-1表示出错。
    */
    if (!lua_checkstack(L, nres + 1)) {
      lua_pop(co, nres);  /* remove results anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }

	/* 
	** 将位于协程栈顶部的nres个resume操作的返回值移到调用协程的线程的栈顶部，同时返回
	** resume操作的返回值个数。
	*/
    lua_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  else {
    /* 
    ** 程序进入这个分支，说明resume操作出错，则将位于协程栈顶部的错误信息移到调用协程的
    ** 线程的栈顶中。同时该函数返回-1表示出错。
    */
    lua_xmove(co, L, 1);  /* move error message */
    return -1;  /* error flag */
  }
}

/* resume的接口函数 */
static int luaB_coresume (lua_State *L) {

  /* 从当前线程的栈中获取新创建的协程对象。 */
  lua_State *co = getco(L);
  int r;
  
  /*
  ** 调用辅助函数auxresume()来执行resume操作，当auxresume()函数的返回值小于0时，说明
  ** resume操作出错，那么在auxresume()函数中会将错误信息压入栈顶。此时luaB_coresume()在
  ** 栈顶中压入了bool值false，然后对调错误信息和false在栈中的位置，对调完成后错误信息跑到
  ** 了栈顶部，false跑到了栈次顶部；如果auxresume()函数的返回值大于0，说明resume操作成功，
  ** 在auxresume()函数中也会将resume操作的返回值（可能有多个）依次压入栈顶部。此时
  ** luaB_coresume()会在栈顶部压入bool值true，然后对调true和resume操作的返回值（多个）在
  ** 栈中的位置，对调完成后resume操作的返回值在栈顶部，而true则跑到了resume操作最先压入栈中的
  ** 那个返回值的上一个栈单元中。
  ** luaB_coresume()函数的返回值表示resume操作的总的结果个数，第一个结果是表示resume成功与否的
  ** bool值，接下来的是错误信息或者resume的返回值（可能有多个，看yield传递的参数个数。）。
  */
  r = auxresume(L, co, lua_gettop(L) - 1);
  if (r < 0) {
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;  /* return false + error message */
  }
  else {
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}

/* luaB_cowrap()的辅助函数 */
static int luaB_auxwrap (lua_State *L) {
  /* 
  ** 取出该函数的索引值为1的自由变量，从luaB_cowrap()中知道，luaB_auxwrap()其实就一个自由变量，
  ** 该自由变量就是新创建的线程对象。
  */
  lua_State *co = lua_tothread(L, lua_upvalueindex(1));

  /*
  ** 调用辅助函数auxresume()来执行resume操作，当resume操作出错时，auxresume()函数的返回值小于0，
  ** 那么在auxresume()函数中会将错误信息压入栈顶；当resume操作成功时，auxresume()函数的返回值大于0，
  ** 在auxresume()函数中也会将resume操作的返回值（可能有多个）压入栈顶部。
  */
  int r = auxresume(L, co, lua_gettop(L));
  if (r < 0) {
    /* 程序进入这个分支，说明执行resume操作出错了。 */

    /*
    ** 判断错误信息是不是一个字符串，如果是的话，则向栈顶部压入描述出错函数的位置等信息，
    ** 并与此时位于栈次顶部的错误信息对调一下位置，然后将位于栈顶部的错误信息和栈次顶部的
    ** 错误函数的信息链接起来，将链接后的字符串放置于栈次顶部，接着将栈指针减1，使得栈次顶部
    ** 变成了新的栈顶部。因此整个错误信息此时就在栈顶部。
    */
    if (lua_type(L, -1) == LUA_TSTRING) {  /* error object is a string? */
      luaL_where(L, 1);  /* add extra info */
      lua_insert(L, -2);
      lua_concat(L, 2);
    }

    /* 抛出异常或者对错误进行处理 */
    return lua_error(L);  /* propagate error */
  }

  /* 
  ** 程序执行到这里，说明resume操作时成功的，那么就返回resume操作返回值的个数，
  ** 而对应的返回值则在auxresume()函数中依次压入了栈顶部。此时返回值就在栈顶部
  ** 的r个栈单元中。
  */
  return r;
}

/* 
** 创建一个协程  的接口，同时将L中栈索引值为1的数据类型是函数的TValue对象移动到
** 新创建的线程的栈顶部。执行完该函数之后，包含了新创建线程lua_State对象的TValue对象
** 位于当前线程L的栈的栈顶部，同时要在新线程中执行的函数已经压入到新线程的栈的栈顶部了。
*/
static int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  
  /* 检查栈索引值为1的TValue的对象所包含的数据的类型是不是函数类型 */
  luaL_checktype(L, 1, LUA_TFUNCTION);
  
  /* 创建一个新的线程对象，返回线程的对外接口数据结构lua_State的对象，并将其压入栈顶部 */
  NL = lua_newthread(L);

  /* 将栈索引值为1的函数对象再次压入L的栈的栈顶部 */
  lua_pushvalue(L, 1);  /* move function to top */
  
  /* 将此时位于当前线程L栈中的栈顶部元素移到新线程NL的栈顶部中。 */
  lua_xmove(L, NL, 1);  /* move function from L to NL */

  /* 程序执行到这里，包含了新创建线程lua_State对象的TValue对象位于当前线程L的栈顶部。 */
  return 1;
}


/* wrap接口函数 */
static int luaB_cowrap (lua_State *L) {
  /* 
  ** 创建一个协程（其实也就是一个lua线程），协程中要执行的函数已经放入到新创建的
  ** 协程对应的lua_State对象的栈顶部了。同时包含了协程对应的lua_State对象的TValue对象
  ** 此时就位于当前线程L的栈顶部。新创建的线程lua_State对象会作为函数luaB_auxwrap()
  ** 的一个自由变量。
  */
  luaB_cocreate(L);

  /*
  ** 程序执行到这里，新创建的线程此时就位于栈顶部，在lua_pushcclosure()函数中，会创建一个
  ** 函数luaB_auxwrap对应的CClosure对象，并将此时位于栈顶部的新创建的协程当作该函数的一个
  ** 自由变量，同时将该CClosure对象压入当前线程的栈顶部。
  */
  lua_pushcclosure(L, luaB_auxwrap, 1);
  return 1;
}


/* 
** yield操作的接口函数，调用该函数之前，传递给yield的参数已经存放到栈中了，这些参数位于
** 协程栈顶部。
*/
static int luaB_yield (lua_State *L) {
  return lua_yield(L, lua_gettop(L));
}


/* 获取当前线程所创建的协程的状态，并将描述状态的字符串对象压入当前线程的栈顶部 */
static int luaB_costatus (lua_State *L) {
  lua_State *co = getco(L);
  if (L == co) lua_pushliteral(L, "running");
  else {
    switch (lua_status(co)) {
      case LUA_YIELD:
        lua_pushliteral(L, "suspended");
        break;
      case LUA_OK: {
        lua_Debug ar;
        if (lua_getstack(co, 0, &ar) > 0)  /* does it have frames? */
          lua_pushliteral(L, "normal");  /* it is running */
        else if (lua_gettop(co) == 0)
            /* 没有栈帧的协程是dead的 */
            lua_pushliteral(L, "dead");
        else
          lua_pushliteral(L, "suspended");  /* initial state */
        break;
      }
      default:  /* some error occurred */
        lua_pushliteral(L, "dead");
        break;
    }
  }
  return 1;
}

/* 判断当前线程是不是可中断执行的，并将判断结果压入栈顶部。 */
static int luaB_yieldable (lua_State *L) {
  lua_pushboolean(L, lua_isyieldable(L));
  return 1;
}


/*
** 获取当前在运行的协程，获取的协程位于栈次顶部单元，栈顶部单元的值用于表明获取到的线程是不是主线程，
** 如果为1，表明是主线程，否则不是主线程。
*/
static int luaB_corunning (lua_State *L) {
  /* 向栈顶部压入当前线程自身，并判断当前线程是不是主线程，是的话，则返回1；否则为0。 */
  int ismain = lua_pushthread(L);

  /* 将上条语句的结果压入栈顶部 */
  lua_pushboolean(L, ismain);
  return 2;
}


/* coroutine库包含的接口函数 */
static const luaL_Reg co_funcs[] = {
  {"create", luaB_cocreate},
  {"resume", luaB_coresume},
  {"running", luaB_corunning},
  {"status", luaB_costatus},
  {"wrap", luaB_cowrap},
  {"yield", luaB_yield},
  {"isyieldable", luaB_yieldable},
  {NULL, NULL}
};



/*
** coroutine库的加载函数，执行完该函数之后，位于栈顶部的就是coroutine库对应的库级别的table，
** 这个table中包含了coroutine中的所有函数对应的CClosure对象，键值是函数名字。
*/
LUAMOD_API int luaopen_coroutine (lua_State *L) {
  luaL_newlib(L, co_funcs);
  return 1;
}

