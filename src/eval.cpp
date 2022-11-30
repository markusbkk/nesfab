#include "eval.hpp"

#include <cassert>
#include <chrono>
#include <iostream> // TODO

#include <boost/container/small_vector.hpp>

#include "alloca.hpp"
#include "bitset.hpp"
#include "rval.hpp"
#include "decl.hpp"
#include "globals.hpp"
#include "file.hpp"
#include "options.hpp"
#include "ir.hpp"
#include "ir_util.hpp"
#include "stmt.hpp"
#include "eternal_new.hpp"
#include "lt.hpp"
#include "group.hpp"
#include "fnv1a.hpp"
#include "ast.hpp"
#include "compiler_error.hpp"
#include "asm_proc.hpp"
#include "text.hpp"
#include "switch.hpp"
#include "rom_decl.hpp"

namespace sc = std::chrono;
namespace bc = boost::container;
using namespace lex;

class eval_t;

using ssa_value_array_t = bc::small_vector<ssa_value_t, 1>;

// Data associated with each block node, to be used when making IRs.
struct block_d
{
    using vector_t = std::vector<ssa_value_array_t>;

    ssa_value_array_t& var(var_ht v) { passert(v.id < vars.size(), v.id, vars.size()); return vars[v.id]; }
    ssa_value_array_t& unsealed_phi(var_ht v) { passert(v.id < unsealed_phis.size(), v.id, vars.size()); return unsealed_phis[v.id]; }

    // An array of size 'num_vars()'
    // Keeps track of which ssa node a var refers to.
    // A handle of {} means the local var isn't in the block.
    vector_t vars;

    // An array of size 'num_vars()'
    // Phi nodes in the block which have yet to be sealed.
    vector_t unsealed_phis;

    // Only used for labels.
    pstring_t label_name = {};

    // The exit of an inline call points to the entrance,
    // allowing lookup function to skip over the inlined bits.
    cfg_ht pre_inline = {};

    // A CFG node is sealed when all its predecessors are set.
    bool sealed = false;

    // A root node represents the start of a function (including inlined functions).
    bool is_root = false;

#ifndef NDEBUG
    eval_t* creator = nullptr;
#endif
};

class eval_t
{
private:
    pstring_t pstring = {};
    fn_t* fn = nullptr;
    stmt_t const* stmt = nullptr;
    ir_t* ir = nullptr;
    bc::small_vector<rval_t, 8> interpret_locals;
    bc::small_vector<type_t, 8> var_types;

    using clock = sc::steady_clock;
    sc::time_point<clock> start_time;

    struct logical_data_t
    {
        cfg_ht branch_node;
        pstring_t lhs_pstring;
    };

    struct label_t
    {
        cfg_ht node = {};
        bc::small_vector<cfg_ht, 2> inputs;
    };

    struct switch_t
    {
        cfg_ht cfg;
        static_bitset_t<256> case_set;
    };

    // Data used by the ir builder can go inside this struct (for organization).
    struct ir_builder_t
    {
        // !!!
        // UPDATE 'clear()' WHEN MODIFYING THIS CODE!
        // !!!

        cfg_ht cfg = {}; // The current CFG node

        bc::small_vector<logical_data_t, 8> logical_stack;

        bc::small_vector<bc::small_vector<cfg_ht, 4>, 4> break_stack;
        bc::small_vector<bc::small_vector<cfg_ht, 4>, 4> continue_stack;
        bc::small_vector<switch_t, 4> switch_stack;

        // Holds switch cfgs that cover every case.
        bc::small_vector<cfg_ht, 1> exhaustive_switches;

        bc::small_vector<ssa_value_array_t, 8> return_values;
        bc::small_vector<cfg_ht, 8> return_jumps;

        rh::robin_map<stmt_t const*, label_t> label_map;

        void clear()
        {
            cfg = {};

            logical_stack.clear();

            break_stack.clear();
            continue_stack.clear();
            switch_stack.clear();

            exhaustive_switches.clear();
            
            return_values.clear();
            return_jumps.clear();

            label_map.clear();
        }
    };
    
    static thread_local ir_builder_t default_builder;
    ir_builder_t& builder = default_builder;

public:
    rpair_t final_result;
    byte_block_data_t byte_block_data; // Only used when defining byte blocks
    local_const_t const* local_consts = nullptr;
    precheck_tracked_t* precheck_tracked = nullptr; // Various things callers of 'eval_t' may want.
    romv_t romv = {};
    unsigned num_globals = 0;

    enum do_t
    {
        CHECK,         // Resolves types and syntax, but not values.
        INTERPRET_CE,  // Like INTERPRET, but can't read/write locals.
        INTERPRET_ASM, // Like INTERPRET, but for inline assembly.
        INTERPRET_LINK,// Like INTERPRET, but for link time.
        INTERPRET,     // Calculates values at compile-time.
        COMPILE,       // Generates the SSA IR.
    };

    static constexpr bool is_check(do_t d) { return d == CHECK; }
    static constexpr bool is_interpret(do_t d) { return d == INTERPRET_CE || d == INTERPRET_ASM || d == INTERPRET || d == INTERPRET_LINK; }
    static constexpr bool is_compile(do_t d) { return d == COMPILE; }
    static constexpr bool is_link(do_t d) { return d == INTERPRET_LINK; }

    type_t const& var_type(var_ht v) const { assert(v.id < var_types.size()); return var_types[v.id]; }
    type_t& var_type(var_ht v) { assert(v.id < var_types.size()); return var_types[v.id]; }

    stmt_ht stmt_handle() const { return { stmt - fn->def().stmts.data() }; }
    pstring_mods_t stmt_pstring_mods() const { return { stmt->pstring, fn->def().mods_of(stmt_handle()) }; }

    template<do_t Do>
    struct do_wrapper_t { static constexpr auto D = Do; };

    template<do_t D>
    eval_t(do_wrapper_t<D>, pstring_t pstring, fn_t* fn_ref,
           ast_node_t const& expr, type_t expected_type, 
           local_const_t const* local_consts = nullptr, romv_t romv = {});

    template<do_t D>
    eval_t(do_wrapper_t<D>, pstring_t pstring, fn_t& fn_ref, 
           precheck_tracked_t* tracked, rval_t const* args, unsigned num_args,
           local_const_t const* local_consts = nullptr);

    eval_t(ir_t& ir_ref, fn_t& fn_ref);

    // Inline version:
    eval_t(eval_t const& parent, ir_t& ir_ref, fn_t& fn_ref, ir_builder_t& builder,
           cfg_ht pre_entry, cfg_ht& exit, expr_value_t const* args, rval_t& return_rval);

    struct access_t
    {
        type_t type = {};
        unsigned member = 0;
        ssa_value_t index = {};
    };

    void check_time();

    template<do_t D>
    void interpret_stmts();

    void compile_block();

    template<do_t D>
    expr_value_t do_var_init_expr(var_ht var_i, ast_node_t const& expr);

    template<eval_t::do_t D>
    expr_value_t do_expr(ast_node_t const& ast);

    template<do_t D>
    void do_expr_result(ast_node_t const&, type_t expected_result);

