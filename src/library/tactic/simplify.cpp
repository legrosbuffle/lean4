/*
Copyright (c) 2015 Daniel Selsam. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Author: Daniel Selsam, Leonardo de Moura
*/
#include <functional>
#include <iostream>
#include "util/flet.h"
#include "util/freset.h"
#include "util/pair.h"
#include "util/optional.h"
#include "util/interrupt.h"
#include "util/sexpr/option_declarations.h"
#include "kernel/abstract.h"
#include "kernel/expr_maps.h"
#include "kernel/find_fn.h"
#include "kernel/instantiate.h"
#include "library/trace.h"
#include "library/constants.h"
#include "library/normalize.h"
#include "library/expr_lt.h"
#include "library/locals.h"
#include "library/num.h"
#include "library/util.h"
#include "library/norm_num.h"
#include "library/attribute_manager.h"
#include "library/defeq_canonizer.h"
#include "library/relation_manager.h"
#include "library/app_builder.h"
#include "library/congr_lemma.h"
#include "library/fun_info.h"
#include "library/vm/vm_expr.h"
#include "library/vm/vm_option.h"
#include "library/vm/vm_list.h"
#include "library/vm/vm_name.h"
#include "library/tactic/tactic_state.h"
#include "library/tactic/ac_tactics.h"
#include "library/tactic/app_builder_tactics.h"
#include "library/tactic/simp_lemmas.h"
#include "library/tactic/simplify.h"

#ifndef LEAN_DEFAULT_SIMPLIFY_MAX_STEPS
#define LEAN_DEFAULT_SIMPLIFY_MAX_STEPS 1000000
#endif
#ifndef LEAN_DEFAULT_SIMPLIFY_CONTEXTUAL
#define LEAN_DEFAULT_SIMPLIFY_CONTEXTUAL true
#endif
#ifndef LEAN_DEFAULT_SIMPLIFY_REWRITE
#define LEAN_DEFAULT_SIMPLIFY_REWRITE true
#endif
#ifndef LEAN_DEFAULT_SIMPLIFY_LIFT_EQ
#define LEAN_DEFAULT_SIMPLIFY_LIFT_EQ true
#endif
#ifndef LEAN_DEFAULT_SIMPLIFY_DEFEQ_CANONIZE_INSTANCES_FIXED_POINT
#define LEAN_DEFAULT_SIMPLIFY_DEFEQ_CANONIZE_INSTANCES_FIXED_POINT false
#endif
#ifndef LEAN_DEFAULT_SIMPLIFY_DEFEQ_CANONIZE_PROOFS_FIXED_POINT
#define LEAN_DEFAULT_SIMPLIFY_DEFEQ_CANONIZE_PROOFS_FIXED_POINT false
#endif

