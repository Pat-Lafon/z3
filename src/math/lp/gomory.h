/*++
Copyright (c) 2017 Microsoft Corporation

Module Name:

    <name>

Abstract:

    <abstract>

Author:
    Nikolaj Bjorner (nbjorner)
    Lev Nachmanson (levnach)

Revision History:
--*/
#pragma once
#include "math/lp/lar_term.h"
#include "math/lp/lia_move.h"
#include "math/lp/explanation.h"
#include "math/lp/static_matrix.h"

namespace lp {
    class int_solver;
    class lar_solver;
    class gomory {
        class int_solver& lia;
        class lar_solver& lra;
        lia_move cut(lar_term & t, mpq & k, explanation* ex, unsigned basic_inf_int_j, const row_strip<mpq>& row);
    public:
        gomory(int_solver& lia);
        lia_move get_cut(lpvar j);
    };
}