    template<do_t D>
    expr_value_t do_assign(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    expr_value_t compile_binary_operator(
        expr_value_t const& lhs, expr_value_t const& rhs, 
        ssa_op_t op, type_t result_type, bool carry = false);

    template<typename Policy>
    expr_value_t do_compare(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_arith(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_add(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_shift(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_rotate(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_mul(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    //template<typename Policy>
    //expr_value_t interpret_shift(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_assign_arith(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_assign_add(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_assign_shift(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_assign_rotate(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<typename Policy>
    expr_value_t do_assign_mul(expr_value_t lhs, expr_value_t rhs, token_t const& token);

    template<do_t D>
    expr_value_t do_logical(ast_node_t const& ast);

    void req_quantity(token_t const& token, expr_value_t const& value);
    void req_quantity(token_t const& token, expr_value_t const& lhs, expr_value_t const& rhs);

    template<do_t D>
    expr_value_t force_truncate(expr_value_t value, type_t to_type, pstring_t cast_pstring);
    template<do_t D>
    expr_value_t force_promote(expr_value_t value, type_t to_type, pstring_t cast_pstring);
    template<do_t D>
    expr_value_t force_intify_ptr(expr_value_t value, type_t to_type, pstring_t cast_pstring);
    template<do_t D>
    expr_value_t force_ptrify_int(expr_value_t value, type_t to_type, pstring_t cast_pstring);
    template<do_t D>
    expr_value_t force_convert_int(expr_value_t value, type_t to_type, bool implicit, pstring_t cast_pstring);
    template<do_t D>
    expr_value_t force_round_real(expr_value_t value, type_t to_type, bool implicit, pstring_t cast_pstring);
    template<do_t D>
    expr_value_t force_boolify(expr_value_t value, pstring_t cast_pstring);
    template<do_t D>
    expr_value_t force_resize_tea(expr_value_t value, type_t to_type, pstring_t cast_pstring);

    template<do_t D>
    bool cast(expr_value_t& v, type_t to_type, bool implicit, pstring_t pstring = {});

    template<do_t D>
    expr_value_t throwing_cast(expr_value_t value, type_t to_type, bool implicit, pstring_t pstring = {});

    template<do_t D>
    int cast_args(pstring_t pstring, expr_value_t* begin, expr_value_t* end, type_t const* type_begin, bool implicit);

    std::size_t num_local_vars() const { assert(fn); return fn->def().local_vars.size(); }

    type_t var_i_type(var_ht var_i) const;
    void init_rval(access_t a, rval_t& rval);
    //access_t access(rpn_value_t const& rpn_value) const;
    ssa_value_t const& get_local(pstring_t pstring, var_ht var_i, unsigned member, unsigned index) const;
    ssa_value_t& get_local(pstring_t pstring, var_ht var_i, unsigned member, unsigned index);

    template<eval_t::do_t D>
    ssa_value_t from_variant(ct_variant_t const& v, type_t type);

    template<do_t D>
    expr_value_t to_rval(expr_value_t v);

    ///////////////////////
    // compiler-specific //
    ///////////////////////

    std::size_t num_global_vars() const { return num_globals; }
    std::size_t num_vars() const { return num_local_vars() + num_global_vars(); }

    var_ht to_var_i(unsigned i) const { return { i + num_global_vars() }; }
    var_ht to_var_i(gmanager_t::index_t index) const 
    { 
        passert(index, index); 
        assert(index.id < num_global_vars());
        return { index.id }; 
    }
    var_ht to_var_i(gvar_ht gvar) const { assert(ir); return to_var_i(ir->gmanager.var_i(gvar)); }
    var_ht to_var_i(gmember_ht gmember) const { assert(ir); return to_var_i(ir->gmanager.var_i(gmember)); }

    unsigned to_local_i(var_ht var_i) const { assert(var_i.id >= num_global_vars()); return var_i.id - num_global_vars(); }
    bool is_param(var_ht var_i) const { return var_i >= to_var_i(0) && var_i < to_var_i(fn->def().num_params); }
    bool is_local(var_ht var_i) const { return var_i >= to_var_i(0); }
    bool is_global(var_ht var_i) const { return var_i < to_var_i(0); }

    // Block and local variable functions
    void seal_block(block_d& block_data);
    void fill_phi_args(ssa_ht phi, var_ht var_i, unsigned member);
    ssa_value_t var_lookup(cfg_ht node, var_ht var_i, unsigned member);
    rval_t var_lookup(cfg_ht node, var_ht var_i);
    ssa_value_array_t from_rval(rval_t const& rval, type_t type);
    ssa_value_array_t from_rval(expr_value_t const& value);

    cfg_ht insert_cfg(bool seal, pstring_t label_name = {});
    void cfg_exits_with_jump();
    void cfg_exits_with_branch(ssa_value_t condition);
    cfg_ht compile_goto();

    //void compile_binary_operator(rpn_stack_t&, ssa_op_t op, type_t result_type, bool carry = false);

    ///////////////////
    // link-specific //
    ///////////////////

    template<do_t D>
    locator_t handle_lt(
        type_t const& type, token_t const& token, 
        expr_value_t const* begin, expr_value_t const* end);

    template<do_t D, typename... Args>
    locator_t handle_lt(
        type_t const& type, token_t const& token, 
        Args const&...);

    locator_t make_lt(
        type_t const& type, token_t const& token, 
        expr_value_t const* begin, expr_value_t const* end);

    template<typename... Args>
    locator_t make_lt(
        type_t const& type, token_t const& token, 
        Args const&...);
};

thread_local eval_t::ir_builder_t eval_t::default_builder;

static token_t _make_token(expr_value_t const& value);
static rval_t _lt_rval(type_t const& type, locator_t loc);

rpair_t interpret_local_const(pstring_t pstring, fn_t* fn, ast_node_t const& expr,
                              type_t expected_type, local_const_t const* local_consts)
{
    eval_t i(eval_t::do_wrapper_t<eval_t::INTERPRET_CE>{}, pstring, fn, expr, expected_type, local_consts);
    return i.final_result;
}

void check_local_const(pstring_t pstring, fn_t* fn, ast_node_t const& expr,
                       local_const_t const* local_consts)
{
    eval_t i(eval_t::do_wrapper_t<eval_t::CHECK>{}, pstring, fn, expr, TYPE_VOID, local_consts);
}

rpair_t interpret_expr(pstring_t pstring, ast_node_t const& expr, type_t expected_type, eval_t* env)
{
    if(env)
    {
        env->do_expr_result<eval_t::INTERPRET_CE>(expr, expected_type);
        return env->final_result;
    }
    else
    {
        eval_t i(eval_t::do_wrapper_t<eval_t::INTERPRET>{}, pstring, nullptr, expr, expected_type);
        return i.final_result;
    }
}


byte_block_data_t interpret_byte_block(pstring_t pstring, ast_node_t const& expr, fn_t* fn, 
                                       local_const_t const* local_consts)
{
    eval_t i(eval_t::do_wrapper_t<eval_t::INTERPRET>{}, pstring, fn, expr, TYPE_VOID, local_consts);
    return std::move(i.byte_block_data);
}

rpair_t interpret_lt(romv_t romv, ast_node_t const& ast, type_t expected_type)
{
    eval_t i(eval_t::do_wrapper_t<eval_t::INTERPRET_LINK>{}, ast.token.pstring, nullptr, ast, expected_type, nullptr, romv);
    return std::move(i.final_result);
}

precheck_tracked_t build_tracked(fn_t& fn, local_const_t const* local_consts)
{
    precheck_tracked_t tracked;
    eval_t eval(eval_t::do_wrapper_t<eval_t::CHECK>{}, {}, fn, &tracked, nullptr, 0, local_consts);
    return tracked;
}

void build_ir(ir_t& ir, fn_t& fn)
{
    assert(cfg_data_pool::array_size() == 0);
    assert(ssa_data_pool::array_size() == 0);
    
    cfg_data_pool::scope_guard_t<block_d> cg(0);

    eval_t eval(ir, fn);
}

template<eval_t::do_t D>
eval_t::eval_t(do_wrapper_t<D>, pstring_t pstring, fn_t* fn_ref,
               ast_node_t const& expr, type_t expected_type, 
               local_const_t const* local_consts,
               romv_t romv)
: pstring(pstring)
, fn(fn_ref)
, start_time(clock::now())
, local_consts(local_consts)
, romv(romv)
{
    if(fn)
    {
        std::size_t const num_locals = num_local_vars();

        var_types.resize(num_locals);
        for(unsigned i = 0; i < num_locals; ++i)
            var_types[i] = ::dethunkify(fn->def().local_vars[i].decl.src_type, true, this);
    }
    do_expr_result<D>(expr, expected_type);
}

template<eval_t::do_t D>
eval_t::eval_t(do_wrapper_t<D>, pstring_t pstring, fn_t& fn_ref, 
               precheck_tracked_t* tracked, rval_t const* args, unsigned num_args,
               local_const_t const* local_consts)
: pstring(pstring)
, fn(&fn_ref)
, stmt(fn_ref.def().stmts.data())
, start_time(clock::now())
, local_consts(local_consts)
, precheck_tracked(tracked)
{
    std::size_t const num_locals = num_local_vars();

    var_types.resize(num_locals);
    for(unsigned i = 0; i < num_locals; ++i)
        var_types[i] = ::dethunkify(fn->def().local_vars[i].decl.src_type, true, this);

    static_assert(!is_compile(D));

    if(!is_check(D))
    {
        interpret_locals.resize(num_locals);

        assert(args);
        assert(num_args <= num_locals);
        for(unsigned i = 0; i < num_args; ++i)
        {
            assert(args[i].size() == num_members(var_types[i]));
            interpret_locals[i] = args[i];
        }
    }

    interpret_stmts<D>();
}

eval_t::eval_t(ir_t& ir_ref, fn_t& fn_ref)
: fn(&fn_ref)
, stmt(fn_ref.def().stmts.data())
, ir(&ir_ref)
, start_time(clock::now())
, local_consts(fn_ref.def().local_consts.data())
{
    // Reset the static thread-local state:
    builder.clear(); // TODO: make sure this isn't called in recursion
    ir->gmanager.init(fn->handle());
    num_globals = ir->gmanager.num_locators();

    std::size_t const num_locals = num_local_vars();

    var_types.resize(num_vars(), TYPE_VOID);

    // Add local vars to 'var_types':
    for(unsigned i = 0; i < num_locals; ++i)
        var_type(to_var_i(i)) = ::dethunkify(fn->def().local_vars[i].decl.src_type, true, this);

    // Add global vars to 'var_types':
    var_types.reserve(num_vars());
    ir->gmanager.for_each_gvar([&](gvar_ht gvar, auto) { var_type(to_var_i(gvar)) = gvar->type(); });

    // OK! var_types is built.

    ir->root = builder.cfg = insert_cfg(true);
    ir->root.data<block_d>().is_root = true;

    ssa_ht const entry = ir->root->emplace_ssa(SSA_entry, TYPE_VOID);
    entry->append_daisy();

    // Insert nodes for the arguments
    unsigned const nparams = fn->def().num_params;
    for(unsigned i = 0; i < nparams; ++i)
    {
        var_ht const var_i = to_var_i(i);
        type_t const type = var_type(var_i);
        unsigned nmember = ::num_members(type);

        passert(ir->root.data<block_d>().var(var_i).size() == nmember, 
                ir->root.data<block_d>().var(var_i).size(), nmember);

        for(unsigned m = 0; m < nmember; ++m)
        {
            ir->root.data<block_d>().var(var_i)[m] = ir->root->emplace_ssa(
                SSA_read_global, member_type(type, m), entry, 
                locator_t::arg(fn->handle(), i, m, 0));
        }
    }

    // Insert nodes for gmember reads
    ir->gmanager.for_each_gvar([&](gvar_ht gvar, gmanager_t::index_t i)
    {
        var_ht const var_i = to_var_i(i);
        auto& block = ir->root.data<block_d>();
        assert(block.vars.size() == var_types.size());

        for(gmember_ht m = gvar->begin(); m != gvar->end(); ++m)
        {
            assert(m->member() < block.var(var_i).size());

            block.var(var_i)[m->member()] = ir->root->emplace_ssa(
                SSA_read_global, member_type(var_type(var_i), m->member()), entry, locator_t::gmember(m, 0));
        }
    });

    // Insert nodes for gmember set reads
    ir->gmanager.for_each_gmember_set(fn->handle(),
    [&](bitset_uint_t const* gmember_set, gmanager_t::index_t i, locator_t locator)
    {
        var_ht const var_i = to_var_i(i);
        auto& block = ir->root.data<block_d>();
        assert(block.vars.size() == var_types.size());
        assert(block.var(var_i).size() == 1);

        block.var(var_i)[0] = ir->root->emplace_ssa(
            SSA_read_global, var_type(var_i), entry, locator);
    });

    // Create all of the SSA graph, minus the exit node:
    compile_block();
    cfg_exits_with_jump();
    cfg_ht const end = builder.cfg;

    // Now create the exit block.
    // All return statements create a jump, which will jump to the exit node.
    type_t const return_type = fn->type().return_type();
    if(return_type != TYPE_VOID)
    {
        ssa_value_array_t array;

        unsigned const num_m = ::num_members(return_type);
        for(unsigned m = 0; m < num_m; ++m)
            array.push_back(end->emplace_ssa(SSA_uninitialized, ::member_type(return_type, m)));

        builder.return_values.push_back(std::move(array));
    }

    cfg_ht const exit = insert_cfg(true);

    for(cfg_ht node : builder.return_jumps)
        node->build_set_output(0, exit);
    end->build_set_output(0, exit);

    // Write all globals at the exit:
    std::vector<ssa_value_t> return_inputs;
    return_inputs.reserve(ir->gmanager.num_locators() * 2);

    ir->gmanager.for_each_gvar([&](gvar_ht gvar, gmanager_t::index_t i)
    {
        var_ht const var_i = to_var_i(i);

        for(gmember_ht m = gvar->begin(); m != gvar->end(); ++m)
        {
            return_inputs.push_back(var_lookup(exit, var_i, m->member()));
            return_inputs.push_back(locator_t::gmember(m, 0));
        }
    });

    ssa_ht ret = exit->emplace_ssa(SSA_return, TYPE_VOID);

    // Append the return value, if it exists:
    if(return_type != TYPE_VOID)
    {
        unsigned const num_m = ::num_members(return_type);
        for(unsigned m = 0; m < num_m; ++m)
        {
            ssa_ht phi = exit->emplace_ssa(SSA_phi, member_type(return_type, m));
            
            unsigned const size = builder.return_values.size();
            phi->alloc_input(size);

            for(unsigned i = 0; i < size; ++i)
                phi->build_set_input(i, builder.return_values[i][m]);

            return_inputs.push_back(phi);
            return_inputs.push_back(locator_t::ret(fn->handle(), m, 0));
        }
    }

    assert(return_inputs.size() % 2 == 0);
    ret->assign_input(&*return_inputs.begin(), &*return_inputs.end());
    ret->append_daisy();

#ifndef NDEBUG
    for(cfg_ht h = ir->cfg_begin(); h; ++h)
        assert(h.data<block_d>().sealed);
#endif

    // Handle exhaustive switches
    for(cfg_ht switch_cfg : builder.exhaustive_switches)
    {
        ssa_ht const switch_ssa = switch_cfg->last_daisy();
        assert(switch_ssa->input_size() == MAX_CFG_OUTPUT + 1);
        assert(switch_cfg->output_size() == MAX_CFG_OUTPUT + 1);
        passert(switch_ssa->op() == SSA_switch_partial, switch_ssa->op());

        switch_partial_to_full(*switch_ssa);

        assert(switch_ssa->input_size() == MAX_CFG_OUTPUT + 1);
        assert(switch_cfg->output_size() == MAX_CFG_OUTPUT);
        assert(switch_ssa->in_daisy());
    }
}

// Inline version
#if 1
eval_t::eval_t(eval_t const& parent, ir_t& ir_ref, fn_t& fn_ref, ir_builder_t& builder,
               cfg_ht pre_entry, cfg_ht& exit, expr_value_t const* args, rval_t& return_rval)
: fn(&fn_ref)
, stmt(fn_ref.def().stmts.data())
, ir(&ir_ref)
, start_time(clock::now())
, builder(builder)
, local_consts(fn_ref.def().local_consts.data())
, num_globals(parent.num_globals)
{
    var_types.resize(num_vars(), TYPE_VOID);

    // Copy global var types from 'parent':
    std::copy_n(parent.var_types.begin(), num_global_vars(), var_types.begin());

    // Add local vars to 'var_types':
    std::size_t const num_locals = num_local_vars();
    for(unsigned i = 0; i < num_locals; ++i)
        var_type(to_var_i(i)) = ::dethunkify(fn->def().local_vars[i].decl.src_type, true, this);

    // OK! var_types is built.

    cfg_ht const entry = builder.cfg = insert_cfg(true);
    pre_entry->build_set_output(0, entry);

    // Map arguments to the entry block:
    unsigned const nparams = fn->def().num_params;
    for(unsigned i = 0; i < nparams; ++i)
    {
        var_ht const var_i = to_var_i(i);
        type_t const type = var_type(var_i);
        unsigned nmember = ::num_members(type);

        assert(nmember == args[i].rval().size());

        for(unsigned m = 0; m < nmember; ++m)
            entry.data<block_d>().var(var_i)[m] = from_variant<COMPILE>(args[i].rval()[m], member_type(type, m));
    }

    // Create all of the SSA graph, minus the exit node:
    compile_block();
    cfg_exits_with_jump();
    cfg_ht const end = builder.cfg;

    // Now create the exit block.
    // All return statements create a jump, which will jump to the exit node.
    type_t const return_type = fn->type().return_type();
    if(return_type != TYPE_VOID)
    {
        ssa_value_array_t array;

        unsigned const num_m = ::num_members(return_type);
        for(unsigned m = 0; m < num_m; ++m)
            array.push_back(end->emplace_ssa(SSA_uninitialized, ::member_type(return_type, m)));

        builder.return_values.push_back(std::move(array));
    }

    exit = insert_cfg(true);

    for(cfg_ht node : builder.return_jumps)
        node->build_set_output(0, exit);
    end->build_set_output(0, exit);

    // Write all globals at the exit:
    // Append the return value, if it exists:
    if(return_type != TYPE_VOID)
    {
        unsigned const num_m = ::num_members(return_type);
        for(unsigned m = 0; m < num_m; ++m)
        {
            ssa_ht phi = exit->emplace_ssa(SSA_phi, member_type(return_type, m));
            
            unsigned const size = builder.return_values.size();
            phi->alloc_input(size);

            for(unsigned i = 0; i < size; ++i)
                phi->build_set_input(i, builder.return_values[i][m]);

            return_rval.push_back(phi);
        }
    }

    // Handle exhaustive switches
    for(cfg_ht switch_cfg : builder.exhaustive_switches)
    {
        ssa_ht const switch_ssa = switch_cfg->last_daisy();
        assert(switch_ssa->input_size() == MAX_CFG_OUTPUT + 1);
        assert(switch_cfg->output_size() == MAX_CFG_OUTPUT + 1);
        passert(switch_ssa->op() == SSA_switch_partial, switch_ssa->op());

        switch_partial_to_full(*switch_ssa);

        assert(switch_ssa->input_size() == MAX_CFG_OUTPUT + 1);
        assert(switch_cfg->output_size() == MAX_CFG_OUTPUT);
        assert(switch_ssa->in_daisy());
    }
}
#endif

template<eval_t::do_t D>
void eval_t::do_expr_result(ast_node_t const& expr, type_t expected_type)
{
    expr_value_t v = to_rval<D>(do_expr<D>(expr));

    if(expected_type.name() != TYPE_VOID)
    {
        if(!can_size_unsized_array(v.type, expected_type))
            v = throwing_cast<D>(std::move(v), expected_type, true);

        if(is_interpret(D))
            final_result.value = v.rval();

        final_result.type = v.type;
    }
    else
        final_result.type = TYPE_VOID;
}

void eval_t::check_time()
{
    auto elapsed = clock::now() - start_time;
    if(compiler_options().time_limit > 0)
    {
        if(elapsed > sc::milliseconds(compiler_options().time_limit))
        {
            throw out_of_time_t(
                fmt_error(this->pstring, "Ran out of time executing expression.")
                + fmt_note("Computation is likely divergent.\n")
                + fmt_note(fmt("Use compiler flag --timelimit 0 to ignore this error.\n", compiler_options().time_limit)));
        }
    }
}

///////////////////////////////////////////////////////////////////////////////

template<eval_t::do_t D>
expr_value_t eval_t::do_var_init_expr(var_ht var_i, ast_node_t const& expr)
{
    expr_value_t v = do_expr<D>(expr);

    if(can_size_unsized_array(v.type, var_type(var_i)))
        var_type(var_i).set_array_length(v.type.array_length(), v.pstring);

    return throwing_cast<D>(std::move(v), var_type(var_i), true);
}

template<eval_t::do_t D>
void eval_t::interpret_stmts()
{
    static_assert(D != COMPILE);

    auto const do_condition = [&](bool check_value) -> bool
    { 
        expr_value_t v = throwing_cast<D>(do_expr<D>(*stmt->expr), TYPE_BOOL, true);
        if(!is_interpret(D))
            return check_value;
        return v.fixed().value;
    };

    while(true)
    {
        check_time();

        switch(stmt->name)
        {
        default: // Handles var inits
            if(is_var_init(stmt->name))
            {
                if(D == INTERPRET_CE)
                    compiler_error(stmt->pstring, "Expression cannot be evaluated at compile-time.");

                unsigned const local_i = ::get_local_i(stmt->name);
                var_ht const var_i = to_var_i(local_i);

                // Prepare the type.
                if(var_type(var_i).name() == TYPE_VOID)
                    var_type(var_i) = dethunkify(fn->def().local_vars[local_i].decl.src_type, true, this);

                if(stmt->expr)
                {
                    expr_value_t v = do_var_init_expr<D>(var_i, *stmt->expr);

                    if(is_interpret(D))
                        interpret_locals[local_i] = std::move(v.rval());
                }
                else if(is_interpret(D))
                {
                    type_t const type = var_type(var_i);
                    unsigned const num = num_members(type);
                    assert(num > 0);

                    rval_t rval;
                    rval.reserve(num);

                    for(unsigned i = 0; i < num; ++i)
                    {
                        type_t const mt = member_type(type, i);
                        if(mt.name() == TYPE_TEA)
                            rval.emplace_back(make_ct_array(mt.array_length()));
                        else
                            rval.emplace_back();
                    }

                    interpret_locals[local_i] = std::move(rval);
                }

                ++stmt;
            }
            else
                compiler_error(stmt->pstring, fmt("Statement % cannot appear in constant evaluation.", to_string(stmt->name)));
            break;

        case STMT_GOTO_MODE:
            if(!is_check(D))
                compiler_error(stmt->pstring, "Statement cannot appear in constant evaluation.");
            // fall-through
        case STMT_EXPR:
        case STMT_FOR_EFFECT:
            if(stmt->expr)
                do_expr<D>(*stmt->expr);
            ++stmt;
            break;

        case STMT_DO_WHILE:
        case STMT_DO_FOR:
        case STMT_END_IF:
        case STMT_LABEL:
        case STMT_END_SWITCH:
        case STMT_CASE:
        case STMT_DEFAULT:
            ++stmt;
            break;

        case STMT_ELSE:
        case STMT_END_WHILE:
        case STMT_END_FOR:
        case STMT_BREAK:
        case STMT_CONTINUE:
        case STMT_GOTO:
            if(is_interpret(D))
                stmt = &fn->def()[stmt->link];
            else
                ++stmt;
            break;

        case STMT_IF:
            if(do_condition(true))
                ++stmt;
            else
            {
                stmt = &fn->def()[stmt->link];
                if(stmt->name == STMT_ELSE)
                    ++stmt;
            }
            break;

        case STMT_WHILE:
        case STMT_FOR:
            if(do_condition(true))
                ++stmt;
            else
                stmt = &fn->def()[stmt->link];
            break;

        case STMT_END_DO_WHILE:
        case STMT_END_DO_FOR:
            if(do_condition(false))
                stmt = &fn->def()[stmt->link];
            else
                ++stmt;
            break;

        case STMT_SWITCH:
            {
                expr_value_t switch_expr = do_expr<D>(*stmt->expr);
                switch_expr = throwing_cast<D>(std::move(switch_expr), is_signed(switch_expr.type.name()) ? TYPE_S : TYPE_U, true);

                if(!is_interpret(D))
                    ++stmt;
                else while(true)
                {
                    assert(stmt->link);
                    stmt = &fn->def()[stmt->link];

                    if(stmt->name == STMT_CASE)
                    {
                        expr_value_t case_expr = throwing_cast<D>(do_expr<D>(*stmt->expr), switch_expr.type, true);

                        if(switch_expr.fixed() == case_expr.fixed())
                        {
                            ++stmt;
                            break;
                        }
                    }
                    else if(stmt->name == STMT_DEFAULT)
                    {
                        ++stmt;
                        break;
                    }
                    else
                        assert(false);
                }
            }

            break;

        case STMT_RETURN:
            {
                type_t const return_type = fn->type().return_type();
                if(stmt->expr)
                {
                    expr_value_t v = throwing_cast<D>(do_expr<D>(*stmt->expr), return_type, true);
                    if(is_interpret(D))
                        final_result.value = std::move(v.rval());
                    final_result.type = std::move(v.type);
                }
                else if(return_type.name() != TYPE_VOID)
                {
                    compiler_error(stmt->pstring, fmt(
                        "Expecting return expression of type %.", return_type));
                }
            }
            if(!is_check(D))
                return;
            ++stmt;
            break;

        case STMT_END_FN:
            if(!is_check(D) && !fn->iasm)
            {
                type_t return_type = fn->type().return_type();
                if(return_type.name() != TYPE_VOID)
                {
                    compiler_error(stmt->pstring, fmt(
                        "Interpreter reached end of function without returning %.", return_type));
                }
            }
            return;

        case STMT_NMI:
            if(!is_check(D))
                compiler_error(stmt->pstring, "Cannot wait for nmi at compile-time.");
            if(precheck_tracked)
                precheck_tracked->wait_nmis.push_back(stmt_pstring_mods());
            ++stmt;
            break;

        case STMT_FENCE:
            if(precheck_tracked)
                precheck_tracked->fences.push_back(stmt_pstring_mods());
            ++stmt;
            break;
        }
    }
    assert(false);
}

static ssa_value_t _interpret_shift_atom(ssa_value_t v, int shift, pstring_t pstring)
{
    if(v.type().name() == TYPE_U && shift == 0)
        return v;

    if(v.is_locator())
    {
        /* TODO
        if(is_ptr(v.type().name()) && v.locator().is() == IS_PTR)
        {
            if(shift == 0)
                return v.locator().with_is(IS_PTR).with_byteified(true);
            else if(shift == 1)
                return v.locator().with_is(IS_PTR_HI).with_byteified(true);
        }
        */

        // Create linktime:
        ast_node_t new_ast = { 
            .token = { 
                .type = TOK_shift_atom,
                .pstring = pstring,
                .value = shift,
            }, 
            .uint = v.locator().to_uint(),
        };

        return locator_t::lt_expr(alloc_lt_value(TYPE_U, std::move(new_ast)));
    }

    fixed_t result = { v.fixed() };
    if(shift < 0)
        result.value <<= -shift * 8;
    else
        result.value >>= shift * 8;
    result.value &= numeric_bitmask(TYPE_U);
    return ssa_value_t(result, TYPE_U);
}

void eval_t::compile_block()
{
    while(true)
    {
    switch(stmt->name)
    {
    default: // Handles var inits
        if(is_var_init(stmt->name))
        {
            unsigned const local_i = get_local_i(stmt->name);
            var_ht const var_i = to_var_i(local_i);
            type_t const type = var_type(var_i);

            if(is_ct(type))
                compiler_error(stmt->pstring, fmt("Variables of type % are only valid inside ct functions.", type));

            ssa_value_array_t value;

            if(stmt->expr)
                value = from_rval(do_var_init_expr<COMPILE>(var_i, *stmt->expr));
            else
            {
                unsigned const num = num_members(type);
                assert(num > 0);

                value.reserve(num);
                
                for(unsigned i = 0; i < num; ++i)
                {
                    type_t const mt = member_type(type, i);
                    value.emplace_back(builder.cfg->emplace_ssa(SSA_uninitialized, mt));
                }
            }

            builder.cfg.data<block_d>().var(var_i) = std::move(value);
            ++stmt;
        }
        else
            throw std::runtime_error("Unimplemented stmt.");
        break;

    case STMT_END_IF:
    case STMT_END_WHILE:
    case STMT_END_FN:
    case STMT_END_FOR:
        ++stmt;
        // fall-through
    case STMT_END_DO_WHILE:
    case STMT_FOR_EFFECT:
        return;

    case STMT_EXPR:
        do_expr<COMPILE>(*stmt->expr);
        ++stmt;
        break;

    case STMT_IF:
        {
            // Branch the active node.
            expr_value_t v = do_expr<COMPILE>(*stmt->expr);
            ++stmt;
            cfg_ht branch = builder.cfg;
            v = throwing_cast<COMPILE>(std::move(v), TYPE_BOOL, true);
            cfg_exits_with_branch(v.ssa());

            // Create new cfg_node for the 'true' branch.
            cfg_ht const begin_true = builder.cfg = insert_cfg(true);
            branch->build_set_output(1, begin_true);
            compile_block();
            cfg_exits_with_jump();
            cfg_ht const end_true = builder.cfg;

            if(stmt->name == STMT_ELSE)
            {
                // Create new block for the 'false' branch.
                ++stmt;
                cfg_ht const begin_false = builder.cfg = insert_cfg(true);
                branch->build_set_output(0, begin_false);
                compile_block();

                // Repurpose 'branch' to hold end of the 'false' branch.
                // Simplifies the assignment that follows.
                branch = builder.cfg;
                cfg_exits_with_jump();
            }

            // Merge the two nodes.
            builder.cfg = insert_cfg(true);
            end_true->build_set_output(0, builder.cfg);
            branch->build_set_output(0, builder.cfg);
        }
        break;

    case STMT_FOR:
    case STMT_WHILE:
        {
            bool const is_for = stmt->name == STMT_FOR;
            cfg_ht const entry = builder.cfg;

            // The loop condition will go in its own block.
            cfg_exits_with_jump();
            cfg_ht const begin_branch = builder.cfg = insert_cfg(false);
            entry->build_set_output(0, begin_branch);

            expr_value_t v = do_expr<COMPILE>(*stmt->expr);
            ++stmt;
            v = throwing_cast<COMPILE>(std::move(v), TYPE_BOOL, true);
            cfg_ht const end_branch = builder.cfg;
            cfg_exits_with_branch(v.ssa());

            builder.continue_stack.emplace_back();
            builder.break_stack.emplace_back();

            // Compile the body.
            cfg_ht const begin_body = builder.cfg = insert_cfg(true);
            end_branch->build_set_output(1, begin_body);
            compile_block();
            cfg_ht const end_body = builder.cfg;
            cfg_exits_with_jump();

            cfg_ht begin_for_expr = begin_branch; // Will be 'begin_branch' only for WHILE stmts, otherwise see below:
            if(is_for)
            {
                // Compile the 'for' expr in its own block:
                assert(stmt->name == STMT_FOR_EFFECT);
                begin_for_expr = builder.cfg = insert_cfg(true);
            }

            end_body->build_set_output(0, begin_for_expr);

            // All continue statements jump to the 'begin_for_expr'.
            for(cfg_ht node : builder.continue_stack.back())
                node->build_set_output(0, begin_for_expr);

            if(is_for)
            {
                if(stmt->expr)
                    do_expr<COMPILE>(*stmt->expr);
                ++stmt;
                assert(stmt->name == STMT_END_FOR);
                ++stmt;
                cfg_ht const end_for_expr = builder.cfg;
                cfg_exits_with_jump();
                end_for_expr->build_set_output(0, begin_branch);
            }

            seal_block(begin_branch.data<block_d>());

            // Create the exit node.
            cfg_ht const begin_exit = builder.cfg = insert_cfg(true);
            end_branch->build_set_output(0, begin_exit);
            for(cfg_ht node : builder.break_stack.back())
                node->build_set_output(0, begin_exit);

            builder.continue_stack.pop_back();
            builder.break_stack.pop_back();
        }
        break;

    case STMT_DO_WHILE:
    case STMT_DO_FOR:
        {
            bool const is_for = stmt->name == STMT_DO_FOR;
            cfg_ht const entry = builder.cfg;

            ++stmt;
            builder.continue_stack.emplace_back();
            builder.break_stack.emplace_back();

            // Compile the loop body
            cfg_exits_with_jump();
            cfg_ht const begin_body = builder.cfg = insert_cfg(false);
            entry->build_set_output(0, begin_body);
            compile_block();
            cfg_ht const end_body = builder.cfg;
            cfg_exits_with_jump();

            // The loop condition will go in its own block.
            // We'll setup this block later!
            cfg_ht const begin_branch = insert_cfg(true);

            cfg_ht begin_for_expr = begin_branch; // Will be 'begin_branch' only for WHILE stmts, otherwise see below:
            if(is_for)
            {
                // Compile the 'for' expr in its own block:
                assert(stmt->name == STMT_FOR_EFFECT);
                begin_for_expr = builder.cfg = insert_cfg(true);
                end_body->build_set_output(0, begin_for_expr);
            }

            // All continue statements jump to the 'begin_for_expr'.
            for(cfg_ht node : builder.continue_stack.back())
                node->build_set_output(0, begin_for_expr);

            if(is_for)
            {
                assert(stmt->name == STMT_FOR_EFFECT);
                if(stmt->expr)
                    do_expr<COMPILE>(*stmt->expr);
                ++stmt;
                assert(stmt->name == STMT_END_DO_FOR);
                cfg_ht const end_for_expr = builder.cfg;
                cfg_exits_with_jump();
                end_for_expr->build_set_output(0, begin_branch);
            }
            else
                end_body->build_set_output(0, begin_branch);

            assert(stmt->name == (is_for ? STMT_END_DO_FOR : STMT_END_DO_WHILE));

            // Create the loop condition now.
            builder.cfg = begin_branch;
            expr_value_t v = do_expr<COMPILE>(*stmt->expr);
            ++stmt;
            v = throwing_cast<COMPILE>(std::move(v), TYPE_BOOL, true);
            cfg_ht const end_branch = builder.cfg;
            cfg_exits_with_branch(v.ssa());

            end_branch->build_set_output(1, begin_body);
            seal_block(begin_body.data<block_d>());

            // Create the exit cfg_node.
            cfg_ht const begin_exit = builder.cfg = insert_cfg(true);
            end_branch->build_set_output(0, begin_exit);
            for(cfg_ht node : builder.break_stack.back())
                node->build_set_output(0, begin_exit);

            builder.continue_stack.pop_back();
            builder.break_stack.pop_back();
        }
        break;

    case STMT_RETURN:
        {
            type_t const return_type = fn->type().return_type();

            if(stmt->expr)
            {
                expr_value_t v = throwing_cast<COMPILE>(do_expr<COMPILE>(*stmt->expr), return_type, true);
                rval_t& rval = v.rval();

                ssa_value_array_t array;
                array.reserve(rval.size());

                for(unsigned i = 0; i < rval.size(); ++i)
                    array.push_back(from_variant<COMPILE>(rval[i], member_type(v.type, i)));

                builder.return_values.push_back(std::move(array));
            }
            else if(return_type.name() != TYPE_VOID)
            {
                compiler_error(stmt->pstring, fmt(
                    "Expecting return expression of type %.", return_type));
            }

            builder.return_jumps.push_back(builder.cfg);
            builder.cfg = compile_goto();
            ++stmt;
        }
        break;

    case STMT_BREAK:
        if(builder.break_stack.empty())
            compiler_error(stmt->pstring, "break statement outside of loop or switch.");
        builder.break_stack.back().push_back(builder.cfg);
        builder.cfg = compile_goto();
        ++stmt;
        break;

    case STMT_CONTINUE:
        if(builder.continue_stack.empty())
            compiler_error(stmt->pstring, "continue statement outside of loop.");
        builder.continue_stack.back().push_back(builder.cfg);
        builder.cfg = compile_goto();
        ++stmt;
        break;

    case STMT_SWITCH:
        {
            cfg_ht const dead_branch = compile_goto();
            cfg_ht const switch_cfg = insert_cfg(true);
            switch_cfg->alloc_output(1);
            builder.cfg->build_set_output(0, switch_cfg);

            builder.cfg = switch_cfg;
            expr_value_t v = do_expr<COMPILE>(*stmt->expr);
            ++stmt;
            v = throwing_cast<COMPILE>(std::move(v), is_signed(v.type.name()) ? TYPE_S : TYPE_U, true);
            passert(v.type == TYPE_U || v.type == TYPE_S, v.type);

            ssa_ht const switch_ssa = switch_cfg->emplace_ssa(SSA_switch_partial, TYPE_VOID, v.ssa());
            switch_ssa->append_daisy();

            assert(switch_cfg->last_daisy() == switch_ssa);

            builder.break_stack.emplace_back();
            builder.switch_stack.push_back({ switch_cfg });
            builder.cfg = dead_branch;
        }
        break;

    case STMT_END_SWITCH:
        {
            cfg_ht const exit = insert_cfg(true);
            cfg_exits_with_jump();
            builder.cfg->build_set_output(0, exit);
            builder.cfg = exit;

            for(cfg_ht node : builder.break_stack.back())
                node->build_set_output(0, exit);

            cfg_ht const switch_cfg = builder.switch_stack.back().cfg;
            ssa_ht const switch_ssa = switch_cfg->last_daisy();
            passert(switch_ssa->op() == SSA_switch_partial, switch_ssa->op());
            assert(switch_cfg->last_daisy() == switch_ssa);

            assert(switch_cfg->output_size() == switch_ssa->input_size());

            if(switch_cfg->output_size() > MAX_CFG_OUTPUT)
            {
                assert(switch_cfg->output_size() == MAX_CFG_OUTPUT + 1);
                builder.exhaustive_switches.push_back(switch_cfg);
            }

            builder.break_stack.pop_back();
            builder.switch_stack.pop_back();

            ++stmt;
        }
        break;

    case STMT_CASE:
        {
            cfg_ht const entry = builder.cfg;
            cfg_exits_with_jump();

            cfg_ht const case_cfg = builder.cfg = insert_cfg(true);
            entry->build_set_output(0, case_cfg);

            assert(builder.switch_stack.size() > 0);

            assert(builder.switch_stack.size() > 0);
            cfg_ht const switch_cfg = builder.switch_stack.back().cfg;
            ssa_ht const switch_ssa = switch_cfg->last_daisy();
            passert(switch_ssa->op() == SSA_switch_partial, switch_ssa->op());

            expr_value_t case_value = do_expr<INTERPRET_CE>(*stmt->expr);
            ++stmt;
            case_value = throwing_cast<INTERPRET_CE>(std::move(case_value), switch_ssa->input(0).type(), true);

            if(!case_value.ssa().is_num())
                compiler_error(case_value.pstring, "case values must be known at compile-time.");

            std::uint8_t const case_u8 = case_value.ssa().whole();

            auto& case_set = builder.switch_stack.back().case_set;
            if(case_set.test(case_u8))
                compiler_error(case_value.pstring, "Duplicate case value.");
            case_set.set(case_u8);

            switch_cfg->build_append_output(case_cfg);
            assert(switch_cfg->output_size() >= 1);
            switch_ssa->link_append_input(case_value.ssa());
        }
        break;

    case STMT_DEFAULT:
        {
            cfg_ht const entry = builder.cfg;
            cfg_exits_with_jump();

            assert(builder.switch_stack.size() > 0);
            cfg_ht const switch_cfg = builder.switch_stack.back().cfg;
            ssa_ht const switch_ssa = switch_cfg->last_daisy();
            passert(switch_ssa->op() == SSA_switch_partial, switch_ssa->op());

            auto const& case_set = builder.switch_stack.back().case_set;
            assert(switch_cfg->output_size() == case_set.popcount() + 1);

            cfg_ht const case_cfg = builder.cfg = insert_cfg(true);
            entry->build_set_output(0, case_cfg);

            assert(switch_cfg->output_size() >= 1);
            assert(!switch_cfg->output(0));
            switch_cfg->build_set_output(0, case_cfg);

            ++stmt;
        }
        break;

    case STMT_LABEL:
        {
            label_t& label = builder.label_map[stmt];
            unsigned const use_count = stmt->use_count;
            pstring_t const label_name = stmt->pstring;
            ++stmt;

            // If there's no goto to this label, just ignore it.
            if(use_count == 0)
                break;

            cfg_exits_with_jump();
            label.inputs.push_back(builder.cfg);

            if(use_count + 1 == label.inputs.size())
            {
                // All the gotos to this label have been compiled,
                // that means this block can be sealed immediately!
                label.node = builder.cfg = insert_cfg(true, label_name);
                for(cfg_ht node : label.inputs)
                    node->build_set_output(0, label.node);
            }
            else // Otherwise, seal the node at a later time.
                label.node = builder.cfg = insert_cfg(false, label_name);
        }
        break;

    case STMT_GOTO:
        {
            label_t& label = builder.label_map[&fn->def()[stmt->link]];

            stmt_t const& label_stmt = fn->def()[stmt->link];
            assert(label_stmt.name == STMT_LABEL);
            unsigned const label_use_count = label_stmt.use_count;
            assert(label_use_count > 0);

            ++stmt;

            // Add the jump to the label.
            label.inputs.push_back(builder.cfg);
            builder.cfg = compile_goto();

            // If this is the last goto, finish and seal the node.
            if(label_use_count + 1 == label.inputs.size())
            {
                assert(label.node);
                for(cfg_ht node : label.inputs)
                    node->build_set_output(0, label.node);
                // Seal the block.
                seal_block(label.node.data<block_d>());
            }
            break;
        }

    case STMT_GOTO_MODE:
        {
            assert(stmt->expr);

            cfg_ht const branch = builder.cfg;
            cfg_ht const dead = compile_goto();
            cfg_ht const mode = builder.cfg = insert_cfg(true);
            branch->build_set_output(0, mode);
            do_expr<COMPILE>(*stmt->expr);
            ++stmt;
            builder.cfg = dead;
        }
        break;

    case STMT_NMI:
        if(fn->fclass == FN_NMI)
            compiler_error(stmt->pstring, "Cannot wait for nmi inside nmi.");
        if(precheck_tracked)
            precheck_tracked->wait_nmis.push_back(stmt_pstring_mods());
        goto do_fence;
    case STMT_FENCE:
        if(precheck_tracked)
            precheck_tracked->fences.push_back(stmt_pstring_mods());
        // fall-through
    do_fence:
        {
            assert(fn->precheck_fences());
            bc::small_vector<ssa_value_t, 32> inputs;

            block_d& block_data = builder.cfg.data<block_d>();
            ssa_ht const fenced = builder.cfg->emplace_ssa(
                stmt->name == STMT_NMI ? SSA_wait_nmi : SSA_fence, TYPE_VOID);
            fenced->append_daisy();

            ir->gmanager.for_each_gvar([&](gvar_ht gvar, gmanager_t::index_t index)
            {
                for(gmember_ht m : gvar->handles())
                {
                    //if(fn->fence_rw().test(m.id))
                    {
                        inputs.push_back(var_lookup(builder.cfg, to_var_i(index), m->member()));
                        inputs.push_back(locator_t::gmember(m, 0));

                        // Create writes after reads:
                        ssa_ht const read = builder.cfg->emplace_ssa(
                            SSA_read_global, m->type(), fenced, locator_t::gmember(m, 0));
                        block_data.var(to_var_i(index))[m->member()] = read;
                    }
                }
            });

            xbitset_t<gmember_ht> rw(0);

            ir->gmanager.for_each_gmember_set(fn->handle(),
            [&](bitset_uint_t const* gmember_set, gmanager_t::index_t index,locator_t locator)
            {
                rw = fn->fence_rw();

                bitset_and(rw.size(), rw.data(), gmember_set);

                if(!rw.all_clear())
                {
                    inputs.push_back(var_lookup(builder.cfg, to_var_i(index), 0));
                    inputs.push_back(locator);

                    // Create writes after reads:
                    ssa_ht const read = builder.cfg->emplace_ssa(
                        SSA_read_global, TYPE_VOID, fenced, locator);
                    block_data.var(to_var_i(index))[0] = read;
                }
            });

            fenced->link_append_input(&*inputs.begin(), &*inputs.end());
        }
        ++stmt;
        break;
    }
    }
    assert(false);
}

template<eval_t::do_t D>
expr_value_t eval_t::do_expr(ast_node_t const& ast)
{
    using U = fixed_uint_t;
    using S = fixed_sint_t;

    auto const make_ptr = [&](locator_t loc, type_t type, bool banked) -> expr_value_t
    {
        rval_t rval = { loc.with_is(IS_PTR) };
        if(banked)
            rval.push_back(loc.with_is(IS_BANK));

        return
        {
            .val = std::move(rval),
            .type = type,
            .pstring = ast.token.pstring,
        };
    };

    auto const infix = [&](auto const& fn, bool flipped = false, bool lhs_lval = false) -> expr_value_t
    {
        auto* ast_lhs = &ast.children[0];
        auto* ast_rhs = &ast.children[1];

        if(flipped)
            std::swap(ast_lhs, ast_rhs);

        expr_value_t lhs = do_expr<D>(*ast_lhs);
        if(!lhs_lval)
            lhs = to_rval<D>(std::move(lhs));
        expr_value_t rhs = to_rval<D>(do_expr<D>(*ast_rhs));
        return (this->*fn)(std::move(lhs), std::move(rhs), ast.token);
    };

    // Declare cross-label vars before switch.
    ssa_value_t common_value;
    type_t common_type;

    switch(ast.token.type)
    {
    default:
        throw std::runtime_error(fmt("Invalid token '%' in expression.", token_string(ast.token.type)));

    case TOK_rpair:
        {
            rpair_t const* rpair = ast.token.ptr<rpair_t>();

            rval_t rval = rpair->value;

            for(unsigned i = 0; i < rval.size(); ++i)
            {
                ssa_value_t* ssa = std::get_if<ssa_value_t>(&rval[i]);

                if(!ssa || !ssa->is_locator())
                    continue;

                locator_t const loc = ssa->locator();

                if(loc.lclass() == LOC_LT_EXPR)
                {
                    loc.link(romv);
                    assert(loc.member() < loc.lt()->results[romv].rval.size());
                    rval[i] = loc.lt()->results[romv].rval[loc.member()];
                }
                else
                {
                    std::uint16_t const data = linked_to_rom(loc.link(romv), true);
                    *ssa = ssa_value_t(data, ::member_type(rpair->type, i).name());
                }
            }

            return 
            {
                .val = std::move(rval),
                .type = rpair->type,
                .pstring = ast.token.pstring,
            };
        }

    case TOK_ident:
        if(ast.token.signed_() < 0) // If we have a local const
        {
            assert(local_consts);
            unsigned const const_i = ~ast.token.value;

            expr_value_t result =
            {
                .type = local_consts[const_i].type(),
                .pstring = ast.token.pstring,
            };

            if(!is_check(D))
                result.val = local_consts[const_i].value;

            return result;
        }
        else
        {
            if(is_link(D)) // TODO: perhaps this should move to 'to_rval'.
                compiler_error(ast.token.pstring, "Expression cannot be evaluated at link-time.");

            unsigned const local_i = ast.token.value;
            var_ht var_i = to_var_i(local_i);

            // TODO
            //assert(var_i < var_types.size());
            //assert(var_i < num_local_vars());

            expr_value_t result =
            {
                .val = lval_t{ .vvar_i = var_i },
                .type = var_type(var_i),
                .pstring = ast.token.pstring,
            };

            if(is_compile(D))
                result.time = RT;
            else if(D == INTERPRET && interpret_locals[local_i].empty())
            {
                compiler_error(ast.token.pstring, 
                    "Variable is invalid because goto jumped past its initialization.");
            }

            assert(result.is_lval());
            return result;
        }

    case TOK_global_ident:
        {
            if(is_link(D))
                compiler_error(ast.token.pstring, "Expression cannot be evaluated at link-time.");

            global_t const* global = ast.token.ptr<global_t>();
            assert(global);

            switch(global->gclass())
            {
            default: 
                throw std::runtime_error(fmt("Unimplemented global in expression. (%)", global));

            case GLOBAL_VAR:
                {
                    expr_value_t result =
                    {
                        .val = lval_t{ .flags = LVALF_IS_GLOBAL, .vglobal = global },
                        .type = global->impl<gvar_t>().type(),
                        .pstring = ast.token.pstring,
                    };

                    if(is_compile(D))
                        result.time = RT;

                    return result;
                }

            case GLOBAL_CONST:
                {
                    const_t const& c = global->impl<const_t>();
                    assert(!is_thunk(c.type().name()));

                    expr_value_t result =
                    {
                        .val = lval_t{ .flags = LVALF_IS_GLOBAL, .vglobal = global },
                        .type = c.type(),
                        .pstring = ast.token.pstring,
                    };

                    return result;
                }

            case GLOBAL_FN:
                return 
                {
                    .val = lval_t{ .flags = LVALF_IS_GLOBAL, .vglobal = global },
                    .type = global->impl<fn_t>().type(), 
                    .pstring = ast.token.pstring 
                };

            case GLOBAL_CHARMAP:
                return
                {
                    .val = lval_t{ .flags = LVALF_IS_GLOBAL, .vglobal = global },
                    .type = TYPE_CHARMAP, 
                    .pstring = ast.token.pstring 
                };
            }
        }
        break;

    case TOK_return:
        {
            if(is_link(D)) // TODO: perhaps this should move to 'to_rval'.
                compiler_error(ast.token.pstring, "Expression cannot be evaluated at link-time.");

            expr_value_t result =
            {
                .val = lval_t{ .flags = LVALF_IS_GLOBAL, .arg = lval_t::RETURN_ARG, .vglobal = &fn->global },
                .type = fn->type().return_type(),
                .pstring = ast.token.pstring,
            };

            if(is_compile(D))
                result.time = RT;

            assert(result.is_lval());
            return result;
        }
        break;

    case TOK_at:
        {
            expr_value_t value = do_expr<D>(ast.children[0]);

            strval_t const* strval;

            if(lval_t const* lval = value.is_lval())
            {
                if(!(lval->flags & LVALF_IS_GLOBAL) || !is_paa(value.type.name()))
                    goto at_error;

                switch(lval->global().gclass())
                {
                case GLOBAL_CONST:
                    {
                        const_t const& c = lval->global().impl<const_t>();
                        bool const banked = c.group_data->banked_ptrs();

                        if(!c.is_paa)
                            goto at_error;

                        return make_ptr(locator_t::gconst(c.handle()), type_t::ptr(c.group(), false, banked), banked);
                    }

                case GLOBAL_VAR:
                    {
                        gvar_t const& v = lval->global().impl<gvar_t>();

                        if(!v.is_paa)
                            goto at_error;

                        return make_ptr(locator_t::gmember(v.begin()), type_t::ptr(v.group(), true, false), false);
                    }

                default: 
                    goto at_error;
                }
            }
            else if((strval = value.is_strval()))
            {
                rom_array_ht const rom_array = sl_manager.get_rom_array(&strval->charmap->global, strval->index, strval->compressed);
                assert(rom_array);
                assert(strval->charmap->group_data());

                group_data_ht const group = strval->charmap->group_data();
                bool const banked = group->banked_ptrs();

                return make_ptr(locator_t::rom_array(rom_array), type_t::ptr(group->group.handle(), false, banked), banked);
            }
            else
            {
            at_error:
                compiler_error(ast.token.pstring, 
                    "Cannot get pointer. String literal or pointer-addressable array lvalue required as unary '@' operand.");
            }

        }
        break;


    case TOK_unary_ref:
        {
            expr_value_t value = do_expr<D>(ast.children[0]);
            if(lval_t const* lval = value.is_lval())
            {
                type_t base_type = value.type;
                if(is_paa(base_type.name()))
                    base_type = TYPE_U;
                else if(is_tea(base_type.name()))
                {
                    base_type = base_type.elem_type();
                    if(total_bytes(base_type.name()) > 1 && lval->atom < 0)
                        goto err_mb;
                }
                else if(total_bytes(base_type.name()) > 1 && lval->atom < 0)
                {
                err_mb:
                    char const* s = is_tea(base_type.name()) ? "array" : "type";
                    throw compiler_error_t(
                        fmt_error(value.pstring, fmt("Cannot get address of multi-byte % using unary '&'.", s))
                        + fmt_note(fmt("Type is %.", base_type))
                        + fmt_note(fmt("Use the '.' operator to get a single byte %.", s)));
                }

                if(!is_arithmetic(base_type.name()))
                {
                    throw compiler_error_t(
                        fmt_error(value.pstring, "Cannot get address of non-arithmetic value using unary '&'.")
                        + fmt_note(fmt("Type is %.", base_type)));
                }

                std::uint16_t offset = 0;
                if(lval->index)
                {
                    if(lval->index.is_num())
                        offset = lval->index.whole();
                    else
                    {
                        // TODO
                        assert(false);
                    }
                }

                if(lval->arg == lval_t::READY_ARG)
                {
                    locator_t const loc = locator_t::runtime_ram(RTRAM_nmi_ready, offset);
                    return make_ptr(loc, type_t::addr(false), false);
                }

                if(lval->is_global())
                {
                    switch(lval->global().gclass())
                    {
                    case GLOBAL_CONST:
                        {
                            const_t const& c = lval->global().impl<const_t>();

                            if(!c.group_data)
                                compiler_error(value.pstring, "Cannot get address of a constant that doesn't belong to a group.");

                            bool const banked = c.group_data->banked_ptrs();

                            // TODO: not every const has an address. This is buggy.
                            return make_ptr(locator_t::gconst(c.handle(), lval->member, lval->uatom(), offset), 
                                            type_t::addr(banked), banked);
                        }

                    case GLOBAL_VAR:
                        {
                            gvar_t const& gvar = lval->global().impl<gvar_t>();
                            return make_ptr(locator_t::gmember(gvar.begin() + lval->member, lval->uatom(), offset), 
                                            type_t::addr(false), false);
                        }

                    case GLOBAL_FN:
                        {
                            fn_ht const fn = lval->global().handle<fn_ht>();
                            if(lval->arg < 0)
                                return make_ptr(locator_t::fn(fn, lval->ulabel(), offset), type_t::addr(true), true);
                            else
                            {
                                locator_t loc;
                                if(lval->arg == lval_t::RETURN_ARG)
                                {
                                    if(is_check(D))
                                        fn->mark_referenced_return();
                                    else
                                        assert(fn->referenced_return());
                                    loc = locator_t::ret(fn, lval->member, lval->uatom(), offset); 
                                }
                                else
                                {
                                    // Referencing a parameter.
                                    if(is_check(D))
                                        fn->mark_referenced_param(lval->arg);
                                    else
                                        assert(fn->referenced_param(lval->arg));
                                    loc = locator_t::arg(fn, lval->arg, lval->member, lval->uatom(), offset); 
                                }
                                return make_ptr(loc, type_t::addr(false), false);
                            }
                        }

                    default: 
                        throw std::runtime_error("Unimplemented global in expression.");
                    }
                }
                else
                {
                    if(!fn || fn->fclass == FN_CT)
                    {
                    cannot_get_address:
                        compiler_error(value.pstring, "Cannot get address.");
                    }

                    locator_t loc;
                    var_ht const var_i = lval->var_i();

                    if(is_param(var_i))
                    {
                        // Referencing a parameter.
                        unsigned const local_i = to_local_i(var_i);
                        if(is_check(D))
                            fn->mark_referenced_param(local_i);
                        else
                            assert(fn->referenced_param(local_i));
                        loc = locator_t::arg(fn->handle(), local_i, lval->member, lval->uatom());
                    }
                    else if(fn->iasm)
                        loc = locator_t::asm_local_var(fn->handle(), to_local_i(var_i), lval->member, lval->uatom());
                    else
                        goto cannot_get_address;

                    return make_ptr(loc, type_t::addr(false), false);
                }
            }
            else if(strval_t const* strval = value.is_strval())
            {
                rom_array_ht const rom_array = sl_manager.get_rom_array(&strval->charmap->global, strval->index, strval->compressed);
                assert(rom_array);
                assert(strval->charmap->group_data());

                group_data_ht const group = strval->charmap->group_data();
                bool const banked = group->banked_ptrs();

                return make_ptr(locator_t::rom_array(rom_array), type_t::addr(banked), banked);
            }
            else
                compiler_error(ast.token.pstring, "lvalue or string literal required as unary '&' operand.");
        }
        break;

    case TOK_ready:
        {
            if(is_interpret(D)) // TODO: perhaps this should move to 'to_rval'.
                compiler_error(ast.token.pstring, "Expression cannot be evaluated at link-time.");

            expr_value_t result =
            {
                .val = lval_t{ /*.flags = LVALF_IS_GLOBAL,*/ .arg = lval_t::READY_ARG },
                .type = TYPE_BOOL,
                .pstring = ast.token.pstring,
            };

            if(is_compile(D))
                result.time = RT;

            assert(result.is_lval());
            return result;
        }

    case TOK_true:
    case TOK_false:
        {
            expr_value_t result = { .type = TYPE_BOOL, .pstring = ast.token.pstring };
            if(!is_check(D))
                result.val = rval_t{ ssa_value_t(unsigned(ast.token.type == TOK_true), TYPE_BOOL) };
            return result;
        }

    case TOK_int:
        common_value.set(mask_numeric(fixed_t{ ast.token.value }, TYPE_INT), TYPE_INT);
    push_int:
        {
            expr_value_t result = { .type = TYPE_INT, .pstring = ast.token.pstring };
            if(!is_check(D))
                result.val = rval_t{ common_value };
            return result;
        }

    case TOK_real:
        common_value.set(mask_numeric(fixed_t{ ast.token.value }, TYPE_REAL), TYPE_REAL);
        {
            expr_value_t result = { .type = TYPE_REAL, .pstring = ast.token.pstring };
            if(!is_check(D))
                result.val = rval_t{ common_value };
            return result;
        }
        break;

    case TOK_period:
        {
            // Periods represent struct member access.
            
            using namespace std::literals;

            expr_value_t lhs = do_expr<D>(ast.children[0]);
            std::uint64_t const hash = ast.token.value;

            type_t elem_type = lhs.type;
            unsigned tea_length = 0;
            bool const is_tea = ::is_tea(elem_type.name());
            if(is_tea)
            {
                tea_length = elem_type.size();
                elem_type = elem_type.elem_type();
            }

            // Used later on:
            int member = -1;
            int atom = -1;
            int shift = -1;
            type_t result_type = TYPE_U;

            if(lval_t const* lval = lhs.is_lval())
            {
                if(lval->is_global()) switch(lval->global().gclass())
                {
                case GLOBAL_CHARMAP:
                    {
                        charmap_t const& charmap = lval->global().impl<charmap_t>();
                        switch(hash)
                        {
                        case fnv1a<std::uint64_t>::hash("size"sv): 
                            lhs.type = TYPE_INT;
                            lhs.val = rval_t{ ssa_value_t(charmap.size(), TYPE_INT) };
                            break;
                        case fnv1a<std::uint64_t>::hash("sentinel"sv): 
                            if(charmap.sentinel() < 0)
                                goto bad_global_accessor;
                            lhs.type = TYPE_U;
                            lhs.val = rval_t{ ssa_value_t(charmap.sentinel(), TYPE_U) };
                            break;
                        default:
                            goto bad_global_accessor;
                        }

                    }
                    goto finish_period;

                case GLOBAL_FN:
                    {
                        if(lhs.lval().arg == lval_t::RETURN_ARG)
                            break;

                        if(!lhs.is_lval())
                            compiler_error(ast.token.pstring, "Expecting lvalue.");

                        fn_t const& fn = lhs.lval().global().impl<fn_t>();

                        using namespace std::literals;
                        if(hash == fnv1a<std::uint64_t>::hash("return"sv))
                        {
                            lhs.lval().arg = lval_t::RETURN_ARG;
                            lhs.type = fn.type().return_type();
                        }
                        else
                        {
                            auto const& hashes = fn.def().name_hashes;
                            auto it = std::find(hashes.begin(), hashes.end(), hash);

                            if(it == hashes.end())
                                goto bad_global_accessor;

                            unsigned i = it - hashes.begin();

                            if(i < fn.def().num_params) // If it's a param
                            {
                                lhs.lval().arg = i;
                                lhs.type = fn.def().local_vars[i].decl.src_type.type;
                            }
                            else // It's a label
                            {
                                if(lhs.lval().label >= 0)
                                    goto bad_global_accessor;

                                assert(fn.iasm);

                                // Determine the corresponding 'local_consts' index to this label:

                                i -= fn.def().num_params;
                                for(unsigned j = 0; j < fn.def().local_consts.size(); ++j)
                                {
                                    if(fn.def().local_consts[j].is_label() && i-- == 0)
                                    {
                                        lhs.lval().label = j; // OK! Found the label.
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    goto finish_period;

                default:
                    break;

                bad_global_accessor:
                    file_contents_t file(ast.token.pstring.file_i);
                    compiler_error(ast.token.pstring, fmt(
                        "% isn't a member of %.", 
                        ast.token.pstring.view(file.source()), lval->global().name));
                }
            }

            if(elem_type.name() == TYPE_STRUCT)
            {
                struct_t const& s = elem_type.struct_();
                auto const ptr = s.fields().lookup(hash);

                if(!ptr)
                    goto bad_accessor;

                unsigned const field_i = ptr - s.fields().begin();
                unsigned const member_i = member_index(lhs.type, field_i);

                if(lhs.is_lval())
                    lhs.lval().member += member_i;
                else if(lhs.is_rval())
                {
                    if(!is_check(D))
                    {
                        rval_t& rval = lhs.rval();

                        // Shrink the rval to only contain the specified field.
                        unsigned const size = num_members(ptr->second.type());

                        assert(rval.size() == num_members(lhs.type));
                        assert(size + member_i <= rval.size());

                        if(member_i != 0)
                            for(unsigned i = 0; i < size; ++i)
                                rval[i] = std::move(rval[i + member_i]);
                        rval.resize(size);
                    }
                }
                else
                    goto bad_accessor;

                if(is_tea)
                    lhs.type = type_t::tea(ptr->second.type(), tea_length);
                else
                    lhs.type = ptr->second.type();
            }
            else if(lhs.type.name() == TYPE_FN)
            {
                if(!lhs.is_lval())
                    compiler_error(ast.token.pstring, "Expecting lvalue.");

                fn_t const& fn = *to_rval<D>(lhs).ssa().locator().fn();

                using namespace std::literals;
                if(hash == fnv1a<std::uint64_t>::hash("return"sv))
                {
                    lhs.lval().arg = lval_t::RETURN_ARG;
                    lhs.type = fn.type().return_type();
                }
                else
                {
                    auto const& hashes = fn.def().name_hashes;
                    auto it = std::find(hashes.begin(), hashes.end(), hash);

                    if(it == hashes.end())
                        goto bad_accessor;

                    unsigned i = it - hashes.begin();

                    if(i < fn.def().num_params) // If it's a param
                    {
                        lhs.lval().arg = i;
                        lhs.type = fn.def().local_vars[i].decl.src_type.type;
                    }
                    else // It's a label
                    {
                        if(lhs.lval().label >= 0)
                            goto bad_accessor;

                        assert(fn.iasm);

                        // Determine the corresponding 'local_consts' index to this label:

                        i -= fn.def().num_params;
                        for(unsigned j = 0; j < fn.def().local_consts.size(); ++j)
                        {
                            if(fn.def().local_consts[j].is_label() && i-- == 0)
                            {
                                lhs.lval().label = j; // OK! Found the label.
                                break;
                            }
                        }
                    }
                }
            }
            else if(is_banked_ptr(elem_type.name()))
            {
                using namespace std::literals;

                switch(hash)
                {
                case fnv1a<std::uint64_t>::hash('c'):
                case fnv1a<std::uint64_t>::hash("bank"sv):
                    member = 1;
                    atom = 0;
                    break;

                case fnv1a<std::uint64_t>::hash("ptr"sv):
                    member = 0;
                    atom = -1;
                    result_type = elem_type.with_banked(false);
                    goto have_member;

                case fnv1a<std::uint64_t>::hash('b'):
                    member = 0;
                    atom = 1;
                    break;

                case fnv1a<std::uint64_t>::hash('a'):
                    member = 0;
                    atom = 0;
                    break;

                default:
                    goto bad_accessor;
                }

                shift = atom;
                goto have_member_atom;
            }
            else if(is_scalar(elem_type.name()))
            {
                switch(hash)
                {
                case fnv1a<std::uint64_t>::hash('c'): shift =  2; break;
                case fnv1a<std::uint64_t>::hash('b'): shift =  1; break;
                case fnv1a<std::uint64_t>::hash('a'): shift =  0; break;
                case fnv1a<std::uint64_t>::hash('z'): shift = -1; break;
                case fnv1a<std::uint64_t>::hash('y'): shift = -2; break;
                case fnv1a<std::uint64_t>::hash('x'): shift = -3; break;
                default:
                bad_accessor:
                    file_contents_t file(ast.token.pstring.file_i);
                    compiler_error(ast.token.pstring, fmt(
                        "% isn't a member of %.", 
                        ast.token.pstring.view(file.source()), lhs.type), &file);
                }

                member = 0;
                atom = shift + frac_bytes(lhs.type.name());

            have_member_atom:

                if(member < 0 || atom < 0 || atom >= int(total_bytes(elem_type.name())))
                    goto bad_accessor;

                if(is_tea)
                    result_type = type_t::tea(result_type, tea_length);

                assert(shift >= 0);

            have_member:

                if(lhs.is_strval())
                    lhs = to_rval<D>(std::move(lhs));

                if(lhs.is_lval())
                {
                    assert(lhs.lval().atom < 0 || atom == 0);
                    lhs.lval().atom = atom;
                    lhs.lval().member += member;
                }
                else if(is_interpret(D) && lhs.is_rval())
                {
                    if(atom < 0)
                        lhs.rval() = { lhs.rval()[member] };
                    else
                    {
                        if(tea_length > 0)
                        {
                            ct_array_t const& from = std::get<ct_array_t>(lhs.rval()[member]);
                            ct_array_t to = make_ct_array(tea_length);

                            for(unsigned i = 0; i < tea_length; ++i)
                                to[i] = _interpret_shift_atom(from[i], shift, lhs.pstring);

                            lhs.rval() = { std::move(to) };
                        }
                        else
                            lhs.rval() = { _interpret_shift_atom(lhs.ssa(member), shift, lhs.pstring) };
                    }
                }
                else if(is_compile(D) && lhs.is_rval())
                {
                    if(atom < 0)
                        lhs.rval() = { lhs.rval()[member] };
                    else
                    {
                        ssa_value_t const array = from_variant<D>(lhs.rval()[member], result_type);
                        ssa_ht const h = builder.cfg->emplace_ssa(
                            tea_length ? SSA_array_get_byte : SSA_get_byte, 
                            result_type, 
                            array, ssa_value_t(atom, TYPE_U));
                        lhs.rval() = { h };
                    }
                }

                lhs.type = result_type;
            }
            else
                goto bad_accessor;

        finish_period:
            lhs.pstring = concat(lhs.pstring, ast.token.pstring);
            return lhs;
        }

    case TOK_apply:
        {
            // TOK_apply is a psuedo token used to represent application. 
            // The token's 'value' stores the application's arity:
            std::size_t const num_children = ast.token.value;
            std::size_t const num_args = num_children - 1;

            bc::small_vector<expr_value_t, 8> exprs(num_children);
            for(unsigned i = 0; i < num_children; ++i)
                exprs[i] = to_rval<D>(do_expr<D>(ast.children[i]));

            //if(handle_lt<D>(rpn_stack, num_args+1, *token)) // TODO
                //break;

            // The first expression is the function:
            expr_value_t& fn_expr = exprs[0];
            expr_value_t* args = exprs.data() + 1;

            if(fn_expr.type.name() != TYPE_FN)
            {
                compiler_error(ast.children[0].token.pstring, fmt(
                    "Expecting function type. Got %.", fn_expr.type));
            }

            assert(fn_expr.is_rval());
            fn_ht const call = fn_expr.ssa().locator().fn();
            pstring_t const call_pstring = concat(fn_expr.pstring, ast.token.pstring);

            if(call->fclass == FN_NMI)
                compiler_error(call_pstring, "Cannot call nmi function.");

            // TODO: make sure modes can't be called normally.
            if(call->fclass == FN_MODE && is_interpret(D))
                compiler_error(call_pstring, "Cannot goto mode at compile-time.");

            std::size_t const num_params = fn_expr.type.num_params();
            type_t const* const params = fn_expr.type.types();

            if(num_args != num_params)
            {
                compiler_error(
                    fn_expr.pstring, fmt(
                    "Passed % arguments to a function of type %. "
                    "Expecting % arguments.",
                    num_args, fn_expr.type, num_params));
            }

            // Now for the arguments.
            // Cast all arguments to match the fn signature.
            int const cast_result = cast_args<D>(fn_expr.pstring, args, args+num_args, params, true);

            if(cast_result >= 0)
            {
                compiler_error(
                    args[cast_result].pstring, fmt(
                    "Unable to convert type % "
                    "to type % in function application.\n"
                    "Expected signature: % ",
                    args[cast_result].type, params[cast_result], fn_expr.type));
            }

            if(precheck_tracked)
            {
                if(call->fclass == FN_MODE) // Track that we're going to a mode here:
                    precheck_tracked->goto_modes.push_back(std::make_pair(call, stmt_pstring_mods()));
                else if(call->fclass != FN_CT)
                    precheck_tracked->calls.emplace(call, call_pstring);
            }

            // Now do the call!

            expr_value_t result =
            {
                .type = fn_expr.type.return_type(), 
                .pstring = call_pstring,
            };

            if(is_interpret(D))
            {
            interpret_fn:
                bc::small_vector<rval_t, 8> rval_args(num_args);
                for(unsigned i = 0; i < num_args; ++i)
                {
                    if(!args[i].is_ct())
                        compiler_error(args[i].pstring, "Expecting compile-time constant value.");
                    rval_args[i] = args[i].rval();
                }

                try
                {
                    // NOTE: call as INTERPRET, not D.
                    eval_t sub(do_wrapper_t<INTERPRET>{}, call_pstring, *call, nullptr, rval_args.data(), rval_args.size(),
                               call->def().local_consts.data());
                    result.val = std::move(sub.final_result.value);
                }
                catch(out_of_time_t& e)
                {
                    e.msg += fmt_note(this->pstring, "Backtrace:");
                    throw;
                }
            }
            else if(is_compile(D))
            {
                if(call->fclass == FN_CT)
                    goto interpret_fn;

                // Interpret when possible:
                if(call->ct_pure())
                {
                    for(unsigned i = 0; i < num_args; ++i)
                        if(!args[i].is_ct())
                            goto compile_fn;

                    goto interpret_fn;
                }

            compile_fn:

                if(call->fclass == FN_FN && call->always_inline())
                {
                    cfg_exits_with_jump();
                    cfg_ht const pre_entry = builder.cfg;
                    cfg_ht const post_exit = insert_cfg(true);

                    cfg_ht exit = {};

                    ir_builder_t call_builder;
                    rval_t return_rval;

                    std::printf("START INLINE %i\n", int(call->def().local_vars.size()));

                    eval_t inline_eval(
                        *this, *ir, *call, call_builder, 
                        pre_entry, exit, args, return_rval);
                    assert(exit);

                    std::puts("END INLINE");
                    
                    builder.cfg = post_exit;
                    exit->alloc_output(1);
                    exit->build_set_output(0, post_exit);

                    post_exit.data<block_d>().pre_inline = pre_entry;

                    assert(post_exit.data<block_d>().creator == this);
                    assert(pre_entry.data<block_d>().creator == this);
                }
                else
                {
                    bc::small_vector<ssa_value_t, 32> fn_inputs;

                    // The [0] argument holds the fn_t ptr.
                    fn_inputs.push_back(fn_expr.ssa());

                    // For modes, the [1] argument references the stmt,
                    // otherwise it holds the bank, if necessary.
                    if(call->fclass == FN_MODE)
                        fn_inputs.push_back(locator_t::stmt(stmt_handle()));
                    else
                        fn_inputs.push_back({});

                    // Prepare the input globals

                    //bool const is_idep = fn->global.ideps().count(&call->global);
                    //assert(is_idep || call->fclass == FN_MODE);

                    std::size_t const gmember_bs_size = gmanager_t::bitset_size();
                    bitset_uint_t* const temp_bs = ALLOCA_T(bitset_uint_t, gmember_bs_size);

                    // Prepare global inputs:

                    if(call->fclass == FN_MODE)
                    {
                        // 'goto mode's use their modifiers to determine inputs.

                        assert(stmt->name == STMT_GOTO_MODE);
                        mods_t const& mods = fn->def()[stmt->mods];

                        bitset_uint_t* const preserved_bs = ALLOCA_T(bitset_uint_t, gmember_bs_size);
                        bitset_clear_all(gmember_bs_size, preserved_bs);
                        mods.for_each_list_vars(MODL_PRESERVES, [&](group_vars_ht gv, pstring_t)
                        {
                            assert(gmember_bs_size == gv->gmembers().size());
                            bitset_or(gmember_bs_size, preserved_bs, gv->gmembers().data());
                        });

                        ir->gmanager.for_each_gmember_set(fn->handle(),
                        [&](bitset_uint_t const* gmember_set, gmanager_t::index_t index,locator_t locator)
                        {
                            bitset_copy(gmember_bs_size, temp_bs, preserved_bs);
                            bitset_and(gmember_bs_size, temp_bs, gmember_set);
                            if(bitset_all_clear(gmember_bs_size, temp_bs))
                                return;
                            fn_inputs.push_back(var_lookup(builder.cfg, to_var_i(index), 0));
                            fn_inputs.push_back(locator);
                        });

                        ir->gmanager.for_each_gvar(
                        [&](gvar_ht gvar, gmanager_t::index_t index)
                        {
                            if(!mods.in_lists(MODL_PRESERVES, gvar->group()))
                                return;

                            for(gmember_ht m : gvar->handles())
                            {
                                fn_inputs.push_back(var_lookup(builder.cfg, to_var_i(index), m->member()));
                                fn_inputs.push_back(locator_t::gmember(m, 0));
                            }
                        });
                    }
                    else
                    {
                        assert(call->ir_reads().size() == gmember_bs_size);

                        // Use 'ir_reads()' to determine which members are needed by the called fn.

                        ir->gmanager.for_each_gmember_set(fn->handle(),
                        [&](bitset_uint_t const* gmember_set, gmanager_t::index_t index,locator_t locator)
                        {
                            bitset_copy(gmember_bs_size, temp_bs, call->ir_reads().data());
                            bitset_and(gmember_bs_size, temp_bs, gmember_set);
                            if(bitset_all_clear(gmember_bs_size, temp_bs))
                                return;
                            fn_inputs.push_back(var_lookup(builder.cfg, to_var_i(index), 0));
                            fn_inputs.push_back(locator);
                        });

                        ir->gmanager.for_each_gvar(
                        [&](gvar_ht gvar, gmanager_t::index_t index)
                        {
                            for(gmember_ht m = gvar->begin(); m != gvar->end(); ++m)
                            {
                                if(call->ir_reads().test(m.id))
                                {
                                    fn_inputs.push_back(var_lookup(builder.cfg, to_var_i(index), m->member()));
                                    fn_inputs.push_back(locator_t::gmember(m, 0));
                                }
                            }
                        });
                    }

                    locator_t first_bank = {};
                    if(call->fclass == FN_FN)
                        first_bank = call->first_bank_switch().mem_head();

                    // Prepare the arguments
                    for(unsigned i = 0; i < num_params; ++i)
                    {
                        type_t const param_type = call->type().type(i);
                        unsigned const num_param_members = ::num_members(param_type);

                        for(unsigned j = 0; j < num_param_members; ++j)
                        {
                            locator_t const loc = locator_t::arg(call, i, j, 0);

                            type_t const member_type = ::member_type(param_type, j);

                            ssa_value_t arg = from_variant<COMPILE>(args[i].rval()[j], member_type);

                            fn_inputs.push_back(arg);
                            fn_inputs.push_back(loc);

                            // Set the bank:
                            if(loc == first_bank)
                            {
                                assert(!fn_inputs[1]);
                                assert(call->fclass == FN_FN);
                                fn_inputs[1] = arg;
                            }
                        }
                    }

                    // Create the dependent node.
                    ssa_op_t const op = (call->fclass == FN_MODE) ? SSA_goto_mode : SSA_fn_call;
                    ssa_ht const fn_node = builder.cfg->emplace_ssa(op, TYPE_VOID);
                    fn_node->link_append_input(&*fn_inputs.begin(), &*fn_inputs.end());

                    if(call->fclass == FN_MODE || !call->ir_io_pure()/* || ir->gmanager.num_locators() > 0*/)
                        fn_node->append_daisy();

                    if(call->fclass != FN_MODE)
                    {
                        assert(fn->global.ideps().count(&call->global));

                        // After the fn is called, read all the globals it has written to:

                        ir->gmanager.for_each_gvar([&](gvar_ht gvar, gmanager_t::index_t index)
                        {
                            for(gmember_ht m = gvar->begin(); m != gvar->end(); ++m)
                            {
                                if(call->ir_writes().test(m.id))
                                {
                                    ssa_ht read = builder.cfg->emplace_ssa(
                                        SSA_read_global, m->type(), fn_node, locator_t::gmember(m, 0));
                                    block_d& block_data = builder.cfg.data<block_d>();
                                    block_data.var(to_var_i(index))[m->member()] = read;
                                }
                            }
                        });

                        ir->gmanager.for_each_gmember_set(fn->handle(),
                        [&](bitset_uint_t const* gvar_set, gmanager_t::index_t index, locator_t locator)
                        {
                            bitset_copy(gmember_bs_size, temp_bs, gvar_set);
                            bitset_and(gmember_bs_size, temp_bs, call->ir_writes().data());
                            if(!bitset_all_clear(gmember_bs_size, temp_bs))
                            {
                                ssa_ht read = builder.cfg->emplace_ssa(
                                    SSA_read_global, TYPE_VOID, fn_node, locator);
                                block_d& block_data = builder.cfg.data<block_d>();
                                block_data.var(to_var_i(index))[0] = read;
                            }
                        });

                        type_t const return_type = fn_expr.type.return_type();
                        unsigned const return_members = ::num_members(return_type);

                        for(unsigned m = 0; m < return_members; ++m)
                        {
                            ssa_ht ret = builder.cfg->emplace_ssa(
                                SSA_read_global, member_type(return_type, m), fn_node, locator_t::ret(call, m, 0));

                            result.rval().push_back(ret);
                        }
                    }
                }
            }

            return result;
        }

    case TOK_hw_addr:
        {
            expr_value_t result = { .type = TYPE_APTR, .pstring = ast.token.pstring };
            if(!is_check(D))
                result.val = rval_t{ ssa_value_t(ast.token.value, TYPE_APTR) };
            return result;
        }

    case TOK_read_hw:
        {
            if(is_interpret(D))
                compiler_error(ast.token.pstring, "Hardware read expression cannot be evaluated at compile-time.");

            expr_value_t addr = throwing_cast<D>(do_expr<D>(ast.children[0]), TYPE_APTR, true);

            expr_value_t result = 
            {
                .type = TYPE_U, 
                .pstring = ast.token.pstring,
            };

            if(is_compile(D))
            {
                ssa_ht const h = builder.cfg->emplace_ssa(
                    SSA_read_ptr_hw, TYPE_U, 
                    addr.ssa(), ssa_value_t(), ssa_value_t(), 
                    ssa_value_t(0u, TYPE_U));
                h->append_daisy();
                result.val = rval_t{ h };
            }

            return result;
        }

    case TOK_write_hw:
        {
            if(is_interpret(D))
                compiler_error(ast.token.pstring, "Hardware write expression cannot be evaluated at compile-time.");

            expr_value_t addr = throwing_cast<D>(do_expr<D>(ast.children[0]), TYPE_APTR, true);
            expr_value_t arg  = throwing_cast<D>(do_expr<D>(ast.children[1]), TYPE_U, true);

            expr_value_t result = 
            {
                .type = TYPE_VOID, 
                .pstring = ast.token.pstring,
            };

            if(is_compile(D))
            {
                assert(addr.type == TYPE_APTR);
                passert(addr.ssa().type() == TYPE_APTR, addr.ssa().type());

                ssa_ht const h = builder.cfg->emplace_ssa(
                    SSA_write_ptr_hw, TYPE_VOID, 
                    addr.ssa(), ssa_value_t(), ssa_value_t(), 
                    ssa_value_t(0u, TYPE_U), arg.ssa());
                h->append_daisy();
                result.val = rval_t{ h };
            }

            return result;
        }

    case TOK_byte_block_data:
        if(!is_check(D))
        {
            std::vector<locator_t> paa;

            unsigned const n = ast.num_children();
            for(unsigned i = 0; i < n; ++i)
            {
                ast_node_t const& sub = ast.children[i];

                switch(sub.token.type)
                {
                case TOK_byte_block_byte_array:
                    {
                        auto const* data = sub.token.ptr<std::vector<std::uint8_t>>();
                        paa.reserve(paa.size() + data->size());
                        for(std::uint8_t i : *data)
                            paa.push_back(locator_t::const_byte(i));
                    }
                    break;

                case TOK_byte_block_locator_array:
                    {
                        auto const* data = sub.token.ptr<std::vector<locator_t>>();
                        paa.insert(paa.end(), data->begin(), data->end());
                    }
                    break;

                default:
                    {
                        expr_value_t arg = to_rval<D>(do_expr<D>(sub));
                        if(sub.token.type != TOK_cast && is_ct(arg.type))
                            throw compiler_error_t(
                                fmt_error(sub.token.pstring, fmt("Expression of type % in byte block.", arg.type))
                                + fmt_note("Use an explicit cast to override."));
                        ::append_locator_bytes(paa, arg.rval(), arg.type, arg.pstring);
                    }
                    break;
                }
            }

            byte_block_data = std::move(paa);
        }
        return {};

    case TOK_byte_block_proc:
        if(!is_check(D))
        {
            asm_proc_t proc;
            proc.entry_label = locator_t::minor_label(ENTRY_LABEL);
            proc.push_inst({ .op = ASM_LABEL, .arg = proc.entry_label });

            unsigned const n = ast.num_children();
            for(unsigned i = 0; i < n; ++i)
            {
                ast_node_t const& sub = ast.children[i];

                switch(ast.children[i].token.type)
                {
                case TOK_byte_block_asm_op:
                    {
                        op_t const asm_op = op_t(sub.token.value);
                        locator_t value = {};

                        if(sub.children)
                        {
                            assert(op_addr_mode(asm_op) != MODE_IMPLIED);

                            expr_value_t arg = throwing_cast<INTERPRET_CE>(
                                do_expr<INTERPRET_CE>(*sub.children), 
                                op_addr_mode(asm_op) == MODE_IMMEDIATE ? TYPE_U : TYPE_APTR, 
                                false);

                            assert(arg.rval().size() == 1);

                            ssa_value_t const v = arg.ssa();

                            if(v.is_locator())
                                value = v.locator();
                            else if(v.is_num())
                            {
                                if(op_addr_mode(asm_op) == MODE_IMMEDIATE)
                                    value = locator_t::const_byte(v.whole());
                                else
                                    value = locator_t::addr(v.whole());
                            }
                            else
                            {
                                // Likely a bug if this fires:
                                assert(false);
                                compiler_error(sub.token.pstring, "Unable to compile assembly instruction.");
                            }
                        }

                        proc.push_inst({ .op = asm_op, .iasm_child = i, .arg = value });
                        
                    }
                    break;

                case TOK_byte_block_label:
                    proc.push_inst({ .op = ASM_LABEL, .iasm_child = i, .arg = locator_t::minor_label(sub.token.value) });
                    break;

                case TOK_byte_block_call:
                case TOK_byte_block_goto:
                    {
                        global_t const* g = sub.token.ptr<global_t>();

                        if(g->gclass() != GLOBAL_FN || g->impl<fn_t>().fclass != FN_FN)
                            compiler_error(sub.token.pstring, fmt("% is not a callable function.", g->name));

                        if(precheck_tracked)
                            precheck_tracked->calls.emplace(g->handle<fn_ht>(), sub.token.pstring);

                        op_t const op = sub.token.type == TOK_byte_block_call ? BANKED_Y_JSR : BANKED_Y_JMP;
                        proc.push_inst({ .op = LDY_IMMEDIATE, .iasm_child = i, .arg = locator_t::fn(g->handle<fn_ht>()).with_is(IS_BANK) });
                        proc.push_inst({ .op = op, .iasm_child = i, .arg = locator_t::fn(g->handle<fn_ht>()) });
                    }
                    break;

                case TOK_byte_block_goto_mode:
                    {
                        global_t const* g = sub.token.ptr<global_t>();

                        if(g->gclass() != GLOBAL_FN || g->impl<fn_t>().fclass != FN_MODE)
                            compiler_error(sub.token.pstring, fmt("% is not a mode.", g->name));

                        if(precheck_tracked)
                            precheck_tracked->goto_modes.push_back({ g->handle<fn_ht>(), { sub.token.pstring, sub.mods }});

                        // TODO
                        assert(false);
                        throw std::runtime_error("unimplemented");
                    }
                    break;

                case TOK_byte_block_wait_nmi:
                    if(precheck_tracked)
                        precheck_tracked->wait_nmis.push_back({ sub.token.pstring, sub.mods });
                    proc.push_inst({ .op = JSR_ABSOLUTE, .iasm_child = i, .arg = locator_t::runtime_rom(RTROM_wait_nmi) });
                    break;

                case TOK_byte_block_byte_array:
                    {
                        auto const* data = sub.token.ptr<std::vector<std::uint8_t>>();
                        proc.code.reserve(proc.code.size() + data->size());
                        for(std::uint8_t i : *data)
                            proc.push_inst({ .op = ASM_DATA, .iasm_child = i, .arg = locator_t::const_byte(i) });
                    }
                    break;

                case TOK_byte_block_locator_array:
                    {
                        auto const* data = sub.token.ptr<std::vector<locator_t>>();
                        proc.code.reserve(proc.code.size() + data->size());
                        for(locator_t loc : *data)
                            proc.push_inst({ .op = ASM_DATA, .iasm_child = i, .arg = loc });
                    }
                    break;

                default:
                    {
                        // TODO! this is SLOWW
                        std::puts("TODO: FIX SLOW");
                        thread_local loc_vec_t vec_temp;
                        vec_temp.clear();

                        expr_value_t arg = to_rval<D>(do_expr<D>(sub));
                        ::append_locator_bytes(vec_temp, arg.rval(), arg.type, arg.pstring);

                        for(locator_t loc : vec_temp)
                            proc.push_inst({ .op = ASM_DATA, .iasm_child = i, .arg = loc });
                    }
                }
            }

            byte_block_data = std::move(proc);
        }
        return {};

    case TOK_cast:
    case TOK_implicit_cast:
        {
            static int count = 0;
            int const cc = count++;
            // TOK_cast are pseudo tokens used to implement type casts.

            bool const implicit = ast.token.type == TOK_implicit_cast;

            // Extract how many args this cast parsed:
            std::size_t const num_children = ast.token.value;
            std::size_t const num_args = num_children - 1;

            bc::small_vector<expr_value_t, 8> args(num_args);
            for(unsigned i = 0; i < num_args; ++i)
                args[i] = to_rval<D>(do_expr<D>(ast.children[i + 1]));

            // The first expr holds the type.
            assert(ast.children[0].token.type == TOK_cast_type);
            type_t type = dethunkify({ ast.children[0].token.pstring, *ast.children[0].token.ptr<type_t const>() }, true, this);

            // Only handle LT for non-aggregates.
            // TODO
            //if(!is_aggregate(type.name()) && handle_lt<D>(rpn_stack, argn, token-1, token+1))
                //break;

            if(num_args == 0)
            {
                expr_value_t result = { .type = type, .pstring = ast.token.pstring };
                if(!is_check(D))
                    result.val = default_init(type, ast.token.pstring);
                return result;
            }

            auto const check_argn = [&](unsigned size)
            { 
                if(num_args != size)
                    compiler_error(ast.token.pstring, fmt(
                        "Too % arguments to %. Expecting %.", 
                        num_args < size ? "few" : "many", type, size));
            };

            if(is_aggregate(type.name()))
            {
                expr_value_t result = { .pstring = ast.token.pstring };

                if(type.name() == TYPE_STRUCT)
                {
                    struct_t const& s = type.struct_();

                    check_argn(s.fields().size());

                    type_t* const types = ALLOCA_T(type_t, s.fields().size());
                    for(unsigned i = 0; i < s.fields().size(); ++i)
                    {
                        types[i] = s.field(i).type();
                        assert(!is_thunk(types[i].name()));
                    }

                    int const cast_result = cast_args<D>(
                        ast.token.pstring, args.data(), args.data() + num_args, types, implicit);

                    if(cast_result >= 0)
                    {
                        assert(cast_result < (int)s.fields().size());
                        compiler_error(
                            args[cast_result].pstring, fmt(
                            "Unable to convert type % to type % in cast to %.\n",
                            args[cast_result].type, types[cast_result], s.global.name));
                    }

                    if(!is_check(D))
                    {
                        // Create a new rval.
                        rval_t new_rval;
                        new_rval.reserve(num_members(type));

                        for(unsigned i = 0; i < num_args; ++i)
                            for(auto& v : args[i].rval())
                                new_rval.push_back(std::move(v));

                        assert(new_rval.size() == num_members(type));

                        result.val = std::move(new_rval);
                    }
                }
                else if(type.name() == TYPE_TEA)
                {
                    if(num_args == 1)
                    {
                        if(args[0].type.name() == TYPE_TEA)
                        {
                            if(type.size() == 0)
                                type.unsafe_set_size(args[0].type.size());

                            return throwing_cast<D>(std::move(args[0]), type, implicit);
                        }
                        else if(type.size() != 0)
                        {
                            // Generate a fill.

                            expr_value_t fill_with = throwing_cast<D>(std::move(args[0]), type.elem_type(), implicit);

                            unsigned const num_m = num_members(type);
                            unsigned const size = type.size();
                            assert(size);
                            assert(num_m == num_members(type.elem_type()));

                            if(!is_check(D))
                            {
                                passert(num_m == fill_with.rval().size(), num_m, fill_with.rval().size());

                                rval_t new_rval;
                                new_rval.reserve(num_m);

                                for(unsigned m = 0; m < num_m; ++m)
                                {
                                    ct_array_t array = make_ct_array(size);
                                    for(unsigned i = 0; i < size; ++i)
                                        array[i] = std::get<ssa_value_t>(fill_with.rval()[m]);
                                    new_rval.push_back(std::move(array));
                                }

                                result.val = std::move(new_rval);
                            }

                            result.type = type;
                            return result;
                        }
                    }

                    if(type.size() == 0)
                    {
                        if(num_args == 0)
                            compiler_error(ast.token.pstring, "Invalid array length of 0.");
                        type.unsafe_set_size(num_args);
                    }
                    else 
                        check_argn(type.size());

                    for(unsigned i = 0; i < num_args; ++i)
                        args[i] = throwing_cast<D>(std::move(args[i]), type.elem_type(), implicit);

                    if(is_interpret(D))
                    {
                    interpret_cast_array:
                        // Create a new rval.
                        rval_t new_rval(num_members(type));

                        for(unsigned i = 0; i < new_rval.size(); ++i)
                        {
                            ct_array_t shared = make_ct_array(num_args);
                            for(unsigned j = 0; j < num_args; ++j)
                                shared[j] = std::get<ssa_value_t>(args[j].rval()[i]);
                            new_rval[i] = std::move(shared);
                        }

                        result.val = std::move(new_rval);
                    }
                    else if(is_compile(D))
                    {
                        unsigned const num_mem = num_members(type);

                        for(unsigned i = 0; i < num_mem; ++i)
                        {
                            type_t const mt = member_type(type, i);
                            assert(mt.name() == TYPE_TEA);

                            for(unsigned j = 0; j < num_args; ++j)
                                if(!args[j].is_ct())
                                    goto isnt_ct;
                        }
                        goto interpret_cast_array;
                    isnt_ct:

                        // Create a new rval.
                        rval_t new_rval(num_mem);

                        for(unsigned i = 0; i < num_mem; ++i)
                        {
                            type_t const mt = member_type(type, i);
                            assert(mt.name() == TYPE_TEA);

                            ssa_ht h = builder.cfg->emplace_ssa(SSA_init_array, type);
                            h->alloc_input(num_args);
                            for(unsigned j = 0; j < num_args; ++j)
                                h->build_set_input(j, std::get<ssa_value_t>(args[j].rval()[i]));

                            new_rval[i] = h;
                        }

                        result.val = std::move(new_rval);
                    }
                }

                result.type = type;
                return result;
            }
            else if(is_scalar(type.name()) || is_banked_ptr(type.name()))
            {
                check_argn(1);
                return throwing_cast<D>(args[0], type, implicit);
            }
            else
                compiler_error(ast.token.pstring, fmt("Unable to cast to %.", type));
        }
        assert(0);

    case TOK_index8:
    case TOK_index16:
        {
            bool const is8 = ast.token.type == TOK_index8;

            /* TODO
            if(locator_t loc = handle_lt<Policy::D>(result.type, { .type = ast.token.type, .pstring = result.pstring }, lhs, rhs))
            {
                result.val = _lt_rval(result.type, loc);
                result.time = LT;
            }
                */

            // TOK_index is a psuedo token used to implement array indexing. 

            // TODO
            //if(handle_lt<D>(rpn_stack, 2, *token))
                //break;

            expr_value_t array_val = do_expr<D>(ast.children[0]);

            bool const is_ptr = ::is_ptr(array_val.type.name());
            bool const is_mptr = ::is_mptr(array_val.type.name());

            if(is_ptr && is_interpret(D))
                compiler_error(ast.token.pstring, "Pointers cannot be dereferenced at compile-time.");

            if(is_ptr && ::is_aptr(array_val.type.name()))
            {
                compiler_error(ast.token.pstring, fmt(
                    "Unable to dereference type % using '%'.", array_val.type,
                    is8 ? "[]" : "{}"));
            }

            if(!is_tea(array_val.type.name()) && !is_ptr)
            {
                std::string note;
                if(is_paa(array_val.type.name()))
                    note = fmt_note("Did you forget to use operator '@'?");
                throw compiler_error_t(
                    fmt_error(array_val.pstring, fmt(
                        "Expecting array or pointer type to '%'. Got %.", 
                        is8 ? "[]" : "{}", array_val.type))
                    + note);
            }

            if(is_ptr && precheck_tracked)
            {
                // TODO: Update this when inlining is added.
                unsigned const size = array_val.type.group_tail_size();
                precheck_tracked->deref_types.insert(array_val.type);
                for(unsigned i = 0; i < size; ++i)
                    precheck_tracked->deref_groups.emplace(array_val.type.group(i), src_type_t{ array_val.pstring, array_val.type });
            }

            expr_value_t array_index = throwing_cast<D>(do_expr<D>(ast.children[1]), is8 ? TYPE_U : TYPE_U20, true);

            auto const compile_read_ptr = [&](ssa_value_t ptr, ssa_value_t bank)
            {
                if(is8)
                {
                    return builder.cfg->emplace_ssa(
                        SSA_read_ptr, TYPE_U, 
                        ptr, ssa_value_t(), bank,
                        std::get<ssa_value_t>(array_index.rval()[0]));
                }
                else
                {
                    ssa_ht h = builder.cfg->emplace_ssa(
                        SSA_add, TYPE_APTR, 
                        ptr,
                        std::get<ssa_value_t>(array_index.rval()[0]),
                        ssa_value_t(0u, TYPE_BOOL));

                    return builder.cfg->emplace_ssa(
                        SSA_read_ptr, TYPE_U, 
                        h, ssa_value_t(), bank,
                        ssa_value_t(0u, TYPE_U));
                }
            };

            /* TODO
            if(precheck_tracked && array_val.is_lval())
            {
                lval_t* lval = array_val.is_lval();
                if(lval->is_global())
                {
                    global_t const& global = lval->global();
                    precheck_tracked->gvars_used.emplace(global.handle<gvar_ht>(), array_val.pstring);
                }
            }
            */

            if(!is_check(D) && array_val.is_lval())
            {
                if(is_ptr)
                {
                    assert(is_compile(D));

                    rval_t rval = to_rval<D>(array_val).rval();

                    bool const is_banked = is_banked_ptr(array_val.type.name());

                    deref_t deref =
                    {
                        .ptr = from_variant<D>(rval[0], array_val.type), 
                        .bank = is_banked ? from_variant<D>(rval[1], TYPE_U) : ssa_value_t(), 
                        .index = array_index.ssa()
                    };

                    if(!is8)
                    {
                         deref.ptr = builder.cfg->emplace_ssa(
                            SSA_add, TYPE_APTR, 
                            deref.ptr, deref.index,
                            ssa_value_t(0u, TYPE_BOOL));

                         deref.index = ssa_value_t(0u, TYPE_U);
                    }

                    array_val.val = std::move(deref);
                }
                else
                {
                    if(!is8)
                        array_val.lval().flags |= LVALF_INDEX_16;
                    assert(!array_val.lval().index);
                    array_val.lval().index = array_index.ssa();
                    assert(array_val.lval().index);
                }
            }
            else if(rval_t* rval_ptr = array_val.is_rval())
            {
                rval_t& rval = *rval_ptr;

                if(is_interpret(D))
                {
                    unsigned const index = array_index.whole();

                    if(index >= array_val.type.array_length())
                    {
                        compiler_error(array_index.pstring, 
                            fmt("Array index is out of bounds. (index: % >= size: %)", 
                                index, array_val.type.array_length()));
                    }

                    for(auto& v : rval) // TODO: handle link
                        v = std::get<ct_array_t>(v)[index];
                }
                else if(is_compile(D))
                {
                    if(is_ptr)
                    {
                        bool const is_banked = is_banked_ptr(array_val.type.name());
                        assert(array_val.rval().size() == is_banked ? 2 : 1);

                        // TODO
                        //ssa_value_t prev_in_order = {};
                        //if(auto ptr_i = ir->gmanager.ptr_i(array_val.type))
                            //prev_in_order = var_lookup(builder.cfg, to_var_i(ptr_i), 0);

                        if(is_mptr)
                        {
                            // TODO: combine this with deref code above.

                            deref_t deref =
                            {
                                .ptr = from_variant<D>(rval[0], array_val.type), 
                                .bank = is_banked ? from_variant<D>(rval[1], TYPE_U) : ssa_value_t(), 
                                .index = array_index.ssa()
                            };

                            if(!is8)
                            {
                                 deref.ptr = builder.cfg->emplace_ssa(
                                    SSA_add, TYPE_APTR, 
                                    deref.ptr, deref.index,
                                    ssa_value_t(0u, TYPE_BOOL));

                                 deref.index = ssa_value_t(0u, TYPE_U);
                            }

                            array_val.val = std::move(deref);
                        }
                        else
                        {
                            ssa_ht const h = compile_read_ptr(
                                from_variant<D>(rval[0], array_val.type),
                                is_banked ? from_variant<D>(rval[1], TYPE_U) : ssa_value_t());

                            if(ptr_to_vars(array_val.type))
                                h->append_daisy();

                            rval[0] = h;
                            rval.resize(1);
                        }
                    }
                    else
                    {
                        for(unsigned i = 0; i < rval.size(); ++i)
                        {
                            type_t const etype = ::member_type(array_val.type, i);
                            assert(etype.name() == TYPE_TEA);

                            rval[i] = builder.cfg->emplace_ssa(
                                is8 ? SSA_read_array8 : SSA_read_array16, etype.elem_type(), 
                                from_variant<D>(rval[i], etype), ssa_value_t(0u, TYPE_U20), 
                                std::get<ssa_value_t>(array_index.rval()[0]));
                        }
                    }
                }
            }
            else if(strval_t* strval = array_val.is_strval())
            {
                std::string const& str = strval->get_string();

                if(is_interpret(D) || (is_compile(D) && array_index.ssa().is_num()))
                {
                    unsigned const i = array_index.whole();

                    if(i >= str.size())
                    {
                        compiler_error(array_index.pstring, 
                            fmt("Array index is out of bounds. (index: % >= size: %)", 
                                i, str.size()));
                    }

                    array_val.val = rval_t{ ssa_value_t(std::uint8_t(str[i]), TYPE_U) };
                }
                else if(is_compile(D))
                {
                    // convert to rom data
                    rom_array_ht const rom_array = sl_manager.get_rom_array(&strval->charmap->global, strval->index, strval->compressed);
                    assert(rom_array);
                    assert(strval->charmap->group_data());
                    bool const is_banked = strval->charmap->group_data()->banked_ptrs();

                    locator_t const loc = locator_t::rom_array(rom_array);

                    ssa_ht const h = compile_read_ptr(
                        loc, is_banked ? loc.with_is(IS_BANK) : ssa_value_t());

                    array_val.val = rval_t { h };
                }
            }

            if(is_ptr)
                array_val.type = TYPE_U;
            else
                array_val.type = array_val.type.elem_type();

            array_val.pstring = concat(array_val.pstring, array_index.pstring);

            return array_val;
        }

        /* TODO: remove
    case TOK_dquote:
        {
            string_literal_t const* literal = token.ptr<string_literal_t>();
            assert(literal);

            global_t const* charmap = literal->charmap;
            if(!charmap)
                TODO;

            if(charmap->gclass() != GLOBAL_CHARMAP)
                compiler_error(literal->pstring, fmt("% is not a charmap.", charmap->name));

            assert(false);
        }
        break;
        */

    case TOK_character:
        {
            assert(ast.charmap);
            charmap_t const& charmap = get_charmap(ast.token.pstring, *ast.charmap);

            int const result = charmap.convert(ast.token.value);
            if(result < 0)
                compiler_error(ast.token.pstring, fmt("Character isn't in %.", charmap.global.name));

            common_value.set(result, TYPE_INT);
            goto push_int;
        }

    case TOK_string_compressed:
    case TOK_string_uncompressed:
        {
            assert(ast.charmap);
            charmap_t const& charmap = get_charmap(ast.token.pstring, *ast.charmap);

            bool const compressed = ast.token.type == TOK_string_compressed;
            unsigned const index = ast.token.value;
            unsigned const str_size = sl_manager.get_string(&charmap.global, index, compressed).size();

            expr_value_t v =
            { 
                .val = strval_t{ .charmap = &charmap, .compressed = compressed, .index = ast.token.value },
                .type = type_t::tea(TYPE_U, str_size), 
                .pstring = ast.token.pstring,
            };

            return v;
        }

    case TOK_sizeof_expr:
        common_type = do_expr<CHECK>(ast.children[0]).type;
        goto do_sizeof;

    case TOK_sizeof:
        {
            common_type = dethunkify({ ast.token.pstring, *ast.token.ptr<type_t const>() }, true, this);
        do_sizeof:
            unsigned const size = common_type.size_of();

            if(size == 0)
                compiler_error(ast.token.pstring, fmt("Type % has no size.", common_type));

            common_value.set(size, TYPE_INT);
            goto push_int;
        }

    case TOK_len_expr:
        common_type = do_expr<CHECK>(ast.children[0]).type;
        goto do_len;

    case TOK_len:
        {
            common_type = dethunkify({ ast.token.pstring, *ast.token.ptr<type_t const>() }, true, this);
        do_len:
            if(is_check(D))
                goto push_int;

            unsigned const size = common_type.array_length();

            if(size == 0)
                compiler_error(ast.token.pstring, fmt("Type % isn't an array.", common_type));

            common_value.set(size, TYPE_INT);
            goto push_int;
        }

    case TOK_assign:
        return infix(&eval_t::do_assign<D>, false, true);

    case TOK_logical_and:
    case TOK_logical_or:
        return do_logical<D>(ast);

    case TOK_eq:
        struct eq_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_eq; }
            static bool interpret(S lhs, S rhs, pstring_t) { return lhs == rhs; }
            static ssa_op_t op() { return SSA_eq; }
        };
        return infix(&eval_t::do_compare<eq_p>);
    case TOK_not_eq:
        struct not_eq_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_not_eq; }
            static bool interpret(S lhs, S rhs, pstring_t) { return lhs != rhs; }
            static ssa_op_t op() { return SSA_not_eq; }
        };
        return infix(&eval_t::do_compare<not_eq_p>);
    case TOK_lt:
    case TOK_gt:
        struct lt_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_lt; }
            static bool interpret(S lhs, S rhs, pstring_t) { return lhs < rhs; }
            static ssa_op_t op() { return SSA_lt; }
        };
        return infix(&eval_t::do_compare<lt_p>, ast.token.type == TOK_gt);
    case TOK_lte:
    case TOK_gte:
        struct lte_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_lte; }
            static bool interpret(S lhs, S rhs, pstring_t) { return lhs <= rhs; }
            static ssa_op_t op() { return SSA_lte; }
        };
        return infix(&eval_t::do_compare<lte_p>, ast.token.type == TOK_gte);

    case TOK_asterisk:
        struct multiply_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_asterisk; }
            static S interpret(S lhs, S rhs, pstring_t) { return fixed_mul(lhs, rhs); }
            static ssa_op_t op() { return SSA_mul; }
        };
        return infix(&eval_t::do_mul<multiply_p>);

    case TOK_times_assign:
        return infix(&eval_t::do_assign_mul<multiply_p>, false, true);

    case TOK_fslash:
        struct div_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_fslash; }
            static S interpret(S lhs, S rhs, pstring_t at) 
            { 
                if(!rhs)
                    compiler_error(at, "Division by zero.");
                return fixed_div(lhs, rhs); 
            }
            static ssa_op_t op() { return SSA_null; }
        };
        return infix(&eval_t::do_arith<div_p>);

    case TOK_div_assign:
        return infix(&eval_t::do_assign_arith<div_p>, false, true);

    case TOK_plus:
        struct plus_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_plus; }
            static auto lt_assign() { return TOK_plus_assign; }
            static S interpret(S lhs, S rhs, pstring_t) { return lhs + rhs; }
            static bool interpret_carry(U lhs, U rhs, U mask, pstring_t) 
            {
                return (lhs + rhs) & (high_bit_only(mask) << 1);
            }
            static ssa_op_t op() { return SSA_add; }
        };
        return infix(&eval_t::do_add<plus_p>);

    case TOK_plus_assign:
        return infix(&eval_t::do_assign_add<plus_p>, false, true);

    case TOK_minus: 
        struct minus_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_minus; }
            static auto lt_assign() { return TOK_minus_assign; }
            static S interpret(S lhs, S rhs, pstring_t) { return lhs - rhs; }
            static bool interpret_carry(U lhs, U rhs, U mask, pstring_t) 
            {
                return !((lhs - rhs) & (high_bit_only(mask) << 1));
            }
            static ssa_op_t op() { return SSA_sub; }
        };
        return infix(&eval_t::do_add<minus_p>);
    case TOK_minus_assign:
        return infix(&eval_t::do_assign_add<minus_p>, false, true);

    case TOK_bitwise_and: 
        struct bitwise_and_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_bitwise_and; }
            static S interpret(S lhs, S rhs, pstring_t) { return lhs & rhs; }
            static ssa_op_t op() { return SSA_and; }
        };
        return infix(&eval_t::do_arith<bitwise_and_p>);
    case TOK_bitwise_and_assign:
        return infix(&eval_t::do_assign_arith<bitwise_and_p>, false, true);

    case TOK_bitwise_or:  
        struct bitwise_or_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_bitwise_or; }
            static S interpret(S lhs, S rhs, pstring_t) { return lhs | rhs; }
            static ssa_op_t op() { return SSA_or; }
        };
        return infix(&eval_t::do_arith<bitwise_or_p>);
    case TOK_bitwise_or_assign:
        return infix(&eval_t::do_assign_arith<bitwise_or_p>, false, true);

    case TOK_bitwise_xor:
        struct bitwise_xor_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_bitwise_xor; }
            static S interpret(S lhs, S rhs, pstring_t) { return lhs ^ rhs; }
            static ssa_op_t op() { return SSA_xor; }
        };
        return infix(&eval_t::do_arith<bitwise_xor_p>);
    case TOK_bitwise_xor_assign:
        return infix(&eval_t::do_assign_arith<bitwise_xor_p>, false, true);

    case TOK_rol:
        struct rol_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_rol; }
            static auto lt_assign() { return TOK_rol_assign; }
            static U interpret(U operand, bool carry, U mask, pstring_t) 
            { 
                return (operand << 1) | (carry * low_bit_only(mask)); 
            }
            static bool interpret_carry(U operand, U mask, pstring_t) 
            {
                return operand & high_bit_only(mask); 
            }
            static ssa_op_t op() { return SSA_rol; }
        };
        return infix(&eval_t::do_rotate<rol_p>);
    case TOK_rol_assign:
        return infix(&eval_t::do_assign_rotate<rol_p>, false, true);

    case TOK_ror:
        struct ror_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_ror_flip; }
            static auto lt_assign() { return TOK_ror_assign_flip; }
            static U interpret(U operand, bool carry, fixed_uint_t mask, pstring_t) 
            { 
                return (operand >> 1) | (carry * high_bit_only(mask)); 
            }
            static bool interpret_carry(U operand, U mask, pstring_t) 
            {
                return operand & low_bit_only(mask); 
            }
            static ssa_op_t op() { return SSA_ror; }
        };
        return infix(&eval_t::do_rotate<ror_p>, true);
    case TOK_ror_flip:
        return infix(&eval_t::do_rotate<ror_p>);
    case TOK_ror_assign:
        return infix(&eval_t::do_assign_rotate<ror_p>, true, true);
    case TOK_ror_assign_flip:
        return infix(&eval_t::do_assign_rotate<ror_p>, false, true);


    case TOK_lshift:
        struct lshift_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_lshift; }
            static auto lt_assign() { return TOK_lshift_assign; }
            static S interpret(S lhs, std::uint8_t shift, pstring_t) { return lhs << shift; }
            static bool interpret_carry(S lhs, std::uint8_t shift, fixed_uint_t mask, pstring_t) 
            {
                assert(high_bit_only(mask) << 1ull); 
                return (lhs << shift) & (high_bit_only(mask) << 1ull); 
            }
            static ssa_op_t op() { return SSA_shl; }
        };
        return infix(&eval_t::do_shift<lshift_p>);
    case TOK_lshift_assign:
        return infix(&eval_t::do_assign_shift<lshift_p>, false, true);

    case TOK_rshift:
        struct rshift_p : do_wrapper_t<D>
        {
            static auto lt() { return TOK_rshift; }
            static auto lt_assign() { return TOK_rshift_assign; }
            static S interpret(S lhs, std::uint8_t shift, pstring_t) { return lhs >> shift; }
            static bool interpret_carry(S lhs, std::uint8_t shift, fixed_uint_t mask, pstring_t) 
            {
                assert(low_bit_only(mask)); 
                return ((lhs << 1ull) >> shift) & (low_bit_only(mask)); 
            }
            static ssa_op_t op() { return SSA_shr; }
        };
        return infix(&eval_t::do_shift<rshift_p>);
    case TOK_rshift_assign:
        return infix(&eval_t::do_assign_shift<rshift_p>, false, true);

    case TOK_unary_negate:
        {
            expr_value_t v = throwing_cast<D>(do_expr<D>(ast.children[0]), TYPE_BOOL, true);

            if(locator_t loc = handle_lt<D>(TYPE_BOOL, ast.token, v))
            {
                v.val = _lt_rval(TYPE_BOOL, loc);
                v.time = LT;
            }
            else if(is_interpret(D) || (is_compile(D) && is_ct(v.type.name())))
                v.ssa().set(unsigned(!v.fixed()), TYPE_BOOL);
            else if(is_compile(D))
            {
                // Must be two lines; reference invalidation lurks.
                ssa_ht const ssa = builder.cfg->emplace_ssa(SSA_xor, TYPE_BOOL, v.ssa(), ssa_value_t(1u, TYPE_BOOL));
                v.ssa() = ssa;
            }

            return v;
        }

    case TOK_unary_plus:
        {
            expr_value_t v = to_rval<D>(do_expr<D>(ast.children[0]));
            req_quantity(ast.token, v);

            if(locator_t loc = handle_lt<D>(v.type, ast.token, v))
            {
                v.val = _lt_rval(v.type, loc);
                v.time = LT;
            }
            else if(!is_arithmetic(v.type.name()) && !is_ptr(v.type.name()))
            {
                compiler_error(v.pstring, fmt("% expects arithmetic or pointer inputs. (Operand is %)", 
                                              token_string(ast.token.type), v.type));
            }
            return v;
        }

    case TOK_unary_minus:
        {
            expr_value_t v = to_rval<D>(do_expr<D>(ast.children[0]));
            req_quantity(ast.token, v);

            if(locator_t loc = handle_lt<D>(v.type, ast.token, v))
            {
                v.val = _lt_rval(v.type, loc);
                v.time = LT;
            }
            else if(is_interpret(D) || (is_compile(D) && is_ct(v.type.name())))
                v.ssa().set(mask_numeric(fixed_t{ -v.fixed().value }, v.type.name()), v.type.name());
            else if(is_compile(D))
            {
                // Must be two lines; reference invalidation lurks.
                ssa_ht const ssa = builder.cfg->emplace_ssa(SSA_sub, v.type, ssa_value_t(0u, v.type.name()), v.ssa());
                v.ssa() = ssa;
            }

            return v;
        }

    case TOK_unary_xor:
        {
            expr_value_t v = to_rval<D>(do_expr<D>(ast.children[0]));
            req_quantity(ast.token, v);

            if(locator_t loc = handle_lt<D>(v.type, ast.token, v))
            {
                v.val = _lt_rval(v.type, loc);
                v.time = LT;
            }
            else if(is_interpret(D) || (is_compile(D) && is_ct(v.type.name())))
                v.ssa().set(mask_numeric(fixed_t{ ~v.fixed().value }, v.type.name()), v.type.name());
            else if(is_compile(D))
            {
                // Must be two lines; reference invalidation lurks.
                ssa_ht const ssa = builder.cfg->emplace_ssa(
                    SSA_xor, v.type, ssa_value_t(numeric_bitmask(v.type.name()), v.type.name()), v.ssa());
                v.ssa() = ssa;
            }

            return v;
        }

    case TOK_shift_atom:
        {
            assert(is_link(D));

            locator_t const loc = locator_t::from_uint(ast.uint);
            int const shift = static_cast<int>(ast.token.value);

            ssa_value_t v;

            if(loc.lclass() == LOC_LT_EXPR)
            {
                loc.link(romv);
                assert(loc.member() < loc.lt()->results[romv].rval.size());
                v = std::get<ssa_value_t>(loc.lt()->results[romv].rval[loc.member()]);
            }
            else
            {
                std::uint16_t const data = linked_to_rom(loc.link(romv), true);
                v = ssa_value_t(data, loc.type().name());
            }

            return 
            {
                .val = rval_t{ _interpret_shift_atom(v, shift, ast.token.pstring) },
                .type = TYPE_U,
                .pstring = ast.token.pstring,
            };
        }
    }
    assert(false);
    return {};
}