namespace lean {
#define lean_simp_trace(CTX, N, CODE) lean_trace(N, scope_trace_env _scope1(CTX.env(), CTX); CODE)

/* -----------------------------------
   Core simplification procedure.
   ------------------------------------ */

simp_result simplify_core_fn::join(simp_result const & r1, simp_result const & r2) {
    return ::lean::join(m_ctx, m_rel, r1, r2);
}

void simplify_core_fn::inc_num_steps() {
    m_num_steps++;
    if (m_num_steps > m_max_steps)
        throw exception("simplify failed, maximum number of steps exceeded");
}

bool simplify_core_fn::is_dependent_fn(expr const & f) {
    expr f_type = m_ctx.whnf(m_ctx.infer(f));
    lean_assert(is_pi(f_type));
    return !is_arrow(f_type);
}

bool simplify_core_fn::instantiate_emetas(tmp_type_context & tmp_ctx, unsigned num_emeta,
                                          list<expr> const & emetas, list<bool> const & instances) {
    bool failed = false;
    unsigned i  = num_emeta;
    for_each2(emetas, instances, [&](expr const & mvar, bool const & is_instance) {
            i--;
            if (failed) return;
            expr mvar_type = tmp_ctx.instantiate_mvars(tmp_ctx.infer(mvar));
            if (has_metavar(mvar_type)) {
                failed = true;
                return;
            }

            if (tmp_ctx.is_eassigned(i)) return;

            if (is_instance) {
                if (auto v = m_ctx.mk_class_instance(mvar_type)) {
                    if (!tmp_ctx.is_def_eq(mvar, *v)) {
                        lean_simp_trace(tmp_ctx, name({"simplify", "failure"}),
                                        tout() << "unable to assign instance for: " << mvar_type << "\n";);
                        failed = true;
                        return;
                    }
                } else {
                    lean_simp_trace(tmp_ctx, name({"simplify", "failure"}),
                                    tout() << "unable to synthesize instance for: " << mvar_type << "\n";);
                    failed = true;
                    return;
                }
            }

            if (tmp_ctx.is_eassigned(i)) return;

            if (m_ctx.is_prop(mvar_type)) {
                if (auto pf = prove(mvar_type)) {
                    lean_verify(tmp_ctx.is_def_eq(mvar, *pf));
                    return;
                }
            }

            lean_simp_trace(tmp_ctx, name({"simplify", "failure"}),
                            tout() << "failed to assign: " << mvar << " : " << mvar_type << "\n";);

            failed = true;
            return;
        });
    return !failed;
}

simp_result simplify_core_fn::lift_from_eq(simp_result const & r_eq) {
    if (!r_eq.has_proof()) return r_eq;
    expr new_pr = ::lean::lift_from_eq(m_ctx, m_rel, r_eq.get_proof());
    return simp_result(r_eq.get_new(), new_pr);
}

simp_lemmas simplify_core_fn::add_to_slss(simp_lemmas const & _slss, buffer<expr> const & ls) {
    simp_lemmas slss = _slss;
    for (unsigned i = 0; i < ls.size(); i++) {
        expr const & l = ls[i];
        try {
            slss = add(m_ctx, slss, mlocal_name(l), m_ctx.infer(l), l, LEAN_DEFAULT_PRIORITY);
            lean_simp_trace(m_ctx, name({"simplify", "context"}),
                            tout() << mlocal_name(l) << " : " << m_ctx.infer(l) << "\n";);
        } catch (exception & e) {}
    }
    return slss;
}

/* Given the application 'e', remove unnecessary casts of the form (eq.nrec a rfl) and (eq.drec a rfl) */
expr simplify_core_fn::remove_unnecessary_casts(expr const & e) {
    buffer<expr> args;
    expr f = get_app_args(e, args);
    ss_param_infos ss_infos = get_specialized_subsingleton_info(m_ctx, e);
    int i = -1;
    bool modified = false;
    for (ss_param_info const & ss_info : ss_infos) {
        i++;
        if (ss_info.is_subsingleton()) {
            while (is_constant(get_app_fn(args[i]))) {
                buffer<expr> cast_args;
                expr f_cast = get_app_args(args[i], cast_args);
                name n_f = const_name(f_cast);
                if (n_f == get_eq_rec_name() || n_f == get_eq_drec_name() || n_f == get_eq_nrec_name()) {
                    lean_assert(cast_args.size() == 6);
                    expr major_premise = cast_args[5];
                    expr f_major_premise = get_app_fn(major_premise);
                    if (is_constant(f_major_premise) && const_name(f_major_premise) == get_eq_refl_name()) {
                        args[i] = cast_args[3];
                        modified = true;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
        }
    }
    return modified ? mk_app(f, args) : e;
}

expr simplify_core_fn::defeq_canonize_args_step(expr const & e) {
    buffer<expr> args;
    bool modified = false;
    expr f        = get_app_args(e, args);
    fun_info info = get_fun_info(m_ctx, f, args.size());
    unsigned i    = 0;
    for (param_info const & pinfo : info.get_params_info()) {
        lean_assert(i < args.size());
        expr new_a;
        if ((m_canonize_instances && pinfo.is_inst_implicit()) || (m_canonize_proofs && pinfo.is_prop())) {
            new_a = ::lean::defeq_canonize(m_ctx, args[i], m_need_restart);
            lean_simp_trace(m_ctx, name({"simplify", "canonize"}),
                            tout() << "\n" << args[i] << "\n==>\n" << new_a << "\n";);
            if (new_a != args[i]) {
                modified = true;
                args[i] = new_a;
            }
        }
        i++;
    }

    return modified ? mk_app(f, args) : e;
}

/* Try user defined congruence lemmas */
simp_result simplify_core_fn::try_user_congrs(expr const & e) {
    simp_lemmas_for const * sls = m_slss.find(m_rel);
    if (!sls) return simp_result(e);

    list<simp_lemma> const * cls = sls->find_congr(e);
    if (!cls) return simp_result(e);

    for (simp_lemma const & cl : *cls) {
        simp_result r = try_user_congr(e, cl);
        if (r.get_new() != e)
            return r;
    }
    return simp_result(e);
}

simp_result simplify_core_fn::try_user_congr(expr const & e, simp_lemma const & cl) {
    tmp_type_context tmp_ctx(m_ctx, cl.get_num_umeta(), cl.get_num_emeta());
    if (!tmp_ctx.is_def_eq(e, cl.get_lhs()))
        return simp_result(e);

    lean_simp_trace(tmp_ctx, name({"debug", "simplify", "try_congruence"}),
                    tout() << "(" << cl.get_id() << ") " << e << "\n";);

    bool simplified = false;

    buffer<expr> congr_hyps;
    to_buffer(cl.get_congr_hyps(), congr_hyps);

    buffer<simp_result> congr_hyp_results;
    buffer<type_context::tmp_locals> factories;
    buffer<name> relations;
    for (expr const & m : congr_hyps) {
        factories.emplace_back(m_ctx);
        type_context::tmp_locals & local_factory = factories.back();
        expr m_type = tmp_ctx.instantiate_mvars(tmp_ctx.infer(m));

        while (is_pi(m_type)) {
            expr d = instantiate_rev(binding_domain(m_type), local_factory.as_buffer().size(),
                                     local_factory.as_buffer().data());
            expr l = local_factory.push_local(binding_name(m_type), d, binding_info(m_type));
            lean_assert(!has_metavar(l));
            m_type = binding_body(m_type);
        }
        m_type = instantiate_rev(m_type, local_factory.as_buffer().size(), local_factory.as_buffer().data());

        expr h_rel, h_lhs, h_rhs;
        lean_verify(is_simp_relation(tmp_ctx.env(), m_type, h_rel, h_lhs, h_rhs) && is_constant(h_rel));
        {
            relations.push_back(const_name(h_rel));
            flet<simp_lemmas> set_slss(m_slss, m_contextual ? add_to_slss(m_slss, local_factory.as_buffer()) : m_slss);

            h_lhs = tmp_ctx.instantiate_mvars(h_lhs);

            if (m_contextual || m_rel != const_name(h_rel)) {
                flet<name> set_name(m_rel, const_name(h_rel));
                freset<simplify_cache> reset_cache(m_cache);
                congr_hyp_results.emplace_back(visit(h_lhs, some_expr(e)));
            } else {
                congr_hyp_results.emplace_back(visit(h_lhs, some_expr(e)));
            }
            simp_result const & r_congr_hyp = congr_hyp_results.back();

            if (r_congr_hyp.has_proof())
                simplified = true;

            lean_assert(is_meta(h_rhs));
            buffer<expr> new_val_meta_args;
            expr new_val_meta = get_app_args(h_rhs, new_val_meta_args);
            lean_assert(is_metavar(new_val_meta));
            expr new_val = tmp_ctx.mk_lambda(new_val_meta_args, r_congr_hyp.get_new());
            tmp_ctx.assign(new_val_meta, new_val);
        }
    }

    if (!simplified)
        return simp_result(e);

    lean_assert(congr_hyps.size() == congr_hyp_results.size());
    for (unsigned i = 0; i < congr_hyps.size(); ++i) {
        expr const & pf_meta = congr_hyps[i];
        simp_result const & r_congr_hyp = congr_hyp_results[i];
        name const & rel = relations[i];
        type_context::tmp_locals & local_factory = factories[i];
        expr hyp = finalize(m_ctx, rel, r_congr_hyp).get_proof();
        // This is the current bottleneck
        // Can address somewhat by keeping the proofs as small as possible using macros
        expr pf = local_factory.mk_lambda(hyp);
        tmp_ctx.assign(pf_meta, pf);
    }

    if (!instantiate_emetas(tmp_ctx, cl.get_num_emeta(), cl.get_emetas(), cl.get_instances()))
        return simp_result(e);

    for (unsigned i = 0; i < cl.get_num_umeta(); i++) {
        if (!tmp_ctx.is_uassigned(i))
            return simp_result(e);
    }

    expr e_s = tmp_ctx.instantiate_mvars(cl.get_rhs());
    expr pf = tmp_ctx.instantiate_mvars(cl.get_proof());

    simp_result r(e_s, pf);

    lean_simp_trace(tmp_ctx, name({"simplify", "congruence"}),
                    tout() << "(" << cl.get_id() << ") "
                    << "[" << e << " ==> " << e_s << "]\n";);

    return r;
}

/* Try to use congruence lemmas generated by the congr_lemma module.
   \remark these lemmas are for the equality relation. */
optional<simp_result> simplify_core_fn::try_auto_eq_congr(expr const & e) {
    lean_assert(m_rel == get_eq_name());
    lean_assert(is_app(e));
    buffer<expr> args;
    expr f = get_app_args(e, args);
    auto congr_lemma = mk_specialized_congr_simp(m_ctx, e);
    if (!congr_lemma) return optional<simp_result>();

    buffer<simp_result> r_args;
    buffer<expr>        new_args;
    bool has_proof = false;
    bool has_cast = false;
    bool has_simplified = false;

    unsigned i = 0;

    // First pass, try to simplify all the Eq arguments
    for (congr_arg_kind const & ckind : congr_lemma->get_arg_kinds()) {
        switch (ckind) {
        case congr_arg_kind::HEq:
            lean_unreachable();
        case congr_arg_kind::Fixed:
        case congr_arg_kind::FixedNoParam:
            new_args.emplace_back(args[i]);
            break;
        case congr_arg_kind::Eq:
            {
                r_args.emplace_back(visit(args[i], some_expr(e)));
                simp_result & r_arg = r_args.back();
                new_args.emplace_back(r_arg.get_new());
                if (r_arg.has_proof())
                    has_proof = true;
                if (r_arg.get_new() != args[i])
                    has_simplified = true;
            }
            break;
        case congr_arg_kind::Cast:
            has_cast = true;
            new_args.emplace_back(args[i]);
            break;
        }
        i++;
    }

    if (!has_simplified) {
        simp_result r = simp_result(e);
        return optional<simp_result>(r);
    }

    if (!has_proof) {
        simp_result r = simp_result(mk_app(f, new_args));
        return optional<simp_result>(r);
    }

    // We have a proof, so we need to build the congruence lemma
    expr proof = congr_lemma->get_proof();
    expr type = congr_lemma->get_type();
    buffer<expr> subst;

    i = 0;
    unsigned i_eq = 0;
    for (congr_arg_kind const & ckind : congr_lemma->get_arg_kinds()) {
        switch (ckind) {
        case congr_arg_kind::HEq:
            lean_unreachable();
        case congr_arg_kind::Fixed:
            proof = mk_app(proof, args[i]);
            subst.push_back(args[i]);
            type = binding_body(type);
            break;
        case congr_arg_kind::FixedNoParam:
            break;
        case congr_arg_kind::Eq:
            proof = mk_app(proof, args[i]);
            subst.push_back(args[i]);
            type = binding_body(type);
            {
                simp_result r_arg = finalize(m_ctx, m_rel, r_args[i_eq]);
                proof = mk_app(proof, r_arg.get_new(), r_arg.get_proof());
                subst.push_back(r_arg.get_new());
                subst.push_back(r_arg.get_proof());
            }
            type = binding_body(binding_body(type));
            i_eq++;
            break;
        case congr_arg_kind::Cast:
            lean_assert(has_cast);
            proof = mk_app(proof, args[i]);
            subst.push_back(args[i]);
            type = binding_body(type);
            break;
        }
        i++;
    }
    lean_assert(is_eq(type));
    expr rhs   = instantiate_rev(app_arg(type), subst.size(), subst.data());
    simp_result r(rhs, proof);

    if (has_cast) {
        r.update(remove_unnecessary_casts(r.get_new()));
    }

    return optional<simp_result>(r);
}

simp_result simplify_core_fn::congr_fun_arg(simp_result const & r_f, simp_result const & r_arg) {
    if (!r_f.has_proof() && !r_arg.has_proof()) return simp_result(mk_app(r_f.get_new(), r_arg.get_new()));
    else if (!r_f.has_proof()) return congr_arg(r_f.get_new(), r_arg);
    else if (!r_arg.has_proof()) return congr_fun(r_f, r_arg.get_new());
    else return congr(r_f, r_arg);
}

simp_result simplify_core_fn::congr(simp_result const & r_f, simp_result const & r_arg) {
    lean_assert(r_f.has_proof() && r_arg.has_proof());
    // theorem congr {A B : Type} {f₁ f₂ : A → B} {a₁ a₂ : A} (H₁ : f₁ = f₂) (H₂ : a₁ = a₂) : f₁ a₁ = f₂ a₂
    expr e  = mk_app(r_f.get_new(), r_arg.get_new());
    expr pf = mk_congr(m_ctx, r_f.get_proof(), r_arg.get_proof());
    return simp_result(e, pf);
}

simp_result simplify_core_fn::congr_fun(simp_result const & r_f, expr const & arg) {
    lean_assert(r_f.has_proof());
    // theorem congr_fun {A : Type} {B : A → Type} {f g : Π x, B x} (H : f = g) (a : A) : f a = g a
    expr e  = mk_app(r_f.get_new(), arg);
    expr pf = mk_congr_fun(m_ctx, r_f.get_proof(), arg);
    return simp_result(e, pf);
}

simp_result simplify_core_fn::congr_arg(expr const & f, simp_result const & r_arg) {
    lean_assert(r_arg.has_proof());
    // theorem congr_arg {A B : Type} {a₁ a₂ : A} (f : A → B) : a₁ = a₂ → f a₁ = f a₂
    expr e  = mk_app(f, r_arg.get_new());
    expr pf = mk_congr_arg(m_ctx, f, r_arg.get_proof());
    return simp_result(e, pf);
}

simp_result simplify_core_fn::congr_funs(simp_result const & r_f, buffer<expr> const & args) {
    expr e = r_f.get_new();
    for (unsigned i = 0; i < args.size(); ++i) {
        e  = mk_app(e, args[i]);
    }
    if (!r_f.has_proof())
        return simp_result(e);
    expr pf = r_f.get_proof();
    for (unsigned i = 0; i < args.size(); ++i) {
        pf = mk_congr_fun(m_ctx, pf, args[i]);
    }
    return simp_result(e, pf);
}

simp_result simplify_core_fn::rewrite(expr const & e) {
    simp_lemmas_for const * sr = m_slss.find(m_rel);
    if (!sr) return simp_result(e);

    list<simp_lemma> const * srs = sr->find(e);
    if (!srs) {
        lean_trace_d(name({"debug", "simplify", "try_rewrite"}), tout() << "no simp lemmas for: " << e << "\n";);
        return simp_result(e);
    }

    for (simp_lemma const & lemma : *srs) {
        simp_result r = rewrite(e, lemma);
        if (!is_eqp(r.get_new(), e)) {
            lean_trace_d(name({"simplify", "rewrite"}), tout() << "[" << lemma.get_id() << "]: " << e
                         << " ==> " << r.get_new() << "\n";);
            return r;
        }
    }

    return simp_result(e);
}

simp_result simplify_core_fn::rewrite(expr const & e, simp_lemma const & sl) {
    tmp_type_context tmp_ctx(m_ctx, sl.get_num_umeta(), sl.get_num_emeta());
    if (!tmp_ctx.is_def_eq(e, sl.get_lhs())) {
        lean_trace_d(name({"debug", "simplify", "rewrite"}), tout() << "fail to unify: " << sl.get_id() << "\n";);
        return simp_result(e);
    }

    if (!instantiate_emetas(tmp_ctx, sl.get_num_emeta(), sl.get_emetas(), sl.get_instances())) {
        lean_trace_d(name({"debug", "simplify", "rewrite"}), tout() << "fail to instantiate emetas: " <<
                     sl.get_id() << "\n";);
        return simp_result(e);
    }

    for (unsigned i = 0; i < sl.get_num_umeta(); i++) {
        if (!tmp_ctx.is_uassigned(i))
            return simp_result(e);
    }

    expr new_lhs = tmp_ctx.instantiate_mvars(sl.get_lhs());
    expr new_rhs = tmp_ctx.instantiate_mvars(sl.get_rhs());
    if (sl.is_permutation()) {
        if (!is_lt(new_rhs, new_lhs, false)) {
            lean_simp_trace(tmp_ctx, name({"simplify", "perm"}),
                            tout() << "perm rejected: " << new_rhs << " !< " << new_lhs << "\n";);
            return simp_result(e);
        }
    }

    if (sl.is_refl()) {
        return simp_result(new_rhs);
    } else {
        expr pf = tmp_ctx.instantiate_mvars(sl.get_proof());
        return simp_result(new_rhs, pf);
    }
}

simp_result simplify_core_fn::visit(expr const & e, optional<expr> const & parent) {
    check_system("simplify");
    inc_num_steps();
    lean_trace_inc_depth("simplify");
    lean_simp_trace(m_ctx, "simplify", tout() << m_rel << ": " << e << "\n";);

    auto it = m_cache.find(e);
    if (it != m_cache.end())
        return it->second;

    simp_result curr_result(e);
    if (auto r1 = pre(e, parent)) {
        if (!r1->second) {
            m_cache.insert(mk_pair(e, r1->first));
            return r1->first;
        }
        curr_result = r1->first;
    }

    while (true) {
        simp_result new_result;
        switch (curr_result.get_new().kind()) {
        case expr_kind::Local:
        case expr_kind::Meta:
        case expr_kind::Sort:
        case expr_kind::Constant:
        case expr_kind::Macro:
            new_result = curr_result;
            break;
        case expr_kind::Var:
            lean_unreachable();
        case expr_kind::Lambda:
            new_result = join(curr_result, visit_lambda(curr_result.get_new()));
            break;
        case expr_kind::Pi:
            new_result = join(curr_result, visit_pi(curr_result.get_new()));
            break;
        case expr_kind::App:
            new_result = join(curr_result, visit_app(curr_result.get_new()));
            break;
        case expr_kind::Let:
            new_result = join(curr_result, visit_let(curr_result.get_new()));
            break;
        }

        if (auto r2 = post(new_result.get_new(), parent)) {
            if (!r2->second) {
                curr_result = join(new_result, r2->first);
                break;
            } else if (r2->first.get_new() == curr_result.get_new()) {
                break;
            } else {
                /* continue simplifying */
                curr_result = join(new_result, r2->first);
            }
        } else {
            curr_result = new_result;
            break;
        }
    }

    if (m_lift_eq && m_rel != get_eq_name()) {
        simp_result eq_result;
        {
            flet<name> use_eq(m_rel, get_eq_name());
            freset<simplify_cache> reset_cache(m_cache);
            eq_result = visit(curr_result.get_new(), parent);
        }
        if (eq_result.get_new() != curr_result.get_new()) {
            curr_result = join(curr_result, lift_from_eq(eq_result));
            curr_result = join(curr_result, visit(curr_result.get_new(), parent));
        }
    }

    m_cache.insert(mk_pair(e, curr_result));
    return curr_result;
}

simp_result simplify_core_fn::visit_fn(expr const & e) {
    lean_assert(m_rel == get_eq_name());
    lean_assert(is_app(e));
    buffer<expr> args;
    expr const & f = get_app_args(e, args);
    simp_result r_f = visit(f, some_expr(e));
    return congr_funs(r_f, args);
}

simp_result simplify_core_fn::visit_app(expr const & _e) {
    lean_assert(is_app(_e));
    expr e = should_defeq_canonize() ? defeq_canonize_args_step(_e) : _e;

    // (1) Try user-defined congruences
    simp_result r_user = try_user_congrs(e);
    if (r_user.has_proof()) {
        if (m_rel == get_eq_name()) {
            return join(r_user, visit_fn(r_user.get_new()));
        } else {
            return r_user;
        }
    }

    if (m_rel == get_eq_name()) {
        // (2) Synthesize congruence lemma
        optional<simp_result> r_args = try_auto_eq_congr(e);
        if (r_args) return join(*r_args, visit_fn(r_args->get_new()));

        // (3) Fall back on generic binary congruence
        expr const & f = app_fn(e);
        expr const & arg = app_arg(e);

        simp_result r_f = visit(f, some_expr(e));

        if (is_dependent_fn(f)) {
            if (r_f.has_proof()) return congr_fun(r_f, arg);
            else return mk_app(r_f.get_new(), arg);
        } else {
            simp_result r_arg = visit(arg, some_expr(e));
            return congr_fun_arg(r_f, r_arg);
        }
    }

    return simp_result(e);
}

optional<expr> simplify_core_fn::prove(expr const &) {
    return none_expr();
}

simp_result simplify_core_fn::visit_lambda(expr const & e) {
    return simp_result(e);
}

simp_result simplify_core_fn::visit_pi(expr const & e) {
    return try_user_congrs(e);
}

simp_result simplify_core_fn::visit_let(expr const & e) {
    return simp_result(e);
}

simp_result simplify_core_fn::visit_macro(expr const & e) {
    return simp_result(e);
}

optional<pair<simp_result, bool>> simplify_core_fn::pre(expr const &, optional<expr> const &) {
    return optional<pair<simp_result, bool>>();
}

optional<pair<simp_result, bool>> simplify_core_fn::post(expr const &, optional<expr> const &) {
    return optional<pair<simp_result, bool>>();
}

simplify_core_fn::simplify_core_fn(type_context & ctx, simp_lemmas const & slss,
                                   unsigned max_steps, bool contextual, bool lift_eq,
                                   bool canonize_instances, bool canonize_proofs):
    m_ctx(ctx), m_slss(slss), m_max_steps(max_steps), m_contextual(contextual),
    m_lift_eq(lift_eq), m_canonize_instances(canonize_instances), m_canonize_proofs(canonize_proofs) {
}

simp_result simplify_core_fn::operator()(name const & rel, expr const & e) {
    m_rel = rel;
    m_cache.clear();
    simp_result r(e);
    while (true) {
        m_need_restart = false;
        r = join(r, visit(r.get_new(), none_expr()));
        if (!m_need_restart || !should_defeq_canonize())
            return r;
        m_cache.clear();
    }
}

optional<expr> simplify_core_fn::prove_by_simp(name const & rel, expr const & e) {
    lean_assert(rel == get_eq_name() || rel == get_iff_name());
    simp_result r = operator()(rel, e);
    name const & mpr = rel == get_eq_name() ? get_eq_mpr_name() : get_iff_mpr_name();

    name rrel;
    expr lhs, rhs;
    if (is_relation(m_ctx.env(), r.get_new(), rrel, lhs, rhs) &&
        is_refl_relation(m_ctx.env(), rrel) &&
        m_ctx.is_def_eq(lhs, rhs)) {
        if (r.has_proof()) {
            return some_expr(mk_app(m_ctx, mpr, r.get_proof(), mk_refl(m_ctx, rrel, lhs)));
        } else {
            return some_expr(mk_refl(m_ctx, rrel, lhs));
        }
    } else if (is_true(r.get_new())) {
        if (r.has_proof()) {
            return some_expr(mk_app(m_ctx, mpr, r.get_proof(), mk_true_intro()));
        } else {
            return some_expr(mk_true_intro());
        }
    }
    return none_expr();
}

/* -----------------------------------
   simplify_ext_core_fn
   ------------------------------------ */

simplify_ext_core_fn::simplify_ext_core_fn(type_context & ctx, simp_lemmas const & slss,
                                           unsigned max_steps, bool contextual, bool lift_eq,
                                           bool canonize_instances, bool canonize_proofs,
                                           bool use_axioms):
    simplify_core_fn(ctx, slss, max_steps, contextual, lift_eq, canonize_instances, canonize_proofs),
    m_use_axioms (use_axioms) {
}

simp_result simplify_ext_core_fn::visit_lambda(expr const & e) {
    if (m_rel != get_eq_name() || !m_use_axioms) return simp_result(e);
    type_context::tmp_locals locals(m_ctx);
    expr it = e;
    while (is_lambda(it)) {
        expr d = instantiate_rev(binding_domain(it), locals.size(), locals.as_buffer().data());
        expr l = locals.push_local(binding_name(it), d, binding_info(it));
        it = binding_body(it);
    }
    it = instantiate_rev(it, locals.size(), locals.as_buffer().data());

    simp_result r = visit(it, some_expr(e));
    expr new_body = r.get_new();

    if (new_body == it)
        return simp_result(e);

    if (!r.has_proof())
        return simp_result(locals.mk_lambda(new_body));

    // TODO(Leo): create funext proof
    return simp_result(e);
}

simp_result simplify_ext_core_fn::forall_congr(expr const & e) {
    lean_assert(m_rel == get_eq_name() || m_rel == get_iff_name());
    buffer<expr> pis;
    type_context::tmp_locals locals(m_ctx);
    expr it = e;
    while (is_pi(it)) {
        expr d = instantiate_rev(binding_domain(it), locals.as_buffer().size(), locals.as_buffer().data());
        if (m_ctx.is_prop(d))
            break;
        pis.push_back(it);
        locals.push_local(binding_name(it), d, binding_info(it));
        it = binding_body(it);
    }
    buffer<expr> const & ls = locals.as_buffer();
    lean_assert(pis.size() == ls.size());
    expr body          = instantiate_rev(it, ls.size(), ls.data());
    simp_result body_r = visit(body, some_expr(e));
    expr new_body      = body_r.get_new();
    expr abst_new_body = abstract_locals(new_body, ls.size(), ls.data());
    name lemma_name    = m_rel == get_eq_name() ? get_forall_congr_eq_name() : get_forall_congr_name();
    if (body_r.has_proof()) {
        expr pr      = body_r.get_proof();
        expr Pr      = abstract_locals(pr, ls.size(), ls.data());
        unsigned i   = pis.size();
        expr Q       = abst_new_body;
        expr R       = abst_new_body;
        while (i > 0) {
            --i;
            expr pi      = pis[i];
            expr A       = binding_domain(pi);
            level A_lvl  = get_level(m_ctx, m_ctx.infer(ls[i]));
            expr P       = mk_lambda(binding_name(pi), A, binding_body(pi));
            expr Q       = mk_lambda(binding_name(pi), A, R);
            expr H       = mk_lambda(binding_name(pi), A, Pr);
            Pr           = mk_app(mk_constant(lemma_name, {A_lvl}), A, P, Q, H);
            R            = update_binding(pi, A, R);
        }
        lean_assert(closed(Pr));
        return simp_result(R, Pr);
    } else if (new_body == body) {
        return simp_result(e);
    } else {
        expr R = abst_new_body;
        unsigned i = pis.size();
        while (i > 0) {
            --i;
            R = update_binding(pis[i], binding_domain(pis[i]), R);
        }
        return simp_result(R);
    }
    return simp_result(e);
}

simp_result simplify_ext_core_fn::imp_congr(expr const & e) {
    // TODO(Leo)
    return simp_result(e);
}

simp_result simplify_ext_core_fn::visit_pi(expr const & e) {
    if ((m_rel == get_eq_name() && m_use_axioms) || m_rel == get_iff_name()) {
        if (m_ctx.is_prop(e)) {
            if (!m_ctx.is_prop(binding_domain(e)))
                return forall_congr(e);
            else if (is_arrow(e))
                return imp_congr(e);
        }
    }
    return simplify_core_fn::visit_pi(e);
}

simp_result simplify_ext_core_fn::visit_let(expr const & e) {
    /* TODO(Leo): we need to implement efficient code for checking whether the abstraction of
       a let-body is type correct or not */
    return simp_result(e);
}

static optional<pair<simp_result, bool>> to_ext_result(simp_result const & r) {
    return optional<pair<simp_result, bool>>(r, true);
}

static optional<pair<simp_result, bool>> no_ext_result() {
    return optional<pair<simp_result, bool>>();
}

optional<pair<simp_result, bool>> simplify_fn::pre(expr const & e, optional<expr> const &) {
    if (auto r = m_ctx.reduce_projection(e))
        return to_ext_result(simp_result(*r));
    else
        return no_ext_result();
}

optional<pair<simp_result, bool>> simplify_fn::post(expr const & e, optional<expr> const &) {
    simp_result r = rewrite(e);
    if (r.get_new() != e)
        return to_ext_result(r);
    else
        return no_ext_result();
}

class vm_simplify_fn : public simplify_ext_core_fn {
    vm_obj       m_a;
    vm_obj       m_prove;
    vm_obj       m_pre;
    vm_obj       m_post;
    tactic_state m_s;

    optional<pair<simp_result, bool>> invoke_fn(vm_obj const & fn, expr const & e, optional<expr> const & parent) {
        m_s = set_mctx_lctx(m_s, m_ctx.mctx(), m_ctx.lctx());
        vm_obj r = invoke(fn, m_a, to_obj(m_rel), to_obj(m_slss), to_obj(parent), to_obj(e), to_obj(m_s));
        /* r : tactic_state (A × expr × option expr × bool) */
        if (optional<tactic_state> new_s = is_tactic_success(r)) {
            m_s = *new_s;
            m_ctx.set_mctx(m_s.mctx());
            vm_obj t = cfield(r, 0);
            /* t : A × expr × option expr × bool */
            m_a        = cfield(t, 0);
            vm_obj t1  = cfield(t, 1);
            expr new_e = to_expr(cfield(t1, 0));
            vm_obj t2  = cfield(t1, 1);
            optional<expr> new_pr;
            vm_obj vpr = cfield(t2, 0);
            if (!is_none(vpr))
                new_pr = to_expr(get_some_value(vpr));
            bool flag  = to_bool(cfield(t2, 1));
            return optional<pair<simp_result, bool>>(simp_result(new_e, new_pr), flag);
        } else {
            return no_ext_result();
        }
    }

    virtual optional<pair<simp_result, bool>> pre(expr const & e, optional<expr> const & parent) override {
        return invoke_fn(m_pre, e, parent);
    }

    virtual optional<pair<simp_result, bool>> post(expr const & e, optional<expr> const & parent) override {
        return invoke_fn(m_post, e, parent);
    }

    virtual optional<expr> prove(expr const & e) override {
        tactic_state s         = mk_tactic_state_for(m_ctx.env(), m_ctx.get_options(), m_ctx.lctx(), e);
        vm_obj r_obj           = invoke(m_prove, m_a, to_obj(s));
        optional<tactic_state> s_new = is_tactic_success(r_obj);
        if (!s_new || s_new->goals()) return none_expr();
        metavar_context mctx   = s_new->mctx();
        expr result            = mctx.instantiate_mvars(s_new->main());
        if (has_expr_metavar(result)) return none_expr();
        m_a = cfield(r_obj, 0);
        m_ctx.set_mctx(mctx);
        return some_expr(result);
    }

public:
    vm_simplify_fn(type_context & ctx, simp_lemmas const & slss,
                   unsigned max_steps, bool contextual, bool lift_eq,
                   bool canonize_instances, bool canonize_proofs, bool use_axioms,
                   vm_obj const & prove, vm_obj const & pre, vm_obj const & post):
        simplify_ext_core_fn(ctx, slss, max_steps, contextual, lift_eq,
                             canonize_instances, canonize_proofs, use_axioms),
        m_prove(prove), m_pre(pre), m_post(post),
        m_s(mk_tactic_state_for(ctx.env(), ctx.get_options(), ctx.mctx(), ctx.lctx(), mk_true())) {}
};

void initialize_new_simplify() {
}

void finalize_new_simplify() {
}

/* LEGACY */



static name * g_simplify_prefix = nullptr;
name get_simplify_prefix_name() { return *g_simplify_prefix; }

/* Options */

static name * g_simplify_max_steps                      = nullptr;
static name * g_simplify_contextual                     = nullptr;
static name * g_simplify_rewrite                        = nullptr;
static name * g_simplify_lift_eq                        = nullptr;
static name * g_simplify_canonize_instances_fixed_point = nullptr;
static name * g_simplify_canonize_proofs_fixed_point    = nullptr;

name get_simplify_max_steps_name() { return *g_simplify_max_steps; }
name get_simplify_contextual_name() { return *g_simplify_contextual; }
name get_simplify_rewrite_name() { return *g_simplify_rewrite; }
name get_simplify_lift_eq_name() { return *g_simplify_lift_eq; }
name get_simplify_canonize_instances_fixed_point_name() { return *g_simplify_canonize_instances_fixed_point; }
name get_simplify_canonize_proofs_fixed_point_name() { return *g_simplify_canonize_proofs_fixed_point; }

static unsigned get_simplify_max_steps(options const & o) {
    return o.get_unsigned(*g_simplify_max_steps, LEAN_DEFAULT_SIMPLIFY_MAX_STEPS);
}

static bool get_simplify_contextual(options const & o) {
    return o.get_bool(*g_simplify_contextual, LEAN_DEFAULT_SIMPLIFY_CONTEXTUAL);
}

static bool get_simplify_rewrite(options const & o) {
    return o.get_bool(*g_simplify_rewrite, LEAN_DEFAULT_SIMPLIFY_REWRITE);
}

static bool get_simplify_lift_eq(options const & o) {
    return o.get_bool(*g_simplify_lift_eq, LEAN_DEFAULT_SIMPLIFY_LIFT_EQ);
}

static bool get_simplify_canonize_instances_fixed_point(options const & o) {
    return o.get_bool(*g_simplify_canonize_instances_fixed_point,
                      LEAN_DEFAULT_SIMPLIFY_DEFEQ_CANONIZE_INSTANCES_FIXED_POINT);
}

static bool get_simplify_canonize_proofs_fixed_point(options const & o) {
    return o.get_bool(*g_simplify_canonize_proofs_fixed_point, LEAN_DEFAULT_SIMPLIFY_DEFEQ_CANONIZE_PROOFS_FIXED_POINT);
}

/* Main simplifier class */

class simplifier {
    type_context              m_tctx, m_tctx_whnf;

    name                      m_rel;

    simp_lemmas               m_slss;

    optional<vm_obj>          m_prove_fn;

    /* Logging */
    unsigned                  m_num_steps{0};

    bool                      m_need_restart{false};

    /* Options */
    unsigned                  m_max_steps;
    bool                      m_contextual;
    bool                      m_rewrite;
    bool                      m_lift_eq;
    bool                      m_canonize_instances_fixed_point;
    bool                      m_canonize_proofs_fixed_point;

    typedef expr_struct_map<simp_result> simplify_cache;
    simplify_cache m_cache;
    optional<simp_result> cache_lookup(expr const & e);
    void cache_save(expr const & e, simp_result const & r);

    /* Basic helpers */
    environment const & env() const { return m_tctx.env(); }

    simp_result join(simp_result const & r1, simp_result const & r2) { return ::lean::join(m_tctx, m_rel, r1, r2); }

    bool using_eq() { return m_rel == get_eq_name(); }

    bool is_dependent_fn(expr const & f) {
        expr f_type = m_tctx.whnf(m_tctx.infer(f));
        lean_assert(is_pi(f_type));
        return has_free_vars(binding_body(f_type));
    }

    simp_lemmas add_to_slss(simp_lemmas const & _slss, buffer<expr> const & ls) {
        simp_lemmas slss = _slss;
        for (unsigned i = 0; i < ls.size(); i++) {
            expr const & l = ls[i];
            try {
                // TODO(Leo,Daniel): should we allow the user to set the priority of local lemmas?
                slss = add(m_tctx, slss, mlocal_name(l), m_tctx.infer(l), l, LEAN_DEFAULT_PRIORITY);
                lean_trace_d(name({"simplifier", "context"}), tout() << mlocal_name(l) << " : " << m_tctx.infer(l) << "\n";);
            } catch (exception e) {
            }
        }
        return slss;
    }

    bool should_defeq_canonize() {
        return m_canonize_instances_fixed_point || m_canonize_proofs_fixed_point;
    }

    bool instantiate_emetas(tmp_type_context & tmp_tctx, unsigned num_emeta,
                            list<expr> const & emetas, list<bool> const & instances);

    /* Simp_Results */
    simp_result lift_from_eq(expr const & old_e, simp_result const & r_eq);

    /* Main simplify method */
    simp_result simplify(expr const & old_e) {
        m_num_steps++;
        lean_trace_inc_depth("simplifier");
        lean_trace_d("simplifier", tout() << m_rel << ": " << old_e << "\n";);

        if (m_num_steps > m_max_steps) {
            lean_trace(name({"simplifier", "failed"}), tout() << m_rel << ": " << old_e << "\n";);
            throw exception("simplifier failed, maximum number of steps exceeded");
        }

        if (auto it = cache_lookup(old_e))
            return *it;

        expr e = whnf_eta(old_e);
        simp_result r;

        r = simplify_binary(e);

        if (!r.is_done())
            r = join(r, simplify(r.get_new()));

        if (m_lift_eq && !using_eq()) {
            simp_result r_eq;
            {
                flet<name> use_eq(m_rel, get_eq_name());
                r_eq = simplify(r.get_new());
            }
            if (r_eq.get_new() != r.get_new()) {
                r = join(r, lift_from_eq(r.get_new(), r_eq));
                r = join(r, simplify(r.get_new()));
            }
        }

        cache_save(old_e, r);

        return r;
    }

    simp_result simplify_rewrite_binary(expr const & e) {
        simp_lemmas_for const * sr = m_slss.find(m_rel);
        if (!sr) return simp_result(e);

        list<simp_lemma> const * srs = sr->find(e);
        if (!srs) {
            lean_trace_d(name({"debug", "simplifier", "try_rewrite"}), tout() << "no simp lemmas for: " << e << "\n";);
            return simp_result(e);
        }

        for (simp_lemma const & lemma : *srs) {
            simp_result r = rewrite_binary(e, lemma);
            if (!is_eqp(r.get_new(), e)) {
                lean_trace_d(name({"simplifier", "rewrite"}), tout() << "[" << lemma.get_id() << "]: " << e <<
                             " ==> " << r.get_new() << "\n";);
                return r;
            }
        }
        return simp_result(e);
    }

    simp_result rewrite_binary(expr const & e, simp_lemma const & sl) {
        tmp_type_context tmp_tctx(m_tctx, sl.get_num_umeta(), sl.get_num_emeta());
        if (!tmp_tctx.is_def_eq(e, sl.get_lhs())) {
            lean_trace_d(name({"debug", "simplifier", "try_rewrite"}), tout() << "fail to unify: " << sl.get_id() << "\n";);
            return simp_result(e);
        }
        if (!instantiate_emetas(tmp_tctx, sl.get_num_emeta(), sl.get_emetas(), sl.get_instances())) {
            lean_trace_d(name({"debug", "simplifier", "try_rewrite"}), tout() << "fail to instantiate emetas: " <<
                         sl.get_id() << "\n";);
            return simp_result(e);
        }

        for (unsigned i = 0; i < sl.get_num_umeta(); i++) {
            if (!tmp_tctx.is_uassigned(i))
                return simp_result(e);
        }

        expr new_lhs = tmp_tctx.instantiate_mvars(sl.get_lhs());
        expr new_rhs = tmp_tctx.instantiate_mvars(sl.get_rhs());
        if (sl.is_permutation()) {
            if (!is_lt(new_rhs, new_lhs, false)) {
                lean_simp_trace(tmp_tctx, name({"simplifier", "perm"}),
                                tout() << "perm rejected: " << new_rhs << " !< " << new_lhs << "\n";);
                return simp_result(e);
            }
        }

        if (sl.is_refl()) {
            return simp_result(new_rhs);
        } else {
            expr pf = tmp_tctx.instantiate_mvars(sl.get_proof());
            return simp_result(new_rhs, pf);
        }
    }

    simp_result simplify_subterms_app_binary(expr const & _e) {
        lean_assert(is_app(_e));
        expr e = should_defeq_canonize() ? defeq_canonize_args_step(_e) : _e;

        // (1) Try user-defined congruences
        simp_result r_user = try_congrs(e);
        if (r_user.has_proof()) {
            if (using_eq()) {
                return join(r_user, simplify_operator_of_app(r_user.get_new()));
            } else {
                return r_user;
            }
        }

        // (2) Synthesize congruence lemma
        if (using_eq()) {
            optional<simp_result> r_args = synth_congr(e);
            if (r_args) return join(*r_args, simplify_operator_of_app(r_args->get_new()));
        }
        // (3) Fall back on generic binary congruence
        if (using_eq()) {
            expr const & f = app_fn(e);
            expr const & arg = app_arg(e);

            simp_result r_f = simplify(f);

            if (is_dependent_fn(f)) {
                if (r_f.has_proof()) return congr_fun(r_f, arg);
                else return mk_app(r_f.get_new(), arg);
            } else {
                simp_result r_arg = simplify(arg);
                return congr_fun_arg(r_f, r_arg);
            }
        }
        return simp_result(e);
    }


    // Main binary simplify method
    simp_result simplify_binary(expr const & e) {
        // [1] Simplify subterms using congruence
        simp_result r(e);

        switch (r.get_new().kind()) {
        case expr_kind::Local:
        case expr_kind::Meta:
        case expr_kind::Sort:
        case expr_kind::Constant:
        case expr_kind::Macro:
            // no-op
            break;
        case expr_kind::Lambda:
            if (using_eq())
                r = simplify_subterms_lambda(r.get_new());
            break;
        case expr_kind::Pi:
            r = simplify_subterms_pi(r.get_new());
            break;
        case expr_kind::App:
            r = simplify_subterms_app_binary(r.get_new());
            break;
        case expr_kind::Let:
            // whnf unfolds let-expressions
            // TODO(dhs, leo): add flag for type_context not to unfold let-expressions
            lean_unreachable();
        case expr_kind::Var:
            lean_unreachable();
        }

        if (r.get_new() != e) {
            lean_trace_d(name({"simplifier", "congruence"}), tout() << e << " ==> " << r.get_new() << "\n";);
        }

        if (m_rewrite) {
            simp_result r_rewrite = simplify_rewrite_binary(r.get_new());
            if (r_rewrite.get_new() != r.get_new()) {
                lean_trace_d(name({"simplifier", "rewrite"}), tout() << r.get_new() << " ==> " <<
                             r_rewrite.get_new() << "\n";);
                return join(r, r_rewrite);
            }
        }

        r.set_done();
        return r;
    }

    /* Simplifying the necessary subterms */
    simp_result simplify_subterms_lambda(expr const & e);
    simp_result simplify_subterms_pi(expr const & e);
    simp_result simplify_subterms_app(expr const & e);

    /* Called from simplify_subterms_app */
    simp_result simplify_operator_of_app(expr const & e);

    /* Proving */
    optional<expr> prove(expr const & thm);

    /* Canonize */
    expr defeq_canonize_args_step(expr const & e);

    /* Congruence */
    simp_result congr_fun_arg(simp_result const & r_f, simp_result const & r_arg);
    simp_result congr(simp_result const & r_f, simp_result const & r_arg);
    simp_result congr_fun(simp_result const & r_f, expr const & arg);
    simp_result congr_arg(expr const & f, simp_result const & r_arg);
    simp_result congr_funs(simp_result const & r_f, buffer<expr> const & args);
    simp_result funext(simp_result const & r, expr const & l);

    simp_result try_congrs(expr const & e);
    simp_result try_congr(expr const & e, simp_lemma const & cr);

    optional<simp_result> synth_congr(expr const & e);

    expr remove_unnecessary_casts(expr const & e);

    /* Apply whnf and eta-reduction
       \remark We want (Sum n (fun x, f x)) and (Sum n f) to be the same.
       \remark We may want to switch to eta-expansion later (see paper: "The Virtues of Eta-expansion").
       TODO(Daniel, Leo): should we add an option for disabling/enabling eta? */
    expr whnf_eta(expr const & e);

public:
    simplifier(type_context & tctx, type_context & tctx_whnf, name const & rel, simp_lemmas const & slss, optional<vm_obj> const & prove_fn):
        m_tctx(tctx), m_tctx_whnf(tctx_whnf), m_rel(rel), m_slss(slss), m_prove_fn(prove_fn),
        /* Options */
        m_max_steps(get_simplify_max_steps(tctx.get_options())),
        m_contextual(get_simplify_contextual(tctx.get_options())),
        m_rewrite(get_simplify_rewrite(tctx.get_options())),
        m_lift_eq(get_simplify_lift_eq(tctx.get_options())),
        m_canonize_instances_fixed_point(get_simplify_canonize_instances_fixed_point(tctx.get_options())),
        m_canonize_proofs_fixed_point(get_simplify_canonize_proofs_fixed_point(tctx.get_options()))
        {}

    simp_result operator()(expr const & e)  {
        scope_trace_env scope(env(), m_tctx.get_options(), m_tctx);
        simp_result r(e);
        while (true) {
            m_need_restart = false;
            r = join(r, simplify(r.get_new()));
            if (!m_need_restart || !should_defeq_canonize())
                return r;
            m_cache.clear();
        }
        return simplify(e);
    }
};

/* Cache */

optional<simp_result> simplifier::cache_lookup(expr const & e) {
    auto it = m_cache.find(e);
    if (it == m_cache.end()) return optional<simp_result>();
    return optional<simp_result>(it->second);
}

void simplifier::cache_save(expr const & e, simp_result const & r) {
    m_cache.insert(mk_pair(e, r));
}

/* Simp_Results */

simp_result simplifier::lift_from_eq(expr const & old_e, simp_result const & r_eq) {
    if (!r_eq.has_proof()) return r_eq;
    lean_assert(r_eq.has_proof());
    /* r_eq.get_proof() : old_e = r_eq.get_new() */
    /* goal : old_e <m_rel> r_eq.get_new() */
    type_context::tmp_locals local_factory(m_tctx);
    expr local = local_factory.push_local(name(), m_tctx.infer(old_e));
    expr motive_local = mk_app(m_tctx, m_rel, old_e, local);
    /* motive = fun y, old_e <m_rel> y */
    expr motive = local_factory.mk_lambda(motive_local);
    /* Rxx = x <m_rel> x */
    expr Rxx = mk_refl(m_tctx, m_rel, old_e);
    expr pf = mk_eq_rec(m_tctx, motive, Rxx, r_eq.get_proof());
    return simp_result(r_eq.get_new(), pf);
}

/* Whnf + Eta */
expr simplifier::whnf_eta(expr const & e) {
    return try_eta(m_tctx_whnf.whnf(e));
}

simp_result simplifier::simplify_subterms_lambda(expr const & old_e) {
    lean_assert(is_lambda(old_e));
    expr e = old_e;

    buffer<expr> ls;
    while (is_lambda(e)) {
        expr d = instantiate_rev(binding_domain(e), ls.size(), ls.data());
        expr l = m_tctx.push_local(binding_name(e), d, binding_info(e));
        ls.push_back(l);
        e = instantiate(binding_body(e), l);
    }

    simp_result r = simplify(e);
    expr new_e = r.get_new();

    if (r.get_new() == e)
        return old_e;

    if (!r.has_proof())
        return m_tctx.mk_lambda(ls, r.get_new());

    for (int i = ls.size() - 1; i >= 0; --i)
        r = funext(r, ls[i]);

    return r;
}

simp_result simplifier::simplify_subterms_pi(expr const & e) {
    lean_assert(is_pi(e));
    return try_congrs(e);
}

expr simplifier::defeq_canonize_args_step(expr const & e) {
    buffer<expr> args;
    bool modified = false;
    expr f        = get_app_args(e, args);
    fun_info info = get_fun_info(m_tctx, f, args.size());
    unsigned i    = 0;
    for (param_info const & pinfo : info.get_params_info()) {
        lean_assert(i < args.size());
        expr new_a;
        if ((m_canonize_instances_fixed_point && pinfo.is_inst_implicit()) || (m_canonize_proofs_fixed_point && pinfo.is_prop())) {
            new_a = ::lean::defeq_canonize(m_tctx, args[i], m_need_restart);
            lean_trace(name({"simplifier", "canonize"}),
                       tout() << "\n" << args[i] << "\n==>\n" << new_a << "\n";);
            if (new_a != args[i]) {
                modified = true;
                args[i] = new_a;
            }
        }
        i++;
    }

    if (!modified)
        return e;
    else
        return mk_app(f, args);
}

simp_result simplifier::simplify_operator_of_app(expr const & e) {
    lean_assert(is_app(e));
    buffer<expr> args;
    expr const & f = get_app_args(e, args);
    simp_result r_f = simplify(f);
    return congr_funs(r_f, args);
}

/* Proving */
optional<expr> simplifier::prove(expr const & goal) {
    if (!m_prove_fn)
        return none_expr();

    metavar_context mctx = m_tctx.mctx();
    expr goal_mvar = mctx.mk_metavar_decl(m_tctx.lctx(), goal);
    lean_trace(name({"simplifier", "prove"}), tout() << "goal: " << goal_mvar << " : " << goal << "\n";);
    vm_obj s = to_obj(tactic_state(m_tctx.env(), m_tctx.get_options(), mctx, list<expr>(goal_mvar), goal_mvar));
    vm_obj prove_fn_result = invoke(*m_prove_fn, s);
    optional<tactic_state> s_new = is_tactic_success(prove_fn_result);
    if (s_new) {
        if (!s_new->mctx().is_assigned(goal_mvar)) {
            lean_trace(name({"simplifier", "prove"}), tout() << "prove_fn succeeded but did not return a proof\n";);
            return none_expr();
        }
        metavar_context smctx = s_new->mctx();
        expr proof = smctx.instantiate_mvars(goal_mvar);
        optional<expr> new_metavar = find(proof, [&](expr const & e, unsigned) {
                return is_metavar(e) && !static_cast<bool>(m_tctx.mctx().get_metavar_decl(e));
            });
        if (new_metavar) {
            lean_trace(name({"simplifier", "prove"}),
                       tout() << "prove_fn succeeded but left an unrecognized metavariable of type "
                       << smctx.get_metavar_decl(*new_metavar)->get_type() << " in proof\n";);
            return none_expr();
        }
        m_tctx.set_mctx(s_new->mctx());
        lean_trace(name({"simplifier", "prove"}), tout() << "success: " << proof << " : " << m_tctx.infer(proof) << "\n";);
        return some_expr(proof);
    } else {
        lean_trace(name({"simplifier", "prove"}), tout() << "prove_fn failed to prove " << goal << "\n";);
        return none_expr();
    }
}

/* Congruence */

simp_result simplifier::congr_fun_arg(simp_result const & r_f, simp_result const & r_arg) {
    if (!r_f.has_proof() && !r_arg.has_proof()) return simp_result(mk_app(r_f.get_new(), r_arg.get_new()));
    else if (!r_f.has_proof()) return congr_arg(r_f.get_new(), r_arg);
    else if (!r_arg.has_proof()) return congr_fun(r_f, r_arg.get_new());
    else return congr(r_f, r_arg);
}

simp_result simplifier::congr(simp_result const & r_f, simp_result const & r_arg) {
    lean_assert(r_f.has_proof() && r_arg.has_proof());
    // theorem congr {A B : Type} {f₁ f₂ : A → B} {a₁ a₂ : A} (H₁ : f₁ = f₂) (H₂ : a₁ = a₂) : f₁ a₁ = f₂ a₂
    expr e  = mk_app(r_f.get_new(), r_arg.get_new());
    expr pf = mk_congr(m_tctx, r_f.get_proof(), r_arg.get_proof());
    return simp_result(e, pf);
}

simp_result simplifier::congr_fun(simp_result const & r_f, expr const & arg) {
    lean_assert(r_f.has_proof());
    // theorem congr_fun {A : Type} {B : A → Type} {f g : Π x, B x} (H : f = g) (a : A) : f a = g a
    expr e  = mk_app(r_f.get_new(), arg);
    expr pf = mk_congr_fun(m_tctx, r_f.get_proof(), arg);
    return simp_result(e, pf);
}

simp_result simplifier::congr_arg(expr const & f, simp_result const & r_arg) {
    lean_assert(r_arg.has_proof());
    // theorem congr_arg {A B : Type} {a₁ a₂ : A} (f : A → B) : a₁ = a₂ → f a₁ = f a₂
    expr e  = mk_app(f, r_arg.get_new());
    expr pf = mk_congr_arg(m_tctx, f, r_arg.get_proof());
    return simp_result(e, pf);
}

/* Note: handles reflexivity */
simp_result simplifier::congr_funs(simp_result const & r_f, buffer<expr> const & args) {
    expr e = r_f.get_new();
    for (unsigned i = 0; i < args.size(); ++i) {
        e  = mk_app(e, args[i]);
    }
    if (!r_f.has_proof())
        return simp_result(e);
    expr pf = r_f.get_proof();
    for (unsigned i = 0; i < args.size(); ++i) {
        pf = mk_congr_fun(m_tctx, pf, args[i]);
    }
    return simp_result(e, pf);
}

simp_result simplifier::funext(simp_result const & r, expr const & l) {
    expr e = m_tctx.mk_lambda(l, r.get_new());
    if (!r.has_proof())
        return simp_result(e);
    expr lam_pf = m_tctx.mk_lambda(l, r.get_proof());
    expr pf = mk_funext(m_tctx, lam_pf);
    return simp_result(e, pf);
}

simp_result simplifier::try_congrs(expr const & e) {
    simp_lemmas_for const * sls = m_slss.find(m_rel);
    if (!sls) return simp_result(e);

    list<simp_lemma> const * cls = sls->find_congr(e);
    if (!cls) return simp_result(e);

    for (simp_lemma const & cl : *cls) {
        simp_result r = try_congr(e, cl);
        if (r.get_new() != e)
            return r;
    }
    return simp_result(e);
}

simp_result simplifier::try_congr(expr const & e, simp_lemma const & cl) {
    tmp_type_context tmp_tctx(m_tctx, cl.get_num_umeta(), cl.get_num_emeta());
    if (!tmp_tctx.is_def_eq(e, cl.get_lhs()))
        return simp_result(e);

    lean_simp_trace(tmp_tctx, name({"debug", "simplifier", "try_congruence"}),
                    tout() << "(" << cl.get_id() << ") " << e << "\n";);

    bool simplified = false;

    buffer<expr> congr_hyps;
    to_buffer(cl.get_congr_hyps(), congr_hyps);

    buffer<simp_result> congr_hyp_results;
    buffer<type_context::tmp_locals> factories;
    buffer<name> relations;
    for (expr const & m : congr_hyps) {
        factories.emplace_back(m_tctx);
        type_context::tmp_locals & local_factory = factories.back();
        expr m_type = tmp_tctx.instantiate_mvars(tmp_tctx.infer(m));

        while (is_pi(m_type)) {
            expr d = instantiate_rev(binding_domain(m_type), local_factory.as_buffer().size(), local_factory.as_buffer().data());
            expr l = local_factory.push_local(binding_name(m_type), d, binding_info(m_type));
            lean_assert(!has_metavar(l));
            m_type = binding_body(m_type);
        }
        m_type = instantiate_rev(m_type, local_factory.as_buffer().size(), local_factory.as_buffer().data());

        expr h_rel, h_lhs, h_rhs;
        lean_verify(is_simp_relation(tmp_tctx.env(), m_type, h_rel, h_lhs, h_rhs) && is_constant(h_rel));
        {
            relations.push_back(const_name(h_rel));
            flet<simp_lemmas> set_slss(m_slss, m_contextual ? add_to_slss(m_slss, local_factory.as_buffer()) : m_slss);

            h_lhs = tmp_tctx.instantiate_mvars(h_lhs);
            /* TODO(Leo, Daniel): the original assertion was !has_metavar(h_lhs).
               It is incorrect. I got an assertion violation when processing
               a term containing universe meta-variables. So, I relaxed it to !has_expr_metavar(h_lhs).
               We should investigate this. Example: what happens if the input expression has
               regular meta-variables? Perhaps, the right assertion is: h_lhs does *not* have temporary
               universe and regular meta-variables. */
            lean_assert(!has_expr_metavar(h_lhs));

            if (m_contextual || m_rel != const_name(h_rel)) {
                flet<name> set_name(m_rel, const_name(h_rel));
                freset<simplify_cache> reset_cache(m_cache);
                congr_hyp_results.emplace_back(simplify(h_lhs));
            } else {
                congr_hyp_results.emplace_back(simplify(h_lhs));
            }
            simp_result const & r_congr_hyp = congr_hyp_results.back();

            if (r_congr_hyp.has_proof())
                simplified = true;

            lean_assert(is_meta(h_rhs));
            buffer<expr> new_val_meta_args;
            expr new_val_meta = get_app_args(h_rhs, new_val_meta_args);
            lean_assert(is_metavar(new_val_meta));
            expr new_val = tmp_tctx.mk_lambda(new_val_meta_args, r_congr_hyp.get_new());
            tmp_tctx.assign(new_val_meta, new_val);
        }
    }

    if (!simplified)
        return simp_result(e);

    lean_assert(congr_hyps.size() == congr_hyp_results.size());
    for (unsigned i = 0; i < congr_hyps.size(); ++i) {
        expr const & pf_meta = congr_hyps[i];
        simp_result const & r_congr_hyp = congr_hyp_results[i];
        name const & rel = relations[i];
        type_context::tmp_locals & local_factory = factories[i];
        expr hyp = finalize(m_tctx, rel, r_congr_hyp).get_proof();
        // This is the current bottleneck
        // Can address somewhat by keeping the proofs as small as possible using macros
        expr pf = local_factory.mk_lambda(hyp);
        tmp_tctx.assign(pf_meta, pf);
    }

    if (!instantiate_emetas(tmp_tctx, cl.get_num_emeta(), cl.get_emetas(), cl.get_instances()))
        return simp_result(e);

    for (unsigned i = 0; i < cl.get_num_umeta(); i++) {
        if (!tmp_tctx.is_uassigned(i))
            return simp_result(e);
    }

    expr e_s = tmp_tctx.instantiate_mvars(cl.get_rhs());
    expr pf = tmp_tctx.instantiate_mvars(cl.get_proof());

    simp_result r(e_s, pf);

    lean_simp_trace(tmp_tctx, name({"simplifier", "congruence"}),
                    tout() << "(" << cl.get_id() << ") "
                    << "[" << e << " ==> " << e_s << "]\n";);

    return r;
}

bool simplifier::instantiate_emetas(tmp_type_context & tmp_tctx, unsigned num_emeta, list<expr> const & emetas, list<bool> const & instances) {
    bool failed = false;
    unsigned i = num_emeta;
    for_each2(emetas, instances, [&](expr const & m, bool const & is_instance) {
            i--;
            if (failed) return;
            expr m_type = tmp_tctx.instantiate_mvars(tmp_tctx.infer(m));
            if (has_metavar(m_type)) {
                failed = true;
                return;
            }

            if (tmp_tctx.is_eassigned(i)) return;

            if (is_instance) {
                if (auto v = m_tctx.mk_class_instance(m_type)) {
                    if (!tmp_tctx.is_def_eq(m, *v)) {
                        lean_simp_trace(tmp_tctx, name({"simplifier", "failure"}),
                                        tout() << "unable to assign instance for: " << m_type << "\n";);
                        failed = true;
                        return;
                    }
                } else {
                    lean_simp_trace(tmp_tctx, name({"simplifier", "failure"}),
                                    tout() << "unable to synthesize instance for: " << m_type << "\n";);
                    failed = true;
                    return;
                }
            }

            if (tmp_tctx.is_eassigned(i)) return;

            // Note: m_type has no metavars
            if (m_tctx.is_prop(m_type)) {
                if (auto pf = prove(m_type)) {
                    lean_verify(tmp_tctx.is_def_eq(m, *pf));
                    return;
                }
            }

            lean_simp_trace(tmp_tctx, name({"simplifier", "failure"}),
                            tout() << "failed to assign: " << m << " : " << m_type << "\n";);

            failed = true;
            return;
        });

    return !failed;
}

optional<simp_result> simplifier::synth_congr(expr const & e) {
    lean_assert(is_app(e));
    buffer<expr> args;
    expr f = get_app_args(e, args);
    auto congr_lemma = mk_specialized_congr_simp(m_tctx, e);
    if (!congr_lemma) return optional<simp_result>();

    buffer<simp_result> r_args;
    buffer<expr>        new_args;
    bool has_proof = false;
    bool has_cast = false;
    bool has_simplified = false;

    unsigned i = 0;

    // First pass, try to simplify all the Eq arguments
    for (congr_arg_kind const & ckind : congr_lemma->get_arg_kinds()) {
        switch (ckind) {
        case congr_arg_kind::HEq:
            lean_unreachable();
        case congr_arg_kind::Fixed:
        case congr_arg_kind::FixedNoParam:
            new_args.emplace_back(args[i]);
            break;
        case congr_arg_kind::Eq:
            {
                r_args.emplace_back(simplify(args[i]));
                simp_result & r_arg = r_args.back();
                new_args.emplace_back(r_arg.get_new());
                if (r_arg.has_proof())
                    has_proof = true;
                if (r_arg.get_new() != args[i])
                    has_simplified = true;
            }
            break;
        case congr_arg_kind::Cast:
            has_cast = true;
            new_args.emplace_back(args[i]);
            break;
        }
        i++;
    }

    if (!has_simplified) {
        simp_result r = simp_result(e);
        return optional<simp_result>(r);
    }

    if (!has_proof) {
        simp_result r = simp_result(mk_app(f, new_args));
        return optional<simp_result>(r);
    }

    // We have a proof, so we need to build the congruence lemma
    expr proof = congr_lemma->get_proof();
    expr type = congr_lemma->get_type();
    buffer<expr> subst;

    i = 0;
    unsigned i_eq = 0;
    for (congr_arg_kind const & ckind : congr_lemma->get_arg_kinds()) {
        switch (ckind) {
        case congr_arg_kind::HEq:
            lean_unreachable();
        case congr_arg_kind::Fixed:
            proof = mk_app(proof, args[i]);
            subst.push_back(args[i]);
            type = binding_body(type);
            break;
        case congr_arg_kind::FixedNoParam:
            break;
        case congr_arg_kind::Eq:
            proof = mk_app(proof, args[i]);
            subst.push_back(args[i]);
            type = binding_body(type);
            {
                simp_result r_arg = finalize(m_tctx, m_rel, r_args[i_eq]);
                proof = mk_app(proof, r_arg.get_new(), r_arg.get_proof());
                subst.push_back(r_arg.get_new());
                subst.push_back(r_arg.get_proof());
            }
            type = binding_body(binding_body(type));
            i_eq++;
            break;
        case congr_arg_kind::Cast:
            lean_assert(has_cast);
            proof = mk_app(proof, args[i]);
            subst.push_back(args[i]);
            type = binding_body(type);
            break;
        }
        i++;
    }
    lean_assert(is_eq(type));
    expr rhs   = instantiate_rev(app_arg(type), subst.size(), subst.data());
    simp_result r(rhs, proof);

    if (has_cast) {
        r.update(remove_unnecessary_casts(r.get_new()));
    }

    return optional<simp_result>(r);
}

expr simplifier::remove_unnecessary_casts(expr const & e) {
    buffer<expr> args;
    expr f = get_app_args(e, args);
    ss_param_infos ss_infos = get_specialized_subsingleton_info(m_tctx, e);
    int i = -1;
    bool updated = false;
    for (ss_param_info const & ss_info : ss_infos) {
        i++;
        if (ss_info.is_subsingleton()) {
            while (is_constant(get_app_fn(args[i]))) {
                buffer<expr> cast_args;
                expr f_cast = get_app_args(args[i], cast_args);
                name n_f = const_name(f_cast);
                if (n_f == get_eq_rec_name() || n_f == get_eq_drec_name() || n_f == get_eq_nrec_name()) {
                    lean_assert(cast_args.size() == 6);
                    expr major_premise = cast_args[5];
                    expr f_major_premise = get_app_fn(major_premise);
                    if (is_constant(f_major_premise) && const_name(f_major_premise) == get_eq_refl_name()) {
                        args[i] = cast_args[3];
                        updated = true;
                    } else {
                        break;
                    }
                } else {
                    break;
                }
            }
        }
    }
    if (!updated)
        return e;

    expr new_e = mk_app(f, args);
    lean_trace(name({"debug", "simplifier", "remove_casts"}), tout() << e << " ==> " << new_e << "\n";);

    return mk_app(f, args);
}

simp_result simplify(type_context & tctx, name const & rel, simp_lemmas const & simp_lemmas,
                     vm_obj const & prove_fn, expr const & e);

vm_obj simp_lemmas_simplify_core(vm_obj const & lemmas, vm_obj const & prove_fn, vm_obj const & rel_name, vm_obj const & e, vm_obj const & s0) {
    tactic_state const & s   = to_tactic_state(s0);
    try {
        type_context tctx    = mk_type_context_for(s, transparency_mode::Reducible);
        name rel             = to_name(rel_name);
        expr old_e           = to_expr(e);
        simp_result result   = simplify(tctx, rel, to_simp_lemmas(lemmas), prove_fn, old_e);

        if (result.get_new() != old_e) {
            result = finalize(tctx, rel, result);
            return mk_tactic_success(mk_vm_pair(to_obj(result.get_new()), to_obj(result.get_proof())), s);
        } else {
            return mk_tactic_exception("simp tactic failed to simplify", s);
        }
    } catch (exception & e) {
        return mk_tactic_exception(e, s);
    }
}

/* Setup and teardown */
void initialize_simplify() {
    register_trace_class("simplifier");
    register_trace_class(name({"simplifier", "congruence"}));
    register_trace_class(name({"simplifier", "failure"}));
    register_trace_class(name({"simplifier", "failed"}));
    register_trace_class(name({"simplifier", "perm"}));
    register_trace_class(name({"simplifier", "canonize"}));
    register_trace_class(name({"simplifier", "context"}));
    register_trace_class(name({"simplifier", "prove"}));
    register_trace_class(name({"simplifier", "rewrite"}));
    register_trace_class(name({"debug", "simplifier", "try_rewrite"}));
    register_trace_class(name({"debug", "simplifier", "try_congruence"}));
    register_trace_class(name({"debug", "simplifier", "remove_casts"}));

    g_simplify_prefix                         = new name{"simplify"};

    g_simplify_max_steps                      = new name{*g_simplify_prefix, "max_steps"};
    g_simplify_contextual                     = new name{*g_simplify_prefix, "contextual"};
    g_simplify_rewrite                        = new name{*g_simplify_prefix, "rewrite"};
    g_simplify_lift_eq                        = new name{*g_simplify_prefix, "lift_eq"};
    g_simplify_canonize_instances_fixed_point = new name{*g_simplify_prefix, "canonize_instances_fixed_point"};
    g_simplify_canonize_proofs_fixed_point    = new name{*g_simplify_prefix, "canonize_proofs_fixed_point"};

    register_unsigned_option(*g_simplify_max_steps, LEAN_DEFAULT_SIMPLIFY_MAX_STEPS,
                             "(simplify) max number of (large) steps in simplification");
    register_bool_option(*g_simplify_contextual, LEAN_DEFAULT_SIMPLIFY_CONTEXTUAL,
                         "(simplify) use contextual simplification");
    register_bool_option(*g_simplify_rewrite, LEAN_DEFAULT_SIMPLIFY_REWRITE,
                         "(simplify) rewrite with simp_lemmas");
    register_bool_option(*g_simplify_lift_eq, LEAN_DEFAULT_SIMPLIFY_LIFT_EQ,
                         "(simplify) try simplifying with equality when no progress over other relation");
    register_bool_option(*g_simplify_canonize_instances_fixed_point, LEAN_DEFAULT_SIMPLIFY_CANONIZE_INSTANCES_FIXED_POINT,
                         "(simplify) canonize instances, replacing with the smallest seen so far until reaching a fixed point");
    register_bool_option(*g_simplify_canonize_proofs_fixed_point, LEAN_DEFAULT_SIMPLIFY_CANONIZE_PROOFS_FIXED_POINT,
                         "(simplify) canonize proofs, replacing with the smallest seen so far until reaching a fixed point. ");

    DECLARE_VM_BUILTIN(name({"simp_lemmas", "simplify_core"}), simp_lemmas_simplify_core);
}

void finalize_simplify() {
    delete g_simplify_canonize_instances_fixed_point;
    delete g_simplify_canonize_proofs_fixed_point;
    delete g_simplify_lift_eq;
    delete g_simplify_rewrite;
    delete g_simplify_contextual;
    delete g_simplify_max_steps;
}

/* Entry point */
simp_result simplify(type_context & tctx, name const & rel, simp_lemmas const & simp_lemmas, vm_obj const & prove_fn, expr const & e) {
    return simplifier(tctx, tctx, rel, simp_lemmas, optional<vm_obj>(prove_fn))(e);
}

simp_result simplify(type_context & tctx, name const & rel, simp_lemmas const & simp_lemmas, expr const & e) {
    return simplifier(tctx, tctx, rel, simp_lemmas, optional<vm_obj>())(e);
}

simp_result simplify(type_context & tctx, type_context & tctx_whnf, name const & rel, simp_lemmas const & simp_lemmas, vm_obj const & prove_fn, expr const & e) {
    return simplifier(tctx, tctx_whnf, rel, simp_lemmas, optional<vm_obj>(prove_fn))(e);
}

simp_result simplify(type_context & tctx, type_context & tctx_whnf, name const & rel, simp_lemmas const & simp_lemmas, expr const & e) {
    scope_trace_env scope(tctx.env(), tctx);
    tout() << e << "\n";
    return simplifier(tctx, tctx_whnf, rel, simp_lemmas, optional<vm_obj>())(e);
}

optional<expr> prove_eq_by_simp(type_context & tctx, type_context & tctx_whnf, simp_lemmas const & simp_lemmas, expr const & e) {
    scope_trace_env scope(tctx.env(), tctx);
    simp_result r = simplify(tctx, tctx_whnf, get_eq_name(), simp_lemmas, e);
    expr lhs, rhs;
    tout() << r.get_new() << "\n-------------\n";
    if (is_eq(r.get_new(), lhs, rhs) && tctx.is_def_eq(lhs, rhs)) {
        if (r.has_proof()) {
            return some_expr(mk_app(tctx, get_eq_mpr_name(), r.get_proof(), mk_eq_refl(tctx, lhs)));
        } else {
            return some_expr(mk_eq_refl(tctx, lhs));
        }
    } else if (is_true(r.get_new())) {
        if (r.has_proof()) {
            return some_expr(mk_app(tctx, get_eq_mpr_name(), r.get_proof(), mk_true_intro()));
        } else {
            return some_expr(mk_true_intro());
        }
    }
    return none_expr();
}
}
