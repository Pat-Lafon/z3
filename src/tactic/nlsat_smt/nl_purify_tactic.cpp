/*++
Copyright (c) 2015 Microsoft Corporation

Module Name:

    nl_purify_tactic.cpp

Abstract:

    Tactic for purifying quantifier-free formulas that mix QF_NRA and other theories.
    It is designed to allow cooprating between the nlsat solver and other theories
    in a decoupled way.

    Let goal be formula F.
    Let NL goal be formula G.
    Assume F is in NNF.
    Assume F does not contain mix of real/integers.
    Assume F is quantifier-free (please, otherwise we need to reprocess from instantiated satisfiable formula)

    For each atomic nl formula f, 
    - introduce a propositional variable p
    - replace f by p
    - add clauses p => f to G
    
    For each interface term t,
    - introduce interface variable v (or use t if it is already a variable)
    - replace t by v
    
    Check satisfiability of G.
    If satisfiable, then check assignment to p and interface equalities on F
    If unsat:
       Retrieve core and add core to G.
    else:
       For interface equalities from model of F that are not equal in G, add 
       For interface variables that are equal under one model, but not the other model,
       create interface predicate p_vw => v = w, add to both F, G.
       Add interface equations to assumptions, recheck F.
       If unsat retrieve core add to G.
   
Author:

    Nikolaj Bjorner (nbjorner) 2015-5-5.

Revision History:

--*/
#include "tactical.h"
#include "nl_purify_tactic.h"
#include "smt_tactic.h"
#include "rewriter.h"
#include "nlsat_tactic.h"
#include "filter_model_converter.h"
#include "obj_pair_hashtable.h"
#include "rewriter_def.h"
#include "ast_pp.h"
#include "trace.h"
#include "smt_solver.h"
#include "solver.h"

class nl_purify_tactic : public tactic {

    enum polarity_t {
        pol_pos,
        pol_neg,
        pol_dual
    };
    
    ast_manager &   m;
    arith_util      m_util;
    params_ref      m_params;
    bool            m_produce_proofs;
    ref<filter_model_converter> m_fmc;
    bool            m_cancel;       
    tactic_ref      m_nl_tac;       // nlsat tactic
    ref<solver>     m_solver;       // SMT solver
    expr_ref_vector m_eq_preds;     // predicates for equality between pairs of interface variables
    svector<lbool>  m_eq_values;    // truth value of the equality predicates in nlsat 
    app_ref_vector  m_new_reals;    // interface real variables
    app_ref_vector  m_new_preds;    // abstraction predicates for smt_solver (hide real constraints)
    expr_ref_vector m_asms;         // assumptions to pass to SMT solver
    obj_pair_map<expr,expr,expr*> m_eq_pairs;  // map pairs of interface variables to auxiliary predicates
    obj_map<expr,expr*> m_interface_cache;     // map of compound real expression to interface variable.
    obj_map<expr, polarity_t> m_polarities;    // polarities of sub-expressions

public:
    struct rw_cfg : public default_rewriter_cfg {
        enum mode_t {
            mode_interface_var,
            mode_bool_preds
        };
        ast_manager&         m;
        nl_purify_tactic &   m_owner;
        app_ref_vector&      m_new_reals;
        app_ref_vector&      m_new_preds;
        obj_map<expr, polarity_t>& m_polarities;
        obj_map<expr,expr*>& m_interface_cache;
        expr_ref_vector      m_nl_cnstrs;
        proof_ref_vector     m_nl_cnstr_prs;
        expr_ref_vector      m_args;
        mode_t               m_mode;

        rw_cfg(nl_purify_tactic & o):
            m(o.m),
            m_owner(o),
            m_new_reals(o.m_new_reals),
            m_new_preds(o.m_new_preds),
            m_polarities(o.m_polarities),
            m_interface_cache(o.m_interface_cache),
            m_nl_cnstrs(m),
            m_nl_cnstr_prs(m),            
            m_args(m),
            m_mode(mode_interface_var) {
        }