/* TODO
template<eval_t::do_t D>
void eval_t::do_expr(rpn_stack_t& rpn_stack, token_t const* expr)
{
    check_time();

    rpn_stack.clear(); // Reset the stack.

    assert(expr);
    for(token_t const* token = expr; token->type;)
        token = do_token<D>(rpn_stack, token);
}
*/

/* TODO
type_t lval_type(lval_t const& lval)
{
    type_t type = var_types[lval.var_i];
    
    for(unsigned field : lval.fields)
    {
        assert(type.name() == TYPE_STRUCT);
        struct_t const& s = *type.struct_();
        type = s.field(type).type();


    }

    unsigned member = lval.member;
    for(unsigned i = 0; i < lval.fields_accesed; ++i)
    {

        unsigned field = s.member_field(member);
        member -= 
        s.field(field).type();

        unsigned field = type.struct_()->member_field(member);
    }
}
*/

template<eval_t::do_t D>
expr_value_t eval_t::to_rval(expr_value_t v)
{
    if(v.is_rval())
        return v;

    if(lval_t* lval = v.is_lval())
    {
        unsigned const num_members = ::num_members(v.type);
        type_t type;
        rval_t rval;

        if(lval->arg == lval_t::READY_ARG)
        {
            if(is_compile(D))
                v.val = rval_t{ builder.cfg->emplace_ssa(SSA_ready, TYPE_BOOL) };

            return v;
        }

        if(lval->arg == lval_t::RETURN_ARG)
            compiler_error(v.pstring, "Cannot access the value of return.");

        if(lval->is_global())
        {
            global_t const& global = lval->global();

            switch(global.gclass())
            {
            case GLOBAL_CONST:
                if(!is_check(D))
                {
                    const_t const& c = global.impl<const_t>();
                    rval = c.rval();
                    type = c.type();

                    // Shrink the rval to only contain the specified field.
                    if(lval->member != 0)
                        for(unsigned i = 0; i < num_members; ++i)
                            rval[i] = std::move(rval[i + lval->member]);
                }
                break;

            case GLOBAL_VAR:
                if(precheck_tracked)
                {
                    precheck_tracked->gvars_used.emplace(global.handle<gvar_ht>(), v.pstring);
                }
                if(is_compile(D))
                {
                    lval->set_var_i(to_var_i(global.handle<gvar_ht>()));
                    goto have_var_i;
                }
                else if(!is_check(D))
                    compiler_error(v.pstring, "Cannot use global variable in this context.");
                break;

            case GLOBAL_FN:
                if(lval->arg < 0)
                {
                    assert(v.type.name() == TYPE_FN);
                    assert(lval->member == 0);
                    assert(lval->atom < 0);
                    assert(!lval->index);

                    v.val = rval_t{ ssa_value_t(locator_t::fn(global.handle<fn_ht>(), lval->ulabel())) };
                    type = v.type;

                    assert(v.rval().size() == 1);
                }
                else
                    compiler_error(v.pstring, "Cannot use the value of a function parameter or return this way.");
                return v;
            default:
                assert(false);
            }
        }
        else
        {
            assert(lval->is_var());

            if(D == INTERPRET_CE)
            {
                if(fn && fn->iasm)
                    throw compiler_error_t(
                        fmt_error(v.pstring, "Expression cannot be evaluated at compile-time.")
                        + fmt_note("Did you forget to prefix a variable with '&'?"));
                else
                    compiler_error(v.pstring, "Expression cannot be evaluated at compile-time.");
            }
        have_var_i:
            type = var_type(lval->var_i());
            rval.resize(num_members);
        }

        if(is_check(D))
        {
            v.val = std::move(rval);
            return v;
        }

        type = ::member_type(type, lval->member);

        if(is_interpret(D))
        {
            if(lval->is_var())
                for(unsigned i = 0; i < num_members; ++i)
                    rval[i] = interpret_locals[to_local_i(lval->var_i())][lval->member + i];

            if(lval->index)
            {
                assert(is_tea(type.name()));
                assert(lval->index.is_num());

                unsigned const index = lval->index.whole();
                assert(index < type.array_length());
                for(auto& v : rval) // TODO: handle link
                    v = std::get<ct_array_t>(v)[index];

                type = type.elem_type();
            }

            if(lval->atom >= 0)
            {
                if(is_tea(type.name()))
                {
                    assert(is_tea(v.type.name()));
                    assert(v.type.elem_type() == TYPE_U);
                    int const shift = lval->atom - frac_bytes(type.elem_type().name());

                    unsigned const tea_length = type.array_length();
                    ct_array_t const& from = std::get<ct_array_t>(rval[0]);
                    ct_array_t to = make_ct_array(tea_length);

                    for(unsigned i = 0; i < tea_length; ++i)
                        to[i] = _interpret_shift_atom(from[i], shift, v.pstring);

                    rval = { std::move(to) };
                }
                else
                {
                    assert(v.type == TYPE_U);
                    int const shift = lval->atom - frac_bytes(type.name());
                    rval = { _interpret_shift_atom(std::get<ssa_value_t>(rval[0]), shift, v.pstring) };
                }
            }
        }
        else if(is_compile(D))
        {
            if(lval->is_var())
                for(unsigned i = 0; i < num_members; ++i)
                    rval[i] = var_lookup(builder.cfg, lval->var_i(), lval->member + i);

            if(lval->index)
            {
                assert(is_tea(type.name()));
                type_t const elem = type.elem_type();

                for(unsigned i = 0; i < num_members; ++i)
                {
                    // TODO
                    rval[i] = builder.cfg->emplace_ssa(
                        (lval->flags & LVALF_INDEX_16) ? SSA_read_array16 : SSA_read_array8, elem, 
                        from_variant<D>(rval[i], type), ssa_value_t(0u, TYPE_U20), lval->index);
                }

                type = elem;
            }

            if(lval->atom >= 0)
            {
                passert(rval.size() > 0, rval.size(), lval->member);
                ssa_ht const h = builder.cfg->emplace_ssa(
                    is_tea(type.name()) ? SSA_array_get_byte : SSA_get_byte, 
                    is_tea(type.name()) ? type_t::tea(TYPE_U, type.size()) : TYPE_U, 
                    std::get<ssa_value_t>(rval[0]), 
                    ssa_value_t(lval->atom, TYPE_U));
                rval = { h };
            }
        }

        v.val = std::move(rval);
    }
    else if(deref_t const* deref = v.is_deref())
    {
        if(!is_compile(D))
            compiler_error(v.pstring, "Can only dereference at compile time.");

        ssa_ht const read = builder.cfg->emplace_ssa(
            SSA_read_ptr, TYPE_U, deref->ptr, ssa_value_t(), deref->bank, deref->index);

        if(ptr_to_vars(deref->ptr->type()))
            read->append_daisy();

        v.val = rval_t{ read };
    }
    else if(strval_t const* strval = v.is_strval())
    {
        passert(v.type.name() == TYPE_TEA, v.type);
        assert(strval->charmap);

        std::string const& str = strval->get_string();
        assert(str.size() == v.type.size());
        ct_array_t ct_array = make_ct_array(str.size());

        for(unsigned i = 0; i < str.size(); ++i)
            ct_array[i] = ssa_value_t(std::uint8_t(str[i]), TYPE_U);

        v.val = rval_t{ std::move(ct_array) };
    }
    else
        std::runtime_error("Cannot convert to rvalue.");
    return v;
}

