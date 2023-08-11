/*++
Copyright (c) 2020 Microsoft Corporation

Module Name:

    synth_solver.h

Author:

    Petra Hozzova 2023-08-08
    Nikolaj Bjorner (nbjorner) 2020-08-17

--*/

#include "util/heap.h"
#include "ast/for_each_expr.h"
#include "ast/synth_decl_plugin.h"
#include "ast/rewriter/expr_safe_replace.h"
#include "ast/rewriter/th_rewriter.h"
#include "sat/smt/synth_solver.h"
#include "sat/smt/euf_solver.h"

namespace synth {

    solver::solver(euf::solver& ctx):
        th_euf_solver(ctx, symbol("synth"), ctx.get_manager().mk_family_id("synth")), m_rep(m) {
        std::function<void(euf::enode*, euf::enode*)> _on_merge = 
            [&](euf::enode* root, euf::enode* other) { 
            on_merge_eh(root, other); 
        };
        ctx.get_egraph().set_on_merge(_on_merge);
    }
        
    solver::~solver() {}

    bool solver::contains_uncomputable(expr* e) {
        
        auto is_output = [&](expr* e) {
            return any_of(m_synth, [&](synth_objective const& a) { return a.output() == e; });
        };
        return any_of(subterms::all(expr_ref(e, m)), [&](expr* a) { return (is_app(a) && m_uncomputable.contains(to_app(a)->get_decl())) || is_output(a); });
    }

    void solver::add_uncomputable(app* e) {
        for (expr* arg : *e) {
            if (is_app(arg)) {
                func_decl* f = to_app(arg)->get_decl();
                m_uncomputable.insert(f);
                ctx.push(insert_obj_trail(m_uncomputable, f));
            }
        }
    }

    void solver::add_synth_objective(synth_objective const& e) {
        ctx.push_vec(m_synth, e);
        for (auto* arg : e) {
            m_is_computable.reserve(arg->get_id() + 1);
            ctx.push(set_bitvector_trail(m_is_computable, arg->get_id())); // TODO use enode roots instead and test if they are already set.
        }
    }

    void solver::add_specification(app* e, expr* arg) {
	// This assumes that each (assert (constraint (...)) is asserting exactly one app
        sat::literal lit = ctx.mk_literal(arg);
        sat::bool_var bv = ctx.get_si().add_bool_var(e);
        sat::literal lit_e(bv, false);
        ctx.attach_lit(lit_e, e);
        add_clause(~lit_e, lit);
        ctx.push_vec(m_spec, arg);        
    }

    // recognize synthesis objectives here.
    sat::literal solver::internalize(expr* e, bool sign, bool root) {
        internalize(e);
	    sat::literal lit = ctx.expr2literal(e);
        if (sign)
            lit.neg();
        return lit;
    }

    // recognize synthesis objectives here and above
    void solver::internalize(expr* e) {
        SASSERT(is_app(e));
        sat::bool_var bv = ctx.get_si().add_bool_var(e);
        sat::literal lit(bv, false);
        ctx.attach_lit(lit, e);
        synth::util util(m);
        app* a = to_app(e);
        expr* arg = nullptr;
        if (util.is_synthesiz3(e))
            add_synth_objective(synth_objective(a));
        if (util.is_grammar(e))
            add_uncomputable(a);
        if (util.is_specification(e, arg))
            add_specification(a, arg);
    }

    sat::check_result solver::check() {
        // TODO: need to know if there are quantifiers to instantiate
        if (m_solved.size() < m_synth.size()) {
            IF_VERBOSE(2, ctx.display(verbose_stream()));
            return sat::check_result::CR_DONE;
        }
        if (!compute_solutions())
            return sat::check_result::CR_GIVEUP;
        return sat::check_result::CR_CONTINUE;
    }

    // display current state (eg. current set of realizers)
    std::ostream& solver::display(std::ostream& out) const {
        for (auto const& e : m_synth)
            out << "synth objective " << mk_pp(e.output(), m) << "\n";            
        return out;
    }

    // create a clone of the solver.
    euf::th_solver* solver::clone(euf::solver& ctx) {
        return alloc(solver, ctx);
    }

    void solver::on_merge_eh(euf::enode* root, euf::enode* other) {

        auto is_computable = [&](euf::enode* n) { return !contains_uncomputable(n->get_expr()) || m_is_computable.get(n->get_root_id(), false); };
        auto is_uncomputable = [&](func_decl* f) { return m_uncomputable.contains(f); };

        IF_VERBOSE(2, verbose_stream() << "merge " << ctx.bpp(root) << " " << is_computable(root) << " == " << ctx.bpp(other) << " " << is_computable(other) << "\n");

        m_is_computable.reserve(root->get_id() + 1);
        
        // if neither contains uncomputable, then we want to make sure that m_is_computable gets set.
        if (!m_is_computable.get(root->get_root_id(), false) && !contains_uncomputable(root->get_expr()))
            ctx.push(set_bitvector_trail(m_is_computable, root->get_id()));
        else if (!m_is_computable.get(root->get_root_id(), false) && !contains_uncomputable(other->get_expr()))
            ctx.push(set_bitvector_trail(m_is_computable, root->get_id()));
        else if (is_computable(root) == is_computable(other))
            return;

        if (!is_computable(root)) 
            ctx.push(set_bitvector_trail(m_is_computable, root->get_id()));

        euf::enode_vector todo;
        todo.push_back(root);
        for (unsigned i = 0; i < todo.size(); ++i) {
            auto * n = todo[i];
            for (auto* p : euf::enode_parents(n)) {
                if (is_uncomputable(p->get_decl()))
                    continue;
                if (is_computable(p))
                    continue;
                if (!all_of(euf::enode_args(p), [&](auto * ch) { return is_computable(ch); }))
                    continue;
                m_is_computable.reserve(p->get_root_id() + 1);
                ctx.push(set_bitvector_trail(m_is_computable, p->get_root_id()));
                todo.push_back(p);
            }
        }

        if (m_is_solved)
            return;

        for (auto const& e : m_synth) {
            euf::enode* n = expr2enode(e.output());
            if (is_computable(n) && !m_solved.contains(e)) 
                ctx.push_vec(m_solved, e);            
        }
    }