        virtual ~rw_cfg() {}

        arith_util & u() { return m_owner.m_util; }

        bool produce_proofs() const { return m_owner.m_produce_proofs; }

        expr * mk_interface_var(expr* arg) {
            expr* r;
            if (m_interface_cache.find(arg, r)) {
                return r;
            }
            if (is_uninterp_const(arg)) {
                m_interface_cache.insert(arg, arg);                
                return arg;
            }
            r = m.mk_fresh_const(0, u().mk_real());
            m_new_reals.push_back(to_app(r));
            m_interface_cache.insert(arg, r);
            return r;
        }

        void mk_interface_bool(func_decl * f, unsigned num, expr* const* args, expr_ref& result) {
            expr_ref old_pred(m.mk_app(f, num, args), m);
            polarity_t pol;
            TRACE("nlsat_smt", tout << old_pred << "\n";);
            VERIFY(m_polarities.find(old_pred, pol));
            result = m.mk_fresh_const(0, m.mk_bool_sort());
            m_polarities.insert(result, pol);
            m_new_preds.push_back(to_app(result));
            if (pol != pol_neg) {
                m_nl_cnstrs.push_back(m.mk_or(m.mk_not(result), m.mk_app(f, num, args)));
            }
            if (pol != pol_pos) {
                m_nl_cnstrs.push_back(m.mk_or(result, m.mk_not(m.mk_app(f, num, args))));
            }
            TRACE("nlsat_smt", tout << result << " :  " << mk_pp(m_nl_cnstrs.back(), m) << "\n";);
        }

        bool reduce_quantifier(quantifier * old_q, 
                               expr * new_body, 
                               expr * const * new_patterns, 
                               expr * const * new_no_patterns,
                               expr_ref & result,
                               proof_ref & result_pr) {
            throw tactic_exception("quantifiers are not supported in mixed-mode nlsat engine");
        }

        br_status reduce_app(func_decl * f, unsigned num, expr* const* args, expr_ref& result, proof_ref & pr) {
            if (m_mode == mode_bool_preds) {
                return reduce_app_bool(f, num, args, result, pr);
            }
            else {
                return reduce_app_real(f, num, args, result, pr);
            }
        }

        br_status reduce_app_bool(func_decl * f, unsigned num, expr* const* args, expr_ref& result, proof_ref & pr) {
            if (f->get_family_id() == m.get_basic_family_id()) {
                if (f->get_decl_kind() == OP_EQ && u().is_real(args[0])) {
                    mk_interface_bool(f, num, args, result);
                    return BR_DONE;
                }
                else {
                    return BR_FAILED;
                }
            }
            if (f->get_family_id() == u().get_family_id()) {
                switch (f->get_decl_kind()) {
                case OP_LE: case OP_GE: case OP_LT: case OP_GT:
                    // these are the only real cases of non-linear atomic formulas besides equality.
                    mk_interface_bool(f, num, args, result);
                    return BR_DONE;
                default:
                    return BR_FAILED;
                }
            }
            return BR_FAILED;            
        }

        br_status reduce_app_real(func_decl * f, unsigned num, expr* const* args, expr_ref& result, proof_ref & pr) {
            bool has_interface = false;
            if (f->get_family_id() == u().get_family_id()) {
                switch (f->get_decl_kind()) {
                case OP_NUM: case OP_IRRATIONAL_ALGEBRAIC_NUM:
                case OP_ADD: case OP_MUL: case OP_SUB: 
                case OP_UMINUS: case OP_ABS: case OP_POWER: 
                    return BR_FAILED;
                default:
                    break;
                }
            }
            m_args.reset();
            for (unsigned i = 0; i < num; ++i) {
                expr* arg = args[i];
                if (u().is_real(arg)) {
                    has_interface = true;
                    m_args.push_back(mk_interface_var(arg));
                }
                else {
                    m_args.push_back(arg);
                }
            }
            if (has_interface) {
                result = m.mk_app(f, num, m_args.c_ptr());
                TRACE("nlsat_smt", tout << result << "\n";);
                return BR_DONE;
            }
            else {
                return BR_FAILED;
            }
        }
    };

private:

    class rw : public rewriter_tpl<rw_cfg> {
        rw_cfg m_cfg;
    public:
        rw(nl_purify_tactic & o):
            rewriter_tpl<rw_cfg>(o.m, o.m_produce_proofs, m_cfg),
            m_cfg(o) {
        } 
        expr_ref_vector const& nl_cnstrs() const { 
            return m_cfg.m_nl_cnstrs; 
        }
        void set_bool_mode() {
            m_cfg.m_mode = rw_cfg::mode_bool_preds;
        }
        void set_interface_var_mode() {
            m_cfg.m_mode = rw_cfg::mode_interface_var;
        }
    };

    arith_util & u() { return m_util; }

    void check_point() {
        if (m_cancel) {
            throw tactic_exception("canceled");
        }
    }

    void display_result(std::ostream& out, goal_ref_buffer const& result) {
        for (unsigned i = 0; i < result.size(); ++i) {
            result[i]->display(tout << "goal\n");
        }        
    }

    void update_eq_values(model_ref& mdl) {
        expr_ref tmp(m);
        for (unsigned i = 0; i < m_eq_preds.size(); ++i) {
            expr* pred = m_eq_preds[i].get();
            m_eq_values[i] = l_undef;
            if (mdl->eval(pred, tmp)) {
                if (m.is_true(tmp)) {
                    m_eq_values[i] = l_true;
                }
                else if (m.is_false(tmp)) {
                    m_eq_values[i] = l_false;
                }
            }
        }
    }

    void solve(goal_ref const&      nl_g, 
               goal_ref_buffer&     result, 
               model_converter_ref& mc) {        

        while (true) {
            check_point();
            TRACE("nlsat_smt", m_solver->display(tout << "SMT:\n"); nl_g->display(tout << "\nNL:\n"); );
            goal_ref tmp_nl  = alloc(goal, m, true, false);
            model_converter_ref nl_mc;
            proof_converter_ref nl_pc;
            expr_dependency_ref nl_core(m);
            result.reset();
            tmp_nl->copy_from(*nl_g.get());
            (*m_nl_tac)(tmp_nl, result, nl_mc, nl_pc, nl_core);

            if (is_decided_unsat(result)) {
                TRACE("nlsat_smt", tout << "unsat\n";);
                break;
            }
            if (!is_decided_sat(result)) {
                TRACE("nlsat_smt", tout << "not a unit\n";);
                break;
            }
            // extract evaluation on interface variables.
            // assert booleans that evaluate to true.
            // assert equalities between equal interface real variables.

            model_ref mdl_nl, mdl_smt;
            model_converter2model(m, nl_mc.get(), mdl_nl);
            update_eq_values(mdl_nl);
            enforce_equalities(mdl_nl, nl_g);
            
            setup_assumptions(mdl_nl);

            TRACE("nlsat_smt", m_solver->display(tout << "smt goal:\n"); );

            result.reset();
            lbool r = m_solver->check_sat(m_asms.size(), m_asms.c_ptr());
            if (r == l_false) {
                // extract the core from the result 
                ptr_vector<expr> core;
                m_solver->get_unsat_core(core);
                if (core.empty()) {
                    goal_ref g = alloc(goal, m);
                    g->assert_expr(m.mk_false());
                    result.push_back(g.get());
                    break;
                }
                expr_ref_vector clause(m);
                expr_ref fml(m);
                expr* e;
                for (unsigned i = 0; i < core.size(); ++i) {
                    clause.push_back(m.is_not(core[i], e)?e:m.mk_not(core[i]));
                }
                fml = m.mk_or(clause.size(), clause.c_ptr());
                nl_g->assert_expr(fml);
                continue;
            }
            else if (r == l_true) {
                m_solver->get_model(mdl_smt);
                if (enforce_equalities(mdl_smt, nl_g)) {
                    // SMT enforced a new equality that wasn't true for nlsat.
                    continue;
                }
                TRACE("nlsat_smt", 
                      m_fmc->display(tout << "joint state is sat\n");
                      nl_mc->display(tout << "nl\n"););
                merge_models(*mdl_nl.get(), mdl_smt);
                mc = m_fmc.get();
                apply(mc, mdl_smt, 0);
                mc = model2model_converter(mdl_smt.get());
                result.push_back(alloc(goal, m));
            }
            else {
                TRACE("nlsat_smt", tout << "unknown\n";);
            }
            break;
        }
        TRACE("nlsat_smt", display_result(tout, result););
    }

