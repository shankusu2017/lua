#include "lauxlib.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>  
#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

int testF(void) {
  int a = 0;
  return 0;
  int b = 0;
}

int loadFile(lua_State *vm, char *path) {
    int ret = luaL_loadfile(vm, path);
    if (0 == ret) {
        return 0;
    }

    char reason[1024];
    const char *rea;
    size_t len;
    rea = luaL_checklstring(vm, -1, &len);
    printf("parse file(%s) fail, reason(%s)\n", rea);
    return -1;
}

int t_add(lua_State *L) {
    lua_Integer sum = 0;
    sum = lua_tointeger(L, -1) + lua_tointeger(L, -2);
    lua_pushinteger(L, sum);
    printf("sum(%d)\n", sum);
    return 1;
}

int t_db(lua_State *L) {
    lua_Debug ar;
    lua_getfield(L, LUA_GLOBALSINDEX, "ff"); /* get global 'f' */
    lua_getinfo(L, ">S", &ar);
    printf("%d\n", ar.linedefined);
}

int t_fun_c(lua_State *vm) {
    printf("arg.cnt(%d)", lua_gettop(vm));
    lua_getglobal(vm, "g_lua2");
    lua_pushinteger(vm, 1);
    lua_pushinteger(vm, 2);
    //lua_call(vm, 2, 0);
}

static int traceback (lua_State *L) {
  if (!lua_isstring(L, 1))  /* 'message' not a string? */
    return 1;  /* keep it intact */
  lua_getfield(L, LUA_GLOBALSINDEX, "debug");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  lua_getfield(L, -1, "traceback");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return 1;
  }
  lua_pushvalue(L, 1);  /* pass error message */
  lua_pushinteger(L, 2);  /* skip this function and traceback */
  lua_call(L, 2, 1);  /* call debug.traceback */
  size_t len;
  const char *errMsg  = lua_tolstring(L, -1, &len);
  printf("errMsg(%s", errMsg);
  return 1;
}

int t_exe_lua(lua_State *vm) {
    {
        lua_pushcclosure(vm, traceback, 0);
        lua_getglobal(vm, "g_lua");
        lua_pushinteger(vm, 1);
        lua_pushinteger(vm, 2);
        lua_pcall(vm, 2, LUA_MULTRET, -4);

        return 0;
    }
}

// void pp(int cnt) {
//     printf("cnt:%d");
// }

int lluaO_int2fb (unsigned int x) {
  int e = 0;  /* expoent(指数) */
  while (x >= 16) {
    x = (x+1) >> 1;
    e++;
  }
  if (x < 8) return x;
  else return ((e+1) << 3) | ((int)x - 8);
}


/* converts back */
 int lluaO_fb2int (int x) {
  int e = (x >> 3) & 31;
   if (e == 0) return x;
   else return ((x & 7)+8) << (e - 1);
 }

int main(int argc, char *argv[]) {
    {
      int fff;
      int hfesef;
      int testarr[100];
      int actb = 100;
      {
        int actb = actb;
        int test = actb;
        int test2  = actb;
      }
    }
    int ret1 = lluaO_int2fb(31);
    int ret2 = lluaO_fb2int(ret1);
    // {
    //     int cnt = 0;
    //     pp(cnt++);
    //     pp(cnt++);
    // }
    char buf[80];   
    getcwd(buf,sizeof(buf));   
    printf("cwd: %s\n", buf);   
 
    lua_State *vm = luaL_newstate();
    if (NULL == vm) {
        return -1;
    }
    luaL_openlibs(vm);  /* open libraries */

    // int times = 0;
    // for (times = 0; times < 100; times++) {
    //   lua_pushnumber(vm, times);
    //   lua_newtable(vm);
    //   lua_rawset(vm, LUA_GLOBALSINDEX);
    // }
    // lua_close(vm);
    // return 0;

    {   // register C'fun

        // lua_pushcfunction(vm, t_fun_c);
        // lua_setglobal(vm, "t_fun_c");
    }


    // lua_pushcfunction(vm, t_add);
    // lua_pushinteger(vm, 1);
    // lua_pushinteger(vm, 2);
    // lua_call(vm, 2, 1);
    // lua_Integer sum;
    // sum = lua_tointeger(vm, -1);
    // lua_settop(vm, 0);   // 扔掉返回值

    // {
    //     lua_newthread(vm);
    // }

   int ret = 0;
   printf("load lua.script\n");
   ret = loadFile(vm, "../script/pc.lua");
   if (0 != ret) {
       printf("load file fail, ret(%d)", ret);
       return -2;
   }
   printf("parse doen......");
  //  printf("execute lua.script\n");
   {
        // lua_pushnumber(vm, 1);
        // lua_pushnumber(vm, 1);
        // lua_pushnumber(vm, 1);
        // lua_pushnumber(vm, 1);
        // ret = lua_pcall(vm, 0, LUA_MULTRET, 0);
   }
  //  ret = lua_pcall(vm, 0, LUA_MULTRET, 0);
  //  if (0 != ret) {
  //      char  errmsg [1024];
  //      size_t errLen;
  //      printf("call fail, ret(%d). err(%s)", ret, luaL_optlstring(vm, -1, "nil", &errLen));
  //      return -2;
  //  }

//    t_exe_lua(vm);

    //t_db(vm);
}