template<eval_t::do_t D>
expr_value_t eval_t::do_assign(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    pstring_t const pstring = concat(lhs.pstring, lhs.pstring);

    lval_t* const lval = lhs.is_lval();

    if(lval && lval->is_global())
    {
        global_t const& global = lval->global();

        if(global.gclass() == GLOBAL_VAR)
        {
            if(!is_check(D))
                lval->set_var_i(to_var_i(global.handle<gvar_ht>()));

            if(precheck_tracked)
                precheck_tracked->gvars_used.emplace(global.handle<gvar_ht>(), lhs.pstring);
        }
        else
            compiler_error(pstring, fmt("Unable to modify %", global.name));
    }

    if(is_check(D))
        return throwing_cast<D>(std::move(rhs), lhs.type, true);

    if(is_paa(lhs.type.name()))
        compiler_error(pstring, "Cannot assign pointer-addressible arrays.");

    if(lhs.is_lt())
        compiler_error(pstring, "Expression cannot be evaluated at link-time.");

    if(deref_t const* deref = lhs.is_deref())
    {
        if(!is_compile(D))
            compiler_error(pstring, "Can only dereference at compile time.");

        rhs = throwing_cast<D>(std::move(rhs), TYPE_U, true);

        ssa_ht const write = builder.cfg->emplace_ssa(
            SSA_write_ptr, TYPE_VOID,
            deref->ptr, ssa_value_t(), deref->bank, deref->index,
            std::get<ssa_value_t>(rhs.rval()[0]));
        write->append_daisy();

        return rhs;
    }

    if(!is_check(D) && !lval)
        compiler_error(pstring, "Expecting lvalue as operand to assignment.");

    rhs = throwing_cast<D>(std::move(rhs), lhs.type, true);

    // de-atomize
    if(!is_check(D) && lval->atom >= 0)
    {
        expr_value_t without_atom = lhs;
        without_atom.lval().atom = -1;
        without_atom = to_rval<D>(without_atom);

        type_t new_type = member_type(var_type(lval->var_i()), lval->member);
        if(lval->index)
        {
            assert(is_tea(new_type.name()));
            new_type = new_type.elem_type();
        }
        assert(num_members(new_type) == 1);
        assert(num_members(rhs.type) == 1);

        if(is_interpret(D))
        {
            int const shift = lval->atom - frac_bytes(new_type.name());
            assert(shift >= 0);

            fixed_uint_t mask = numeric_bitmask(rhs.type.name());
            if(shift < 0)
                mask >>= (-shift * 8);
            else
                mask <<= (shift * 8);

            auto const convert = [&](ssa_value_t from, ssa_value_t to) -> ssa_value_t
            {
                fixed_uint_t replace = to.fixed().value;
                assert((replace & mask) == replace);

                if(shift < 0)
                    replace >>= (-shift * 8);
                else
                    replace <<= (shift * 8);

                fixed_uint_t u = from.fixed().value;
                u &= ~mask;
                u |= replace;
                return ssa_value_t(u, new_type.name());
            };

            if(is_tea(new_type.name()))
            {
                unsigned const tea_length = new_type.array_length();
                ct_array_t const& from = std::get<ct_array_t>(without_atom.rval()[0]);
                ct_array_t to = make_ct_array(tea_length);

                for(unsigned i = 0; i < tea_length; ++i)
                    to[i] = convert(from[i], to[i]);

                rhs.rval() = { std::move(to) };
            }
            else
                rhs.rval() = { convert(without_atom.ssa(), rhs.ssa()) };

        }
        else if(is_compile(D))
        {
            ssa_ht const h = builder.cfg->emplace_ssa(
                is_tea(new_type.name()) ? SSA_array_replace_byte : SSA_replace_byte, 
                new_type, 
                without_atom.ssa(), ssa_value_t(lval->atom, TYPE_U), rhs.ssa());
            rhs.rval() = { h };
        }

        rhs.type = new_type;
    }

    if(is_check(D))
        goto finish;

    // TODO
    //assert(lval->var_i() < var_types.size());

    // Remap the identifier to point to the new value.
    if(is_interpret(D) && D != INTERPRET_CE)
    {
        //assert(lval->var_i() < interpret_locals.size()); TODO
        rval_t& local = interpret_locals[to_local_i(lval->var_i())];
        rval_t& rval = rhs.rval();

        if(lval->index)
        {
            type_t const mt = member_type(var_type(lval->var_i()), lval->member);
            assert(mt.name() == TYPE_TEA);

            unsigned const array_size = mt.array_length();
            unsigned const index = lval->index.whole();
            assert(index <= array_size);

            for(unsigned i = 0; i < rval.size(); ++i)
            {
                // TODO: handle linked
                ct_array_t& shared = std::get<ct_array_t>(local[i + lval->member]);

                // If the array has multiple owners, copy it, creating a new one.
                if(shared.use_count() > 1)
                {
                    ct_array_t new_shared = make_ct_array(array_size);
                    std::copy(shared.get(), shared.get() + array_size, new_shared.get());
                    shared = std::move(new_shared);
                }

                // TODO: handle linked
                shared[index] = std::get<ssa_value_t>(rval[i]);
            }
        }
        else
        {
            if(lval->atom >= 0)
            {
                assert(false);
                /*
                assert(local.size() == 1);

                type_t const mt = member_type(var_types[lhs.var_i], lhs.member);
                int const shift = lhs.atom - frac_bytes(mt.name());
                assert(shift >= 0);

                fixed_uint_t mask = numeric_bitmask(rhs.type.name());
                fixed_uint_t replace = rhs.rval[i].u();
                assert((replace & mask) == replace);

                if(shift < 0)
                {
                    mask >>= (-shift * 8);
                    replace >>= (-shift * 8);
                }
                else
                {
                    mask <<= (shift * 8);
                    replace <<= (shift * 8);
                }

                fixed_uint_t u = local[lhs.member].u();
                u &= ~mask;
                u |= replace;
                local[lhs.member] = ssa_value_t(u, mt.name());
                */
            }
            else
            {
                for(unsigned i = 0; i < rval.size(); ++i)
                    local[i + lval->member] = rval[i];
            }
        }
    }
    else if(is_compile(D))
    {
        ssa_value_array_t& local = builder.cfg.data<block_d>().var(lval->var_i());
        rval_t& rval = rhs.rval();

        if(lval->index)
        {
            for(unsigned i = 0; i < rval.size(); ++i)
            {
                //ssa_ht read = lhs.ssa(i).handle();
                //assert(read->op() == SSA_read_array);

                type_t const mt = member_type(var_type(lval->var_i()), lval->member + i);
                assert(mt.name() == TYPE_TEA);

                passert(rhs.type.name() != TYPE_TEA, rhs.type);
                //passert(rhs.type == mt.elem_type(), rhs.type, mt.elem_type());

                //type_t const type = type_t::tea(rhs.type, mt.size(), rhs.pstring);
                //assert(type.name() == TYPE_TEA);

                ssa_value_t const prev_array = var_lookup(builder.cfg, lval->var_i(), lval->member + i);

                ssa_ht write = builder.cfg->emplace_ssa(
                    (lval->flags & LVALF_INDEX_16) ? SSA_write_array16 : SSA_write_array8, mt,
                    prev_array, ssa_value_t(0u, TYPE_U20), lval->index, std::get<ssa_value_t>(rval[i]));

                local[i + lval->member] = write;
            }
        }
        else
        {
            for(unsigned i = 0; i < rval.size(); ++i)
                local[i + lval->member] = from_variant<D>(rval[i], member_type(rhs.type, i + lval->member));
        }
    }

finish:
    return rhs;
}