    void setup_assumptions(model_ref& mdl) {
        m_asms.reset();
        app_ref_vector const& fresh_preds = m_new_preds;
        expr_ref tmp(m);
        for (unsigned i = 0; i < fresh_preds.size(); ++i) {
            expr* pred = fresh_preds[i];
            if (mdl->eval(pred, tmp)) {
                TRACE("nlsat_smt", tout << "pred: " << mk_pp(pred, m) << "\n";);
                polarity_t pol = m_polarities.find(pred);
                if (pol != pol_neg && m.is_true(tmp)) {
                    m_asms.push_back(pred);
                }
                else if (pol != pol_pos && m.is_false(tmp)) {
                    m_asms.push_back(m.mk_not(pred));
                }
            }
        }
        for (unsigned i = 0; i < m_eq_preds.size(); ++i) {
            expr* pred = m_eq_preds[i].get();
            switch (m_eq_values[i]) {
            case l_true: 
                m_asms.push_back(pred);
                break;
            case l_false:
                m_asms.push_back(m.mk_not(pred));
                break;
            default:
                break;
            }
        }
    }

    bool enforce_equalities(model_ref& mdl, goal_ref const& nl_g) {
        TRACE("nlsat_smt", tout << "Enforce equalities " << m_interface_cache.size() << "\n";);
        bool new_equality = false;
        expr_ref_vector nums(m);
        obj_map<expr, expr*> num2var;
        obj_map<expr, expr*>::iterator it = m_interface_cache.begin(), end = m_interface_cache.end();
        for (; it != end; ++it) {
            expr_ref r(m);
            expr* v, *w, *pred;            
            w = it->m_value;
            VERIFY(mdl->eval(w, r));
            TRACE("nlsat_smt", tout << mk_pp(w, m) << " |-> " << r << "\n";);
            nums.push_back(r);
            if (num2var.find(r, v)) {
                if (!m_eq_pairs.find(v, w, pred)) {
                    pred = m.mk_fresh_const(0, m.mk_bool_sort());
                    m_eq_preds.push_back(pred);
                    m_eq_values.push_back(l_true);
                    m_fmc->insert(to_app(pred)->get_decl());
                    nl_g->assert_expr(m.mk_or(m.mk_not(pred), m.mk_eq(w, v)));
                    nl_g->assert_expr(m.mk_or(pred, m.mk_not(m.mk_eq(w, v))));
                    m_solver->assert_expr(m.mk_iff(pred, m.mk_eq(w, v)));
                    new_equality = true;
                    m_eq_pairs.insert(v, w, pred);
                }                    
                else {
                    // interface equality is already enforced.
                }                    
            }
            else {
                num2var.insert(r, w);
            }
        }
        return new_equality;
    }