    bool solver::unit_propagate() {
        if (m_is_solved)
            return false;
        if (m_solved.size() < m_synth.size())
            return false;
        IF_VERBOSE(2, verbose_stream() << "propagate\n");
        ctx.push(value_trail(m_is_solved));
        m_is_solved = true;
        return compute_solutions();
    }

    void solver::compute_rep() {
        m_rep.reset();
        auto is_uncomputable = [&](func_decl* f) { return m_uncomputable.contains(f); };

        struct rep_lt {
            expr_ref_vector const& m_rep;
            rep_lt(expr_ref_vector& m_rep) : m_rep(m_rep) {}
            bool operator()(int v1, int v2) const {
                return get_depth(m_rep.get(v1)) < get_depth(m_rep.get(v2));
            };
        };
        rep_lt lt(m_rep);
        heap<rep_lt> heap(1000, lt);
        euf::enode_vector nodes;
        auto insert_m_rep = [&](euf::enode* n, expr* r) {
            unsigned id = n->get_root_id();
            set_rep(n, r);
            nodes.reserve(id + 1);
            nodes[id] = n->get_root();
            heap.reserve(id + 1);
            heap.insert(id);
            };

        for (auto const& e : m_synth) {
            for (expr* arg : e) {
                auto* narg = expr2enode(arg);
                insert_m_rep(narg, arg);
            }
        }
        // make sure we only insert non-input symbols.
        for (auto* n : ctx.get_egraph().nodes()) {
            if (n->num_args() == 0 && !contains_uncomputable(n->get_expr()) && !has_rep(n))
                insert_m_rep(n, n->get_expr());
        }
        while (!heap.empty()) {
            auto* nn = nodes[heap.erase_min()];
            for (auto* p : euf::enode_parents(nn)) {
                if (is_uncomputable(p->get_decl()))
                    continue;
                if (!all_of(euf::enode_args(p), [&](auto* ch) { return has_rep(ch); }))
                    continue;
                expr* r = get_rep(p);
                if (r) {
                    unsigned depth = get_depth(r);
                    if (any_of(euf::enode_args(p), [&](auto* ch) { return get_depth(get_rep(ch)) >= depth; }))
                        continue;
                    heap.erase(p->get_root_id());
                }
                ptr_buffer<expr> args;
                for (auto* ch : euf::enode_args(p))
                    args.push_back(get_rep(ch));
                expr_ref papp(m.mk_app(p->get_decl(), args), m);
                insert_m_rep(p, papp);
            }
        }
    }

    expr_ref solver::compute_solution(synth_objective const& e) {
        auto* n = expr2enode(e.output());
        return expr_ref(m_rep.get(n->get_root_id(), nullptr), m);
    }

    expr_ref solver::compute_condition() {
        expr_ref result(m.mk_and(m_spec), m);
        expr_safe_replace replace(m);
        for (auto const& e : m_synth) 
            replace.insert(e.output(), compute_solution(e));        
        replace(result);
        th_rewriter rw(m);
        rw(result);
        return result;
    }
#if 0

    expr_ref solver::simplify_condition(expr* e) {
        ptr_vector<expr> todo;
        todo.push_back(e);
        while (!todo.empty()) {
            expr* a = todo.back();
            if (m_rep.get(a->get_id(), nullptr)) {
                todo.pop_back();
                continue;
            }
            euf::enode* n = expr2enode(a);
            if (n && has_rep(n)) {
                todo.pop_back();
                continue;
            }
            if (!is_app(a)) {
                m_rep.setx(n->get_id(), a);
                todo.pop_back();
                continue;
            }

            unsigned orig_size = todo.size();
            for (expr* arg : *to_app(a)) {
                if (has_rep(arg))
                    args.push_back(get_rep(arg));
                else
                    todo.push_back(arg);
            }
            if (todo.size() == orig_size) {
                todo.pop_back();
                expr_ref new_a(m.mk_app(to_app(a)->get_decl(), args), m);
                n = expr2enode(new_a);
                if (has_rep(n)) 
                    m_rep.setx(a->get_id(), get_rep(n));
                else 
                    m_rep.setx(a->get_id(), new_a);                
            }            
        }
        euf::enode* n = expr2enode(e);
        if (n && has_rep(n))
            return expr_ref(get_rep(n), m);
        SASSERT(m_rep.get(e->get_id(), nullptr));
        return expr_ref(m_rep.get(e->get_id()), m);
    }
#endif
    

    sat::literal solver::synthesize(synth_objective const& synth_objective) {
        expr_ref sol = compute_solution(synth_objective);
        if (!sol)
            return sat::null_literal;

        IF_VERBOSE(0, verbose_stream() << sol << "\n");
        return eq_internalize(synth_objective.output(), sol);
    }

    bool solver::compute_solutions() {
        sat::literal_vector clause;
        compute_rep();

        for (synth_objective const& e : m_synth) {
            auto lit = synthesize(e);
            if (lit == sat::null_literal)
                return false;
            clause.push_back(~lit);
        }
        add_clause(clause);
        expr_ref cond = compute_condition();
        add_unit(~mk_literal(cond));
        IF_VERBOSE(0, verbose_stream() << "if " << cond << "\n");
        return true;
    }

}