void eval_t::req_quantity(token_t const& token, expr_value_t const& value)
{
    if(!is_quantity(value.type.name()))
    {
        compiler_error(value.pstring, fmt("% expects arithmetic quantity inputs. (Operand is %)", 
                                          token_string(token.type), value.type));
    }
}
    
void eval_t::req_quantity(token_t const& token, expr_value_t const& lhs, expr_value_t const& rhs)
{
    if(!is_quantity(lhs.type.name()) || !is_quantity(rhs.type.name()))
    {
        pstring_t pstring = concat(lhs.pstring, rhs.pstring);
        compiler_error(pstring, fmt("% expects arithmetic quantity inputs. (Operands are % and %)", 
                                    token_string(token.type), lhs.type, rhs.type));
    }
}

expr_value_t eval_t::compile_binary_operator(
    expr_value_t const& lhs, expr_value_t const& rhs, 
    ssa_op_t op, type_t result_type, bool carry)
{
    ssa_value_t result;
    if(carry)
        result = builder.cfg->emplace_ssa(
            op, result_type, lhs.ssa(), rhs.ssa(), ssa_value_t(op == SSA_sub, TYPE_BOOL));
    else
        result = builder.cfg->emplace_ssa(op, result_type, lhs.ssa(), rhs.ssa());

    return
    {
        .val = rval_t{ result },
        .type = result_type, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };
}