    void merge_models(model const& mdl_nl, model_ref& mdl_smt) {
        obj_map<expr,expr*> num2num;
        expr_ref result(m), val2(m);
        expr_ref_vector args(m), trail(m);
        unsigned sz = mdl_nl.get_num_constants();
        for (unsigned i = 0; i < sz; ++i) {
            func_decl* v = mdl_nl.get_constant(i);
            if (u().is_real(v->get_range())) {
                expr* val = mdl_nl.get_const_interp(v);
                if (mdl_smt->eval(v, val2)) {
                    if (val != val2) {
                        num2num.insert(val2, val);
                        trail.push_back(val2);                        
                    }
                }
            }
        }
        sz = mdl_smt->get_num_functions();
        for (unsigned i = 0; i < sz; ++i) {
            func_decl* f = mdl_smt->get_function(i);
            if (has_real(f)) {
                unsigned arity = f->get_arity();
                func_interp* f1 = mdl_smt->get_func_interp(f);
                func_interp* f2 = alloc(func_interp, m, f->get_arity());
                for (unsigned j = 0; j < f1->num_entries(); ++j) {
                    args.reset();                    
                    func_entry const* entry = f1->get_entry(j);
                    for (unsigned k = 0; k < arity; ++k) {
                        args.push_back(translate(num2num, entry->get_arg(k)));
                    }
                    result = translate(num2num, entry->get_result());
                    f2->insert_entry(args.c_ptr(), result);
                }
                expr* e = f1->get_else();
                result = translate(num2num, e);
                f2->set_else(result);
                mdl_smt->register_decl(f, f2);
            }            
        }
        mdl_smt->copy_const_interps(mdl_nl);
    }

    bool has_real(func_decl* f) {
        for (unsigned i = 0; i < f->get_arity(); ++i) {
            if (u().is_real(f->get_domain(i))) return true;
        }
        return u().is_real(f->get_range());
    }

    expr* translate(obj_map<expr, expr*> const& num2num, expr* e) {
        if (!e || !u().is_real(e)) return e;
        expr* result = 0;
        if (num2num.find(e, result)) return result;
        return e;
    }

    void get_polarities(goal const& g) {
        ptr_vector<expr> forms;
        svector<polarity_t> pols;
        unsigned sz = g.size();
        for (unsigned i = 0; i < sz; ++i) {
            forms.push_back(g.form(i));
            pols.push_back(pol_pos);
        }
        polarity_t p, q;
        while (!forms.empty()) {
            expr* e = forms.back();
            p = pols.back();
            forms.pop_back();
            pols.pop_back();
            if (m_polarities.find(e, q)) {
                if (p == q || q == pol_dual) continue;
                p = pol_dual;
            }
            TRACE("nlsat_smt", tout << mk_pp(e, m) << "\n";);
            m_polarities.insert(e, p);
            if (is_quantifier(e) || is_var(e)) {
                throw tactic_exception("nl-purify tactic does not support quantifiers");                
            }
            SASSERT(is_app(e));
            app* a = to_app(e);
            func_decl* f = a->get_decl();
            if (f->get_family_id() == m.get_basic_family_id() && p != pol_dual) {
                switch(f->get_decl_kind()) {
                case OP_NOT:
                    p = neg(p);
                    break;
                case OP_AND:
                case OP_OR:
                    break;
                default:
                    p = pol_dual;
                    break;
                }
            }
            else {
                p = pol_dual;
            }
            for (unsigned i = 0; i < a->get_num_args(); ++i) {
                forms.push_back(a->get_arg(i));
                pols.push_back(p);
            }
        }
    }

    polarity_t neg(polarity_t p) {
        switch (p) {
        case pol_pos: return pol_neg;
        case pol_neg: return pol_pos;
        case pol_dual: return pol_dual;
        }
        return pol_dual;
    }

    polarity_t join(polarity_t p, polarity_t q) {
        if (p == q) return p;
        return pol_dual;
    }

