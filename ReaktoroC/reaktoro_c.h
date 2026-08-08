// A flat C API over the part of Reaktoro that DWSIM calls.
//
// Reaktoro's own interface is C++: templates, std::string, std::vector, Eigen arrays and autodiff
// types in the signatures, none of which crosses a P/Invoke boundary. This is the shim that lets a
// managed caller reach the equilibrium solver without a Python interpreter in between.
//
// The surface is deliberately the one DWSIM needs and nothing more: build a chemical system from an
// aqueous and a gaseous species list, equilibrate a set of substance amounts at a temperature and a
// pressure, and read back the species amounts, their activity coefficients and the amount in each
// phase.
//
// Every function that can fail says so in its return value and leaves the reason where
// reaktoro_last_error reads it. Nothing throws across the boundary.

#ifndef REAKTORO_C_H
#define REAKTORO_C_H

#if defined(_WIN32)
    #if defined(REAKTORO_C_BUILDING)
        #define REAKTORO_C_API __declspec(dllexport)
    #else
        #define REAKTORO_C_API __declspec(dllimport)
    #endif
#else
    #define REAKTORO_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// A chemical system and the phases built on it. Opaque: created by reaktoro_create, released by
/// reaktoro_destroy.
typedef struct ReaktoroSystem ReaktoroSystem;

/// Reaktoro's version, written into buffer. Returns the number of characters the version needs,
/// buffer or no buffer, so a caller can size one.
REAKTORO_C_API int reaktoro_version(char* buffer, int size);

/// Why the last call on this thread failed. Empty when nothing has failed.
REAKTORO_C_API int reaktoro_last_error(char* buffer, int size);

/// Builds a chemical system from a database and two species lists, each a run of species names
/// separated by spaces or semicolons. The gaseous list may be empty, in which case the system has
/// an aqueous phase alone.
///
/// The aqueous phase carries the HKF activity model with Drummond's correction for dissolved CO2,
/// which is what DWSIM asked Reaktoro 1 for. gaseous_model selects the model of the gaseous phase:
/// "PengRobinson", "SoaveRedlichKwong" or "IdealGas". A null or empty string means Peng-Robinson.
///
/// Returns null on failure, with the reason in reaktoro_last_error.
REAKTORO_C_API ReaktoroSystem* reaktoro_create(const char* database,
                                               const char* aqueous_species,
                                               const char* gaseous_species,
                                               const char* gaseous_model);

/// Releases a system. Null is accepted and ignored.
REAKTORO_C_API void reaktoro_destroy(ReaktoroSystem* system);

/// How many species the system holds. This is the length of every array the equilibrium writes, and
/// the order they are all in. Returns a negative number if the system is null.
REAKTORO_C_API int reaktoro_species_count(const ReaktoroSystem* system);

/// The species names in that order, joined by semicolons. Returns the number of characters needed.
REAKTORO_C_API int reaktoro_species_names(const ReaktoroSystem* system, char* buffer, int size);

/// Equilibrium at T kelvin and P pascal.
///
/// The substances are added by name or by chemical formula, as Reaktoro 1's equilibrium problem
/// took them: "H2O", "CO2", "NaCl". substances is a semicolon-separated list and amounts holds one
/// value per entry, in moles.
///
/// species_amounts and ln_activity_coefficients each receive reaktoro_species_count values, in
/// species order. aqueous_amount and gaseous_amount receive the total amount in each phase, in
/// moles; the gaseous one is zero where the system has no gaseous phase. Any output pointer may be
/// null, and that quantity is not written.
///
/// Returns 0 on success.
REAKTORO_C_API int reaktoro_equilibrate(ReaktoroSystem* system,
                                        double temperature,
                                        double pressure,
                                        const char* substances,
                                        const double* amounts,
                                        int amounts_size,
                                        double* species_amounts,
                                        double* ln_activity_coefficients,
                                        double* aqueous_amount,
                                        double* gaseous_amount);

#ifdef __cplusplus
}
#endif

#endif