template<typename Policy>
expr_value_t eval_t::do_compare(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    req_quantity(token, lhs, rhs);

    expr_value_t result =
    {
        .type = TYPE_BOOL, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };

    if(!is_scalar(lhs.type.name()) || !is_scalar(rhs.type.name()))
    {
        compiler_error(
            concat(lhs.pstring, rhs.pstring), 
            fmt("% isn't defined for this type combination. (% and %)",
                token_string(token.type), lhs.type, rhs.type));
    }

    if(lhs.type != rhs.type)
    {
        if(is_ct(lhs.type) && can_cast(lhs.type, rhs.type, true))
            lhs = throwing_cast<Policy::D>(std::move(lhs), rhs.type, true);
        else if(is_ct(rhs.type) && can_cast(rhs.type, lhs.type, true))
            rhs = throwing_cast<Policy::D>(std::move(rhs), lhs.type, true);
    }

    if(locator_t loc = handle_lt<Policy::D>(result.type, { .type = Policy::lt(), .pstring = result.pstring }, lhs, rhs))
    {
        result.val = _lt_rval(result.type, loc);
        result.time = LT;
    }
    else if(is_interpret(Policy::D) || (Policy::D == COMPILE && lhs.is_ct() && rhs.is_ct()))
        result.val = rval_t{ ssa_value_t(Policy::interpret(lhs.s(), rhs.s(), result.pstring), TYPE_BOOL) };
    else if(is_compile(Policy::D))
    {
        // The implementation is kept simpler if both types being compared have the same size.
        if((Policy::op() == SSA_eq || Policy::op() == SSA_not_eq) 
           && !same_scalar_layout(lhs.type.name(), rhs.type.name()))
        {
            unsigned const w = std::max(whole_bytes(lhs.type.name()), whole_bytes(rhs.type.name()));
            unsigned const f = std::max(frac_bytes(lhs.type.name()), frac_bytes(rhs.type.name()));

            lhs = throwing_cast<Policy::D>(std::move(lhs), type_s_or_u(w, f, is_signed(lhs.type.name())), true, lhs.pstring);
            rhs = throwing_cast<Policy::D>(std::move(rhs), type_s_or_u(w, f, is_signed(rhs.type.name())), true, rhs.pstring);

            assert(same_scalar_layout(lhs.type.name(), rhs.type.name()));
        }

        return compile_binary_operator(lhs, rhs, Policy::op(), TYPE_BOOL);
    }

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_arith(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    assert(lhs.is_rval() && rhs.is_rval());

    ssa_op_t const op = Policy::op();
    bool const summable = op == SSA_add || op == SSA_sub;

    if(summable)
    {
        if(!is_summable(lhs.type.name()) || !is_summable(rhs.type.name()))
        {
        invalid_input:
            pstring_t pstring = concat(lhs.pstring, rhs.pstring);
            compiler_error(pstring, fmt("Operator % is not defined for these types. (Operands are % and %)", 
                                        token_string(token.type), lhs.type, rhs.type));
        }
    }
    else
        req_quantity(token, lhs, rhs);

    expr_value_t result = { .pstring = concat(lhs.pstring, rhs.pstring) };

    if(lhs.type != rhs.type)
    {
        if(is_ct(lhs.type) && can_cast(lhs.type, rhs.type, true))
        {
            result.type = rhs.type;
            lhs = throwing_cast<Policy::D>(std::move(lhs), result.type, true);
        }
        else if(is_ct(rhs.type) && can_cast(rhs.type, lhs.type, true))
        {
            result.type = lhs.type;
            rhs = throwing_cast<Policy::D>(std::move(rhs), result.type, true);
        }
        else
            goto invalid_input;
    }
    else
        result.type = lhs.type;

    assert(is_arithmetic(result.type.name()));
    assert(lhs.type == result.type);
    assert(rhs.type == result.type);

    if(locator_t loc = handle_lt<Policy::D>(result.type, { .type = Policy::lt(), .pstring = result.pstring }, lhs, rhs))
    {
        result.val = _lt_rval(result.type, loc);
        result.time = LT;
    }
    else if(is_interpret(Policy::D) || (is_compile(Policy::D) && lhs.is_ct() && rhs.is_ct()))
    {
        assert(is_masked(lhs.fixed(), lhs.type.name()));
        assert(is_masked(rhs.fixed(), rhs.type.name()));

        fixed_t f = { Policy::interpret(lhs.s(), rhs.s(), result.pstring) };
        f.value &= numeric_bitmask(result.type.name());
        result.val = rval_t{ ssa_value_t(f, result.type.name()) };
    }
    else if(is_compile(Policy::D))
    {
        if(!Policy::op())
            compiler_error(result.pstring, "Cannot perform division at run-time.");

        return compile_binary_operator(lhs, rhs, Policy::op(), result.type, ssa_argn(Policy::op()) > 2);
    }

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_assign_arith(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    rhs = throwing_cast<Policy::D>(std::move(rhs), lhs.type, true);
    expr_value_t lhs_copy = to_rval<Policy::D>(lhs);
    return do_assign<Policy::D>(std::move(lhs), do_arith<Policy>(std::move(lhs_copy), rhs, token), token);
}

static locator_t _loc_ptr(rval_t const& rval)
{
    if(rval.size() < 1)
        return {};
    if(ssa_value_t const* ssa = std::get_if<ssa_value_t>(&rval[0]))
        if(ssa->is_locator() && is_ptr(ssa->locator().type().name()))
            return ssa->locator();
    return {};
}

template<typename Policy>
expr_value_t eval_t::do_add(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    assert(lhs.is_rval() && rhs.is_rval());

    ssa_op_t const op = Policy::op();

    if(!is_summable(lhs.type.name()) || !is_summable(rhs.type.name()))
    {
    invalid_input:
        pstring_t pstring = concat(lhs.pstring, rhs.pstring);
        compiler_error(pstring, fmt("Operator % is not defined for these types. (Operands are % and %)", 
                                    token_string(token.type), lhs.type, rhs.type));
    }

    expr_value_t result = { .pstring = concat(lhs.pstring, rhs.pstring) };

    bool const lptr = is_ptr(lhs.type.name());
    bool const rptr = is_ptr(rhs.type.name());
    
    if(lptr)
    {
        if(rptr)
        {
            if(op == SSA_sub)
            {
                result.type = TYPE_U20;

                if(!is_check(Policy::D))
                {
                    if(lhs.is_ct() && rhs.is_ct())
                    {
                    interpret:
                        auto const l = lhs.ssa(0).signed_fixed();
                        auto const r = rhs.ssa(0).signed_fixed();

                        fixed_t f = { Policy::interpret(l, r, result.pstring) };
                        f.value &= numeric_bitmask(TYPE_U20);
                        result.val = rval_t{ ssa_value_t(f, result.type.name()) };
                    }
                    else if(is_link(Policy::D))
                        compiler_error(result.pstring, "Unable to link.");
                    else
                    {
                        // If both are locators with the same handle,
                        // we can calculate this at CT
                        locator_t const l = _loc_ptr(lhs.rval());
                        locator_t const r = _loc_ptr(rhs.rval());

                        if(l && r && l.with_offset(0) == r.with_offset(0))
                        {
                            std::uint16_t const diff = l.offset() - r.offset();
                            result.val = rval_t{ ssa_value_t(diff, TYPE_U20) };
                        }
                        else
                        {
                            if(is_interpret(Policy::D) || (is_compile(Policy::D) && !lhs.is_rt() && !rhs.is_rt()))
                            {
                                // Create a link-time expression.
                                locator_t const lt = eval_t::make_lt(
                                    TYPE_U20, token_t{ .type = Policy::lt(), .pstring = result.pstring }, lhs, rhs);
                                result.val = _lt_rval(TYPE_U20, lt);
                                result.time = LT;
                            }
                            else if(is_compile(Policy::D))
                            {
                                return compile_binary_operator(
                                    throwing_cast<Policy::D>(std::move(lhs), result.type, false),
                                    throwing_cast<Policy::D>(std::move(rhs), result.type, false),
                                    Policy::op(), result.type, ssa_argn(Policy::op()) > 2);
                            }
                        }
                    }
                }
            }
            else
                goto invalid_input;
        }
        else
        {
        ptr_int:
            result.type = lhs.type;
            assert(is_ptr(result.type.name()));
            assert(!is_ptr(rhs.type.name()));

            bool const banked = is_banked_ptr(result.type.name());

            rhs = throwing_cast<Policy::D>(std::move(rhs), TYPE_U20, true);

            if(!is_check(Policy::D))
            {
                if(lhs.is_ct() && rhs.is_ct())
                    goto interpret;
                else if(is_link(Policy::D))
                    compiler_error(result.pstring, "Unable to link.");
                else
                {
                    locator_t const l = _loc_ptr(lhs.rval());

                    if(l && rhs.is_ct())
                    {
                        locator_t new_l = l;
                        if(op == SSA_sub)
                            new_l.advance_offset(-rhs.whole());
                        else
                            new_l.advance_offset(rhs.whole());

                        result.val = rval_t{ new_l };
                        if(banked)
                            result.rval().push_back(new_l.with_is(IS_BANK));
                    }
                    else
                    {
                        if(is_interpret(Policy::D) || (is_compile(Policy::D) && !lhs.is_rt() && !rhs.is_rt()))
                        {
                            // Create a link-time expression.
                            locator_t const lt = eval_t::make_lt(
                                result.type, token_t{ .type = Policy::lt(), .pstring = result.pstring }, lhs, rhs);
                            result.val = _lt_rval(result.type, lt);
                            result.time = LT;
                        }
                        else if(is_compile(Policy::D))
                        {
                            ct_variant_t bank;
                            if(banked)
                            {
                                assert(lhs.rval().size() == 2);
                                bank = lhs.rval()[1];
                            }

                            lhs = throwing_cast<Policy::D>(std::move(lhs), TYPE_U20, false);
                            rhs = throwing_cast<Policy::D>(std::move(rhs), TYPE_U20, false);

                            ssa_ht const sum = builder.cfg->emplace_ssa(
                                Policy::op(), TYPE_U20, 
                                lhs.ssa(0), rhs.ssa(0), 
                                ssa_value_t(Policy::op() == SSA_sub, TYPE_BOOL));

                            ssa_ht const cast = builder.cfg->emplace_ssa(
                                SSA_cast, result.type.with_banked(false), sum);

                            if(banked)
                                result.val = rval_t{ cast, bank };
                            else
                                result.val = rval_t{ cast };
                        }
                    }
                }
            }
        }
    }
    else if(rptr)
    {
        if(op != SSA_add)
            goto invalid_input;

        std::swap(lhs, rhs);
        goto ptr_int;
    }
    else
        return do_arith<Policy>(std::move(lhs), std::move(rhs), token);

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_assign_add(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    expr_value_t result =
    {
        .type = TYPE_BOOL, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };

    if(is_ptr(lhs.type.name()))
        rhs = throwing_cast<Policy::D>(std::move(rhs), TYPE_U20, true);
    else
        rhs = throwing_cast<Policy::D>(std::move(rhs), lhs.type, true);
    
    expr_value_t lhs_rval = to_rval<Policy::D>(lhs);
    expr_value_t add = do_add<Policy>(lhs_rval, rhs, token);

    if(is_link(Policy::D))
        goto interpret_carry;

    if(is_interpret(Policy::D) || (is_compile(Policy::D) && lhs_rval.is_ct() && rhs.is_ct()))
    {
        if(add.is_lt())
        {
            locator_t const lt = eval_t::make_lt(
                TYPE_BOOL, token_t{ .type = Policy::lt_assign(), .pstring = result.pstring }, lhs_rval, rhs);
            result.val = _lt_rval(TYPE_BOOL, lt);
            result.time = LT;
        }
        else
        {
        interpret_carry:
            assert(is_masked(lhs_rval.fixed(), lhs.type.name()));
            assert(is_masked(rhs.fixed(), rhs.type.name()));

            bool b = Policy::interpret_carry(lhs_rval.s(), rhs.s(), numeric_bitmask(add.type.name()), result.pstring);
            result.val = rval_t{ ssa_value_t(b, TYPE_BOOL) };
        }
    }
    else if(is_compile(Policy::D))
        result.val = rval_t{ builder.cfg->emplace_ssa(SSA_carry, TYPE_BOOL, add.ssa()) };

    if(!is_link(Policy::D))
        do_assign<Policy::D>(std::move(lhs), std::move(add), token);

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_shift(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    req_quantity(token, lhs, rhs);

    if(rhs.type.name() == TYPE_INT)
        rhs = throwing_cast<Policy::D>(std::move(rhs), { TYPE_U }, true);
    else if(rhs.type.name() != TYPE_U)
        compiler_error(rhs.pstring, fmt("Ride-hand side of operator % must be type U or Int.", 
                                        token_string(token.type)));

    type_t const result_type = lhs.type;
    assert(is_arithmetic(result_type.name()));

    expr_value_t result =
    {
        .type = result_type, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };

    if(locator_t loc = handle_lt<Policy::D>(result.type, { .type = Policy::lt(), .pstring = result.pstring }, lhs, rhs))
    {
        result.val = _lt_rval(result.type, loc);
        result.time = LT;
    }
    else if(is_interpret(Policy::D) || (is_compile(Policy::D) && lhs.is_ct() && rhs.is_ct()))
    {
        assert(is_masked(to_rval<Policy::D>(lhs).fixed(), lhs.type.name()));
        assert(is_masked(rhs.fixed(), rhs.type.name()));

        fixed_t f = { Policy::interpret(lhs.s(), rhs.whole(), result.pstring) };
        f.value &= numeric_bitmask(result_type.name());
        result.val = rval_t{ ssa_value_t(f, result_type.name()) };
    }
    else if(is_compile(Policy::D))
    {
        if(is_ct(result_type.name()))
        {
            throw compiler_error_t(
                fmt_error(lhs.pstring, fmt("Cannot shift expression of type % at run-time.", result_type))
                + fmt_note("Addding an explicit cast will fix."));
        }
        return compile_binary_operator(std::move(lhs), std::move(rhs), Policy::op(), result_type);
    }

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_assign_shift(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    expr_value_t result =
    {
        .type = TYPE_BOOL, 
        .pstring = concat(lhs.pstring, rhs.pstring)
    };

    expr_value_t lhs_rval = to_rval<Policy::D>(lhs);
    expr_value_t shift = do_shift<Policy>(lhs_rval, rhs, token);

    if(is_link(Policy::D))
        goto interpret_carry;

    if(is_interpret(Policy::D) || (is_compile(Policy::D) && lhs_rval.is_ct() && rhs.is_ct()))
    {
        if(shift.is_lt())
        {
            locator_t const lt = eval_t::make_lt(
                TYPE_BOOL, token_t{ .type = Policy::lt_assign(), .pstring = result.pstring }, lhs_rval, rhs);
            result.val = _lt_rval(TYPE_BOOL, lt);
            result.time = LT;
        }
        else
        {
        interpret_carry:
            assert(is_masked(lhs_rval.fixed(), lhs.type.name()));
            assert(is_masked(rhs.fixed(), rhs.type.name()));

            bool b = Policy::interpret_carry(lhs_rval.s(), rhs.whole(), numeric_bitmask(shift.type.name()), result.pstring);
            result.val = rval_t{ ssa_value_t(b, TYPE_BOOL) };
        }
    }
    else if(is_compile(Policy::D))
        result.val = rval_t{ builder.cfg->emplace_ssa(SSA_carry, TYPE_BOOL, shift.ssa()) };

    if(!is_link(Policy::D))
        do_assign<Policy::D>(std::move(lhs), std::move(shift), token);

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_rotate(expr_value_t operand, expr_value_t carry, token_t const& token)
{
    req_quantity(token, operand);

    if(carry.type.name() != TYPE_BOOL)
        compiler_error(carry.pstring, fmt("Ride-hand side of operator % must be type Bool.", 
                                          token_string(token.type)));

    type_t const result_type = operand.type;
    assert(is_arithmetic(result_type.name()));

    expr_value_t result =
    {
        .type = result_type, 
        .pstring = concat(operand.pstring, carry.pstring)
    };

    if(locator_t loc = handle_lt<Policy::D>(result.type, { .type = Policy::lt(), .pstring = result.pstring }, operand, carry))
    {
        result.val = _lt_rval(result.type, loc);
        result.time = LT;
    }
    else if(is_interpret(Policy::D) || (is_compile(Policy::D) && operand.is_ct() && carry.is_ct()))
    {
        assert(is_masked(operand.fixed(), operand.type.name()));
        assert(is_masked(carry.fixed(), carry.type.name()));

        fixed_t f = { Policy::interpret(operand.u(), carry.whole(), numeric_bitmask(result_type.name()), result.pstring) };
        f.value &= numeric_bitmask(result_type.name());
        result.val = rval_t{ ssa_value_t(f, result_type.name()) };
    }
    else if(is_compile(Policy::D))
    {
        if(is_ct(result_type.name()))
        {
            throw compiler_error_t(
                fmt_error(operand.pstring, fmt("Cannot rotate expression of type % at run-time.", result_type))
                + fmt_note("Addding an explicit cast will fix."));
        }
        return compile_binary_operator(std::move(operand), std::move(carry), Policy::op(), result_type);
    }

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_assign_rotate(expr_value_t operand, expr_value_t carry, token_t const& token)
{
    expr_value_t result =
    {
        .type = TYPE_BOOL, 
        .pstring = concat(operand.pstring, carry.pstring)
    };

    expr_value_t operand_rval = to_rval<Policy::D>(operand);
    expr_value_t rotate = do_rotate<Policy>(operand_rval, carry, token);

    if(is_link(Policy::D))
        goto interpret_carry;

    if(is_interpret(Policy::D) || (is_compile(Policy::D) && operand_rval.is_ct() && carry.is_ct()))
    {
        if(rotate.is_lt())
        {
            locator_t const lt = eval_t::make_lt(
                TYPE_BOOL, token_t{ .type = Policy::lt_assign(), .pstring = result.pstring }, operand_rval, carry);
            result.val = _lt_rval(TYPE_BOOL, lt);
            result.time = LT;
        }
        else
        {
        interpret_carry:
            assert(is_masked(operand_rval.fixed(), operand_rval.type.name()));
            assert(is_masked(carry.fixed(), carry.type.name()));

            bool b = Policy::interpret_carry(operand_rval.u(), numeric_bitmask(rotate.type.name()), result.pstring);
            result.val = rval_t{ ssa_value_t(b, TYPE_BOOL) };
        }
    }
    else if(is_compile(Policy::D))
        result.val = rval_t{ builder.cfg->emplace_ssa(SSA_carry, TYPE_BOOL, rotate.ssa()) };

    if(!is_link(Policy::D))
        do_assign<Policy::D>(std::move(operand), std::move(rotate), token);

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_mul(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    req_quantity(token, lhs, rhs);

    expr_value_t result = { .pstring = concat(lhs.pstring, rhs.pstring) };

    if(is_ct(lhs.type) && is_ct(rhs.type))
    {
        if(can_cast(lhs.type, rhs.type, true))
        {
            result.type = rhs.type;
            lhs = throwing_cast<Policy::D>(std::move(lhs), result.type, true);
        }
        else
        {
            assert(can_cast(rhs.type, lhs.type, true));
            result.type = lhs.type;
            rhs = throwing_cast<Policy::D>(std::move(rhs), result.type, true);
        }
    }
    else
    {
        // Multiplications result in larger types:

        unsigned const lhs_whole = whole_bytes(lhs.type.name());
        unsigned const lhs_frac  =  frac_bytes(lhs.type.name());

        unsigned const rhs_whole = whole_bytes(rhs.type.name());
        unsigned const rhs_frac  =  frac_bytes(rhs.type.name());

        unsigned const result_whole = std::min<unsigned>(lhs_whole + rhs_whole, max_rt_whole_bytes);
        unsigned const result_frac  = std::min<unsigned>(lhs_frac  + rhs_frac,  max_rt_frac_bytes);

        bool const result_sign = is_signed(lhs.type.name()) || is_signed(rhs.type.name());

        result.type = type_s_or_u(result_whole, result_frac, result_sign);
        passert(is_arithmetic(result.type.name()), result_whole, result_frac, result_sign);

        if(is_ct(lhs.type))
            lhs = throwing_cast<Policy::D>(std::move(lhs), result.type, true);
        if(is_ct(rhs.type))
            rhs = throwing_cast<Policy::D>(std::move(rhs), result.type, true);
    }

    if(locator_t loc = handle_lt<Policy::D>(result.type, { .type = Policy::lt(), .pstring = result.pstring }, lhs, rhs))
    {
        result.val = _lt_rval(result.type, loc);
        result.time = LT;
    }
    else if(is_interpret(Policy::D) || (is_compile(Policy::D) && lhs.is_ct() && rhs.is_ct()))
    {
        assert(is_masked(lhs.fixed(), lhs.type.name()));
        assert(is_masked(rhs.fixed(), rhs.type.name()));

        fixed_t f = { Policy::interpret(lhs.s(), rhs.s(), result.pstring) };
        f.value &= numeric_bitmask(result.type.name());
        result.val = rval_t{ ssa_value_t(f, result.type.name()) };
    }
    else if(is_compile(Policy::D))
        return compile_binary_operator(std::move(lhs), std::move(rhs), Policy::op(), result.type);

    return result;
}

template<typename Policy>
expr_value_t eval_t::do_assign_mul(expr_value_t lhs, expr_value_t rhs, token_t const& token)
{
    return do_assign<Policy::D>(std::move(lhs), throwing_cast<Policy::D>(do_mul<Policy>(to_rval<Policy::D>(lhs), rhs, token), lhs.type, false), token);
}

template<eval_t::do_t D>
expr_value_t eval_t::do_logical(ast_node_t const& ast)
{
    assert(ast.token.type == TOK_logical_or || ast.token.type == TOK_logical_and);

    expr_value_t const lhs = throwing_cast<D>(do_expr<D>(ast.children[0]), TYPE_BOOL, true);
    bool const is_or = ast.token.type == TOK_logical_or;

    if(is_interpret(D))
    {
        if(lhs.is_lt())
        {
        lt:
            // This is unimplemented for now.
            // TODO: implement properly.
            throw compiler_error_t(
                fmt_error(ast.token.pstring, "Unable to interpret at compile-time. Conditional expression depends on link-time value.")
                + fmt_note("You can use a bitwise operation instead."));
        }
        if(bool(lhs.fixed()) == is_or)
            return lhs;
        expr_value_t const rhs = throwing_cast<D>(do_expr<D>(ast.children[1]), TYPE_BOOL, true);

        if(rhs.is_lt())
            goto lt;

        return lhs;
    }
    else if(is_compile(D))
    {
        cfg_ht const branch_node = builder.cfg;
        cfg_exits_with_branch(lhs.ssa());

        cfg_ht const long_cut = builder.cfg = insert_cfg(true);
        branch_node->build_set_output(!is_or, long_cut);

        expr_value_t const rhs = throwing_cast<D>(do_expr<D>(ast.children[1]), TYPE_BOOL, true);

        cfg_ht const merge_node = insert_cfg(true);
        cfg_exits_with_jump();
        builder.cfg->build_set_output(0, merge_node);
        branch_node->build_set_output(is_or, merge_node);
        builder.cfg = merge_node;

        expr_value_t result =
        {
            .val = rval_t{ merge_node->emplace_ssa(
                SSA_phi, TYPE_BOOL, rhs.ssa(), ssa_value_t(is_or, TYPE_BOOL)) },
            .type = TYPE_BOOL,
            .pstring = concat(lhs.pstring, rhs.pstring),
        };

        return result;
    }

    expr_value_t const rhs = throwing_cast<D>(do_expr<D>(ast.children[1]), TYPE_BOOL, true);
    return { .type = TYPE_BOOL, .pstring = concat(lhs.pstring, rhs.pstring) };
}

template<eval_t::do_t D>
expr_value_t eval_t::force_truncate(expr_value_t value, type_t to_type, pstring_t cast_pstring)
{
    assert(!is_ct(value.type));
    assert(!is_ct(to_type));
    assert(is_arithmetic(to_type.name()) && is_arithmetic(value.type.name()));

    value = to_rval<D>(std::move(value));

    expr_value_t result =
    {
        .type = to_type, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
    };

    if(is_interpret(D) || (is_compile(D) && value.is_ct()))
        result.val = rval_t{ ssa_value_t(mask_numeric(value.fixed(), to_type.name()), to_type.name()) };
    else if(is_compile(D))
        result.val = rval_t{ builder.cfg->emplace_ssa(SSA_cast, to_type, value.ssa()) };

    return result;
}

template<eval_t::do_t D>
expr_value_t eval_t::force_promote(expr_value_t value, type_t to_type, pstring_t cast_pstring)
{
    assert(!is_ct(value.type));
    assert(is_arithmetic(to_type.name()) && is_arithmetic(value.type.name()));

    value = to_rval<D>(std::move(value));

    expr_value_t result =
    {
        .type = to_type, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
    };

    if(is_interpret(D) || (is_compile(D) && value.is_ct()))
        result.val = rval_t{ ssa_value_t(mask_numeric({ value.s() }, to_type.name()), to_type.name()) };
    else if(is_compile(D))
    {
        if(is_ct(to_type))
            compiler_error(value.pstring, fmt("Cannot promote type % to type % at run-time.", value.type, to_type));
        result.val = rval_t{ builder.cfg->emplace_ssa(SSA_cast, to_type, value.ssa()) };
    }

    return result;
}

template<eval_t::do_t D>
expr_value_t eval_t::force_intify_ptr(expr_value_t value, type_t to_type, pstring_t cast_pstring)
{
    assert(!is_ct(value.type));
    assert(!is_ct(to_type));
    assert(is_arithmetic(to_type.name()) && is_ptr(value.type.name()));

    bool const is_banked = is_banked_ptr(value.type.name());

    value = to_rval<D>(std::move(value));

    expr_value_t result =
    {
        .type = to_type, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
    };

    if(is_interpret(D))
    {
        assert(value.ssa(0).is_num());
        fixed_t f = value.ssa(0).fixed();

        if(is_banked)
        {
            passert(value.rval().size() == 2, value.rval().size());
            assert(value.ssa(1).is_num());
            f.value |= (value.ssa(1).fixed().value & numeric_bitmask(TYPE_U)) << 16;
        }

        if(value.is_ct())
            result.val = rval_t{ ssa_value_t(mask_numeric(f, to_type.name()), to_type.name()) };
        else
            compiler_error(cast_pstring, fmt("Unable to convert to type %.", to_type));
    }
    else if(is_compile(D))
    {
        ssa_value_t cast = builder.cfg->emplace_ssa(SSA_cast, is_banked ? TYPE_U30 : TYPE_U20, value.ssa(0));

        if(is_banked)
            cast = builder.cfg->emplace_ssa(SSA_replace_byte, TYPE_U30, cast, ssa_value_t(2, TYPE_U), value.ssa(1));

        result.val = rval_t{ cast };
    }

    return result;
}

template<eval_t::do_t D>
expr_value_t eval_t::force_ptrify_int(expr_value_t value, type_t to_type, pstring_t cast_pstring)
{
    //passert(!is_ct(value.type), value.type);
    passert(!is_ct(to_type), to_type);
    assert(is_ptr(to_type.name()) && is_arithmetic(value.type.name()));

    bool const is_banked = is_banked_ptr(to_type.name());
    type_t const unbanked_type = to_type.with_banked(false);

    value = throwing_cast<D>(std::move(value), is_banked ? TYPE_U30 : TYPE_U20, true, pstring);

    expr_value_t result =
    {
        .type = to_type, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
    };

    if(is_interpret(D))
    {
        rval_t rval;

        fixed_t f = value.fixed();

        rval.push_back(ssa_value_t(fixed_t{ f.value & numeric_bitmask(TYPE_U20) }, unbanked_type.name()));

        if(is_banked)
            rval.push_back(ssa_value_t(fixed_t{ (f.value >> 16) & numeric_bitmask(TYPE_U) }, TYPE_U));

        result.val = std::move(rval);
    }
    else if(is_compile(D))
    {
        rval_t rval;

        rval.push_back(builder.cfg->emplace_ssa(SSA_cast, unbanked_type, value.ssa()));

        if(is_banked_ptr(to_type.name()))
            rval.push_back(builder.cfg->emplace_ssa(SSA_get_byte, TYPE_U, value.ssa(), ssa_value_t(2, TYPE_U)));

        result.val = std::move(rval);
    }

    return result;
}

template<eval_t::do_t D>
expr_value_t eval_t::force_convert_int(expr_value_t value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    assert(value.type.name() == TYPE_INT);
    assert(is_arithmetic(to_type.name()) && is_arithmetic(value.type.name()));

    value = to_rval<D>(std::move(value));

    expr_value_t result =
    {
        .type = to_type, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
    };

    if(!is_check(D))
    {
        fixed_t const masked = mask_numeric(value.fixed(), to_type.name());

        if(implicit && to_signed(masked.value, to_type.name()) != value.s())
        {
            throw compiler_error_t(
                fmt_error(result.pstring, fmt(
                    "Int value of % cannot be represented in type %. (Implicit type conversion)", 
                    to_double(fixed_t{ value.s() }), to_type))
                + fmt_note("Add an explicit cast operator to override.")
                );
        }

        result.val = rval_t{ ssa_value_t(masked, to_type.name()) };
    }

    return result;
}

template<eval_t::do_t D>
expr_value_t eval_t::force_round_real(expr_value_t value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    assert(value.type.name() == TYPE_REAL);
    assert(is_arithmetic(to_type.name()));

    value = to_rval<D>(std::move(value));

    expr_value_t result =
    {
        .type = to_type, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
    };

    if(!is_check(D))
    {
        fixed_sint_t const original = to_signed(value.u(), TYPE_REAL);
        fixed_uint_t u = value.u();
        fixed_uint_t const mask = numeric_bitmask(to_type.name());
        if(fixed_uint_t z = builtin::ctz(mask))
            u += (1ull << (z - 1)) & u;
        u &= mask;

        if(implicit)
        {
            fixed_uint_t const supermask = ::supermask(numeric_bitmask(to_type.name()));
            if(static_cast<fixed_sint_t>(original & supermask) != to_signed(original & mask, to_type.name()))
            {
                throw compiler_error_t(
                    fmt_error(value.pstring, fmt(
                        "Num value of % doesn't fit in type %. (Implicit type conversion)", 
                        to_double(fixed_t{original}), to_type))
                    + fmt_note("Add an explicit cast operator to override.")
                    );
            }
        }

        assert(is_masked({u}, to_type.name()));

        result.val = rval_t{ ssa_value_t(fixed_t{ u }, to_type.name()) };
        assert(result.u() == u);
    }

    return result;
}


// This is used to implement the other cast functions.
template<eval_t::do_t D>
expr_value_t eval_t::force_boolify(expr_value_t value, pstring_t cast_pstring)
{
    value = to_rval<D>(std::move(value));

    expr_value_t result =
    {
        .type = TYPE_BOOL, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
        .time = is_compile(D) ? RT : CT, // TODO: what is this for?
    };

    if(is_interpret(D) || (is_compile(D) && value.is_ct()))
    {
        if(is_arithmetic(value.type.name()))
            result.val = rval_t{ ssa_value_t(boolify(value.fixed()), TYPE_BOOL) };
    }
    else if(is_compile(D))
    {
        result.val = rval_t{ builder.cfg->emplace_ssa(
            SSA_not_eq, TYPE_BOOL, value.ssa(), ssa_value_t(0u, value.type.name())) };
    }

    return result;
}

template<eval_t::do_t D>
expr_value_t eval_t::force_resize_tea(expr_value_t value, type_t to_type, pstring_t cast_pstring)
{
    assert(is_tea(value.type.name()));
    assert(is_tea(to_type.name()));
    assert(value.type.elem_type() == to_type.elem_type());

    value = to_rval<D>(std::move(value));
    rval_t& rval = value.rval();

    type_t const elem_type = to_type.elem_type();
    unsigned const from_size = value.type.size();
    unsigned const to_size = to_type.size();

    expr_value_t result =
    {
        .type = to_type, 
        .pstring = cast_pstring ? concat(value.pstring, cast_pstring) : value.pstring,
    };

    if(from_size == to_size)
        result.val = std::move(value.val);
    else if(is_interpret(D) || (is_compile(D) && value.is_ct()))
    {
        if(to_size > from_size)
        {
            for(unsigned m = 0; m < rval.size(); ++m)
            {
                ct_array_t const& from = std::get<ct_array_t>(rval[m]);
                ct_array_t to = make_ct_array(to_type.size());
                std::copy(from.get(), from.get() + from_size, to.get());

                // Zero-init the rest:
                ssa_value_t fill(0u, ::member_type(elem_type, m).name());
                for(unsigned i = from_size; i < to_size; ++i)
                    to[i] = fill;

                rval[m] = std::move(to);
            }
        }

        result.val = std::move(rval);
    }
    else if(is_compile(D))
    {
        for(unsigned m = 0; m < rval.size(); ++m)
        {
            type_t const mt = ::member_type(value.type, m);
            assert(is_tea(mt.name()));

            rval[m] = builder.cfg->emplace_ssa(
                SSA_resize_array, type_t::tea(mt.elem_type(), to_size),
                from_variant<D>(rval[m], mt));
        }

        result.val = std::move(rval);
    }

    return result;
}

template<eval_t::do_t D>
bool eval_t::cast(expr_value_t& value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    auto const can = can_cast(value.type, to_type, implicit);

    if(can == CAST_FAIL)
        return false;
    else if(can == CAST_NOP)
    {
        value.type = to_type;
        value = to_rval<D>(std::move(value));
        return true;
    }

    if(!is_check(D) && !is_link(D) && value.is_lt())
    {
        if(!cast_pstring)
            cast_pstring = value.pstring;

        ast_node_t* children = eternal_new<ast_node_t>(2);

        children[0].token = { token_t::make_ptr(TOK_cast_type, cast_pstring, type_t::new_type(to_type)) };
        children[1] = { _make_token(value) };

        ast_node_t new_ast = { 
            .token = { 
                .type = implicit ? TOK_implicit_cast : TOK_cast,
                .pstring = cast_pstring,
                .value = 2,
            }, 
            .children = children 
        };

        assert(new_ast.num_children() == 2);

        locator_t const loc =locator_t::lt_expr(alloc_lt_value(to_type, std::move(new_ast)));

        value.val = _lt_rval(to_type, loc);
        value.type = to_type;
        value.time = LT;

        return true;
    }

    switch(can)
    {
    default: 
        assert(false);
        return false;
    case CAST_NOP_RETYPE:
        value.type = to_type;
        value = to_rval<D>(std::move(value));
        {
            type_t t = to_type;
            if(is_banked_ptr(to_type.name()))
               t.set_banked(false);
            assert(!is_banked_ptr(t.name()));

            if(is_interpret(D) || (is_compile(D) && value.is_ct()))
                value.rval()[0] = ssa_value_t(value.ssa(0).fixed(), t.name());
            else if(is_compile(D))
                value.rval()[0] = builder.cfg->emplace_ssa(SSA_cast, t, value.ssa(0));
        }
        return true;
    case CAST_PROMOTE:
        value = force_promote<D>(std::move(value), to_type, cast_pstring);
        return true;
    case CAST_TRUNCATE:
        value = force_truncate<D>(std::move(value), to_type, cast_pstring);
        return true;
    case CAST_BOOLIFY:
        value = force_boolify<D>(std::move(value), cast_pstring);
        return true;
    case CAST_CONVERT_INT:
        value = force_convert_int<D>(std::move(value), to_type, implicit, cast_pstring);
        return true;
    case CAST_ROUND_REAL:
        value = force_round_real<D>(std::move(value), to_type, implicit, cast_pstring);
        return true;
    case CAST_INTIFY_PTR:
        value = force_intify_ptr<D>(std::move(value), to_type, cast_pstring);
        return true;
    case CAST_PTRIFY_INT:
        value = force_ptrify_int<D>(std::move(value), to_type, cast_pstring);
        return true;
    case CAST_RESIZE_TEA:
        value = force_resize_tea<D>(std::move(value), to_type, cast_pstring);
        return true;
    }
}

template<eval_t::do_t D>
expr_value_t eval_t::throwing_cast(expr_value_t value, type_t to_type, bool implicit, pstring_t cast_pstring)
{
    if(!cast<D>(value, to_type, implicit, cast_pstring))
    {
        compiler_error(value.pstring, fmt(
            "Unable to perform % type cast from % to %.", 
            (implicit ? "implicit": "explicit"), value.type, to_type));
    }

    return value;
}

// Converts multiple values at once, but only if all casts are valid.
// On success, -1 is returned and 'val_begin' to 'val_end' may be modified to their casted type.
// On failure, an andex into 'begin' is return, with the failed cast.
template<eval_t::do_t D>
int eval_t::cast_args(
    pstring_t pstring, expr_value_t* begin, expr_value_t* end, 
    type_t const* type_begin, bool implicit)
{
    assert(begin <= end);
    std::size_t const size = end - begin;

    cast_result_t* results = ALLOCA_T(cast_result_t, size);
    for(std::size_t i = 0; i != size; ++i)
        if(!(results[i] = can_cast(begin[i].type, type_begin[i], true)))
            return i;

    for(std::size_t i = 0; i != size; ++i)
        cast<D>(begin[i], type_begin[i], implicit, pstring);

    return -1; // means no errors!
}

cfg_ht eval_t::insert_cfg(bool seal, pstring_t label_name)
{
    cfg_ht const new_node = ir->emplace_cfg();
    cfg_data_pool::resize<block_d>(cfg_pool::array_size());
    block_d& block_data = new_node.data<block_d>();
    block_data.label_name = label_name;
    block_data.sealed = seal;

#ifndef NDEBUG
    block_data.creator = this;
#endif

    auto const init_vector = [this](block_d::vector_t& vec)
    {
        assert(num_vars() == var_types.size());
        vec.resize(var_types.size());
        for(unsigned i = 0; i < var_types.size(); ++i)
            vec[i].resize(std::max<unsigned>(1, ::num_members(var_types[i])));
    };

    init_vector(block_data.vars);
    if(!seal)
    {
        init_vector(block_data.unsealed_phis);
        assert(block_data.unsealed_phis.size() == block_data.vars.size());
    }

    return new_node;
}

void eval_t::seal_block(block_d& block_data)
{
    assert(block_data.sealed == false);
    ssa_value_t v;
    for(unsigned i = 0; i < num_vars(); ++i)
        for(unsigned member = 0; member < block_data.unsealed_phis[i].size(); ++member)
            if((v = block_data.unsealed_phis[i][member]) && v.holds_ref())
                fill_phi_args(v->handle(), var_ht{ i }, member);
    block_data.unsealed_phis.clear();
    block_data.sealed = true;
}

// Relevant paper:
//   Simple and Efficient Construction of Static Single Assignment Form
ssa_value_t eval_t::var_lookup(cfg_ht cfg_node, var_ht var_i, unsigned member)
{
    block_d& block_data = cfg_node.data<block_d>();

    passert(block_data.creator, cfg_node);
    passert(is_global(var_i) || block_data.creator == this, cfg_node, block_data.creator, this);
    assert(var_i.id < block_data.vars.size());
    passert(is_global(var_i) || block_data.vars.size() == var_types.size(), block_data.vars.size(), var_types.size());
    assert(member < block_data.var(var_i).size());

    if(ssa_value_t lookup = from_variant<COMPILE>(block_data.var(var_i)[member], member_type(var_type(var_i), member)))
        return lookup;
    else if(block_data.sealed)
    {
        // If the block doesn't contain a definition for 'var_i',
        // recursively look up its definition in predecessor nodes.
        // If there are multiple predecessors, a phi node will be created.
        try
        {
            if(block_data.is_root)
                throw var_lookup_error_t();

            if(block_data.pre_inline && is_local(var_i))
                return var_lookup(block_data.pre_inline, var_i, member);

            switch(cfg_node->input_size())
            {
            case 0:
                throw var_lookup_error_t();
            case 1:
                return var_lookup(cfg_node->input(0), var_i, member);
            default:
                ssa_ht const phi = cfg_node->emplace_ssa(SSA_phi, ::member_type(var_type(var_i), member));
                block_data.var(var_i)[member] = phi;
                fill_phi_args(phi, var_i, member);
            #ifndef NDEBUG
                for(unsigned i = 0; i < phi->input_size(); ++i)
                    assert(phi->input(i));
                assert(phi);
            #endif
                return phi;
            }
        }
        catch(var_lookup_error_t&)
        {
            if(block_data.label_name.size && is_local(var_i))
            {
                unsigned const local_i = to_local_i(var_i);
                pstring_t var_name = fn->def().local_vars[local_i].decl.name;
                file_contents_t file(var_name.file_i);
                throw compiler_error_t(
                    fmt_error(block_data.label_name, fmt(
                        "Jump to label crosses initialization "
                        "of variable %.", var_name.view(file.source())), &file)
                    + fmt_note(var_name, "Variable is defined here:", &file));
            }
            throw;
        }
    }
    else 
    {
        // If the node is unsealed, the predecessors are not fully known,
        // and thus it's impossible to determine the var's definition.
        // To work around this, an incomplete phi node can be created, which
        // will then be filled when the node is sealed.
        assert(block_data.unsealed_phis.size() == block_data.vars.size());
        ssa_ht const phi = cfg_node->emplace_ssa(SSA_phi, ::member_type(var_type(var_i), member));
        block_data.var(var_i)[member] = phi;
        block_data.unsealed_phi(var_i)[member] = phi;
        assert(phi);
        return phi;
    }
}

rval_t eval_t::var_lookup(cfg_ht cfg_node, var_ht var_i)
{
    block_d& block_data = cfg_node.data<block_d>();

    rval_t rval(block_data.var(var_i).size());

    assert(rval.size() == num_members(var_type(var_i)));

    for(unsigned member = 0; member < rval.size(); ++member)
    {
        assert(member < block_data.var(var_i).size());
        rval[member] = var_lookup(cfg_node, var_i, member);
    }

    return rval;
}

void eval_t::fill_phi_args(ssa_ht phi, var_ht var_i, unsigned member)
{
    // Input must be an empty phi node.
    assert(phi->op() == SSA_phi);
    assert(phi->input_size() == 0);

    // Fill the input array using local lookups.
    cfg_ht const cfg_node = phi->cfg_node();

    unsigned const input_size = cfg_node->input_size();
    phi->alloc_input(input_size);
    for(unsigned i = 0; i < input_size; ++i)
    {
        // This has to be on two lines, otherwise reference invalidation lurks.
        ssa_value_t v = var_lookup(cfg_node->input(i), var_i, member);
        phi->build_set_input(i, v);
    }
}


template<eval_t::do_t D>
ssa_value_t eval_t::from_variant(ct_variant_t const& v, type_t type)
{
    if(ssa_value_t const* value = std::get_if<ssa_value_t>(&v))
       return *value;
    else if(ct_array_t const* array = std::get_if<ct_array_t>(&v))
    {
        if(!is_compile(D))
            throw std::runtime_error("Cannot convert ct_array_t to ssa_value_t.");
        assert(num_members(type) == 1);

        unsigned const length = type.array_length();
        assert(length);

        // Determine if the array is a fill.
        ssa_value_t const first = (*array)[0];
        for(unsigned i = 1; i < length; ++i)
            if((*array)[i] != first)
                goto not_fill;

        // Fill:
        return builder.cfg->emplace_ssa(SSA_fill_array, type, first);

    not_fill:
        ssa_ht h = builder.cfg->emplace_ssa(SSA_init_array, type);
        h->alloc_input(length);
        for(unsigned i = 0; i < length; ++i)
        {
            ssa_value_t v = (*array)[i];
            if(!v)
                v = builder.cfg->emplace_ssa(SSA_uninitialized, type.elem_type());
            h->build_set_input(i, v);
        }

        return h;
    }
    /* TODO
    else if(ast_node_t const* const* ast = std::get_if<ast_node_t const*>(&v))
    {
        // TODO
        assert(false);
        //return locator_t::lt_expr(alloc_lt_value(type, *vec));
    }
    */
    return {};
}

ssa_value_array_t eval_t::from_rval(rval_t const& rval, type_t type)
{
    ssa_value_array_t array;
    array.reserve(rval.size());

    for(unsigned i = 0; i < rval.size(); ++i)
        array.push_back(from_variant<COMPILE>(rval[i], member_type(type, i)));

    return array;
}

ssa_value_array_t eval_t::from_rval(expr_value_t const& value)
{
    return from_rval(value.rval(), value.type);
}

void eval_t::cfg_exits_with_jump()
{
    builder.cfg->alloc_output(1);
}

void eval_t::cfg_exits_with_branch(ssa_value_t condition)
{
    ssa_ht const if_h = builder.cfg->emplace_ssa(SSA_if, TYPE_VOID, condition);
    if_h->append_daisy();
    builder.cfg->alloc_output(2);
    assert(builder.cfg->output_size() == 2);
    assert(builder.cfg->last_daisy() == if_h);
}

// Jumps are like 'break', 'continue', 'goto', etc.
cfg_ht eval_t::compile_goto()
{
    // The syntax allows code to exist following a jump statement.
    // Said code is unreachable, but gets compiled anyway.
    // Implement using a conditional that always takes the false branch.
    // (This will be optimized out later)

    cfg_exits_with_branch(ssa_value_t(0u, TYPE_BOOL));
    cfg_ht dead_branch = insert_cfg(true);
    builder.cfg->build_set_output(1, dead_branch);
    return dead_branch;
}

static token_t _make_token(expr_value_t const& value)
{
    assert(value.is_rval());

    if(value.is_ct())
    {
        if(value.type == TYPE_INT)
            return { .type = TOK_int, .pstring = value.pstring, .value = value.s() };
        if(value.type == TYPE_REAL)
            return { .type = TOK_real, .pstring = value.pstring, .value = value.s() };
    }

    token_t tok = { .type = TOK_rpair, .pstring = value.pstring };
    tok.set_ptr(eternal_emplace<rpair_t>(value.rval(), value.type));
    return tok;
}

static rval_t _lt_rval(type_t const& type, locator_t loc)
{
    rval_t rval;
    
    unsigned const n = num_members(type);
    for(unsigned i = 0; i < n; ++i)
    {
        locator_t l = loc;
        l.set_member(i);
        assert(l.member() == i);
        rval.push_back(l);
    }

    return rval;
}

template<eval_t::do_t D>
locator_t eval_t::handle_lt(
    type_t const& type, token_t const& token, 
    expr_value_t const* begin, expr_value_t const* end)
{
    if(is_check(D) || is_link(D))
        return {};

    bool has_lt_arg = false;

    // Every arg must either be a LT value, or a CT value,
    // and we need to find at least one LT value.
    for(auto it = begin; it != end; ++it)
    {
        if(it->is_lt())
            has_lt_arg = true;
        else if(!it->is_ct())
            return {};
    }

    if(!has_lt_arg)
        return {};

    return make_lt(type, token, begin, end);
}

template<eval_t::do_t D, typename... Args>
locator_t eval_t::handle_lt(
    type_t const& type, token_t const& token, Args const&... args)
{
    if(is_check(D) || is_link(D))
        return {};

    // Every arg must either be a LT value, or a CT value,
    // and we need to find at least one LT value.
    if((args.is_rt() || ... || false))
        return {};

    if((args.is_lt() || ... || false))
        return make_lt(type, token, args...);

    return {};
}

locator_t eval_t::make_lt(
    type_t const& type, token_t const& token, 
    expr_value_t const* begin, expr_value_t const* end)
{
    std::size_t const argn = end - begin;

    ast_node_t* children = eternal_new<ast_node_t>(argn);
    for(unsigned i = 0; i < argn; ++i)
        children[i] = { .token = _make_token(begin[i]), .children = nullptr };

    ast_node_t new_ast = { .token = token };
    new_ast.children = children;
    assert(new_ast.num_children() == argn);

    return locator_t::lt_expr(alloc_lt_value(type, std::move(new_ast)));
}

template<typename... Args>
locator_t eval_t::make_lt(
    type_t const& type, token_t const& token, Args const&... args)
{
    std::size_t const argn = sizeof...(Args);

    ast_node_t* children = eternal_new<ast_node_t>(argn);
    unsigned i = 0;
    ((children[i++] = { .token = _make_token(args), .children = nullptr }), ...);

    ast_node_t new_ast = { .token = token };
    new_ast.children = children;
    assert(new_ast.num_children() == argn);

    return locator_t::lt_expr(alloc_lt_value(type, std::move(new_ast)));
}

