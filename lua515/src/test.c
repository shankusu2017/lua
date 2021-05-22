#include "lauxlib.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>  
#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

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

int t_fun_c(lua_State *L) {
    printf("arg.cnt(%d)", lua_gettop(L));
}

int t_exe_lua(lua_State *vm) {
    {
        lua_getglobal(vm, "g_lua");
        lua_pushinteger(vm, 1);
        lua_pushinteger(vm, 2);
        lua_call(vm, 2, 0);

        return 0;
    }
}

int main(int argc, char *argv[]) {
    char buf[80];   
    getcwd(buf,sizeof(buf));   
    printf("cwd: %s\n", buf);   
 
    lua_State *vm = luaL_newstate();
    if (NULL == vm) {
        return -1;
    }
    luaL_openlibs(vm);  /* open libraries */

    {

        // lua_pushcfunction(vm, t_fun_c);
        // lua_setglobal(vm, "fun_c");
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
   ret = loadFile(vm, "../script/opcode.lua");
   if (0 != ret) {
       printf("load file fail, ret(%d)", ret);
       return -2;
   }
   printf("execute lua.script\n");
   {
        // lua_pushnumber(vm, 1);
        // lua_pushnumber(vm, 1);
        // lua_pushnumber(vm, 1);
        // lua_pushnumber(vm, 1);
        // ret = lua_pcall(vm, 0, LUA_MULTRET, 0);
   }
   ret = lua_pcall(vm, 0, LUA_MULTRET, 0);
   if (0 != ret) {
       char  errmsg [1024];
       size_t errLen;
       printf("call fail, ret(%d). err(%s)", ret, luaL_optlstring(vm, -1, "nil", &errLen));
       return -2;
   }

   t_exe_lua(vm);

    //t_db(vm);
}