    void rewrite_goal(rw& r, goal_ref const& g) {
        expr_ref   new_curr(m);
        proof_ref  new_pr(m);
        unsigned sz = g->size();
        for (unsigned i = 0; i < sz; i++) {
            expr * curr = g->form(i);
            r(curr, new_curr, new_pr);
            if (m_produce_proofs) {
                proof * pr = g->pr(i);
                new_pr     = m.mk_modus_ponens(pr, new_pr);
            }
            g->update(i, new_curr, new_pr, g->dep(i)); 
        }
    }

public:

    nl_purify_tactic(ast_manager & m, params_ref const& p):
        m(m),
        m_util(m),
        m_params(p),
        m_nl_tac(mk_nlsat_tactic(m, p)),
        m_solver(mk_smt_solver(m, p, symbol::null)),
        m_fmc(0),
        m_cancel(false),
        m_eq_preds(m),
        m_new_reals(m),
        m_new_preds(m),
        m_asms(m)
    {}

    virtual ~nl_purify_tactic() {}

    virtual void updt_params(params_ref const & p) {
        m_params = p;
    }

    virtual tactic * translate(ast_manager& m) {
        return alloc(nl_purify_tactic, m, m_params);
    }

    virtual void set_cancel(bool f) {
        if (f) {
            m_nl_tac->cancel();
            m_solver->cancel();
        }
        else {
            m_solver->reset_cancel();
            m_nl_tac->reset_cancel();
        }
        m_cancel = f;
    }

    virtual void cleanup() {
        m_solver = mk_smt_solver(m, m_params, symbol::null);
        m_nl_tac->cleanup();
        m_eq_preds.reset();
        m_eq_values.reset();
        m_new_reals.reset();
        m_new_preds.reset();
        m_eq_pairs.reset();
        m_polarities.reset();
    }
    
    virtual void operator()(goal_ref const & g, 
                            goal_ref_buffer & result, 
                            model_converter_ref & mc, 
                            proof_converter_ref & pc,
                            expr_dependency_ref & core) {

        tactic_report report("qfufnl-purify", *g);
        m_produce_proofs = g->proofs_enabled();
        mc = 0; pc = 0; core = 0;

        fail_if_proof_generation("qfufnra-purify", g);
        fail_if_unsat_core_generation("qfufnra-purify", g);        
        rw r(*this);
        goal_ref nlg = alloc(goal, m, true, false);
                
        TRACE("nlsat_smt", g->display(tout););

        // first hoist interface variables, 
        // then annotate subformulas by polarities,
        // finally extract polynomial inequalities by
        // creating a place-holder predicate inside the
        // original goal and extracing pure nlsat clauses.
        r.set_interface_var_mode();
        rewrite_goal(r, g);        
        get_polarities(*g.get());
        r.set_bool_mode();
        rewrite_goal(r, g);        

        m_fmc = alloc(filter_model_converter, m);
        app_ref_vector const& vars1 = m_new_reals;
        for (unsigned i = 0; i < vars1.size(); ++i) {
            SASSERT(is_uninterp_const(vars1[i]));
            m_fmc->insert(vars1[i]->get_decl());
        }
        app_ref_vector const& vars2 = m_new_preds;
        for (unsigned i = 0; i < vars2.size(); ++i) {
            SASSERT(is_uninterp_const(vars2[i]));
            m_fmc->insert(vars2[i]->get_decl());
        }
        
        // add constraints to nlg.
        unsigned sz = r.nl_cnstrs().size();
        for (unsigned i = 0; i < sz; i++) {
            nlg->assert_expr(r.nl_cnstrs()[i], m_produce_proofs ? r.cfg().m_nl_cnstr_prs.get(i) : 0, 0);
        }
        g->inc_depth();
        for (unsigned i = 0; i < g->size(); ++i) {
            m_solver->assert_expr(g->form(i));
        }
        g->inc_depth();
        solve(nlg, result, mc);        
    }
};


tactic * mk_nl_purify_tactic(ast_manager& m, params_ref const& p) {
    return alloc(nl_purify_tactic, m, p);
}
