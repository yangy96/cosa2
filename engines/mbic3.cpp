/*********************                                                  */
/*! \file mbic3.cpp
** \verbatim
** Top contributors (to current version):
**   Makai Mann, Ahmed Irfan
** This file is part of the pono project.
** Copyright (c) 2019 by the authors listed in the file AUTHORS
** in the top-level source directory) and their institutional affiliations.
** All rights reserved.  See the file LICENSE in the top-level source
** directory for licensing information.\endverbatim
**
** \brief Simple implementation of IC3 using model values
**
**/

#include "engines/mbic3.h"

#include <algorithm>
#include <random>

#include "smt-switch/utils.h"
#include "smt/available_solvers.h"
#include "utils/logger.h"

using namespace smt;
using namespace std;

namespace pono {

static const std::unordered_set<smt::PrimOp> expected_ops(
    { Equal, BVComp, Ge, Gt, Le, Lt, BVUle, BVUlt, BVUge, BVUgt });

static bool disjoint_set_rank(const Term & t1, const Term & t2)
{
  if (!t1->is_value() && !t2->is_value()) {
    return t1 < t2;
  }
  return !t1->is_value();
}

static void split_eq(SmtSolver & solver, const TermVec & in, TermVec & out)
{
  TermVec args;
  for (auto a : in) {
    if (a->get_op().prim_op == PrimOp::Equal) {
      args.clear();
      for (auto aa : a) {
        args.push_back(aa);
      }

      Sort s = args[0]->get_sort();
      SortKind sk = s->get_sort_kind();
      if (sk == SortKind::BOOL) {
        out.push_back(a);  // assert(false);
      } else if (sk == SortKind::BV) {
        out.push_back(solver->make_term(BVUle, args[0], args[1]));
        out.push_back(solver->make_term(BVUle, args[1], args[0]));
      } else if (sk == SortKind::INT || sk == SortKind::REAL) {
        out.push_back(solver->make_term(Le, args[0], args[1]));
        out.push_back(solver->make_term(Le, args[1], args[0]));
      } else {
        out.push_back(a);
      }
    } else {
      out.push_back(a);
    }
  }
}

ModelBasedIC3::ModelBasedIC3(const Property & p, const TransitionSystem & ts,
                             const SmtSolver & slv,
                             PonoOptions opt)
  : super(p, ts, slv, opt)
{
  engine_ = Engine::MBIC3;
  solver_->set_opt("produce-unsat-cores", "true");
}

IC3Formula ModelBasedIC3::get_model_ic3formula(TermVec * out_inputs,
                                               TermVec * out_nexts) const
{
  DisjointSet ds(disjoint_set_rank);
  TermVec cube_lits;
  const UnorderedTermSet & statevars = ts_.statevars();

  if (out_inputs) {
    out_inputs->reserve(ts_.inputvars().size());
  }
  if (out_nexts) {
    out_nexts->reserve(statevars.size());
  }

  for (const auto &v : statevars) {
    Term val = solver_->get_value(v);
    cube_lits.push_back(solver_->make_term(Equal, v, val));
    ds.add(v, val);
    assert(ts_.is_curr_var(v));

    if (out_nexts) {
      Term nv = ts_.next(v);
      out_nexts->push_back(
          solver_->make_term(Equal, nv, solver_->get_value(nv)));
    }
  }

  // add equalities from disjoint set
  for (const auto &v : statevars) {
    Term t = ds.find(v);
    if (t != v) {
      cube_lits.push_back(solver_->make_term(Equal, t, v));
    }
  }

  if (out_inputs) {
    for (const auto &iv : ts_.inputvars()) {
      out_inputs->push_back(
          solver_->make_term(Equal, iv, solver_->get_value(iv)));
    }
  }

  return ic3formula_conjunction(cube_lits);
}

bool ModelBasedIC3::ic3formula_check_valid(const IC3Formula & u) const
{
  const Sort &boolsort = solver_->make_sort(BOOL);
  // check that children are literals
  Op op;
  for (const auto &c : u.children) {
    if (c->get_sort() != boolsort) {
      return false;
    }
  }

  // not checking that they are equalities etc...
  // hard with rewriting
  // but more specifically, it's not always true
  // in this IC3 version, we add the whole bad as a proof goal
  // (or at least the conjuncts in a conjunctive partition of bad)
  // regardless of the shape

  // got through all checks without failing
  return true;
}

vector<IC3Formula> ModelBasedIC3::inductive_generalization(size_t i,
                                                           const IC3Formula & c)
{
  // only need interpolator if in mode 2
  assert(options_.mbic3_indgen_mode == 2 || interpolator_ == nullptr);

  assert(!c.is_disjunction());
  vector<IC3Formula> gen_res;

  if (options_.ic3_indgen_) {
    if (options_.mbic3_indgen_mode == 0) {
      UnorderedTermSet keep, core_set;
      TermVec bool_assump, tmp, new_tmp, removed, lits;
      split_eq(solver_, c.children, lits);

      if (options_.random_seed_ > 0) {
        shuffle(lits.begin(), lits.end(),
                default_random_engine(options_.random_seed_));
      }

      int iter = 0;
      bool progress = true;
      while (iter <= options_.ic3_gen_max_iter_ && lits.size() > 1 &&
             progress) {
        iter = options_.ic3_gen_max_iter_ > 0 ? iter+1 : iter;
        size_t prev_size = lits.size();
        for (const auto &a : lits) {
          // check if we can drop a
          if (keep.find(a) != keep.end()) {
            continue;
          }
          tmp.clear();
          for (const auto &aa : lits) {
            if (a != aa) {
              tmp.push_back(aa);
            }
          }

          Term tmp_and_term = make_and(tmp);
          if (!check_intersects_initial(tmp_and_term)) {
            assert(solver_context_ == 0);
            push_solver_context();
            assert_frame_labels(i - 1);
            assert_trans_label();
            solver_->assert_formula(solver_->make_term(Not, tmp_and_term));

            Term l;
            bool_assump.clear();
            for (const auto &t : tmp) {
              l = label(t);
              solver_->assert_formula(
                  solver_->make_term(Implies, l, ts_.next(t)));
              bool_assump.push_back(l);
            }

            Result r = check_sat_assuming(bool_assump);
            assert(!r.is_unknown());

            if (r.is_sat()) {
              // we cannot drop a
              pop_solver_context();
            } else {
              new_tmp.clear();
              removed.clear();
              core_set.clear();
              // filter using unsatcore
              solver_->get_unsat_core(core_set);
              for (size_t j = 0; j < bool_assump.size(); ++j) {
                if (core_set.find(bool_assump[j]) != core_set.end()) {
                  new_tmp.push_back(tmp[j]);
                } else {
                  removed.push_back(tmp[j]);
                }
              }

              pop_solver_context();

              // keep in mind that you cannot drop a literal if it causes c to
              // intersect with the initial states
              size_t size = new_tmp.size();
              fix_if_intersects_initial(new_tmp, removed);
              // remember the literals which cannot be dropped
              for (size_t i = size; i < new_tmp.size(); ++i) {
                keep.insert(new_tmp[i]);
              }

              lits = new_tmp;
              break;  // next iteration
            }
          }
        }

        progress = lits.size() < prev_size;
      }

      gen_res.push_back(ic3formula_negate(ic3formula_conjunction(lits)));

    } else if (options_.mbic3_indgen_mode == 1) {
      TermVec tmp, lits, red_lits;
      for (const auto &a : c.children) {
        tmp.push_back(ts_.next(a));
      }
      split_eq(solver_, tmp, lits);

      // ( (frame /\ trans /\ not(c)) \/ init') /\ c' is unsat
      Term formula = make_and({ get_frame_term(i - 1),
                                ts_.trans(),
                                solver_->make_term(Not, c.term) });
      formula = solver_->make_term(Or, formula, ts_.next(ts_.init()));
      reducer_.reduce_assump_unsatcore(formula, lits, red_lits, NULL,
                                       options_.ic3_gen_max_iter_,
                                       options_.random_seed_);
      TermVec curr_lits;
      curr_lits.reserve(red_lits.size());
      for (const auto &l : red_lits) {
        curr_lits.push_back(ts_.curr(l));
      }
      gen_res.push_back(ic3formula_negate(ic3formula_conjunction(curr_lits)));
    } else if (options_.mbic3_indgen_mode == 2) {
      // TODO: consider creating a separate derived class for this mode
      interpolator_->reset_assertions();

      TermVec conjuncts;
      split_eq(solver_, c.children, conjuncts);

      // ( (frame /\ trans /\ not(c)) \/ init') /\ c' is unsat
      Term formula = make_and({ get_frame_term(i - 1),
                                ts_.trans(),
                                solver_->make_term(Not, make_and(conjuncts)) });
      formula = solver_->make_term(Or, formula, ts_.next(ts_.init()));

      Term int_A = to_interpolator_->transfer_term(formula, BOOL);
      // still use c in B
      // only split equalities in A to encourage more general unsat proofs /
      // interpolants
      Term int_B = to_interpolator_->transfer_term(ts_.next(c.term), BOOL);

      Term interp;
      Result r = interpolator_->get_interpolant(int_A, int_B, interp);
      assert(r.is_unsat());

      Term solver_interp = to_solver_->transfer_term(interp);
      logger.log(3, "Got interpolant: {}", solver_interp);
      TermVec interp_conjuncts;
      conjunctive_partition(solver_interp, interp_conjuncts, true);

      for (const auto &c : interp_conjuncts) {
        // these will not necessarily be a disjunction actually, they'll just be
        // a single formula, {c} from the conjunctive partition of the
        // interpolant
        // --> interpolant not guaranteed to be a clause
        gen_res.push_back(ic3formula_disjunction({ ts_.curr(c) }));
      }

    } else {
      assert(false);
    }
  }

  assert(gen_res.size());
  return gen_res;
}

IC3Formula ModelBasedIC3::generalize_predecessor(size_t i, const IC3Formula & c)
{
  assert(solver_context_  == 1);  // shouldn't use solver, solving all in
                                  // reducer_
  DisjointSet ds(disjoint_set_rank);
  UnorderedTermMap model;
  const UnorderedTermSet & statevars = ts_.statevars();

  TermVec cube_lits;
  cube_lits.reserve(statevars.size());
  TermVec next_lits;
  next_lits.reserve(statevars.size());

  for (const auto &v : statevars) {
    Term val = solver_->get_value(v);
    cube_lits.push_back(solver_->make_term(Equal, v, val));
    ds.add(v, val);
    assert(ts_.is_curr_var(v));
    assert(model.find(v) == model.end());
    model[v] = val;

    Term nv = ts_.next(v);
    assert(ts_.is_next_var(nv));
    Term next_val = solver_->get_value(nv);
    next_lits.push_back(solver_->make_term(Equal, nv, next_val));
    assert(model.find(nv) == model.end());
    model[nv] = next_val;
  }

  // collect input assignments
  const UnorderedTermSet & inputvars = ts_.inputvars();
  TermVec input_lits;
  input_lits.reserve(inputvars.size());
  for (const auto &v : inputvars) {
    Term val = solver_->get_value(v);
    input_lits.push_back(solver_->make_term(Equal, v, val));
    assert(model.find(v) == model.end());
    model[v] = val;
  }

  // if not generalized, the current state assignments in cube_lits
  // are the predecessor
  IC3Formula res = ic3formula_conjunction(cube_lits);

  assert(i > 0);
  if (i == 1) {
    // don't need to generalize if i == 1
    // the predecessor is an initial state
    return res;
  }

  if (options_.ic3_pregen_ && !options_.ic3_functional_preimage_) {
    // add congruent equalities to cube_lits
    for (const auto &v : statevars) {
      Term t = ds.find(v);
      if (t != v) {
        cube_lits.push_back(solver_->make_term(Equal, t, v));
      }
    }

    Term formula = make_and(input_lits);

    if (ts_.is_deterministic()) {
      // NOTE: reducer doesn't have semantics for trans_label_
      // better to just use whole trans for now
      // Optionally in the future we could add those semantics
      // to the reducer_'s solver
      formula = solver_->make_term(And, formula, ts_.trans());
      formula = solver_->make_term(
          And, formula, solver_->make_term(Not, ts_.next(c.term)));
    } else {
      formula = solver_->make_term(And, formula, make_and(next_lits));

      // preimage formula
      // NOTE: because this will be negated, it is important to use the
      // whole frame and trans, not just the labels
      // because label is: trans_label_ -> trans
      // so if it is negated, that doesn't force trans to be false
      // the implication could be more efficient than iff so we want to leave it
      // that way
      Term pre_formula = get_frame_term(i - 1);
      pre_formula = solver_->make_term(And, pre_formula, ts_.trans());
      pre_formula =
          solver_->make_term(And, pre_formula, solver_->make_term(Not, c.term));
      pre_formula = solver_->make_term(And, pre_formula, ts_.next(c.term));

      formula = solver_->make_term(
          And, formula, solver_->make_term(Not, pre_formula));
    }

    TermVec splits, red_cube_lits, rem_cube_lits;
    split_eq(solver_, cube_lits, splits);
    reducer_.reduce_assump_unsatcore(formula, splits, red_cube_lits,
                                     &rem_cube_lits,
                                     options_.ic3_gen_max_iter_,
                                     options_.random_seed_);
    // should need some assumptions
    // formula should not be unsat on its own
    assert(red_cube_lits.size() > 0);

    // update res to the generalization
    res = ic3formula_conjunction(red_cube_lits);

  } else if (options_.ic3_pregen_ && options_.ic3_functional_preimage_) {
    assert(ts_.is_deterministic());

    UnorderedTermMap m;
    for (const auto &v : inputvars) {
      m[v] = model.at(v);
    }
    for (const auto &v : statevars) {
      Term nv = ts_.next(v);
      m[nv] = model.at(nv);
    }

    Term fun_preimage = solver_->substitute(trans_label_, m);
    TermVec conjuncts;
    conjunctive_partition(fun_preimage, conjuncts, true);
    res = ic3formula_conjunction(conjuncts);
  }
  return res;
}

void ModelBasedIC3::check_ts() const
{
  // check if there are arrays or uninterpreted sorts and fail if so
  for (const auto &vec : { ts_.statevars(), ts_.inputvars() }) {
    for (const auto &st : vec) {
      SortKind sk = st->get_sort()->get_sort_kind();
      if (sk == ARRAY) {
        throw PonoException("ModelBasedIC3 does not support arrays yet");
      } else if (sk == UNINTERPRETED) {
        throw PonoException(
            "ModelBasedIC3 does not support uninterpreted sorts yet.");
      }
    }
  }
}

bool ModelBasedIC3::intersects_bad()
{
  push_solver_context();
  // assert the last frame (conjunction over clauses)
  assert_frame_labels(reached_k_ + 1);
  // see if it intersects with bad
  solver_->assert_formula(bad_);
  Result r = check_sat();

  if (r.is_sat()) {
    // push bad as a proof goal
    TermVec conjuncts;
    conjunctive_partition(bad_, conjuncts, true);
    const IC3Formula &bad_at_last_frame = ic3formula_conjunction(conjuncts);
    add_proof_goal(bad_at_last_frame, reached_k_ + 1, NULL);
  }

  pop_solver_context();

  assert(!r.is_unknown());
  return r.is_sat();
}

void ModelBasedIC3::initialize()
{
  super::initialize();

  // only need interpolator infrastructure for mode 2 (interpolation)
  if (options_.mbic3_indgen_mode == 2) {
    interpolator_ = create_interpolating_solver(MSAT_INTERPOLATOR);
    to_interpolator_ = std::make_unique<TermTranslator>(interpolator_);
    to_solver_ = std::make_unique<TermTranslator>(solver_);

    UnorderedTermMap & cache = to_solver_->get_cache();
    Term ns;
    for (const auto &s : ts_.statevars()) {
      // common variables are next states, unless used for refinement in IC3IA
      // then will refer to current state variables after untiming
      // need to cache both
      cache[to_interpolator_->transfer_term(s)] = s;
      ns = ts_.next(s);
      cache[to_interpolator_->transfer_term(ns)] = ns;
    }

    // need to add uninterpreted functions as well
    // first need to find them all
    // NOTE need to use get_free_symbols NOT get_free_symbolic_consts
    // because the latter ignores uninterpreted functions
    UnorderedTermSet free_symbols;
    get_free_symbols(ts_.init(), free_symbols);
    get_free_symbols(ts_.trans(), free_symbols);
    get_free_symbols(bad_, free_symbols);

    for (const auto &s : free_symbols) {
      assert(s->is_symbol());
      if (s->is_symbolic_const()) {
        // ignore constants
        continue;
      }
      cache[to_interpolator_->transfer_term(s)] = s;
    }
  }
}

}  // namespace pono
