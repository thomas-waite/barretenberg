#pragma once

#include "../../../curves/bn254/fq12.hpp"
#include "../../../curves/bn254/g1.hpp"
#include "../../../curves/bn254/g2.hpp"
#include "../../../curves/bn254/pairing.hpp"
#include "../../../curves/bn254/scalar_multiplication/scalar_multiplication.hpp"
#include "../../../polynomials/evaluation_domain.hpp"
#include "../../../polynomials/polynomial_arithmetic.hpp"

#include "../../../types.hpp"

#include "../../reference_string/reference_string.hpp"

#include "../linearizer.hpp"
#include "../widgets/base_widget.hpp"

#include "../transcript_helpers.hpp"


namespace waffle {
template <typename program_settings>
VerifierBase<program_settings>::VerifierBase(const size_t subgroup_size, const transcript::Manifest &input_manifest, bool has_fourth_wire)
    : n(subgroup_size), manifest(input_manifest), __DEBUG_HAS_FOURTH_WIRE(has_fourth_wire)
{}

template <typename program_settings>
VerifierBase<program_settings>::VerifierBase(VerifierBase&& other)
    : n(other.n), manifest(other.manifest), __DEBUG_HAS_FOURTH_WIRE(other.__DEBUG_HAS_FOURTH_WIRE)
{
    reference_string = std::move(other.reference_string);
    for (size_t i = 0; i < SIGMA.size(); ++i)
    {
        SIGMA[i] = other.SIGMA[i];
    }
    for (size_t i = 0; i < other.verifier_widgets.size(); ++i) {
        verifier_widgets.emplace_back(std::move(other.verifier_widgets[i]));
    }
}

template <typename program_settings>
VerifierBase<program_settings>& VerifierBase<program_settings>::operator=(VerifierBase&& other)
{
    n = other.n;
    manifest = other.manifest;
    reference_string = std::move(other.reference_string);
    for (size_t i = 0; i < SIGMA.size(); ++i)
    {
        SIGMA[i] = other.SIGMA[i];
    }
    verifier_widgets.resize(0);
    for (size_t i = 0; i < other.verifier_widgets.size(); ++i) {
        verifier_widgets.emplace_back(std::move(other.verifier_widgets[i]));
    }
    __DEBUG_HAS_FOURTH_WIRE = other.__DEBUG_HAS_FOURTH_WIRE;

    return *this;
}

template <typename program_settings>
bool VerifierBase<program_settings>::verify_proof(const waffle::plonk_proof &proof)
{
    barretenberg::evaluation_domain domain = barretenberg::evaluation_domain(n);

    transcript::Transcript transcript = transcript::Transcript(proof.proof_data, manifest);

    std::array<g1::affine_element, program_settings::program_width> T;
    std::array<g1::affine_element, program_settings::program_width> W;

    std::array<fr::field_t, program_settings::program_width> wire_evaluations;
    std::array<fr::field_t, program_settings::program_width - 1> sigma_evaluations;

    for (size_t i = 0; i < program_settings::program_width; ++i)
    {
        std::string index = std::to_string(i + 1);
        T[i] = g1::serialize_from_buffer(&transcript.get_element("T_" + index)[0]);
        W[i] = g1::serialize_from_buffer(&transcript.get_element("W_" + index)[0]);
        wire_evaluations[i] = fr::serialize_from_buffer(&transcript.get_element("w_" + index)[0]);
    }
    for (size_t i = 0; i < program_settings::program_width - 1; ++i)
    {
        std::string index = std::to_string(i + 1);
        sigma_evaluations[i] = fr::serialize_from_buffer(&transcript.get_element("sigma_" + index)[0]);
    }
    g1::affine_element Z_1 = g1::serialize_from_buffer(&transcript.get_element("Z")[0]);
    g1::affine_element PI_Z = g1::serialize_from_buffer(&transcript.get_element("PI_Z")[0]);
    g1::affine_element PI_Z_OMEGA = g1::serialize_from_buffer(&transcript.get_element("PI_Z_OMEGA")[0]);

    fr::field_t linear_eval = fr::serialize_from_buffer(&transcript.get_element("r")[0]);
    fr::field_t z_1_shifted_eval = fr::serialize_from_buffer(&transcript.get_element("z_omega")[0]);

    bool inputs_valid = g1::on_curve(T[0])
                        && g1::on_curve(Z_1) && g1::on_curve(PI_Z);

    if (!inputs_valid) {
        printf("inputs not valid!\n");
        return false;
    }

    bool instance_valid = true;
    for (size_t i = 0; i < program_settings::program_width; ++i)
    {
        instance_valid = instance_valid && g1::on_curve(SIGMA[i]);
    }
    if (!instance_valid) {
        printf("instance not valid!\n");
        return false;
    }

    bool widget_instance_valid = true;
    for (size_t i = 0; i < verifier_widgets.size(); ++i) {
        widget_instance_valid = widget_instance_valid && verifier_widgets[i]->verify_instance_commitments();
    }
    if (!widget_instance_valid) {
        printf("widget instance not valid!\n");
        return false;
    }

    bool field_elements_valid = true;
    for (size_t i = 0; i < program_settings::program_width - 1; ++i)
    {
        field_elements_valid = field_elements_valid && !fr::eq(sigma_evaluations[i], fr::zero);
    }
    field_elements_valid = field_elements_valid && !fr::eq(linear_eval, fr::zero);

    if (!field_elements_valid) {
        printf("proof field elements not valid!\n");
        return false;
    }

    // reconstruct challenges
    fr::field_t alpha_pow[4];
    fr::field_t nu_pow[11];

    transcript.apply_fiat_shamir("init");
    fr::field_t beta = fr::serialize_from_buffer(transcript.apply_fiat_shamir("beta").begin());
    fr::field_t gamma = fr::serialize_from_buffer(transcript.apply_fiat_shamir("gamma").begin());
    fr::field_t alpha = fr::serialize_from_buffer(transcript.apply_fiat_shamir("alpha").begin());
    fr::field_t z_challenge = fr::serialize_from_buffer(transcript.apply_fiat_shamir("z").begin());

    barretenberg::polynomial_arithmetic::lagrange_evaluations lagrange_evals =
        barretenberg::polynomial_arithmetic::get_lagrange_evaluations(z_challenge, domain);

    // compute the terms we need to derive R(X)
    plonk_linear_terms linear_terms = compute_linear_terms<program_settings>(transcript, lagrange_evals.l_1);

    // reconstruct evaluation of quotient polynomial from prover messages
    fr::field_t t_eval;
    fr::field_t T0;
    fr::field_t T1;
    fr::field_t T2;
    fr::__copy(alpha, alpha_pow[0]);
    for (size_t i = 1; i < 4; ++i) {
        fr::__mul(alpha_pow[i - 1], alpha_pow[0], alpha_pow[i]);
    }

    fr::field_t sigma_contribution = fr::one;
    for (size_t i = 0; i < program_settings::program_width - 1; ++i)
    {
        fr::__mul(sigma_evaluations[i], beta, T0);
        fr::__add(wire_evaluations[i], gamma, T1);
        fr::__add(T0, T1, T0);
        fr::__mul(sigma_contribution, T0, sigma_contribution);
    }

    fr::__add(wire_evaluations[program_settings::program_width - 1], gamma, T0);
    fr::__mul(sigma_contribution, T0, sigma_contribution);
    fr::__mul(sigma_contribution, z_1_shifted_eval, sigma_contribution);
    fr::__mul(sigma_contribution, alpha_pow[0], sigma_contribution);

    // fr::__mul(z_1_shifted_eval, alpha_pow[0], T0);
    fr::__sub(z_1_shifted_eval, fr::one, T1);
    fr::__mul(T1, lagrange_evals.l_n_minus_1, T1);
    fr::__mul(T1, alpha_pow[1], T1);

    fr::__mul(lagrange_evals.l_1, alpha_pow[2], T2);
    fr::__sub(T1, T2, T1);
    fr::__sub(T1, sigma_contribution, T1);
    fr::__add(T1, linear_eval, t_eval);

    fr::field_t alpha_base = fr::sqr(fr::sqr(alpha));
    for (size_t i = 0; i < verifier_widgets.size(); ++i) {
        alpha_base = verifier_widgets[i]->compute_quotient_evaluation_contribution(alpha_base, transcript, t_eval);
    }
    fr::__invert(lagrange_evals.vanishing_poly, T0);
    fr::__mul(t_eval, T0, t_eval);

    transcript.add_element("t", transcript_helpers::convert_field_element(t_eval));

    fr::field_t nu = fr::serialize_from_buffer(transcript.apply_fiat_shamir("nu").begin());

    fr::field_t u = fr::serialize_from_buffer(transcript.apply_fiat_shamir("separator").begin());
    fr::__copy(nu, nu_pow[0]);
    for (size_t i = 1; i < 10; ++i) {
        fr::__mul(nu_pow[i - 1], nu_pow[0], nu_pow[i]);
    }

    // reconstruct Kate opening commitments from committed values
    fr::__mul(linear_terms.z_1, nu_pow[0], linear_terms.z_1);
    fr::__mul(linear_terms.sigma_last, nu_pow[0], linear_terms.sigma_last);

    fr::__mul(nu_pow[7], u, T0);
    fr::__add(linear_terms.z_1, T0, linear_terms.z_1);

    fr::field_t batch_evaluation;
    fr::__copy(t_eval, batch_evaluation);
    fr::__mul(nu_pow[0], linear_eval, T0);
    fr::__add(batch_evaluation, T0, batch_evaluation);

    for (size_t i = 0; i < program_settings::program_width; ++i)
    {
        fr::__mul(nu_pow[i + 1], wire_evaluations[i], T0);
        fr::__add(batch_evaluation, T0, batch_evaluation);
    }

    for (size_t i = 0; i < program_settings::program_width - 1; ++i)
    {
        fr::__mul(nu_pow[5 + i], sigma_evaluations[i], T0);
        fr::__add(batch_evaluation, T0, batch_evaluation);        
    }

    fr::__mul(nu_pow[7], u, T0);
    fr::__mul(T0, z_1_shifted_eval, T0);
    fr::__add(batch_evaluation, T0, batch_evaluation);


    fr::field_t nu_base = nu_pow[8];

    for (size_t i = 0; i < program_settings::program_width; ++i) {
        if (program_settings::requires_shifted_wire(program_settings::wire_shift_settings, i)) {
            fr::field_t wire_shifted_eval = fr::serialize_from_buffer(&transcript.get_element("w_" + std::to_string(i + 1) + "_omega")[0]);
            fr::__mul(wire_shifted_eval, nu_base, T0);
            fr::__mul(T0, u, T0);
            fr::__add(batch_evaluation, T0, batch_evaluation);
            fr::__mul(nu_base, nu_pow[0], nu_base);
        }
    }

    for (size_t i = 0; i < verifier_widgets.size(); ++i) {
        nu_base =
            verifier_widgets[i]->compute_batch_evaluation_contribution(batch_evaluation, nu_base, transcript);
    }

    fr::__neg(batch_evaluation, batch_evaluation);

    fr::field_t z_omega_scalar;
    fr::__mul(z_challenge, domain.root, z_omega_scalar);
    fr::__mul(z_omega_scalar, u, z_omega_scalar);

    std::vector<fr::field_t> scalars;
    std::vector<g1::affine_element> elements;

    elements.emplace_back(Z_1);
    scalars.emplace_back(linear_terms.z_1);

    fr::__copy(nu_pow[8], nu_base);

    for (size_t i = 0; i < program_settings::program_width; ++i) {
        if (g1::on_curve(W[i])) {
            elements.emplace_back(W[i]);
            if (program_settings::requires_shifted_wire(program_settings::wire_shift_settings, i)) {
                fr::__mul(nu_base, u, T0);
                fr::__add(T0, nu_pow[1 + i], T0);
                scalars.emplace_back(T0);
                fr::__mul(nu_base, nu_pow[0], nu_base);
            } else {
                scalars.emplace_back(nu_pow[1 + i]);
            }
        }
    }

    for (size_t i = 0; i < program_settings::program_width - 1; ++i)
    {
        elements.emplace_back(SIGMA[i]);
        scalars.emplace_back(nu_pow[5 + i]);

    }

    elements.emplace_back(SIGMA[program_settings::program_width - 1]);
    scalars.emplace_back(linear_terms.sigma_last);

    elements.emplace_back(g1::affine_one);
    scalars.emplace_back(batch_evaluation);

    if (g1::on_curve(PI_Z_OMEGA)) {
        elements.emplace_back(PI_Z_OMEGA);
        scalars.emplace_back(z_omega_scalar);
    }

    elements.emplace_back(PI_Z);
    scalars.emplace_back(z_challenge);

    for (size_t i = 1; i < program_settings::program_width; ++i) {
        fr::field_t z_power = fr::pow_small(z_challenge, n * i);
        if (g1::on_curve(T[i])) {
            elements.emplace_back(T[i]);
            scalars.emplace_back(z_power);
        }
    }

    VerifierBaseWidget::challenge_coefficients coeffs{
        fr::sqr(fr::sqr(alpha)), alpha, nu_base, nu, nu
    };

    for (size_t i = 0; i < verifier_widgets.size(); ++i) {
        coeffs = verifier_widgets[i]->append_scalar_multiplication_inputs(coeffs, transcript, elements, scalars);
    }

    size_t num_elements = elements.size();
    elements.resize(num_elements * 2);
    barretenberg::scalar_multiplication::generate_pippenger_point_table(&elements[0], &elements[0], num_elements);
    g1::element P[2];

    P[0] = g1::group_exponentiation_inner(PI_Z_OMEGA, u);
    P[1] = barretenberg::scalar_multiplication::pippenger(&scalars[0], &elements[0], num_elements);

    g1::mixed_add(P[1], T[0], P[1]);
    g1::mixed_add(P[0], PI_Z, P[0]);
    g1::__neg(P[0], P[0]);
    g1::batch_normalize(P, 2);

    g1::affine_element P_affine[2];
    barretenberg::fq::__copy(P[0].x, P_affine[1].x);
    barretenberg::fq::__copy(P[0].y, P_affine[1].y);
    barretenberg::fq::__copy(P[1].x, P_affine[0].x);
    barretenberg::fq::__copy(P[1].y, P_affine[0].y);

    barretenberg::fq12::field_t result =
        barretenberg::pairing::reduced_ate_pairing_batch_precomputed(P_affine, reference_string.precomputed_g2_lines, 2);

    return barretenberg::fq12::eq(result, barretenberg::fq12::one);
}

} // namespace waffle