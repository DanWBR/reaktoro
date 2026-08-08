// The implementation of the flat C API declared in reaktoro_c.h.
//
// Two rules run through the whole file. Nothing throws across the boundary: every entry point wraps
// its body and turns an exception into a return value plus a message. And nothing owns memory the
// caller has to free except the system handle, which reaktoro_destroy releases; every other result
// is written into a buffer the caller supplies.

#define REAKTORO_C_BUILDING

#include "reaktoro_c.h"

#include <exception>
#include <string>
#include <vector>

#include <Reaktoro/Reaktoro.hpp>

using namespace Reaktoro;

namespace
{

/// The reason the last call on this thread failed. Per thread, because the equilibrium is called
/// from whatever thread the flowsheet solver happens to be on, and a shared one would cross wires.
thread_local std::string lastError;

/// Writes text into a caller's buffer and returns the number of characters it needs, terminator
/// aside. A buffer too small is left alone: the caller sizes one from the return value and asks
/// again.
auto writeString(const std::string& text, char* buffer, int size) -> int
{
    const int needed = static_cast<int>(text.size());

    if(buffer != nullptr && size > needed)
    {
        std::char_traits<char>::copy(buffer, text.c_str(), needed);
        buffer[needed] = '\0';
    }

    return needed;
}

/// Splits a list of names on spaces, tabs and semicolons, so the caller can use whichever separator
/// its own code already produces.
auto splitNames(const char* text) -> std::vector<std::string>
{
    std::vector<std::string> names;

    if(text == nullptr) return names;

    std::string current;

    for(const char* c = text; *c != '\0'; ++c)
    {
        if(*c == ' ' || *c == '\t' || *c == ';' || *c == '\n' || *c == '\r')
        {
            if(!current.empty()) { names.push_back(current); current.clear(); }
        }
        else current.push_back(*c);
    }

    if(!current.empty()) names.push_back(current);

    return names;
}

/// The activity model of the gaseous phase, by name. Peng-Robinson is the default because it is
/// what Reaktoro 1's chemical editor gave a gaseous phase that was not told otherwise.
auto gaseousActivityModel(const char* name) -> ActivityModelGenerator
{
    const std::string requested = name == nullptr ? "" : name;

    if(requested == "IdealGas") return ActivityModelIdealGas();
    if(requested == "SoaveRedlichKwong") return ActivityModelSoaveRedlichKwong();

    return ActivityModelPengRobinson();
}

} // namespace

/// What a handle carries: the system itself, and the two things every call would otherwise recompute
/// - the species names in system order and where each phase sits in it.
struct ReaktoroSystem
{
    ChemicalSystem system;
    std::vector<std::string> speciesNames;
    Index aqueousPhaseIndex = 0;
    Index gaseousPhaseIndex = 0;
    bool hasGaseousPhase = false;
};

#ifndef REAKTORO_C_VERSION
    #define REAKTORO_C_VERSION "unknown"
#endif

int reaktoro_version(char* buffer, int size)
{
    return writeString(REAKTORO_C_VERSION, buffer, size);
}

int reaktoro_last_error(char* buffer, int size)
{
    return writeString(lastError, buffer, size);
}

ReaktoroSystem* reaktoro_create(const char* database,
                                const char* aqueous_species,
                                const char* gaseous_species,
                                const char* gaseous_model)
{
    lastError.clear();

    try
    {
        const auto aqueousNames = splitNames(aqueous_species);
        const auto gaseousNames = splitNames(gaseous_species);

        if(aqueousNames.empty())
        {
            lastError = "Reaktoro needs at least one aqueous species.";
            return nullptr;
        }

        SupcrtDatabase db(database == nullptr || *database == '\0' ? "supcrt07-organics" : database);

        AqueousPhase aqueous(StringList(aqueousNames));
        aqueous.set(chain(ActivityModelHKF(), ActivityModelDrummond("CO2")));

        auto handle = new ReaktoroSystem();

        if(gaseousNames.empty())
        {
            handle->system = ChemicalSystem(db, aqueous);
        }
        else
        {
            GaseousPhase gaseous(StringList(gaseousNames));
            gaseous.set(gaseousActivityModel(gaseous_model));

            handle->system = ChemicalSystem(db, aqueous, gaseous);
            handle->hasGaseousPhase = true;
        }

        // The phases come out in the order they went in, but read the index back rather than assume
        // it: a caller that later adds a mineral phase should not silently shift these.
        for(Index i = 0; i < handle->system.phases().size(); ++i)
        {
            const auto& name = handle->system.phase(i).name();

            if(name == "AqueousPhase") handle->aqueousPhaseIndex = i;
            if(name == "GaseousPhase") handle->gaseousPhaseIndex = i;
        }

        for(const auto& species : handle->system.species())
            handle->speciesNames.push_back(species.name());

        return handle;
    }
    catch(const std::exception& e)
    {
        lastError = e.what();
        return nullptr;
    }
    catch(...)
    {
        lastError = "Reaktoro failed to build the chemical system.";
        return nullptr;
    }
}

