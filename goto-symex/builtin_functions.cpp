/*******************************************************************\

Module: Symbolic Execution of ANSI-C

Author: Daniel Kroening, kroening@kroening.com

\*******************************************************************/

#include <assert.h>

#include <expr_util.h>
#include <i2string.h>
#include <arith_tools.h>
#include <cprover_prefix.h>
#include <std_types.h>

#include <ansi-c/c_types.h>

#include "goto_symex.h"
#include "execution_state.h"

/*******************************************************************\

Function: goto_symext::symex_malloc

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::symex_malloc(
  statet &state,
  const exprt &lhs,
  const side_effect_exprt &code,
  execution_statet &ex_state,
        unsigned node_id)
{
  if(code.operands().size()!=1)
    throw "malloc expected to have one operand";
    
  if(lhs.is_nil())
    return; // ignore

  // size
  typet type=static_cast<const typet &>(code.cmt_type());
  exprt size=static_cast<const exprt &>(code.cmt_size());
  bool size_is_one;

  if(size.is_nil())
    size_is_one=true;
  else
  {
    state.rename(size, ns, node_id);
    mp_integer i;
    size_is_one=(!to_integer(size, i) && i==1);
  }
  
  if(type.is_nil())
    type=char_type();

  ex_state.dynamic_counter++;

  // value
  symbolt symbol;

  symbol.base_name="dynamic_"+
    i2string(ex_state.dynamic_counter)+
    (size_is_one?"_value":"_array");

  symbol.name="symex_dynamic::"+id2string(symbol.base_name);
  symbol.lvalue=true;
  
  if(size_is_one)
    symbol.type=type;
  else
  {
    symbol.type=typet(typet::t_array);
    symbol.type.subtype()=type;
    symbol.type.size(size);
  }

  symbol.type.dynamic(true);

  symbol.mode="C";

  new_context.add(symbol);
  
  exprt rhs(exprt::addrof, typet(typet::t_pointer));
  
  if(size_is_one)
  {
    rhs.type().subtype()=symbol.type;
    rhs.copy_to_operands(symbol_expr(symbol));
  }
  else
  {
    exprt index_expr(exprt::index, symbol.type.subtype());
    index_expr.copy_to_operands(symbol_expr(symbol), gen_zero(int_type()));
    rhs.type().subtype()=symbol.type.subtype();
    rhs.move_to_operands(index_expr);
  }
  
  if(rhs.type()!=lhs.type())
    rhs.make_typecast(lhs.type());

  state.rename(rhs, ns,node_id);
  
  guardt guard;
  symex_assign_rec(state, ex_state, lhs, rhs, guard,node_id);

  // Mark that object as being dynamic, in the __ESBMC_is_dynamic array
  exprt sym("symbol", array_typet());
  sym.type().subtype() = bool_typet();
  sym.set("identifier", "__ESBMC_is_dynamic");
  exprt pointerobj("pointer_object", signedbv_typet());
  exprt ptrsrc = lhs;
  pointerobj.move_to_operands(ptrsrc);
  exprt index("index", bool_typet());
  index.move_to_operands(sym, pointerobj);
  exprt truth("constant", bool_typet());
  truth.set("value", "true");
  symex_assign_rec(state, ex_state, index, truth, guard,node_id);
}

/*******************************************************************\

Function: goto_symext::symex_printf

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::symex_printf(
  statet &state,
  const exprt &lhs,
  const exprt &rhs,
        unsigned node_id)
{
  if(rhs.operands().empty())
    throw "printf expected to have at least one operand";

  exprt tmp_rhs=rhs;
  state.rename(tmp_rhs, ns, node_id);

  const exprt::operandst &operands=tmp_rhs.operands();
  std::list<exprt> args;

  for(unsigned i=1; i<operands.size(); i++)
    args.push_back(operands[i]);

  const exprt &format=operands[0];
  
  if(format.id()==exprt::addrof &&
     format.operands().size()==1 &&
     format.op0().id()==exprt::index &&
     format.op0().operands().size()==2 &&
     format.op0().op0().id()=="string-constant" &&
     format.op0().op1().is_zero())
  {
    const exprt &fmt_str=format.op0().op0();
    const std::string &fmt=fmt_str.value().as_string();

    target->output(state.guard, state.source, fmt, args);
  }
}

/*******************************************************************\

Function: goto_symext::symex_cpp_new

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::symex_cpp_new(
  statet &state,
  const exprt &lhs,
  const side_effect_exprt &code,
  execution_statet &ex_state,
        unsigned node_id)
{
  bool do_array;

  if(code.type().id()!=typet::t_pointer)
    throw "new expected to return pointer";

  do_array=(code.statement()=="cpp_new[]");
      
  ex_state.dynamic_counter++;

  const std::string count_string(i2string(ex_state.dynamic_counter));

  // value
  symbolt symbol;
  symbol.base_name=
    do_array?"dynamic_"+count_string+"_array":
             "dynamic_"+count_string+"_value";
  symbol.name="symex_dynamic::"+id2string(symbol.base_name);
  symbol.lvalue=true;
  symbol.mode="C++";
  
  if(do_array)
  {
    symbol.type=array_typet();
    symbol.type.subtype()=code.type().subtype();
    symbol.type.size(code.size_irep());
  }
  else
    symbol.type=code.type().subtype();

  //symbol.type.active(symbol_expr(active_symbol));
  symbol.type.dynamic(true);
  
  new_context.add(symbol);

  // make symbol expression

  exprt rhs(exprt::addrof, typet(typet::t_pointer));
  rhs.type().subtype()=code.type().subtype();
  
  if(do_array)
  {
    exprt index_expr(exprt::index, code.type().subtype());
    index_expr.copy_to_operands(symbol_expr(symbol), gen_zero(int_type()));
    rhs.move_to_operands(index_expr);
  }
  else
    rhs.copy_to_operands(symbol_expr(symbol));
  
  state.rename(rhs, ns,node_id);

  guardt guard;
  symex_assign_rec(state, ex_state, lhs, rhs, guard,node_id);
}

/*******************************************************************\

Function: goto_symext::symex_cpp_delete

  Inputs:

 Outputs:

 Purpose:

\*******************************************************************/

void goto_symext::symex_cpp_delete(
  statet &state,
  const codet &code)
{
  //bool do_array=code.statement()=="delete[]";
}
