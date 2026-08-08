#!/usr/bin/env python3
"""Loads a collected runtime and solves one equilibrium through it.

The point is not to test Reaktoro, which has its own suite. It is to prove that the folder about to
be shipped is self-contained and that the flat C API answers: the collection step can copy the
wrong set of libraries, or copy the right ones and leave them pointing at the build machine, and
either way the failure belongs here and not on a user's computer.
"""

import argparse
import ctypes
import os
import sys
from pathlib import Path


def library_name(rid: str) -> str:
    if rid.startswith("win"):
        return "ReaktoroC.dll"
    if rid.startswith("osx"):
        return "libReaktoroC.dylib"
    return "libReaktoroC.so"


def text_from(function, *arguments) -> str:
    needed = function(*arguments, None, 0)
    if needed <= 0:
        return ""
    buffer = ctypes.create_string_buffer(needed + 1)
    function(*arguments, buffer, needed + 1)
    return buffer.value.decode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rid", required=True)
    parser.add_argument("--dir", required=True)
    arguments = parser.parse_args()

    directory = Path(arguments.dir).resolve()
    path = directory / library_name(arguments.rid)

    if not path.exists():
        print(f"No library at {path}.", file=sys.stderr)
        return 1

    # Windows resolves a DLL's own dependencies from wherever it was loaded from only if told to.
    if sys.platform == "win32":
        os.add_dll_directory(str(directory))

    library = ctypes.CDLL(str(path))

    library.reaktoro_version.argtypes = [ctypes.c_char_p, ctypes.c_int]
    library.reaktoro_version.restype = ctypes.c_int
    library.reaktoro_last_error.argtypes = [ctypes.c_char_p, ctypes.c_int]
    library.reaktoro_last_error.restype = ctypes.c_int
    library.reaktoro_create.argtypes = [ctypes.c_char_p] * 4
    library.reaktoro_create.restype = ctypes.c_void_p
    library.reaktoro_destroy.argtypes = [ctypes.c_void_p]
    library.reaktoro_species_count.argtypes = [ctypes.c_void_p]
    library.reaktoro_species_count.restype = ctypes.c_int
    library.reaktoro_species_names.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    library.reaktoro_species_names.restype = ctypes.c_int
    library.reaktoro_equilibrate.argtypes = [
        ctypes.c_void_p, ctypes.c_double, ctypes.c_double, ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_double), ctypes.c_int,
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double)]
    library.reaktoro_equilibrate.restype = ctypes.c_int

    print("Reaktoro " + text_from(library.reaktoro_version))

    system = library.reaktoro_create(b"supcrt07-organics",
                                     b"H2O(aq) H+ OH- CO2(aq) HCO3- CO3-- Na+ Cl-",
                                     b"CO2(g) H2O(g)",
                                     b"PengRobinson")

    if not system:
        print("reaktoro_create failed: " + text_from(library.reaktoro_last_error), file=sys.stderr)
        return 1

    count = library.reaktoro_species_count(system)
    print(f"{count} species: " + text_from(library.reaktoro_species_names, system))

    # A mole of water, a little salt and a little carbon dioxide, at 25 C and one bar.
    substances = b"H2O;NaCl;CO2"
    amounts = (ctypes.c_double * 3)(1.0, 0.01, 0.05)

    species_amounts = (ctypes.c_double * count)()
    ln_gamma = (ctypes.c_double * count)()
    aqueous = ctypes.c_double()
    gaseous = ctypes.c_double()

    status = library.reaktoro_equilibrate(system, 298.15, 1.0e5, substances, amounts, 3,
                                          species_amounts, ln_gamma, aqueous, gaseous)

    if status != 0:
        print("reaktoro_equilibrate failed: " + text_from(library.reaktoro_last_error),
              file=sys.stderr)
        return 1

    total = sum(species_amounts)

    print(f"aqueous {aqueous.value:.6f} mol, gaseous {gaseous.value:.6f} mol, "
          f"species total {total:.6f} mol")

    library.reaktoro_destroy(system)

    # Everything that went in has to come out somewhere: about one mole of water plus what was
    # dissolved. A wildly different number means the equilibrium did not actually run.
    if not 0.9 < total < 1.3:
        print(f"The species amounts sum to {total}, which is not what went in.", file=sys.stderr)
        return 1

    if aqueous.value <= 0.0:
        print("The aqueous phase came back empty.", file=sys.stderr)
        return 1

    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
