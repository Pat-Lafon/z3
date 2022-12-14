/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Polysat core saturation

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6

--*/
#pragma once
#include "math/polysat/clause_builder.h"
#include "math/polysat/conflict.h"

namespace polysat {

    /**
     * Introduce lemmas that derive new (simpler) constraints from the current conflict and partial model.
     */
    class saturation {

        solver& s;
        clause_builder m_lemma;
        char const* m_rule = nullptr;

        void set_rule(char const* r) { m_rule = r; }

        bool is_non_overflow(pdd const& x, pdd const& y, signed_constraint& c);
        signed_constraint ineq(bool strict, pdd const& lhs, pdd const& rhs);

        bool propagate(conflict& core, inequality const& crit1, signed_constraint c);
        bool add_conflict(conflict& core, inequality const& crit1, signed_constraint c);
        bool add_conflict(conflict& core, inequality const& crit1, inequality const& crit2, signed_constraint c);

        bool try_ugt_x(pvar v, conflict& core, inequality const& c);

        bool try_ugt_y(pvar v, conflict& core, inequality const& c);
        bool try_ugt_y(pvar v, conflict& core, inequality const& l_y, inequality const& yx_l_zx, pdd const& x, pdd const& z);

        bool try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& c);
        bool try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& x_l_z, inequality const& y_l_ax, pdd const& a, pdd const& y);

        bool try_ugt_z(pvar z, conflict& core, inequality const& c);
        bool try_ugt_z(pvar z, conflict& core, inequality const& x_l_z0, inequality const& yz_l_xz, pdd const& y, pdd const& x);

        bool try_parity(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_parity_diseq(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_mul_bounds(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_factor_equality(pvar x, conflict& core, inequality const& a_l_b);
        bool try_mul_eq_1(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_mul_odd(pvar x, conflict& core, inequality const& axb_l_y);
        bool try_tangent(pvar v, conflict& core, inequality const& c);

        // c := lhs ~ v
        //  where ~ is < or <=
        bool is_l_v(pvar v, inequality const& c);

        // c := v ~ rhs
        bool is_g_v(pvar v, inequality const& c);

        // c := x ~ Y
        bool is_x_l_Y(pvar x, inequality const& c, pdd& y);

        // c := X*y ~ X*Z
        bool is_Xy_l_XZ(pvar y, inequality const& c, pdd& x, pdd& z);
        bool verify_Xy_l_XZ(pvar y, inequality const& c, pdd const& x, pdd const& z);

        // c := Y ~ Ax
        bool is_Y_l_Ax(pvar x, inequality const& c, pdd& a, pdd& y);
        bool verify_Y_l_Ax(pvar x, inequality const& c, pdd const& a, pdd const& y);

        // c := Ax ~ Y
        bool is_Ax_l_Y(pvar x, inequality const& c, pdd& a, pdd& y);
        bool verify_Ax_l_Y(pvar x, inequality const& c, pdd const& a, pdd const& y);

        // c := Ax + B ~ Y
        bool is_AxB_l_Y(pvar x, inequality const& c, pdd& a, pdd& b, pdd& y);
        bool verify_AxB_l_Y(pvar x, inequality const& c, pdd const& a, pdd const& b, pdd const& y);

        // c := Y ~ Ax + B
        bool is_Y_l_AxB(pvar x, inequality const& c, pdd& y, pdd& a, pdd& b);
        bool verify_Y_l_AxB(pvar x, inequality const& c, pdd const& y, pdd const& a, pdd& b);

        // c := Ax + B ~ Y, val(Y) = 0
        bool is_AxB_eq_0(pvar x, inequality const& c, pdd& a, pdd& b, pdd& y);
        bool verify_AxB_eq_0(pvar x, inequality const& c, pdd const& a, pdd const& b, pdd const& y);

        // c := Ax + B != Y, val(Y) = 0
        bool is_AxB_diseq_0(pvar x, inequality const& c, pdd& a, pdd& b, pdd& y);

        // c := Y*X ~ z*X
        bool is_YX_l_zX(pvar z, inequality const& c, pdd& x, pdd& y);
        bool verify_YX_l_zX(pvar z, inequality const& c, pdd const& x, pdd const& y);

        // c := xY <= xZ
        bool is_xY_l_xZ(pvar x, inequality const& c, pdd& y, pdd& z);

        // xy := x * Y
        bool is_xY(pvar x, pdd const& xy, pdd& y);

        // a * b does not overflow
        bool is_non_overflow(pdd const& a, pdd const& b);

        // p := coeff*x*y where coeff_x = coeff*x, x a variable
        bool is_coeffxY(pdd const& coeff_x, pdd const& p, pdd& y);

        bool is_forced_eq(pdd const& p, rational const& val);
        bool is_forced_eq(pdd const& p, int i) { return is_forced_eq(p, rational(i)); }
        
        bool is_forced_diseq(pdd const& p, int i, signed_constraint& c);

        bool is_forced_odd(pdd const& p, signed_constraint& c);

        bool is_forced_false(signed_constraint const& sc);

        bool is_forced_true(signed_constraint const& sc);

    public:
        saturation(solver& s);
        void perform(pvar v, conflict& core);
        bool perform(pvar v, signed_constraint const& sc, conflict& core);
    };

    /*
     * TODO: we could resolve constraints in cjust[v] against each other to
     * obtain stronger propagation. Example:
     *  (x + 1)*P = 0 and (x + 1)*Q = 0, where gcd(P,Q) = 1, then we have x + 1 = 0.
     * We refer to this process as narrowing.
     * In general form it can rely on factoring.
     * Root finding can further prune viable.
     */

}
