/*
** 本源文件中函数返回类型： l_noret funName(...),
**                       void funName(...), 两者的差异？
**
** 
** Lua完整的BNF(随源码lua-5.3.2.tar/lua-5.3.2/doc/manual.html发布的)
** 实际的代码和下面的BNF有点点差异，建议结合两者来看
**
**    chunk         ::= block
**    block         ::= {stat} [retstat]
**    stat          ::= ‘;’ |
**                       varlist ‘=’ explist |
**                       functioncall |
**                       label |
**                       break |
**                       goto Name |
**                       do block end |
**                       while exp do block end |
**                       repeat block until exp |
**                       if exp then block {elseif exp then block} [else block] end |
**                       for Name ‘=’ exp ‘,’ exp [‘,’ exp] do block end |
**                       for namelist in explist do block end |
**                       function funcname funcbody |
**                       local function Name funcbody |
**                       local namelist [‘=’ explist]
**    retstat       ::= return [explist] [‘;’]
**    label         ::= ‘::’ Name ‘::’
**    funcname      ::= Name {‘.’ Name} [‘:’ Name]
**    varlist       ::= var {‘,’ var}
**    var           ::= Name | prefixexp ‘[’ exp ‘]’ | prefixexp ‘.’ Name
**    namelist      ::= Name {‘,’ Name}
**    explist       ::= exp {‘,’ exp}
**    exp           ::= nil | false | true | Numeral | LiteralString | ‘...’ | functiondef |
**                      prefixexp | tableconstructor | exp binop exp | unop exp
**    prefixexp     ::= var | functioncall | ‘(’ exp ‘)’
**    functioncall  ::= prefixexp args | prefixexp ‘:’ Name args
**    args          ::= ‘(’ [explist] ‘)’ | tableconstructor | LiteralString
**    functiondef   ::= function funcbody
**    funcbody      ::= ‘(’ [parlist] ‘)’ block end
**    parlist       ::= namelist [‘,’ ‘...’] | ‘...’
**    tableconstructor ::= ‘{’ [fieldlist] ‘}’
**    fieldlist     ::= field {fieldsep field} [fieldsep]
**    field         ::= ‘[’ exp ‘]’ ‘=’ exp | Name ‘=’ exp | exp
**    fieldsep      ::= ‘,’ | ‘;’
**    binop         ::= ‘+’ | ‘-’ | ‘*’ | ‘/’ | ‘//’ | ‘^’ | ‘%’ |
**                      ‘&’ | ‘~’ | ‘|’ | ‘>>’ | ‘<<’ | ‘..’ |
**                      ‘<’ | ‘<=’ | ‘>’ | ‘>=’ | ‘==’ | ‘~=’ |
**                      and | or
**    unop          ::= ‘-’ | not | ‘#’ | ‘~’
** 
*/