void reaktoro_destroy(ReaktoroSystem* system)
{
    delete system;
}

int reaktoro_species_count(const ReaktoroSystem* system)
{
    if(system == nullptr) return -1;

    return static_cast<int>(system->speciesNames.size());
}

int reaktoro_species_names(const ReaktoroSystem* system, char* buffer, int size)
{
    if(system == nullptr) return -1;

    std::string joined;

    for(const auto& name : system->speciesNames)
    {
        if(!joined.empty()) joined.push_back(';');
        joined += name;
    }

    return writeString(joined, buffer, size);
}

int reaktoro_equilibrate(ReaktoroSystem* system,
                         double temperature,
                         double pressure,
                         const char* substances,
                         const double* amounts,
                         int amounts_size,
                         double* species_amounts,
                         double* ln_activity_coefficients,
                         double* aqueous_amount,
                         double* gaseous_amount)
{
    lastError.clear();

    if(system == nullptr)
    {
        lastError = "No chemical system.";
        return 1;
    }

    try
    {
        const auto names = splitNames(substances);

        if(static_cast<int>(names.size()) != amounts_size)
        {
            lastError = "The substance list and the amounts do not have the same length.";
            return 2;
        }

        Material material(system->system);

        for(int i = 0; i < amounts_size; ++i)
        {
            // Reaktoro ignores nothing: a substance at zero moles still constrains the elements it
            // is made of, and adding it makes the problem harder for no gain.
            if(amounts[i] > 0.0)
                material.add(names[i], amounts[i], "mol");
        }

        auto state = material.equilibrate(temperature, "K", pressure, "Pa");

        const auto props = ChemicalProps(state);

        if(species_amounts != nullptr)
        {
            const auto values = state.speciesAmounts();

            for(int i = 0; i < reaktoro_species_count(system); ++i)
                species_amounts[i] = values[i];
        }

        if(ln_activity_coefficients != nullptr)
        {
            const auto values = props.speciesActivityCoefficientsLn();

            for(int i = 0; i < reaktoro_species_count(system); ++i)
                ln_activity_coefficients[i] = values[i];
        }

        if(aqueous_amount != nullptr)
            *aqueous_amount = static_cast<double>(props.phaseProps(system->aqueousPhaseIndex).amount());

        if(gaseous_amount != nullptr)
            *gaseous_amount = system->hasGaseousPhase
                ? static_cast<double>(props.phaseProps(system->gaseousPhaseIndex).amount())
                : 0.0;

        return 0;
    }
    catch(const std::exception& e)
    {
        lastError = e.what();
        return 3;
    }
    catch(...)
    {
        lastError = "Reaktoro failed to compute the equilibrium state.";
        return 3;
    }
}

int reaktoro_properties(ReaktoroSystem* system,
                        double temperature,
                        double pressure,
                        const double* species_amounts,
                        int species_amounts_size,
                        double* ln_activity_coefficients)
{
    lastError.clear();

    if(system == nullptr)
    {
        lastError = "No chemical system.";
        return 1;
    }

    const int count = reaktoro_species_count(system);

    if(species_amounts_size != count)
    {
        lastError = "Expected one amount per species.";
        return 2;
    }

    try
    {
        ChemicalState state(system->system);
        state.temperature(temperature);
        state.pressure(pressure);

        ArrayXd amounts(count);
        for(int i = 0; i < count; ++i)
            amounts[i] = species_amounts[i];

        state.setSpeciesAmounts(amounts);

        const auto props = ChemicalProps(state);

        if(ln_activity_coefficients != nullptr)
        {
            const auto values = props.speciesActivityCoefficientsLn();

            for(int i = 0; i < count; ++i)
                ln_activity_coefficients[i] = values[i];
        }

        return 0;
    }
    catch(const std::exception& e)
    {
        lastError = e.what();
        return 3;
    }
    catch(...)
    {
        lastError = "Reaktoro failed to evaluate the chemical properties.";
        return 3;
    }
}
